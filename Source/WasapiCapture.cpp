#include "WasapiCapture.h"

namespace
{
juce::StringArray wasapiTypeNames()
{
    return { "Windows Audio", "Windows Audio (Exclusive Mode)" };
}
} // namespace

WasapiCapture::WasapiCapture()
{
    for (auto& p : peaks)
        p.store (0.0f, std::memory_order_relaxed);

    deviceManager.addChangeListener (this);
    refreshDeviceList();
}

WasapiCapture::~WasapiCapture()
{
    deviceManager.removeChangeListener (this);
    closeDevice();
}

void WasapiCapture::refreshDeviceList()
{
    juce::Array<DeviceInfo> found;

    // Create temporary type objects so we can scan without disturbing an open device.
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type == nullptr || ! wasapiTypeNames().contains (type->getTypeName()))
            continue;

        type->scanForDevices();
        const auto names = type->getDeviceNames (true);

        for (const auto& name : names)
        {
            DeviceInfo info;
            info.name = name;
            info.typeName = type->getTypeName();
            info.inputChannels = 0; // filled after open
            info.sampleRate = 0.0;
            found.add (info);
        }
    }

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

juce::String WasapiCapture::openDevice (const juce::String& deviceName,
                                        double preferredSampleRate,
                                        int preferredBufferSize)
{
    closeDevice();
    lastError.clear();

    juce::String typeForDevice;
    {
        const juce::ScopedLock sl (deviceListLock);
        for (const auto& d : devices)
        {
            if (d.name == deviceName)
            {
                typeForDevice = d.typeName;
                break;
            }
        }
    }

    if (typeForDevice.isEmpty())
        typeForDevice = "Windows Audio";

    deviceManager.setCurrentAudioDeviceType (typeForDevice, true);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = deviceName;
    setup.outputDeviceName.clear();
    setup.sampleRate = preferredSampleRate > 0.0 ? preferredSampleRate : 48000.0;
    setup.bufferSize = preferredBufferSize > 0 ? preferredBufferSize : 512;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    setup.inputChannels.clear();
    for (int i = 0; i < kMaxChannels; ++i)
        setup.inputChannels.setBit (i);

    lastError = deviceManager.setAudioDeviceSetup (setup, true);

    if (lastError.isNotEmpty())
    {
        // Some drivers reject a custom bit mask — fall back to "all default inputs".
        setup.useDefaultInputChannels = true;
        setup.inputChannels.clear();
        lastError = deviceManager.setAudioDeviceSetup (setup, true);
    }

    if (lastError.isNotEmpty())
    {
        notifyStatus();
        return lastError;
    }

    auto* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr)
    {
        lastError = "Device opened but getCurrentAudioDevice() returned null";
        notifyStatus();
        return lastError;
    }

    captureChannels = juce::jmin (kMaxChannels, dev->getActiveInputChannels().countNumberOfSetBits());
    captureSampleRate = dev->getCurrentSampleRate();
    captureBufferSize = dev->getCurrentBufferSizeSamples();
    deviceLatencyIn = dev->getInputLatencyInSamples();

    // Prefer requesting more channels if the device advertises them but defaults were stereo.
    if (captureChannels < kMaxChannels)
    {
        const auto available = dev->getInputChannelNames();
        if (available.size() > captureChannels)
        {
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            const int want = juce::jmin (kMaxChannels, available.size());
            for (int i = 0; i < want; ++i)
                setup.inputChannels.setBit (i);

            const auto retryErr = deviceManager.setAudioDeviceSetup (setup, true);
            if (retryErr.isEmpty())
            {
                dev = deviceManager.getCurrentAudioDevice();
                if (dev != nullptr)
                {
                    captureChannels = juce::jmin (kMaxChannels, dev->getActiveInputChannels().countNumberOfSetBits());
                    captureSampleRate = dev->getCurrentSampleRate();
                    captureBufferSize = dev->getCurrentBufferSizeSamples();
                    deviceLatencyIn = dev->getInputLatencyInSamples();
                    lastError.clear();
                }
            }
        }
    }

    const int ringSize = juce::jmax (8192, (int) std::ceil (captureSampleRate * 0.25));
    ring.resize (kMaxChannels, ringSize);
    ring.reset();
    ring.clearCounters();

    deviceManager.addAudioCallback (this);
    notifyStatus();
    return {};
}

void WasapiCapture::closeDevice()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    captureChannels = 0;
    captureSampleRate = 0.0;
    captureBufferSize = 0;
    deviceLatencyIn = 0;
    ring.reset();
    notifyStatus();
}

bool WasapiCapture::isOpen() const noexcept
{
    return deviceManager.getCurrentAudioDevice() != nullptr && captureChannels > 0;
}

juce::String WasapiCapture::getCurrentDeviceName() const
{
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        return dev->getName();
    return {};
}

int WasapiCapture::getReportedLatencySamples() const noexcept
{
    // Device input latency + modest extra for the capture→process ring.
    const int ringPad = juce::jmax (captureBufferSize * 2, ring.getCapacity() / 8);
    return deviceLatencyIn + ringPad;
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

void WasapiCapture::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                      int numInputChannels,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext&)
{
    juce::ignoreUnused (outputChannelData, numOutputChannels);

    if (inputChannelData == nullptr || numSamples <= 0)
        return;

    const float* src[kMaxChannels] = {};
    const int n = juce::jmin (kMaxChannels, numInputChannels);

    for (int ch = 0; ch < n; ++ch)
    {
        src[ch] = inputChannelData[ch];
        if (src[ch] != nullptr)
        {
            float peak = 0.0f;
            for (int i = 0; i < numSamples; ++i)
                peak = juce::jmax (peak, std::abs (src[ch][i]));

            const float prev = peaks[(size_t) ch].load (std::memory_order_relaxed);
            if (peak > prev)
                peaks[(size_t) ch].store (peak, std::memory_order_relaxed);
        }
    }

    ring.write (src, numSamples);
}

void WasapiCapture::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    captureSampleRate = device->getCurrentSampleRate();
    captureBufferSize = device->getCurrentBufferSizeSamples();
    deviceLatencyIn = device->getInputLatencyInSamples();
    captureChannels = juce::jmin (kMaxChannels, device->getActiveInputChannels().countNumberOfSetBits());
    notifyStatus();
}

void WasapiCapture::audioDeviceStopped()
{
    notifyStatus();
}

void WasapiCapture::changeListenerCallback (juce::ChangeBroadcaster*)
{
    notifyStatus();
}

void WasapiCapture::notifyStatus()
{
    if (statusCallback)
        juce::MessageManager::callAsync ([cb = statusCallback] { cb(); });
}
