#pragma once

#include <JuceHeader.h>
#include "WasapiCapture.h"

class TR8SSideStreamProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kNumStems = 12;

    static const char* stemName (int index) noexcept;

    TR8SSideStreamProcessor();
    ~TR8SSideStreamProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    WasapiCapture& getCapture() noexcept { return capture; }
    const WasapiCapture& getCapture() const noexcept { return capture; }

    juce::String getPreferredDevice() const { return preferredDevice; }
    void setPreferredDevice (const juce::String& name) { preferredDevice = name; }

    double getHostSampleRate() const noexcept { return hostSampleRate; }
    int getHostBlockSize() const noexcept { return hostBlockSize; }

    /** Soft-start ring: wait until this many samples buffered before reading. */
    int getPrimingTarget() const noexcept;

private:
    static juce::AudioProcessor::BusesProperties makeBuses();

    WasapiCapture capture;
    juce::String preferredDevice;
    double hostSampleRate = 48000.0;
    int hostBlockSize = 512;
    bool primed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TR8SSideStreamProcessor)
};
