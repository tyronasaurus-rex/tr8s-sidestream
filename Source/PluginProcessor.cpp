#include "PluginProcessor.h"
#include "PluginEditor.h"

const char* TR8SSideStreamProcessor::stemName (int index) noexcept
{
    static constexpr const char* names[kNumStems] = {
        "BD", "SD", "LT", "HT", "CH", "OH",
        "CY", "RC", "CC", "IN10", "IN11", "EXT"
    };

    if (index < 0 || index >= kNumStems)
        return "?";
    return names[index];
}

juce::AudioProcessor::BusesProperties TR8SSideStreamProcessor::makeBuses()
{
    BusesProperties props;
    props = props.withOutput (stemName (0), juce::AudioChannelSet::mono(), true);
    for (int i = 1; i < kNumStems; ++i)
        props = props.withOutput (stemName (i), juce::AudioChannelSet::mono(), true);
    return props;
}

TR8SSideStreamProcessor::TR8SSideStreamProcessor()
    : AudioProcessor (makeBuses())
{
}

TR8SSideStreamProcessor::~TR8SSideStreamProcessor()
{
    capture.closeDevice();
}

void TR8SSideStreamProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate = sampleRate;
    hostBlockSize = samplesPerBlock;
    primed = false;

    setLatencySamples (capture.isOpen() ? capture.getReportedLatencySamples() : 0);

    if (preferredDevice.isNotEmpty() && ! capture.isOpen())
    {
        capture.openDevice (preferredDevice, hostSampleRate, hostBlockSize);
        setLatencySamples (capture.isOpen() ? capture.getReportedLatencySamples() : 0);
    }
}

void TR8SSideStreamProcessor::releaseResources()
{
    primed = false;
}

bool TR8SSideStreamProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.inputBuses.size() != 0)
        return false;

    if (layouts.outputBuses.size() != kNumStems)
        return false;

    for (int i = 0; i < kNumStems; ++i)
    {
        const auto& set = layouts.getChannelSet (false, i);
        if (set != juce::AudioChannelSet::mono() && set != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

int TR8SSideStreamProcessor::getPrimingTarget() const noexcept
{
    return juce::jmax (hostBlockSize * 2, capture.getCaptureBufferSize());
}

void TR8SSideStreamProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    if (! capture.isOpen() || numSamples <= 0)
        return;

    // Spike: 1:1 sample copy — match Live and Roland panel sample rates.
    if (! primed)
    {
        if (capture.getRing().getAvailableToRead() < getPrimingTarget())
            return;
        primed = true;
    }

    juce::AudioBuffer<float> temp (kNumStems, numSamples);
    temp.clear();

    float* chPtrs[kNumStems] = {};
    for (int i = 0; i < kNumStems; ++i)
        chPtrs[i] = temp.getWritePointer (i);

    capture.getRing().read (chPtrs, numSamples);

    const int captureCh = juce::jmin (kNumStems, capture.getCaptureChannelCount());

    for (int bus = 0; bus < kNumStems; ++bus)
    {
        auto* busPtr = getBus (false, bus);
        if (busPtr == nullptr || ! busPtr->isEnabled())
            continue;

        auto busBuffer = getBusBuffer (buffer, false, bus);
        if (bus < captureCh)
        {
            for (int c = 0; c < busBuffer.getNumChannels(); ++c)
                busBuffer.copyFrom (c, 0, temp, bus, 0, numSamples);
        }
    }

    setLatencySamples (capture.getReportedLatencySamples());
}

void TR8SSideStreamProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("TR8SSideStream");
    state.setProperty ("device", preferredDevice, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TR8SSideStreamProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        const auto state = juce::ValueTree::fromXml (*xml);
        preferredDevice = state.getProperty ("device", {}).toString();
    }
}

juce::AudioProcessorEditor* TR8SSideStreamProcessor::createEditor()
{
    return new TR8SSideStreamEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TR8SSideStreamProcessor();
}
