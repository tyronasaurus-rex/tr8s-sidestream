#include "WasapiCapture.h"
#include <climits>
#include <cmath>

namespace
{
/** Expected stem-pair labels in USB INDIV order. */
const char* pairLabel (int index)
{
    static constexpr const char* labels[WasapiCapture::kNumPairs] = {
        "IN 1-2", "IN 3-4", "IN 5-6", "IN 7-8", "IN 9-10", "IN 11-12"
    };
    if (index < 0 || index >= WasapiCapture::kNumPairs)
        return "?";
    return labels[index];
}

juce::WASAPIDeviceMode modeFromTypeName (const juce::String& typeName)
{
    if (typeName == WasapiCapture::kExclusiveWasapi)
        return juce::WASAPIDeviceMode::exclusive;
    return juce::WASAPIDeviceMode::shared;
}

const char* shortModeName (juce::WASAPIDeviceMode mode)
{
    return mode == juce::WASAPIDeviceMode::exclusive ? "excl" : "shared";
}
} // namespace

//==============================================================================
struct WasapiCapture::PairEndpoint final : private juce::AudioIODeviceCallback
{
    explicit PairEndpoint (WasapiCapture& o, int pairIdx)
        : owner (o), pairIndex (pairIdx), channelOffset (pairIdx * 2)
    {
    }

    ~PairEndpoint() override
    {
        close();
    }

    juce::String open (const juce::String& deviceName,
                       const juce::String& typeName,
                       double preferredSampleRate,
                       int preferredBufferSize)
    {
        close();

        const int period = WasapiCapture::snapWasapiBuffer (
            owner.wasapiBufferSize > 0 ? owner.wasapiBufferSize
                                       : (preferredBufferSize > 0 ? preferredBufferSize : kDefaultWasapiBuffer));
        const double rate = preferredSampleRate > 0.0 ? preferredSampleRate : 48000.0;
        const auto mode = modeFromTypeName (typeName.isNotEmpty() ? typeName
                                                                  : juce::String (kExclusiveWasapi));

        deviceType.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (mode));
        if (deviceType == nullptr)
            return "WASAPI type unavailable (" + juce::String (shortModeName (mode)) + ")";

        deviceType->scanForDevices();
        const auto inputs = deviceType->getDeviceNames (true);
        if (! inputs.contains (deviceName))
            return juce::String (shortModeName (mode)) + " device missing: " + deviceName
                   + " (scan=" + juce::String (inputs.size()) + ")";

        device.reset (deviceType->createDevice ({}, deviceName));
        if (device == nullptr)
            return "createDevice failed (" + juce::String (shortModeName (mode)) + "): " + deviceName;

        juce::BigInteger inCh;
        inCh.setBit (0);
        inCh.setBit (1);

        auto err = device->open (inCh, juce::BigInteger {}, rate, period);
        if (err.isNotEmpty())
        {
            juce::BigInteger allIn;
            const int nIn = device->getInputChannelNames().size();
            for (int i = 0; i < juce::jmin (2, nIn); ++i)
                allIn.setBit (i);
            err = device->open (allIn, juce::BigInteger {}, rate, period);
        }

        if (err.isNotEmpty())
        {
            device.reset();
            deviceType.reset();
            return juce::String (shortModeName (mode)) + " / " + deviceName + ": " + err;
        }

        deviceNameOpen = deviceName;
        typeNameOpen = deviceType->getTypeName();
        sampleRate = device->getCurrentSampleRate();
        bufferSize = device->getCurrentBufferSizeSamples();
        latencyIn = device->getInputLatencyInSamples();
        activeChannels = juce::jmin (2, device->getActiveInputChannels().countNumberOfSetBits());

        if (activeChannels <= 0)
        {
            close();
            return "Device open but 0 input channels: " + deviceName;
        }

        const int ringSize = owner.ringCapacityForRate (sampleRate);
        ring.resize (2, ringSize);
        ring.reset();
        ring.clearCounters();
        resetAdaptive();
        lastWriteTicks.store (juce::Time::getHighResolutionTicks(), std::memory_order_relaxed);
        callbackCount.store (0, std::memory_order_relaxed);
        samplesWritten.store (0, std::memory_order_relaxed);
        resetRateWindow();
        frozenCbHz = 0.0;
        frozenWrRate = 0.0;
        peakCbHz = 0.0;
        peakWrRate = 0.0;
        markedDead = false;

        device->start (this);

        if (! device->isPlaying())
        {
            close();
            return "WASAPI start failed (" + juce::String (shortModeName (mode)) + "): " + deviceName;
        }

        return {};
    }

    void close()
    {
        if (device != nullptr)
        {
            device->stop();
            device->close();
            device.reset();
        }
        deviceType.reset();
        deviceNameOpen.clear();
        typeNameOpen.clear();
        activeChannels = 0;
        sampleRate = 0.0;
        bufferSize = 0;
        latencyIn = 0;
        ring.reset();
        resetAdaptive();
        markedDead = false;
        frozenCbHz = 0.0;
        frozenWrRate = 0.0;
        peakCbHz = 0.0;
        peakWrRate = 0.0;
    }

    void resetAdaptive() noexcept
    {
        fracPhase = 0.0;
        ratio = 1.0;
        integ = 0.0;
    }

    bool isOpen() const noexcept
    {
        return device != nullptr && device->isOpen() && activeChannels > 0;
    }

    uint64_t totalCallbacks() const noexcept
    {
        return callbackCount.load (std::memory_order_relaxed);
    }

    uint64_t totalSamplesWritten() const noexcept
    {
        return samplesWritten.load (std::memory_order_relaxed);
    }

    void resetRateWindow() noexcept
    {
        diagEpochTicks = juce::Time::getHighResolutionTicks();
        diagCbAnchor = callbackCount.load (std::memory_order_relaxed);
        diagWrAnchor = samplesWritten.load (std::memory_order_relaxed);
    }

    /** Update frozen 1s averages only when a full second has elapsed (no flicker). */
    void tickFrozenRates() noexcept
    {
        if (! isOpen())
            return;

        const auto now = juce::Time::getHighResolutionTicks();
        const double elapsed = juce::Time::highResolutionTicksToSeconds (now - diagEpochTicks);
        if (elapsed < 1.0)
            return;

        const uint64_t cb = callbackCount.load (std::memory_order_relaxed);
        const uint64_t wr = samplesWritten.load (std::memory_order_relaxed);
        const double cbHz = (double) (cb - diagCbAnchor) / elapsed;
        const double wrRate = (double) (wr - diagWrAnchor) / elapsed;

        frozenCbHz = cbHz;
        frozenWrRate = wrRate;
        peakCbHz = juce::jmax (peakCbHz, cbHz);
        peakWrRate = juce::jmax (peakWrRate, wrRate);

        diagEpochTicks = now;
        diagCbAnchor = cb;
        diagWrAnchor = wr;
    }

    bool isCallbackAlive (double timeoutSec = 0.50) const noexcept
    {
        if (! isOpen())
            return false;
        const auto last = lastWriteTicks.load (std::memory_order_relaxed);
        if (last == 0)
            return false;
        const double sec = juce::Time::highResolutionTicksToSeconds (
            juce::Time::getHighResolutionTicks() - last);
        return sec < timeoutSec;
    }

    bool isHealthy() const noexcept
    {
        return isOpen() && ! markedDead && isCallbackAlive();
    }

    double updateRatio (int avail, int targetMid, double masterR, double masterBlend) noexcept
    {
        if (targetMid < 1)
            targetMid = 1;

        const double err = ((double) avail - (double) targetMid) / (double) targetMid;
        integ = juce::jlimit (-0.04, 0.04, integ + err * 0.0008);
        double local = 1.0 + 0.12 * err + integ;
        local = juce::jlimit (0.94, 1.06, local);
        ratio = juce::jlimit (0.94, 1.06, (1.0 - masterBlend) * local + masterBlend * masterR);
        return ratio;
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        juce::ignoreUnused (outputChannelData, numOutputChannels);

        if (inputChannelData == nullptr || numSamples <= 0)
            return;

        lastWriteTicks.store (juce::Time::getHighResolutionTicks(), std::memory_order_relaxed);
        callbackCount.fetch_add (1, std::memory_order_relaxed);
        samplesWritten.fetch_add ((uint64_t) numSamples, std::memory_order_relaxed);
        if (markedDead)
            markedDead = false;

        const float* src[2] = {};
        const int n = juce::jmin (2, numInputChannels);

        for (int ch = 0; ch < n; ++ch)
        {
            src[ch] = inputChannelData[ch];
            if (src[ch] != nullptr)
            {
                float peak = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                    peak = juce::jmax (peak, std::abs (src[ch][i]));

                const int globalCh = channelOffset + ch;
                if (globalCh >= 0 && globalCh < kMaxChannels)
                {
                    const float prev = owner.peaks[(size_t) globalCh].load (std::memory_order_relaxed);
                    if (peak > prev)
                        owner.peaks[(size_t) globalCh].store (peak, std::memory_order_relaxed);
                }
            }
        }

        ring.write (src, numSamples);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override
    {
        if (dev == nullptr)
            return;

        sampleRate = dev->getCurrentSampleRate();
        bufferSize = dev->getCurrentBufferSizeSamples();
        latencyIn = dev->getInputLatencyInSamples();
        activeChannels = juce::jmin (2, dev->getActiveInputChannels().countNumberOfSetBits());
        owner.notifyStatus();
    }

    void audioDeviceStopped() override
    {
        owner.notifyStatus();
    }

    WasapiCapture& owner;
    int pairIndex = 0;
    int channelOffset = 0;
    std::unique_ptr<juce::AudioIODeviceType> deviceType;
    std::unique_ptr<juce::AudioIODevice> device;
    PlanarRingBuffer ring;
    juce::String deviceNameOpen;
    juce::String typeNameOpen;
    int activeChannels = 0;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int latencyIn = 0;

    std::atomic<juce::int64> lastWriteTicks { 0 };
    std::atomic<uint64_t> callbackCount { 0 };
    std::atomic<uint64_t> samplesWritten { 0 };
    juce::int64 diagEpochTicks = 0;
    uint64_t diagCbAnchor = 0;
    uint64_t diagWrAnchor = 0;
    double frozenCbHz = 0.0;
    double frozenWrRate = 0.0;
    double peakCbHz = 0.0;
    double peakWrRate = 0.0;
    double fracPhase = 0.0;
    double ratio = 1.0;
    double integ = 0.0;
    bool markedDead = false;
};

//==============================================================================
bool WasapiCapture::isTr8sFamily (const juce::String& name)
{
    return name.containsIgnoreCase ("TR-8S") || name.containsIgnoreCase ("TR8S");
}

bool WasapiCapture::isTr8sMix (const juce::String& name)
{
    return isTr8sFamily (name) && name.containsIgnoreCase ("MIX");
}

int WasapiCapture::pairIndexFromDeviceName (const juce::String& name)
{
    if (! isTr8sFamily (name) || isTr8sMix (name))
        return -1;

    for (int i = 0; i < kNumPairs; ++i)
    {
        if (name.containsIgnoreCase (pairLabel (i)))
            return i;
    }

    return -1;
}

int WasapiCapture::aggregatePairCountFromName (const juce::String& name) noexcept
{
    if (name == kAggregate2Name || name.startsWith (kAggregate2Name))
        return 2;
    if (name == kAggregate4Name || name.startsWith (kAggregate4Name))
        return 4;
    if (name == kAggregateExclusiveName || name.startsWith (kAggregateExclusiveName)
        || name == kAggregateName || name.startsWith (kAggregateName))
        return 6;
    return 0;
}

int WasapiCapture::snapWasapiBuffer (int samples) noexcept
{
    static constexpr int choices[] = { 128, 256, 512, 1024, 2048 };
    int best = 1024;
    int bestDist = INT_MAX;
    for (int c : choices)
    {
        const int d = std::abs (c - samples);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

void WasapiCapture::setLatencyMs (int ms) noexcept
{
    latencyMs = juce::jlimit (kMinLatencyMs, kMaxLatencyMs, ms);
}

void WasapiCapture::setWasapiBufferSize (int samples) noexcept
{
    wasapiBufferSize = snapWasapiBuffer (samples);
}

int WasapiCapture::getTargetFillSamples() const noexcept
{
    const double rate = captureSampleRate > 0.0 ? captureSampleRate : 48000.0;
    return juce::jmax (256, (int) std::lround (rate * (double) latencyMs / 1000.0));
}

int WasapiCapture::midpointTargetSamples() const noexcept
{
    return juce::jmax (128, (int) std::lround ((double) getTargetFillSamples() * 0.60));
}

int WasapiCapture::getPrimingTargetSamples() const noexcept
{
    return midpointTargetSamples() + juce::jmax (captureBufferSize, wasapiBufferSize);
}

int WasapiCapture::pickMasterPair() const noexcept
{
    if (aggregateOpen)
    {
        if (pairs[0] != nullptr && pairs[0]->isHealthy())
            return 0;
        for (int i = 1; i < kNumPairs; ++i)
            if (pairs[(size_t) i] != nullptr && pairs[(size_t) i]->isHealthy())
                return i;
    }
    return 0;
}

int WasapiCapture::ringCapacityForRate (double sampleRate) const noexcept
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int target = juce::jmax (256, (int) std::lround (rate * (double) latencyMs / 1000.0));
    return juce::jmax (16384, target * 4, (int) std::lround (rate * 1.0));
}

WasapiCapture::WasapiCapture()
{
    for (auto& p : peaks)
        p.store (0.0f, std::memory_order_relaxed);

    for (int i = 0; i < kNumPairs; ++i)
        pairs[(size_t) i] = std::make_unique<PairEndpoint> (*this, i);

    refreshDeviceList();
}

WasapiCapture::~WasapiCapture()
{
    healthDumpGeneration.fetch_add (1, std::memory_order_relaxed);
    closeDevice();
}

juce::Array<WasapiCapture::DeviceInfo> WasapiCapture::discoverPhysicalDevices() const
{
    juce::Array<DeviceInfo> found;

    const juce::WASAPIDeviceMode modes[] = {
        juce::WASAPIDeviceMode::shared,
        juce::WASAPIDeviceMode::exclusive
    };

    for (auto mode : modes)
    {
        std::unique_ptr<juce::AudioIODeviceType> type (
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (mode));
        if (type == nullptr)
            continue;

        type->scanForDevices();
        for (const auto& name : type->getDeviceNames (true))
        {
            DeviceInfo info;
            info.name = name;
            info.typeName = type->getTypeName();
            info.isAggregate = false;
            found.add (info);
        }
    }

    return found;
}

void WasapiCapture::refreshDeviceList()
{
    auto physical = discoverPhysicalDevices();
    juce::Array<DeviceInfo> found;

    auto countStemPairs = [&] (const juce::String& typeName) -> int
    {
        bool have[kNumPairs] = {};
        int n = 0;
        for (const auto& d : physical)
        {
            if (d.typeName != typeName)
                continue;
            const int idx = pairIndexFromDeviceName (d.name);
            if (idx >= 0 && ! have[idx])
            {
                have[idx] = true;
                ++n;
            }
        }
        return n;
    };

    const int sharedPairs = countStemPairs (kSharedWasapi);
    const int exclPairs = countStemPairs (kExclusiveWasapi);
    const bool canAgg = sharedPairs == kNumPairs || exclPairs == kNumPairs;

    if (canAgg)
    {
        auto addAgg = [&] (const char* name, int nPairs, const char* type)
        {
            DeviceInfo agg;
            agg.name = name;
            agg.typeName = type;
            agg.inputChannels = nPairs * 2;
            agg.isAggregate = true;
            agg.aggregatePairs = nPairs;
            found.add (agg);
        };

        // Prefer exclusive listing; open path still falls back.
        const char* type = exclPairs == kNumPairs ? kExclusiveWasapi : kSharedWasapi;
        addAgg (kAggregate2Name, 2, type);
        addAgg (kAggregate4Name, 4, type);
        addAgg (kAggregateName, 6, type);
        if (exclPairs == kNumPairs)
            addAgg (kAggregateExclusiveName, 6, kExclusiveWasapi);
    }

    for (const auto& d : physical)
        found.add (d);

    {
        const juce::ScopedLock sl (deviceListLock);
        devices = std::move (found);
    }

    notifyStatus();
}

juce::Array<WasapiCapture::DeviceInfo> WasapiCapture::getDevices() const
{
    const juce::ScopedLock sl (deviceListLock);
    return devices;
}

juce::String WasapiCapture::openAggregate (double preferredSampleRate,
                                           int preferredBufferSize,
                                           int requestedPairs,
                                           bool allowFallback)
{
    closeDevice();
    lastError.clear();
    architectureNote.clear();
    targetPairCount = 0;

    refreshDeviceList();

    requestedPairs = juce::jlimit (1, kNumPairs, requestedPairs);
    // Always try exclusive for multi-pair; shared is known to starve.
    juce::ignoreUnused (preferredBufferSize);

    struct FoundPair
    {
        juce::String name;
        juce::String typeName;
    };

    auto collectPairs = [this] (bool wantExclusive, FoundPair out[kNumPairs]) -> int
    {
        for (int i = 0; i < kNumPairs; ++i)
            out[i] = {};

        const juce::String primaryType = wantExclusive ? juce::String (kExclusiveWasapi)
                                                       : juce::String (kSharedWasapi);
        const juce::String fallbackType = wantExclusive ? juce::String (kSharedWasapi)
                                                        : juce::String (kExclusiveWasapi);
        int foundCount = 0;
        const juce::ScopedLock sl (deviceListLock);

        for (int pass = 0; pass < 2; ++pass)
        {
            const juce::String wantType = (pass == 0 ? primaryType : fallbackType);
            for (const auto& d : devices)
            {
                if (d.isAggregate || d.typeName != wantType)
                    continue;
                const int idx = pairIndexFromDeviceName (d.name);
                if (idx < 0 || out[idx].name.isNotEmpty())
                    continue;
                out[idx] = { d.name, d.typeName };
                ++foundCount;
            }
        }
        return foundCount;
    };

    auto openN = [&] (FoundPair found[kNumPairs], int nPairs, juce::StringArray& errors,
                      double& rate, int& buf, int& latency) -> int
    {
        int opened = 0;
        rate = 0.0;
        buf = 0;
        latency = 0;
        errors.clear();

        for (int i = 0; i < nPairs; ++i)
        {
            if (found[i].name.isEmpty())
            {
                errors.add (juce::String (pairLabel (i)) + ": missing");
                if (i == 0)
                    return 0;
                continue;
            }

            if (i > 0)
                juce::Thread::sleep (40);

            auto err = pairs[(size_t) i]->open (found[i].name,
                                                found[i].typeName,
                                                preferredSampleRate,
                                                wasapiBufferSize);
            if (err.isNotEmpty())
            {
                errors.add (juce::String (pairLabel (i)) + ": " + err);
                if (i == 0)
                    return 0;
                continue;
            }

            ++opened;
            rate = pairs[(size_t) i]->sampleRate;
            buf = juce::jmax (buf, pairs[(size_t) i]->bufferSize);
            latency = juce::jmax (latency, pairs[(size_t) i]->latencyIn);
        }
        return opened;
    };

    auto countSustaining = [this] (int nPairs) -> int
    {
        int ok = 0;
        for (int i = 0; i < nPairs; ++i)
        {
            auto& p = pairs[(size_t) i];
            if (p == nullptr || ! p->isOpen())
                continue;
            p->tickFrozenRates();
            // During probe we may not have a full frozen second yet — use raw window.
            if (p->frozenWrRate >= kMinSustainWriteRate)
                ++ok;
            else
            {
                const auto now = juce::Time::getHighResolutionTicks();
                const double elapsed = juce::jmax (0.2, juce::Time::highResolutionTicksToSeconds (
                    now - p->diagEpochTicks));
                const double wr = (double) (p->totalSamplesWritten() - p->diagWrAnchor) / elapsed;
                if (wr >= kMinSustainWriteRate)
                    ++ok;
            }
        }
        return ok;
    };

    // Probe order: requested size first; if allowFallback, demote 6→4→2.
    juce::Array<int> pairTries;
    pairTries.add (requestedPairs);
    if (allowFallback)
    {
        for (int n : { 4, 2 })
            if (n < requestedPairs)
                pairTries.add (n);
    }

    const int periodTries[] = { juce::jmax (wasapiBufferSize, 1024), 2048 };
    const bool modeTries[] = { true, false }; // exclusive first

    juce::StringArray errors;
    juce::String attemptLog;
    int opened = 0;
    int sustaining = 0;
    int usedPairs = 0;
    double rate = 0.0;
    int buf = 0;
    int latency = 0;
    bool usedExclusive = true;

    for (int nPairs : pairTries)
    {
        for (bool wantExcl : modeTries)
        {
            for (int period : periodTries)
            {
                closeDevice();
                wasapiBufferSize = snapWasapiBuffer (period);

                FoundPair found[kNumPairs];
                const int foundCount = collectPairs (wantExcl, found);
                if (foundCount < nPairs)
                {
                    attemptLog += (wantExcl ? "excl" : "shared");
                    attemptLog += "@" + juce::String (period) + "x" + juce::String (nPairs)
                                  + ":missing(" + juce::String (foundCount) + ") ";
                    continue;
                }

                opened = openN (found, nPairs, errors, rate, buf, latency);
                if (opened < nPairs)
                {
                    attemptLog += (wantExcl ? "excl" : "shared");
                    attemptLog += "@" + juce::String (period) + "x" + juce::String (nPairs)
                                  + ":open=" + juce::String (opened) + " ";
                    continue;
                }

                for (int i = 0; i < nPairs; ++i)
                    if (pairs[(size_t) i] != nullptr)
                        pairs[(size_t) i]->resetRateWindow();

                // 1.0s sustain probe (wr ≥ 40k).
                juce::Thread::sleep (1100);
                for (int i = 0; i < nPairs; ++i)
                    if (pairs[(size_t) i] != nullptr)
                        pairs[(size_t) i]->tickFrozenRates();

                sustaining = countSustaining (nPairs);
                attemptLog += (wantExcl ? "excl" : "shared");
                attemptLog += "@" + juce::String (period) + "x" + juce::String (nPairs)
                              + ":ok=" + juce::String (sustaining) + "/" + juce::String (nPairs) + " ";

                if (sustaining == nPairs)
                {
                    usedPairs = nPairs;
                    usedExclusive = wantExcl;
                    goto aggregate_ok;
                }
            }
        }
    }

aggregate_ok:
    if (opened == 0 || usedPairs == 0)
    {
        // Keep best partial if anything opened during last attempt.
        usedPairs = 0;
        for (int i = 0; i < kNumPairs; ++i)
            if (pairs[(size_t) i] != nullptr && pairs[(size_t) i]->isOpen())
                ++usedPairs;

        if (usedPairs == 0)
        {
            closeDevice();
            lastError = "Aggregate open failed. " + attemptLog + errors.joinIntoString ("; ");
            architectureNote = "Architecture wall: could not sustain any exclusive WASAPI stem pairs "
                               "beside the host. Next path: out-of-process capture engine, or analog into Model 12.";
            notifyStatus();
            return lastError;
        }
    }

    // Close unused higher pairs if we fell back.
    for (int i = usedPairs; i < kNumPairs; ++i)
        if (pairs[(size_t) i] != nullptr)
            pairs[(size_t) i]->close();

    opened = 0;
    rate = 0.0;
    buf = 0;
    latency = 0;
    for (int i = 0; i < usedPairs; ++i)
    {
        auto& p = pairs[(size_t) i];
        if (p == nullptr || ! p->isOpen())
            continue;
        ++opened;
        rate = p->sampleRate;
        buf = juce::jmax (buf, p->bufferSize);
        latency = juce::jmax (latency, p->latencyIn);
        usedExclusive = usedExclusive || p->typeNameOpen == kExclusiveWasapi;
    }

    aggregateOpen = true;
    targetPairCount = usedPairs;
    primed.store (false, std::memory_order_relaxed);
    captureChannels = opened * 2;
    captureSampleRate = rate;
    captureBufferSize = buf;
    deviceLatencyIn = latency;

    const char* baseName = kAggregateName;
    if (usedPairs <= 2)
        baseName = kAggregate2Name;
    else if (usedPairs <= 4)
        baseName = kAggregate4Name;
    else if (usedExclusive)
        baseName = kAggregateExclusiveName;

    currentName = juce::String (baseName)
                  + " (" + juce::String (opened) + " pairs"
                  + ", " + (usedExclusive ? "excl" : "shared")
                  + ", WASAPI " + juce::String (buf)
                  + ", depth " + juce::String (latencyMs) + "ms"
                  + ", sustain " + juce::String (sustaining) + "/" + juce::String (usedPairs) + ")";

    if (usedPairs < requestedPairs)
    {
        architectureNote = "Architecture wall: " + juce::String (requestedPairs)
                           + " in-process WASAPI clients could not sustain ~48k wr beside Ableton ASIO. "
                             "Fell back to " + juce::String (usedPairs)
                           + " pair(s). Full 12 stems need an out-of-process capture engine "
                             "(or analog into Model 12).";
        lastError = architectureNote + " Attempts: " + attemptLog;
    }
    else if (sustaining < usedPairs)
    {
        architectureNote = "Opened " + juce::String (usedPairs)
                           + " pairs but sustain probe was weak. Check health dump.";
        lastError = architectureNote + " " + attemptLog;
    }
    else
    {
        lastError.clear();
        if (usedPairs < kNumPairs)
            architectureNote = "Running workable subset (" + juce::String (usedPairs)
                               + " pairs). Six concurrent in-process WASAPI clients beside ASIO "
                                 "remain best-effort / often unsustainable.";
        else
            architectureNote = "All 6 pairs passed the 1s sustain probe. If underruns still climb in Live, "
                               "the wall is host+capture scheduling — next path is out-of-process capture.";
    }

    clearXrunCounters();
    for (auto& p : pairs)
        if (p != nullptr)
            p->resetRateWindow();

    beginPostOpenHealthDump();
    notifyStatus();
    return (usedPairs >= requestedPairs && sustaining >= usedPairs) ? juce::String() : lastError;
}

juce::String WasapiCapture::openSingle (const juce::String& deviceName,
                                        const juce::String& typeName,
                                        double preferredSampleRate,
                                        int preferredBufferSize)
{
    closeDevice();
    lastError.clear();
    architectureNote.clear();
    targetPairCount = 1;

    int pairIdx = pairIndexFromDeviceName (deviceName);
    if (pairIdx < 0)
        pairIdx = 0;

    single = std::make_unique<PairEndpoint> (*this, pairIdx);
    lastError = single->open (deviceName, typeName, preferredSampleRate, preferredBufferSize);

    if (lastError.isNotEmpty())
    {
        single.reset();
        targetPairCount = 0;
        notifyStatus();
        return lastError;
    }

    aggregateOpen = false;
    captureChannels = single->activeChannels;
    if (pairIndexFromDeviceName (deviceName) >= 0)
        captureChannels = pairIdx * 2 + single->activeChannels;

    captureSampleRate = single->sampleRate;
    captureBufferSize = single->bufferSize;
    deviceLatencyIn = single->latencyIn;
    currentName = deviceName;
    beginPostOpenHealthDump();
    notifyStatus();
    return {};
}

juce::String WasapiCapture::openDevice (const juce::String& deviceName,
                                        double preferredSampleRate,
                                        int preferredBufferSize)
{
    if (wasapiBufferSize <= 0 && preferredBufferSize > 0)
        wasapiBufferSize = snapWasapiBuffer (preferredBufferSize);
    else
        wasapiBufferSize = snapWasapiBuffer (wasapiBufferSize > 0 ? wasapiBufferSize
                                                                  : kDefaultWasapiBuffer);

    primed.store (false, std::memory_order_relaxed);

    const int aggPairs = aggregatePairCountFromName (deviceName);
    if (aggPairs > 0)
    {
        const bool allowFallback = (aggPairs == 6); // 2/4 stay fixed; "all" may demote
        return openAggregate (preferredSampleRate, preferredBufferSize, aggPairs, allowFallback);
    }

    juce::String typeForDevice = kExclusiveWasapi;
    {
        const juce::ScopedLock sl (deviceListLock);
        for (const auto& d : devices)
        {
            if (d.name == deviceName && ! d.isAggregate)
            {
                typeForDevice = d.typeName;
                break;
            }
        }
    }

    return openSingle (deviceName, typeForDevice, preferredSampleRate, preferredBufferSize);
}

void WasapiCapture::closeDevice()
{
    healthDumpGeneration.fetch_add (1, std::memory_order_relaxed);

    for (auto& p : pairs)
        if (p != nullptr)
            p->close();

    if (single != nullptr)
    {
        single->close();
        single.reset();
    }

    aggregateOpen = false;
    primed.store (false, std::memory_order_relaxed);
    captureChannels = 0;
    captureSampleRate = 0.0;
    captureBufferSize = 0;
    deviceLatencyIn = 0;
    targetPairCount = 0;
    currentName.clear();
    notifyStatus();
}

bool WasapiCapture::isOpen() const noexcept
{
    if (aggregateOpen)
    {
        for (auto& p : pairs)
            if (p != nullptr && p->isOpen())
                return true;
        return false;
    }

    return single != nullptr && single->isOpen();
}

int WasapiCapture::getOpenPairCount() const noexcept
{
    if (! aggregateOpen)
        return (single != nullptr && single->isOpen()) ? 1 : 0;

    int n = 0;
    for (auto& p : pairs)
        if (p != nullptr && p->isOpen())
            ++n;
    return n;
}

juce::String WasapiCapture::getOpenPairSummary() const
{
    if (! aggregateOpen)
    {
        if (single != nullptr && single->isOpen())
            return single->deviceNameOpen;
        return {};
    }

    juce::StringArray parts;
    for (int i = 0; i < kNumPairs; ++i)
    {
        auto& p = pairs[(size_t) i];
        if (p != nullptr && p->isOpen())
            parts.add (pairLabel (i));
        else if (i < targetPairCount)
            parts.add (juce::String (pairLabel (i)) + "!");
        else
            parts.add (juce::String (pairLabel (i)) + "-");
    }
    return parts.joinIntoString (", ");
}

juce::String WasapiCapture::getCurrentDeviceName() const
{
    return currentName;
}

int WasapiCapture::getReportedLatencySamples() const noexcept
{
    return deviceLatencyIn + midpointTargetSamples() + juce::jmax (captureBufferSize, wasapiBufferSize);
}

int WasapiCapture::getAvailableToRead() const noexcept
{
    if (aggregateOpen)
    {
        int minAvail = INT_MAX;
        int openCount = 0;
        for (auto& p : pairs)
        {
            if (p == nullptr || ! p->isHealthy())
                continue;
            ++openCount;
            minAvail = juce::jmin (minAvail, p->ring.getAvailableToRead());
        }
        return openCount > 0 ? minAvail : 0;
    }

    if (single != nullptr && single->isHealthy())
        return single->ring.getAvailableToRead();

    return 0;
}

void WasapiCapture::readStems (float* const* dst, int numSamples) noexcept
{
    if (dst == nullptr || numSamples <= 0)
        return;

    for (int ch = 0; ch < kMaxChannels; ++ch)
        if (dst[ch] != nullptr)
            juce::FloatVectorOperations::clear (dst[ch], numSamples);

    const int mid = midpointTargetSamples();
    const int depth = getTargetFillSamples();
    const int highClamp = juce::jmax (mid + numSamples * 4, (int) std::lround ((double) depth * 0.95));

    if (aggregateOpen)
    {
        for (auto& p : pairs)
        {
            if (p == nullptr || ! p->isOpen())
                continue;
            if (primed.load (std::memory_order_relaxed) && ! p->isCallbackAlive (0.50))
                p->markedDead = true;
        }
    }

    auto healthyMinAvail = [this]() -> int
    {
        int minAvail = INT_MAX;
        int n = 0;
        if (aggregateOpen)
        {
            for (auto& p : pairs)
            {
                if (p == nullptr || ! p->isHealthy())
                    continue;
                ++n;
                minAvail = juce::jmin (minAvail, p->ring.getAvailableToRead());
            }
        }
        else if (single != nullptr && single->isHealthy())
        {
            return single->ring.getAvailableToRead();
        }
        return n > 0 ? minAvail : 0;
    };

    if (! primed.load (std::memory_order_relaxed))
    {
        if (healthyMinAvail() < getPrimingTargetSamples())
            return;
        primed.store (true, std::memory_order_relaxed);
        clearXrunCounters();
        for (auto& p : pairs)
            if (p != nullptr)
                p->resetAdaptive();
        if (single != nullptr)
            single->resetAdaptive();
    }

    masterPairIndex = pickMasterPair();
    double masterR = 1.0;

    auto* master = (aggregateOpen && masterPairIndex >= 0 && masterPairIndex < kNumPairs)
                       ? pairs[(size_t) masterPairIndex].get()
                       : single.get();

    if (master != nullptr && master->isHealthy())
    {
        const int mAvail = master->ring.getAvailableToRead();
        if (mAvail > highClamp)
            master->ring.discard (mAvail - mid);

        masterR = master->updateRatio (master->ring.getAvailableToRead(), mid, 1.0, 0.0);
    }
    masterRatio.store (masterR, std::memory_order_relaxed);

    auto readAdaptive = [&] (PairEndpoint& ep, float* left, float* right, bool isMaster)
    {
        float* pairDst[2] = { left, right };

        if (! ep.isOpen() || ep.markedDead || ! ep.isCallbackAlive (0.50))
        {
            ep.markedDead = ep.isOpen();
            return;
        }

        int avail = ep.ring.getAvailableToRead();
        if (avail > highClamp)
        {
            ep.ring.discard (avail - mid);
            avail = ep.ring.getAvailableToRead();
        }

        const double blend = isMaster ? 0.0 : 0.50;
        const double r = ep.updateRatio (avail, mid, masterR, blend);
        ep.ring.readResampled (pairDst, numSamples, r, ep.fracPhase);
    };

    if (aggregateOpen)
    {
        for (int i = 0; i < kNumPairs; ++i)
        {
            auto& p = pairs[(size_t) i];
            if (p == nullptr)
                continue;
            readAdaptive (*p, dst[i * 2], dst[i * 2 + 1], i == masterPairIndex);
        }
        return;
    }

    if (single != nullptr)
    {
        const int off = single->channelOffset;
        readAdaptive (*single,
                      off < kMaxChannels ? dst[off] : nullptr,
                      off + 1 < kMaxChannels ? dst[off + 1] : nullptr,
                      true);
    }
}

int WasapiCapture::getUnderruns() const noexcept
{
    int n = 0;
    if (aggregateOpen)
    {
        for (auto& p : pairs)
            if (p != nullptr)
                n += p->ring.getUnderruns();
    }
    else if (single != nullptr)
    {
        n = single->ring.getUnderruns();
    }
    return n;
}

int WasapiCapture::getOverruns() const noexcept
{
    int n = 0;
    if (aggregateOpen)
    {
        for (auto& p : pairs)
            if (p != nullptr)
                n += p->ring.getOverruns();
    }
    else if (single != nullptr)
    {
        n = single->ring.getOverruns();
    }
    return n;
}

int WasapiCapture::getSlips() const noexcept
{
    int n = 0;
    if (aggregateOpen)
    {
        for (auto& p : pairs)
            if (p != nullptr)
                n += p->ring.getSlips();
    }
    else if (single != nullptr)
    {
        n = single->ring.getSlips();
    }
    return n;
}

void WasapiCapture::clearXrunCounters() noexcept
{
    for (auto& p : pairs)
        if (p != nullptr)
            p->ring.clearCounters();
    if (single != nullptr)
        single->ring.clearCounters();
}

void WasapiCapture::tickDiagnostics() noexcept
{
    for (auto& p : pairs)
        if (p != nullptr)
            p->tickFrozenRates();
    if (single != nullptr)
        single->tickFrozenRates();
}

juce::String WasapiCapture::getPairHealthSummary() const
{
    auto pairDiag = [] (const PairEndpoint& p, bool isMaster) -> juce::String
    {
        juce::String s;
        s << "fifo=" << p.ring.getAvailableToRead()
          << " cb/s=" << juce::String (p.frozenCbHz, 1)
          << " (pk " << juce::String (p.peakCbHz, 1) << ")"
          << " wr=" << juce::String (p.frozenWrRate, 0)
          << " (pk " << juce::String (p.peakWrRate, 0) << ")"
          << " r=" << juce::String (p.ratio, 3)
          << " " << (p.typeNameOpen == kExclusiveWasapi ? "excl" : "shared")
          << "@" << p.bufferSize;
        if (p.markedDead || ! p.isCallbackAlive())
            s << " DEAD";
        else if (p.frozenWrRate > 0.0 && p.frozenWrRate < kMinSustainWriteRate)
            s << " SLOW";
        if (isMaster)
            s << "*";
        return s;
    };

    if (! aggregateOpen)
    {
        if (single == nullptr || ! single->isOpen())
            return {};
        return pairDiag (*single, true);
    }

    juce::StringArray parts;
    for (int i = 0; i < kNumPairs; ++i)
    {
        auto& p = pairs[(size_t) i];
        if (p == nullptr || ! p->isOpen())
        {
            if (i < targetPairCount)
                parts.add (juce::String (pairLabel (i)) + ":!");
            continue;
        }
        parts.add (juce::String (pairLabel (i)) + ":" + pairDiag (*p, i == masterPairIndex));
    }
    return parts.joinIntoString ("\n");
}

juce::File WasapiCapture::getLastHealthDumpFile() const
{
    return lastHealthDumpFile;
}

juce::String WasapiCapture::getArchitectureNote() const
{
    return architectureNote;
}

void WasapiCapture::writeHealthDumpFile (const juce::String& body)
{
    auto file = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                    .getChildFile ("tr8s-sidestream-health.txt");
    if (file.replaceWithText (body))
        lastHealthDumpFile = file;
    else
    {
        file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("tr8s-sidestream-health.txt");
        if (file.replaceWithText (body))
            lastHealthDumpFile = file;
    }
}

void WasapiCapture::beginPostOpenHealthDump()
{
    const int gen = healthDumpGeneration.fetch_add (1, std::memory_order_relaxed) + 1;

    juce::Thread::launch ([this, gen]
    {
        juce::StringArray lines;
        lines.add ("TR-8S SideStream capture health dump (5s)");
        lines.add ("time: " + juce::Time::getCurrentTime().toString (true, true, true, true));
        lines.add ("device: " + currentName);
        lines.add ("targetPairs: " + juce::String (targetPairCount));
        lines.add ("hostNote: plugin process; rates are capture-side");
        lines.add ("----");

        struct Sample
        {
            double cb[kNumPairs] {};
            double wr[kNumPairs] {};
            int fifo[kNumPairs] {};
            int underruns = 0;
        };

        constexpr int kSamples = 25; // 5s @ 200ms
        juce::Array<Sample> series;
        uint64_t prevCb[kNumPairs] = {};
        uint64_t prevWr[kNumPairs] = {};
        auto prevTicks = juce::Time::getHighResolutionTicks();

        for (int i = 0; i < kNumPairs; ++i)
        {
            if (pairs[(size_t) i] != nullptr)
            {
                prevCb[i] = pairs[(size_t) i]->totalCallbacks();
                prevWr[i] = pairs[(size_t) i]->totalSamplesWritten();
            }
        }

        for (int s = 0; s < kSamples; ++s)
        {
            if (healthDumpGeneration.load (std::memory_order_relaxed) != gen)
                return;

            juce::Thread::sleep (200);

            if (healthDumpGeneration.load (std::memory_order_relaxed) != gen)
                return;

            const auto now = juce::Time::getHighResolutionTicks();
            const double elapsed = juce::jmax (0.001, juce::Time::highResolutionTicksToSeconds (now - prevTicks));
            Sample sample;
            sample.underruns = getUnderruns();

            for (int i = 0; i < kNumPairs; ++i)
            {
                auto& p = pairs[(size_t) i];
                if (p == nullptr || ! p->isOpen())
                    continue;

                const uint64_t cb = p->totalCallbacks();
                const uint64_t wr = p->totalSamplesWritten();
                sample.cb[i] = (double) (cb - prevCb[i]) / elapsed;
                sample.wr[i] = (double) (wr - prevWr[i]) / elapsed;
                sample.fifo[i] = p->ring.getAvailableToRead();
                prevCb[i] = cb;
                prevWr[i] = wr;
            }

            prevTicks = now;
            series.add (sample);
        }

        // Averages over the 5s dump.
        double sumCb[kNumPairs] = {};
        double sumWr[kNumPairs] = {};
        double peakWr[kNumPairs] = {};
        double minWr[kNumPairs];
        int count[kNumPairs] = {};
        for (int i = 0; i < kNumPairs; ++i)
            minWr[i] = 1.0e300;

        for (const auto& sample : series)
        {
            for (int i = 0; i < kNumPairs; ++i)
            {
                if (sample.wr[i] <= 0.0 && sample.cb[i] <= 0.0)
                    continue;
                sumCb[i] += sample.cb[i];
                sumWr[i] += sample.wr[i];
                peakWr[i] = juce::jmax (peakWr[i], sample.wr[i]);
                minWr[i] = juce::jmin (minWr[i], sample.wr[i]);
                ++count[i];
            }
        }

        lines.add ("5s averages (cb/s, wr/s, fifo last, wr min/peak):");
        int sustained = 0;
        for (int i = 0; i < kNumPairs; ++i)
        {
            if (count[i] <= 0)
            {
                if (i < targetPairCount)
                    lines.add (juce::String (pairLabel (i)) + ": NO DATA / closed");
                continue;
            }

            const double avgCb = sumCb[i] / (double) count[i];
            const double avgWr = sumWr[i] / (double) count[i];
            const int fifo = series.getLast().fifo[i];
            lines.add (juce::String (pairLabel (i))
                       + ": cb/s=" + juce::String (avgCb, 1)
                       + " wr=" + juce::String (avgWr, 0)
                       + " fifo=" + juce::String (fifo)
                       + " wrMin=" + juce::String (minWr[i], 0)
                       + " wrPk=" + juce::String (peakWr[i], 0)
                       + (avgWr >= kMinSustainWriteRate ? " OK" : " FAIL"));
            if (avgWr >= kMinSustainWriteRate)
                ++sustained;
        }

        lines.add ("----");
        lines.add ("underruns at end: " + juce::String (getUnderruns()));
        lines.add ("sustaining pairs: " + juce::String (sustained) + "/" + juce::String (targetPairCount));
        lines.add ("architecture: " + architectureNote);
        lines.add ("attempt/error: " + lastError);
        lines.add ("VERDICT: "
                   + juce::String (sustained >= targetPairCount && targetPairCount > 0
                                       ? (targetPairCount >= 6
                                              ? "6-pair in-process may be viable on this machine — watch Live underruns"
                                              : "workable subset only; full 12 stems need OOP capture or analog")
                                       : "cannot sustain requested WASAPI clients beside host — architecture wall"));

        writeHealthDumpFile (lines.joinIntoString ("\n"));
        notifyStatus();
    });
}

float WasapiCapture::getPeak (int channel) const noexcept
{
    if (channel < 0 || channel >= kMaxChannels)
        return 0.0f;
    return peaks[(size_t) channel].load (std::memory_order_relaxed);
}

void WasapiCapture::decayPeaks (float factor) noexcept
{
    for (auto& p : peaks)
    {
        const float v = p.load (std::memory_order_relaxed) * factor;
        p.store (v, std::memory_order_relaxed);
    }
}

void WasapiCapture::notifyStatus()
{
    if (statusCallback)
        juce::MessageManager::callAsync ([cb = statusCallback] { cb(); });
}
