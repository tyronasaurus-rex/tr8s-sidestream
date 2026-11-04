#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>

class TR8SSideStreamEditor final : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit TR8SSideStreamEditor (TR8SSideStreamProcessor&);
    ~TR8SSideStreamEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshDeviceCombo();
    void updateStatusLabel();
    void openSelectedDevice();
    void applyBufferSettingsFromUi();

    TR8SSideStreamProcessor& proc;

    juce::ComboBox deviceBox;
    juce::ComboBox wasapiBufBox;
    juce::Slider latencySlider;
    juce::Label latencyLabel;
    juce::Label wasapiLabel;
    juce::TextButton refreshButton { "Refresh" };
    juce::TextButton openButton { "Open" };
    juce::TextButton closeButton { "Close" };
    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Label hintLabel;

    static constexpr int kMeters = TR8SSideStreamProcessor::kNumStems;
    std::array<juce::Rectangle<int>, kMeters> meterBounds {};
    std::array<float, kMeters> meterLevels {};
    int statusTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TR8SSideStreamEditor)
};
