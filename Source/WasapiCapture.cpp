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

    /** Open one TR-8S stereo endpoint via a dedicated WASAPI AudioIODevice.
        Avoids AudioDeviceManager (heavy + shared-state); each pair gets its own
        IAudioClient thread. Exclusive mode is required for reliable 6-client capture. */
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
            // Retry with whatever channel mask the device defaults to.
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
        diagEpochTicks = juce::Time::getHighResolutionTicks();
        diagCbAnchor = 0;
        diagWrAnchor = 0;
        markedDead = false;

        // start() registers this callback and launches the WASAPI thread.
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

    uint64_t callbackDelta() const noexcept
    {
        return callbackCount.load (std::memory_order_relaxed);
    }

    void resetRateWindow() noexcept
    {
        diagEpochTicks = juce::Time::getHighResolutionTicks();
        diagCbAnchor = callbackCount.load (std::memory_order_relaxed);
        diagWrAnchor = samplesWritten.load (std::memory_order_relaxed);
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

    /** PI toward FIFO midpoint; blend with masterRatio (1=follow master only). */
    double updateRatio (int avail, int targetMid, double masterR, double masterBlend) noexcept
    {
        if (targetMid < 1)
            targetMid = 1;

        const double err = ((double) avail - (double) targetMid) / (double) targetMid;
        integ = juce::jlimit (-0.04, 0.04, integ + err * 0.0008);
        // Positive err (too full) → ratio > 1 (consume faster from capture).
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
    mutable juce::int64 diagEpochTicks = 0;
    mutable uint64_t diagCbAnchor = 0;
    mutable uint64_t diagWrAnchor = 0;
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

    // Match "IN 1-2", "IN 11-12", etc. (tolerant of spacing).
    for (int i = 0; i < kNumPairs; ++i)
    {
        if (name.containsIgnoreCase (pairLabel (i)))
            return i;
    }

    return -1;
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
    // Continuously aim for ~60% of configured depth (50–70% band).
    return juce::jmax (128, (int) std::lround ((double) getTargetFillSamples() * 0.60));
}

int WasapiCapture::getPrimingTargetSamples() const noexcept
{
    // Pre-roll to midpoint + one period before adaptive SRC takes over.
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
    // Hold ~1s, at least 4× latency target.
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

    // Inject virtual aggregate when all six shared-WASAPI stem pairs exist.
    bool havePair[kNumPairs] = {};
    int sharedPairCount = 0;
    for (const auto& d : physical)
    {
        if (d.typeName != kSharedWasapi)
            continue;

        const int idx = pairIndexFromDeviceName (d.name);
        if (idx >= 0 && ! havePair[idx])
        {
            havePair[idx] = true;
            ++sharedPairCount;
        }
    }

    if (sharedPairCount == kNumPairs)
    {
        DeviceInfo agg;
        agg.name = kAggregateName;
        agg.typeName = kSharedWasapi;
        agg.inputChannels = kMaxChannels;
        agg.isAggregate = true;
        found.add (agg);
    }

    int exclPairCount = 0;
    bool haveExcl[kNumPairs] = {};
    for (const auto& d : physical)
    {
        if (d.typeName != kExclusiveWasapi)
            continue;
        const int idx = pairIndexFromDeviceName (d.name);
        if (idx >= 0 && ! haveExcl[idx])
        {
            haveExcl[idx] = true;
            ++exclPairCount;
        }
    }
    if (exclPairCount == kNumPairs)
    {
        DeviceInfo agg;
        agg.name = kAggregateExclusiveName;
        agg.typeName = kExclusiveWasapi;
        agg.inputChannels = kMaxChannels;
        agg.isAggregate = true;
        found.add (agg);
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

juce::String WasapiCapture::openAggregate (double preferredSampleRate, int preferredBufferSize, bool exclusive)
{
    closeDevice();
    lastError.clear();

    refreshDeviceList();

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

    auto openAll = [&] (FoundPair found[kNumPairs], juce::StringArray& errors,
                        double& rate, int& buf, int& latency) -> int
    {
        int opened = 0;
        rate = 0.0;
        buf = 0;
        latency = 0;
        errors.clear();

        for (int i = 0; i < kNumPairs; ++i)
        {
            // Stagger opens slightly so USB/WASAPI can settle between clients.
            if (i > 0)
                juce::Thread::sleep (30);

            auto err = pairs[(size_t) i]->open (found[i].name,
                                                found[i].typeName,
                                                preferredSampleRate,
                                                preferredBufferSize);
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

    auto countLiveCallbacks = [this]() -> int
    {
        int live = 0;
        for (auto& p : pairs)
        {
            if (p == nullptr || ! p->isOpen())
                continue;
            // After probe window, expect many callbacks. Threshold is intentionally low
            // so a slightly slow start still counts; real healthy is ~30+ in 350ms.
            if (p->callbackDelta() >= 8)
                ++live;
        }
        return live;
    };

    // Force a usable period for 6-client capture (UI may still request 512).
    wasapiBufferSize = snapWasapiBuffer (juce::jmax (wasapiBufferSize, preferredBufferSize, 512));

    // Try exclusive first (even when UI says shared) — shared 6-client WASAPI starves
    // (~2 cb/s, half the pairs DEAD). Fall back to shared only if exclusive can't open.
    const bool tryOrder[] = { true, false };
    const int periodTries[] = {
        juce::jmax (wasapiBufferSize, 1024),
        2048,
        juce::jmax (wasapiBufferSize, 512)
    };

    juce::StringArray errors;
    int opened = 0;
    int live = 0;
    double rate = 0.0;
    int buf = 0;
    int latency = 0;
    bool usedExclusive = exclusive;
    juce::String attemptLog;

    for (bool wantExcl : tryOrder)
    {
        // If caller insisted on exclusive-only menu entry, skip shared fallback on success path;
        // still allow shared if exclusive fails entirely.
        juce::ignoreUnused (exclusive);

        for (int period : periodTries)
        {
            closeDevice();
            wasapiBufferSize = snapWasapiBuffer (period);

            FoundPair found[kNumPairs];
            const int foundCount = collectPairs (wantExcl, found);
            if (foundCount < kNumPairs)
            {
                attemptLog += (wantExcl ? "excl" : "shared");
                attemptLog += "@" + juce::String (period) + ":missing-pairs("
                              + juce::String (foundCount) + ") ";
                continue;
            }

            opened = openAll (found, errors, rate, buf, latency);
            if (opened == 0)
            {
                attemptLog += (wantExcl ? "excl" : "shared");
                attemptLog += "@" + juce::String (period) + ":canary-fail ";
                continue;
            }

            // Probe callback rate — Tyron's 0.4.1 shared path showed ~2 cb/s.
            for (auto& p : pairs)
                if (p != nullptr)
                    p->resetRateWindow();

            juce::Thread::sleep (400);
            live = countLiveCallbacks();

            attemptLog += (wantExcl ? "excl" : "shared");
            attemptLog += "@" + juce::String (period) + ":open=" + juce::String (opened)
                          + "/live=" + juce::String (live) + " ";

            if (opened == kNumPairs && live == kNumPairs)
            {
                usedExclusive = wantExcl;
                goto aggregate_ok;
            }

            // Partial: retry dead pairs once at this period/mode.
            if (opened >= 3 && live < opened)
            {
                for (int i = 0; i < kNumPairs; ++i)
                {
                    auto& p = pairs[(size_t) i];
                    if (p == nullptr || ! p->isOpen() || p->callbackDelta() >= 8)
                        continue;
                    p->close();
                    juce::Thread::sleep (40);
                    auto err = p->open (found[i].name, found[i].typeName,
                                        preferredSampleRate, preferredBufferSize);
                    if (err.isNotEmpty())
                        errors.add ("retry " + juce::String (pairLabel (i)) + ": " + err);
                }

                for (auto& p : pairs)
                    if (p != nullptr)
                        p->resetRateWindow();
                juce::Thread::sleep (400);
                live = countLiveCallbacks();
                opened = 0;
                for (auto& p : pairs)
                    if (p != nullptr && p->isOpen())
                        ++opened;
                attemptLog += "retry:live=" + juce::String (live) + " ";

                if (opened == kNumPairs && live == kNumPairs)
                {
                    usedExclusive = wantExcl;
                    goto aggregate_ok;
                }
            }
        }
    }

aggregate_ok:
    if (opened == 0)
    {
        closeDevice();
        lastError = "Aggregate open failed. " + attemptLog + errors.joinIntoString ("; ");
        notifyStatus();
        return lastError;
    }

    // Recompute open stats after possible retries.
    opened = 0;
    rate = 0.0;
    buf = 0;
    latency = 0;
    for (auto& p : pairs)
    {
        if (p == nullptr || ! p->isOpen())
            continue;
        ++opened;
        rate = p->sampleRate;
        buf = juce::jmax (buf, p->bufferSize);
        latency = juce::jmax (latency, p->latencyIn);
        usedExclusive = usedExclusive || p->typeNameOpen == kExclusiveWasapi;
    }
    live = countLiveCallbacks();

    aggregateOpen = true;
    primed.store (false, std::memory_order_relaxed);
    captureChannels = opened * 2;
    captureSampleRate = rate;
    captureBufferSize = buf;
    deviceLatencyIn = latency;
    currentName = juce::String (usedExclusive ? kAggregateExclusiveName : kAggregateName)
                  + " (" + juce::String (opened) + " pairs"
                  + ", " + (usedExclusive ? "excl" : "shared")
                  + ", WASAPI " + juce::String (buf)
                  + ", depth " + juce::String (latencyMs) + "ms"
                  + ", live " + juce::String (live) + "/6)";

    if (opened < kNumPairs || live < kNumPairs)
    {
        lastError = "Only " + juce::String (live) + "/6 pairs live (open " + juce::String (opened)
                    + "). " + attemptLog + errors.joinIntoString ("; ");
    }
    else
    {
        lastError.clear();
    }

    clearXrunCounters();
    for (auto& p : pairs)
        if (p != nullptr)
            p->resetRateWindow();
    notifyStatus();
    return (opened == kNumPairs && live == kNumPairs) ? juce::String() : lastError;
}

juce::String WasapiCapture::openSingle (const juce::String& deviceName,
                                        const juce::String& typeName,
                                        double preferredSampleRate,
                                        int preferredBufferSize)
{
    closeDevice();
    lastError.clear();

    int pairIdx = pairIndexFromDeviceName (deviceName);
    if (pairIdx < 0)
        pairIdx = 0; // MIX or unknown → map into channels 0-1

    single = std::make_unique<PairEndpoint> (*this, pairIdx);
    lastError = single->open (deviceName, typeName, preferredSampleRate, preferredBufferSize);

    if (lastError.isNotEmpty())
    {
        single.reset();
        notifyStatus();
        return lastError;
    }

    aggregateOpen = false;
    captureChannels = single->activeChannels;
    // For a stem pair, report global channel span so process maps correctly.
    if (pairIndexFromDeviceName (deviceName) >= 0)
        captureChannels = pairIdx * 2 + single->activeChannels;

    captureSampleRate = single->sampleRate;
    captureBufferSize = single->bufferSize;
    deviceLatencyIn = single->latencyIn;
    currentName = deviceName;
    notifyStatus();
    return {};
}

juce::String WasapiCapture::openDevice (const juce::String& deviceName,
                                        double preferredSampleRate,
                                        int preferredBufferSize)
{
    // Keep all pairs on one snapped WASAPI period (UI setting wins; else host block).
    // Aggregate needs ≥1024 in practice; bump quietly (status shows actual).
    if (wasapiBufferSize <= 0 && preferredBufferSize > 0)
        wasapiBufferSize = snapWasapiBuffer (preferredBufferSize);
    else
        wasapiBufferSize = snapWasapiBuffer (wasapiBufferSize > 0 ? wasapiBufferSize
                                                                  : kDefaultWasapiBuffer);

    primed.store (false, std::memory_order_relaxed);

    // Both aggregate menu entries run exclusive-first with shared fallback.
    if (deviceName == kAggregateExclusiveName
        || deviceName.startsWith (kAggregateExclusiveName)
        || deviceName == kAggregateName
        || deviceName.startsWith (kAggregateName))
    {
        const bool preferExclusive = true;
        return openAggregate (preferredSampleRate, preferredBufferSize, preferExclusive);
    }

    juce::String typeForDevice = kSharedWasapi;
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
        else
            parts.add (juce::String (pairLabel (i)) + "!");
    }
    return parts.joinIntoString (", ");
}

juce::String WasapiCapture::getCurrentDeviceName() const
{
    return currentName;
}

int WasapiCapture::getReportedLatencySamples() const noexcept
{
    // PDC reports the midpoint waterline we servo to (not the full depth ceiling).
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

    // Mark dead pairs (callback stopped writing) so they don't ratchet underruns forever.
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
        // Emergency clamp if master blew past depth (coarse slip, then SRC).
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
            return; // silence; do not underrun-count a dead callback
        }

        int avail = ep.ring.getAvailableToRead();
        if (avail > highClamp)
        {
            ep.ring.discard (avail - mid);
            avail = ep.ring.getAvailableToRead();
        }

        // Slaves blend 50% toward master ratio so host + pairs track one clock.
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

juce::String WasapiCapture::getPairHealthSummary() const
{
    auto pairDiag = [] (const PairEndpoint& p, bool isMaster) -> juce::String
    {
        const auto now = juce::Time::getHighResolutionTicks();
        const double elapsed = juce::jmax (0.05, juce::Time::highResolutionTicksToSeconds (
            now - p.diagEpochTicks));
        const uint64_t cb = p.callbackCount.load (std::memory_order_relaxed);
        const uint64_t wr = p.samplesWritten.load (std::memory_order_relaxed);
        const double cbHz = (double) (cb - p.diagCbAnchor) / elapsed;
        const double wrRate = (double) (wr - p.diagWrAnchor) / elapsed;

        // Roll the window so status tracks recent rate, not lifetime average.
        if (elapsed >= 0.75)
        {
            p.diagEpochTicks = now;
            p.diagCbAnchor = cb;
            p.diagWrAnchor = wr;
        }

        juce::String s;
        s << "fifo=" << p.ring.getAvailableToRead()
          << " cb/s=" << juce::String (cbHz, 1)
          << " wr=" << juce::String (wrRate, 0)
          << " r=" << juce::String (p.ratio, 3)
          << " " << (p.typeNameOpen == kExclusiveWasapi ? "excl" : "shared")
          << "@" << p.bufferSize;
        if (p.markedDead || ! p.isCallbackAlive())
            s << " DEAD";
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
            parts.add (juce::String (pairLabel (i)) + ":!");
        else
            parts.add (juce::String (pairLabel (i)) + ":" + pairDiag (*p, i == masterPairIndex));
    }
    return parts.joinIntoString (" | ");
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
