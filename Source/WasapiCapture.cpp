#include "WasapiCapture.h"
#include <climits>

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

        manager.setCurrentAudioDeviceType (typeName.isNotEmpty() ? typeName
                                                                  : juce::String (kSharedWasapi),
                                           true);

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = deviceName;
        setup.outputDeviceName.clear();
        setup.sampleRate = preferredSampleRate > 0.0 ? preferredSampleRate : 48000.0;
        setup.bufferSize = preferredBufferSize > 0 ? preferredBufferSize : 512;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.inputChannels.clear();
        setup.inputChannels.setBit (0);
        setup.inputChannels.setBit (1);

        auto err = manager.setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty())
        {
            setup.useDefaultInputChannels = true;
            setup.inputChannels.clear();
            err = manager.setAudioDeviceSetup (setup, true);
        }

        if (err.isNotEmpty())
            return err;

        auto* dev = manager.getCurrentAudioDevice();
        if (dev == nullptr)
            return "Opened null device: " + deviceName;

        deviceNameOpen = deviceName;
        sampleRate = dev->getCurrentSampleRate();
        bufferSize = dev->getCurrentBufferSizeSamples();
        latencyIn = dev->getInputLatencyInSamples();
        activeChannels = juce::jmin (2, dev->getActiveInputChannels().countNumberOfSetBits());

        const int ringSize = juce::jmax (8192, (int) std::ceil (sampleRate * 0.25));
        ring.resize (2, ringSize);
        ring.reset();
        ring.clearCounters();

        manager.addAudioCallback (this);
        return {};
    }

    void close()
    {
        manager.removeAudioCallback (this);
        manager.closeAudioDevice();
        deviceNameOpen.clear();
        activeChannels = 0;
        sampleRate = 0.0;
        bufferSize = 0;
        latencyIn = 0;
        ring.reset();
    }

    bool isOpen() const noexcept
    {
        return manager.getCurrentAudioDevice() != nullptr && activeChannels > 0;
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
    int activeChannels = 0;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int latencyIn = 0;
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

void WasapiCapture::prepareRings (double sampleRate)
{
    juce::ignoreUnused (sampleRate);
    // Per-endpoint rings are sized in PairEndpoint::open.
}

juce::String WasapiCapture::openAggregate (double preferredSampleRate, int preferredBufferSize)
{
    closeDevice();
    lastError.clear();

    auto physical = discoverPhysicalDevices();

    struct FoundPair
    {
        juce::String name;
        juce::String typeName;
    };

    FoundPair found[kNumPairs];
    int foundCount = 0;

    // Prefer shared WASAPI for each pair index.
    for (int pass = 0; pass < 2; ++pass)
    {
        const juce::String wantType = (pass == 0 ? juce::String (kSharedWasapi)
                                                 : juce::String (kExclusiveWasapi));

        for (const auto& d : physical)
        {
            if (d.typeName != wantType)
                continue;

            const int idx = pairIndexFromDeviceName (d.name);
            if (idx < 0 || found[idx].name.isNotEmpty())
                continue;

            found[idx] = { d.name, d.typeName };
            ++foundCount;
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

    for (int i = 0; i < kNumPairs; ++i)
    {
        auto err = pairs[(size_t) i]->open (found[i].name,
                                            found[i].typeName,
                                            preferredSampleRate,
                                            preferredBufferSize);
        if (err.isNotEmpty())
        {
            errors.add (found[i].name + ": " + err);
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
    captureChannels = opened * 2;
    captureSampleRate = rate;
    captureBufferSize = buf;
    deviceLatencyIn = latency;
    currentName = juce::String (kAggregateName) + " (" + juce::String (opened) + " pairs)";
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
    const int ringPad = juce::jmax (captureBufferSize * 2, 1024);
    return deviceLatencyIn + ringPad;
}

int WasapiCapture::getAvailableToRead() const noexcept
{
    if (aggregateOpen)
    {
        int minAvail = INT_MAX;
        int openCount = 0;
        for (auto& p : pairs)
        {
            if (p == nullptr || ! p->isOpen())
                continue;
            ++openCount;
            minAvail = juce::jmin (minAvail, p->ring.getAvailableToRead());
        }
        return openCount > 0 ? minAvail : 0;
    }

    if (single != nullptr && single->isOpen())
        return single->ring.getAvailableToRead();

    return 0;
}

void WasapiCapture::readStems (float* const* dst, int numSamples) noexcept
{
    if (dst == nullptr || numSamples <= 0)
        return;

    // Clear all stems first.
    for (int ch = 0; ch < kMaxChannels; ++ch)
        if (dst[ch] != nullptr)
            juce::FloatVectorOperations::clear (dst[ch], numSamples);

    if (aggregateOpen)
    {
        for (int i = 0; i < kNumPairs; ++i)
        {
            auto& p = pairs[(size_t) i];
            if (p == nullptr || ! p->isOpen())
                continue;

            float* pairDst[2] = {
                dst[i * 2],
                dst[i * 2 + 1]
            };
            p->ring.read (pairDst, numSamples);
        }
        return;
    }

    if (single != nullptr && single->isOpen())
    {
        const int off = single->channelOffset;
        float* pairDst[2] = {
            (off < kMaxChannels ? dst[off] : nullptr),
            (off + 1 < kMaxChannels ? dst[off + 1] : nullptr)
        };
        single->ring.read (pairDst, numSamples);
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
