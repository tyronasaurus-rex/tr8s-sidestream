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
    static constexpr const char* kAggregate4Name = "TR-8S (4 stems)";
    static constexpr const char* kAggregate2Name = "TR-8S (2 stems)";
    static constexpr const char* kSharedWasapi = "Windows Audio";
    static constexpr const char* kExclusiveWasapi = "Windows Audio (Exclusive Mode)";

    static constexpr int kDefaultLatencyMs = 150;
    /** 1024 is the practical floor for multi-client WASAPI capture. */
    static constexpr int kDefaultWasapiBuffer = 1024;
    static constexpr const char* kAggregateExclusiveName = "TR-8S (all stems, exclusive)";
    static constexpr int kMinLatencyMs = 40;
    static constexpr int kMaxLatencyMs = 250;
    /** wr/sec below this ⇒ pair is not sustaining host rate. */
    static constexpr double kMinSustainWriteRate = 40000.0;

    struct DeviceInfo
    {
        juce::String name;
        juce::String typeName;
        int inputChannels = 0;
        double sampleRate = 0.0;
        bool isAggregate = false;
        int aggregatePairs = 0; // 0 = physical; 2/4/6 = virtual aggregate
    };

    WasapiCapture();
    ~WasapiCapture();

    void refreshDeviceList();
    juce::Array<DeviceInfo> getDevices() const;

    /** Target FIFO depth (ms) before audio starts + drift waterline. Applied on next open. */
    void setLatencyMs (int ms) noexcept;
    int getLatencyMs() const noexcept { return latencyMs; }

    /** WASAPI period hint for all pairs (256/512/1024…). Applied on next open. */
    void setWasapiBufferSize (int samples) noexcept;
    int getWasapiBufferSize() const noexcept { return wasapiBufferSize; }

    int getTargetFillSamples() const noexcept;
    int getPrimingTargetSamples() const noexcept;

    juce::String openDevice (const juce::String& deviceName,
                             double preferredSampleRate,
                             int preferredBufferSize);

    void closeDevice();
    bool isOpen() const noexcept;
    bool isAggregateOpen() const noexcept { return aggregateOpen; }
    bool isPrimed() const noexcept { return primed.load (std::memory_order_relaxed); }
    void resetPrime() noexcept { primed.store (false, std::memory_order_relaxed); }

    juce::String getCurrentDeviceName() const;
    int getCaptureChannelCount() const noexcept { return captureChannels; }
    double getCaptureSampleRate() const noexcept { return captureSampleRate; }
    int getCaptureBufferSize() const noexcept { return captureBufferSize; }
    int getReportedLatencySamples() const noexcept;
    int getOpenPairCount() const noexcept;
    int getTargetPairCount() const noexcept { return targetPairCount; }
    juce::String getOpenPairSummary() const;

    int getAvailableToRead() const noexcept;
    /** Non-blocking read with per-pair slip (drop) / pad (soft underrun). */
    void readStems (float* const* dst, int numSamples) noexcept;

    int getUnderruns() const noexcept;
    int getOverruns() const noexcept;
    int getSlips() const noexcept;
    void clearXrunCounters() noexcept;

    int getMasterPairIndex() const noexcept { return masterPairIndex; }
    double getMasterRatio() const noexcept { return masterRatio.load (std::memory_order_relaxed); }

    /** Frozen 1s averages + peaks (safe to call from UI ~1 Hz). */
    juce::String getPairHealthSummary() const;
    /** Refresh frozen rate windows; call ~1 Hz from the editor. */
    void tickDiagnostics() noexcept;

    /** Path of last 5s health dump (empty if none yet). */
    juce::File getLastHealthDumpFile() const;
    juce::String getArchitectureNote() const;

    float getPeak (int channel) const noexcept;
    void decayPeaks (float factor) noexcept;

    using StatusCallback = std::function<void()>;
    void setStatusCallback (StatusCallback cb) { statusCallback = std::move (cb); }

    juce::String getLastError() const { return lastError; }

    static int pairIndexFromDeviceName (const juce::String& name);
    static bool isTr8sFamily (const juce::String& name);
    static bool isTr8sMix (const juce::String& name);
    static int snapWasapiBuffer (int samples) noexcept;
    static int aggregatePairCountFromName (const juce::String& name) noexcept;

private:
    struct PairEndpoint;

    void notifyStatus();
    juce::String openAggregate (double preferredSampleRate,
                                int preferredBufferSize,
                                int requestedPairs,
                                bool allowFallback);
    juce::String openSingle (const juce::String& deviceName,
                             const juce::String& typeName,
                             double preferredSampleRate,
                             int preferredBufferSize);
    int ringCapacityForRate (double sampleRate) const noexcept;
    int midpointTargetSamples() const noexcept;
    int pickMasterPair() const noexcept;
    juce::Array<DeviceInfo> discoverPhysicalDevices() const;
    void beginPostOpenHealthDump();
    void writeHealthDumpFile (const juce::String& body);

    std::array<std::unique_ptr<PairEndpoint>, kNumPairs> pairs;
    std::unique_ptr<PairEndpoint> single;

    bool aggregateOpen = false;
    std::atomic<bool> primed { false };
    int latencyMs = kDefaultLatencyMs;
    int wasapiBufferSize = kDefaultWasapiBuffer;
    int captureChannels = 0;
    double captureSampleRate = 0.0;
    int captureBufferSize = 0;
    int deviceLatencyIn = 0;
    int masterPairIndex = 0;
    int targetPairCount = 0;
    std::atomic<double> masterRatio { 1.0 };
    juce::String currentName;
    juce::String lastError;
    juce::String architectureNote;
    juce::File lastHealthDumpFile;
    std::atomic<int> healthDumpGeneration { 0 };
    StatusCallback statusCallback;

    std::array<std::atomic<float>, kMaxChannels> peaks {};
    juce::CriticalSection deviceListLock;
    juce::Array<DeviceInfo> devices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiCapture)
};
