#pragma once

#include <JuceHeader.h>
#include "RingBuffer.h"
#include <array>
#include <functional>
#include <memory>

/** Independent WASAPI capture — single endpoint or aggregated TR-8S stereo pairs. */
class WasapiCapture
{
public:
    static constexpr int kMaxChannels = 12;
    static constexpr int kNumPairs = 6;
    static constexpr const char* kAggregateName = "TR-8S (all stems)";
    static constexpr const char* kSharedWasapi = "Windows Audio";
    static constexpr const char* kExclusiveWasapi = "Windows Audio (Exclusive Mode)";

    struct DeviceInfo
    {
        juce::String name;
        juce::String typeName; // WASAPI type, or "Aggregate" for virtual entry
        int inputChannels = 0;
        double sampleRate = 0.0;
        bool isAggregate = false;
    };

    WasapiCapture();
    ~WasapiCapture();

    void refreshDeviceList();
    juce::Array<DeviceInfo> getDevices() const;

    /** Open by combo name. Aggregate name opens all six IN pairs (shared WASAPI). */
    juce::String openDevice (const juce::String& deviceName,
                             double preferredSampleRate,
                             int preferredBufferSize);

    void closeDevice();
    bool isOpen() const noexcept;
    bool isAggregateOpen() const noexcept { return aggregateOpen; }

    juce::String getCurrentDeviceName() const;
    int getCaptureChannelCount() const noexcept { return captureChannels; }
    double getCaptureSampleRate() const noexcept { return captureSampleRate; }
    int getCaptureBufferSize() const noexcept { return captureBufferSize; }
    int getReportedLatencySamples() const noexcept;
    int getOpenPairCount() const noexcept;
    juce::String getOpenPairSummary() const;

    int getAvailableToRead() const noexcept;
    void readStems (float* const* dst, int numSamples) noexcept;

    int getUnderruns() const noexcept;
    int getOverruns() const noexcept;

    float getPeak (int channel) const noexcept;
    void decayPeaks (float factor) noexcept;

    using StatusCallback = std::function<void()>;
    void setStatusCallback (StatusCallback cb) { statusCallback = std::move (cb); }

    juce::String getLastError() const { return lastError; }

    /** Parse "IN 1-2 (TR-8S)" → pair index 0..5, or -1 if not a stem pair. */
    static int pairIndexFromDeviceName (const juce::String& name);
    static bool isTr8sFamily (const juce::String& name);
    static bool isTr8sMix (const juce::String& name);

private:
    struct PairEndpoint;

    void notifyStatus();
    juce::String openAggregate (double preferredSampleRate, int preferredBufferSize);
    juce::String openSingle (const juce::String& deviceName,
                             const juce::String& typeName,
                             double preferredSampleRate,
                             int preferredBufferSize);
    void prepareRings (double sampleRate);
    juce::Array<DeviceInfo> discoverPhysicalDevices() const;

    std::array<std::unique_ptr<PairEndpoint>, kNumPairs> pairs;
    std::unique_ptr<PairEndpoint> single; // non-aggregate / MIX / lone pair

    bool aggregateOpen = false;
    int captureChannels = 0;
    double captureSampleRate = 0.0;
    int captureBufferSize = 0;
    int deviceLatencyIn = 0;
    juce::String currentName;
    juce::String lastError;
    StatusCallback statusCallback;

    std::array<std::atomic<float>, kMaxChannels> peaks {};
    juce::CriticalSection deviceListLock;
    juce::Array<DeviceInfo> devices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiCapture)
};
