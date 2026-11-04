#include "WasapiCapture.h"
#include <climits>
#include <cmath>

namespace
{
juce::StringArray wasapiTypeNames()
{
    return { WasapiCapture::kSharedWasapi, WasapiCapture::kExclusiveWasapi };
}

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

        // Fresh AudioDeviceManager has empty availableDeviceTypes until initialise/scan.
        // Calling setAudioDeviceSetup alone hits getCurrentDeviceTypeObject()==nullptr and
        // JUCE returns {} while leaving the device null — which produced
        // "Opened null device: IN X-Y (TR-8S)" for all six aggregate pairs.
        // Prefer plugin WASAPI period; fall back to host hint. Same value used for all pairs.
        const int period = WasapiCapture::snapWasapiBuffer (
            owner.wasapiBufferSize > 0 ? owner.wasapiBufferSize
                                       : (preferredBufferSize > 0 ? preferredBufferSize : kDefaultWasapiBuffer));

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = deviceName;
        setup.outputDeviceName.clear();
        setup.sampleRate = preferredSampleRate > 0.0 ? preferredSampleRate : 48000.0;
        setup.bufferSize = period;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.inputChannels.clear();
        setup.inputChannels.setBit (0);
        setup.inputChannels.setBit (1);

        const juce::String type = typeName.isNotEmpty() ? typeName
                                                        : juce::String (kSharedWasapi);

        // Creates device types + sets numInputChansNeeded=2 (required for default channel mask).
        auto err = manager.initialise (2, 0, nullptr, false, deviceName, &setup);

        const bool wrongType = manager.getCurrentAudioDeviceType() != type;
        const bool wrongName = manager.getAudioDeviceSetup().inputDeviceName != deviceName;
        const bool nullDev = manager.getCurrentAudioDevice() == nullptr;

        if (err.isNotEmpty() || wrongType || wrongName || nullDev)
        {
            // Close first so setCurrentAudioDeviceType does not sleep 1.5s per pair.
            manager.closeAudioDevice();
            manager.setCurrentAudioDeviceType (type, false);
            manager.closeAudioDevice();
            err = manager.setAudioDeviceSetup (setup, true);
        }

        if (err.isNotEmpty())
        {
            // Last-chance: default channel mask only.
            setup.useDefaultInputChannels = true;
            setup.inputChannels.clear();
            err = manager.setAudioDeviceSetup (setup, true);
        }

        if (err.isNotEmpty())
            return type + " / " + deviceName + ": " + err;

        auto* dev = manager.getCurrentAudioDevice();
        if (dev == nullptr)
            return "Null device after initialise: " + type + " / " + deviceName
                   + " (availableTypes=" + juce::String (manager.getAvailableDeviceTypes().size()) + ")";

        if (manager.getAudioDeviceSetup().inputDeviceName != deviceName)
            return "Wrong input opened: wanted '" + deviceName + "', got '"
                   + manager.getAudioDeviceSetup().inputDeviceName + "' ["
                   + manager.getCurrentAudioDeviceType() + "]";

        deviceNameOpen = deviceName;
        typeNameOpen = type;
        sampleRate = dev->getCurrentSampleRate();
        bufferSize = dev->getCurrentBufferSizeSamples();
        latencyIn = dev->getInputLatencyInSamples();
        activeChannels = juce::jmin (2, dev->getActiveInputChannels().countNumberOfSetBits());

        if (activeChannels <= 0)
            return "Device open but 0 input channels: " + deviceName;

        // Re-assert period if driver ignored our request.
        if (std::abs (bufferSize - period) > 8)
        {
            auto again = manager.getAudioDeviceSetup();
            again.bufferSize = period;
            again.sampleRate = setup.sampleRate;
            manager.setAudioDeviceSetup (again, true);
            if (auto* d2 = manager.getCurrentAudioDevice())
            {
                bufferSize = d2->getCurrentBufferSizeSamples();
                sampleRate = d2->getCurrentSampleRate();
            }
        }

        const int ringSize = owner.ringCapacityForRate (sampleRate);
        ring.resize (2, ringSize);
        ring.reset();
        ring.clearCounters();
        resetAdaptive();
        lastWriteTicks.store (juce::Time::getHighResolutionTicks(), std::memory_order_relaxed);
        markedDead = false;

        manager.addAudioCallback (this);
        return {};
    }

    void close()
    {
        manager.removeAudioCallback (this);
        manager.closeAudioDevice();
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
        return manager.getCurrentAudioDevice() != nullptr && activeChannels > 0;
    }

    bool isCallbackAlive (double timeoutSec = 0.15) const noexcept
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
        if (markedDead)
            markedDead = false; // resurrect if callback returns

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

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        if (device == nullptr)
            return;

        sampleRate = device->getCurrentSampleRate();
        bufferSize = device->getCurrentBufferSizeSamples();
        latencyIn = device->getInputLatencyInSamples();
        activeChannels = juce::jmin (2, device->getActiveInputChannels().countNumberOfSetBits());
        owner.notifyStatus();
    }

    void audioDeviceStopped() override
    {
        owner.notifyStatus();
    }

    WasapiCapture& owner;
    int pairIndex = 0;
    int channelOffset = 0;
    juce::AudioDeviceManager manager;
    PlanarRingBuffer ring;
    juce::String deviceNameOpen;
    juce::String typeNameOpen;
    int activeChannels = 0;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int latencyIn = 0;

    std::atomic<juce::int64> lastWriteTicks { 0 };
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
    int best = 512;
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

    // Temporary manager only for enumeration (does not hold devices open).
    juce::AudioDeviceManager enumerator;
    for (auto* type : enumerator.getAvailableDeviceTypes())
    {
        if (type == nullptr || ! wasapiTypeNames().contains (type->getTypeName()))
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

    // Physical devices: prefer listing shared first, skip exclusive duplicates of TR-8S
    // pairs in the UI by putting shared ahead; still list exclusive for power users.
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

juce::String WasapiCapture::openAggregate (double preferredSampleRate, int preferredBufferSize)
{
    closeDevice();
    lastError.clear();

    // Refresh so we use the same DeviceInfo name+typeName the combo lists for single opens.
    refreshDeviceList();

    struct FoundPair
    {
        juce::String name;
        juce::String typeName;
    };

    FoundPair found[kNumPairs];
    int foundCount = 0;

    {
        const juce::ScopedLock sl (deviceListLock);

        // Pass 1: shared Windows Audio (preferred). Pass 2: exclusive fallback.
        for (int pass = 0; pass < 2; ++pass)
        {
            const juce::String wantType = (pass == 0 ? juce::String (kSharedWasapi)
                                                     : juce::String (kExclusiveWasapi));

            for (const auto& d : devices)
            {
                if (d.isAggregate || d.typeName != wantType)
                    continue;

                const int idx = pairIndexFromDeviceName (d.name);
                if (idx < 0 || found[idx].name.isNotEmpty())
                    continue;

                found[idx] = { d.name, d.typeName };
                ++foundCount;
            }
        }
    }

    if (foundCount < kNumPairs)
    {
        juce::String missing;
        for (int i = 0; i < kNumPairs; ++i)
            if (found[i].name.isEmpty())
                missing += juce::String (pairLabel (i)) + " ";

        lastError = "Missing TR-8S stem pairs (need 6): " + missing.trim();
        notifyStatus();
        return lastError;
    }

    juce::StringArray errors;
    int opened = 0;
    double rate = 0.0;
    int buf = 0;
    int latency = 0;

    // Open pair 0 first as a canary — same identity as picking IN 1-2 alone.
    for (int i = 0; i < kNumPairs; ++i)
    {
        auto err = pairs[(size_t) i]->open (found[i].name,
                                            found[i].typeName,
                                            preferredSampleRate,
                                            preferredBufferSize);
        if (err.isNotEmpty())
        {
            errors.add ("[" + found[i].typeName + "] " + found[i].name + " → " + err);
            if (i == 0)
            {
                lastError = "Canary pair failed (IN 1-2). " + err;
                closeDevice();
                notifyStatus();
                return lastError;
            }
            continue;
        }

        ++opened;
        rate = pairs[(size_t) i]->sampleRate;
        buf = juce::jmax (buf, pairs[(size_t) i]->bufferSize);
        latency = juce::jmax (latency, pairs[(size_t) i]->latencyIn);
    }

    if (opened < kNumPairs)
    {
        lastError = "Opened " + juce::String (opened) + "/" + juce::String (kNumPairs)
                    + " pairs. " + errors.joinIntoString ("; ");
        // Keep partial open so meters show which pairs work; still mark aggregate.
    }
    else
    {
        lastError.clear();
    }

    if (opened == 0)
    {
        closeDevice();
        notifyStatus();
        return lastError;
    }

    aggregateOpen = true;
    primed.store (false, std::memory_order_relaxed);
    captureChannels = opened * 2;
    captureSampleRate = rate;
    captureBufferSize = buf;
    deviceLatencyIn = latency;
    currentName = juce::String (kAggregateName) + " (" + juce::String (opened) + " pairs"
                  + ", WASAPI " + juce::String (wasapiBufferSize)
                  + ", depth " + juce::String (latencyMs) + "ms)";
    clearXrunCounters();
    notifyStatus();
    return opened == kNumPairs ? juce::String() : lastError;
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
    if (wasapiBufferSize <= 0 && preferredBufferSize > 0)
        wasapiBufferSize = snapWasapiBuffer (preferredBufferSize);
    else
        wasapiBufferSize = snapWasapiBuffer (wasapiBufferSize > 0 ? wasapiBufferSize
                                                                  : kDefaultWasapiBuffer);

    primed.store (false, std::memory_order_relaxed);

    if (deviceName == kAggregateName
        || deviceName.startsWith (kAggregateName))
    {
        return openAggregate (preferredSampleRate, preferredBufferSize);
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
            if (primed.load (std::memory_order_relaxed) && ! p->isCallbackAlive (0.15))
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

        if (! ep.isOpen() || ep.markedDead || ! ep.isCallbackAlive (0.20))
        {
            ep.markedDead = ep.isOpen(); // keep flag if still "open" but silent
            return; // leave silence; do not underrun-count a dead callback
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
    if (! aggregateOpen)
    {
        if (single == nullptr)
            return {};
        return single->isHealthy() ? "OK" : "DEAD/stale";
    }

    juce::StringArray parts;
    for (int i = 0; i < kNumPairs; ++i)
    {
        auto& p = pairs[(size_t) i];
        juce::String s (pairLabel (i));
        if (p == nullptr || ! p->isOpen())
            s << "!";
        else if (p->markedDead || ! p->isCallbackAlive())
            s << "x"; // dead callback
        else
            s << "=" << juce::String (p->ratio, 3);
        if (i == masterPairIndex)
            s << "*";
        parts.add (s);
    }
    return parts.joinIntoString (" ");
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
