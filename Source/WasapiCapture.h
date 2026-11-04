#pragma once

#include <JuceHeader.h>
#include "RingBuffer.h"
#include <array>
#include <functional>

/** Independent WASAPI capture client (not the host ASIO device). */
class WasapiCapture final : private juce::AudioIODeviceCallback,
                            private juce::ChangeListener
{
public:
    static constexpr int kMaxChannels = 12;

    struct DeviceInfo
    {
        juce::String name;
        int inputChannels = 0;
        double sampleRate = 0.0;
        juce::String typeName; // "Windows Audio" / "Windows Audio (Exclusive Mode)"
    };

    WasapiCapture();
    ~WasapiCapture() override;

    void refreshDeviceList();
    juce::Array<DeviceInfo> getDevices() const;

    /** Open by exact device name as shown in the combo. Returns error string or {}. */
    juce::String openDevice (const juce::String& deviceName,
                             double preferredSampleRate,
                             int preferredBufferSize);

    void closeDevice();
    bool isOpen() const noexcept;

    juce::String getCurrentDeviceName() const;
    int getCaptureChannelCount() const noexcept { return captureChannels; }
    double getCaptureSampleRate() const noexcept { return captureSampleRate; }
    int getCaptureBufferSize() const noexcept { return captureBufferSize; }
    int getReportedLatencySamples() const noexcept;

    PlanarRingBuffer& getRing() noexcept { return ring; }
    const PlanarRingBuffer& getRing() const noexcept { return ring; }

    /** Peak levels per capture channel (0..1), updated from capture thread. */
    float getPeak (int channel) const noexcept;
    void decayPeaks (float factor) noexcept;

    using StatusCallback = std::function<void()>;
    void setStatusCallback (StatusCallback cb) { statusCallback = std::move (cb); }

    juce::String getLastError() const { return lastError; }

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    void notifyStatus();

    juce::AudioDeviceManager deviceManager;
    PlanarRingBuffer ring;
    int captureChannels = 0;
    double captureSampleRate = 0.0;
    int captureBufferSize = 0;
    int deviceLatencyIn = 0;
    juce::String lastError;
    StatusCallback statusCallback;

    std::array<std::atomic<float>, kMaxChannels> peaks {};
    juce::CriticalSection deviceListLock;
    juce::Array<DeviceInfo> devices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiCapture)
};
