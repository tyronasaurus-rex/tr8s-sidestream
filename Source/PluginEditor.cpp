#include "PluginEditor.h"

TR8SSideStreamEditor::TR8SSideStreamEditor (TR8SSideStreamProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (560, 420);

    titleLabel.setText ("TR-8S SideStream", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    hintLabel.setText ("Ableton keeps Model 12 ASIO. This plugin opens TR-8S via WASAPI.",
                       juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions (13.0f));
    hintLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (hintLabel);

    addAndMakeVisible (deviceBox);
    addAndMakeVisible (refreshButton);
    addAndMakeVisible (openButton);
    addAndMakeVisible (closeButton);
    addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::topLeft);
    statusLabel.setFont (juce::FontOptions (13.0f));

    refreshButton.onClick = [this]
    {
        proc.getCapture().refreshDeviceList();
        refreshDeviceCombo();
        updateStatusLabel();
    };

    openButton.onClick = [this] { openSelectedDevice(); };
    closeButton.onClick = [this]
    {
        proc.getCapture().closeDevice();
        updateStatusLabel();
        repaint();
    };

    proc.getCapture().setStatusCallback ([this]
    {
        updateStatusLabel();
        repaint();
    });

    refreshDeviceCombo();

    if (proc.getPreferredDevice().isNotEmpty())
    {
        for (int i = 0; i < deviceBox.getNumItems(); ++i)
        {
            if (deviceBox.getItemText (i) == proc.getPreferredDevice())
            {
                deviceBox.setSelectedItemIndex (i, juce::dontSendNotification);
                break;
            }
        }
    }

    updateStatusLabel();
    startTimerHz (24);
}

TR8SSideStreamEditor::~TR8SSideStreamEditor()
{
    proc.getCapture().setStatusCallback (nullptr);
    stopTimer();
}

void TR8SSideStreamEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1d21));

    g.setColour (juce::Colour (0xff2a3038));
    g.fillRoundedRectangle (getLocalBounds().reduced (10).toFloat(), 8.0f);

    for (int i = 0; i < kMeters; ++i)
    {
        const auto area = meterBounds[(size_t) i];
        g.setColour (juce::Colour (0xff101418));
        g.fillRect (area);

        const float level = juce::jlimit (0.0f, 1.0f, meterLevels[(size_t) i]);
        auto fill = area;
        const int h = (int) std::round (level * (float) area.getHeight());
        fill = fill.removeFromBottom (h);

        g.setColour (level > 0.85f ? juce::Colours::orangered
                                   : juce::Colour (0xff3dd68c));
        g.fillRect (fill);

        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (11.0f);
        g.drawFittedText (TR8SSideStreamProcessor::stemName (i),
                          area.withY (area.getBottom() + 2).withHeight (14),
                          juce::Justification::centred, 1);
    }
}

void TR8SSideStreamEditor::resized()
{
    auto r = getLocalBounds().reduced (20);
    titleLabel.setBounds (r.removeFromTop (28));
    r.removeFromTop (4);
    hintLabel.setBounds (r.removeFromTop (20));
    r.removeFromTop (12);

    auto row = r.removeFromTop (28);
    deviceBox.setBounds (row.removeFromLeft (row.getWidth() - 260));
    row.removeFromLeft (8);
    refreshButton.setBounds (row.removeFromLeft (80));
    row.removeFromLeft (6);
    openButton.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (6);
    closeButton.setBounds (row.removeFromLeft (70));

    r.removeFromTop (10);
    statusLabel.setBounds (r.removeFromTop (70));
    r.removeFromTop (8);

    auto meters = r.removeFromTop (180);
    const int gap = 6;
    const int w = (meters.getWidth() - gap * (kMeters - 1)) / kMeters;
    for (int i = 0; i < kMeters; ++i)
    {
        meterBounds[(size_t) i] = { meters.getX() + i * (w + gap), meters.getY(), w, meters.getHeight() - 18 };
    }
}

void TR8SSideStreamEditor::timerCallback()
{
    auto& cap = proc.getCapture();
    for (int i = 0; i < kMeters; ++i)
        meterLevels[(size_t) i] = cap.getPeak (i);

    cap.decayPeaks (0.85f);
    updateStatusLabel();
    repaint();
}

void TR8SSideStreamEditor::refreshDeviceCombo()
{
    const auto selected = deviceBox.getText();
    deviceBox.clear (juce::dontSendNotification);

    const auto devices = proc.getCapture().getDevices();
    int id = 1;
    int preferIndex = -1;

    for (int i = 0; i < devices.size(); ++i)
    {
        const auto& d = devices.getReference (i);
        // Show type suffix so shared vs exclusive is visible; openDevice matches on prefix name.
        deviceBox.addItem (d.name + "  [" + d.typeName + "]", id);

        if (d.name.containsIgnoreCase ("TR-8S") || d.name.containsIgnoreCase ("TR8S"))
        {
            const bool multi = d.name.containsIgnoreCase ("MULTI");
            if (preferIndex < 0 || multi)
                preferIndex = i;
        }

        ++id;
    }

    if (selected.isNotEmpty())
    {
        for (int i = 0; i < deviceBox.getNumItems(); ++i)
            if (deviceBox.getItemText (i) == selected)
            {
                deviceBox.setSelectedItemIndex (i, juce::dontSendNotification);
                return;
            }
    }

    if (proc.getPreferredDevice().isNotEmpty())
    {
        for (int i = 0; i < deviceBox.getNumItems(); ++i)
            if (deviceBox.getItemText (i) == proc.getPreferredDevice())
            {
                deviceBox.setSelectedItemIndex (i, juce::dontSendNotification);
                return;
            }
    }

    if (preferIndex >= 0)
        deviceBox.setSelectedItemIndex (preferIndex, juce::dontSendNotification);
    else if (deviceBox.getNumItems() > 0)
        deviceBox.setSelectedItemIndex (0, juce::dontSendNotification);
}

void TR8SSideStreamEditor::openSelectedDevice()
{
    auto name = deviceBox.getText();
    if (name.isEmpty())
        return;

    // Strip "  [Windows Audio…]" suffix added for display.
    const int bracket = name.lastIndexOf ("  [");
    if (bracket > 0)
        name = name.substring (0, bracket);

    proc.setPreferredDevice (name);
    const auto err = proc.getCapture().openDevice (name, proc.getHostSampleRate(), proc.getHostBlockSize());
    if (err.isNotEmpty())
        statusLabel.setText ("Open failed: " + err, juce::dontSendNotification);
    else
        updateStatusLabel();

    // Persist preferred device into host state on next getStateInformation.
    repaint();
}

void TR8SSideStreamEditor::updateStatusLabel()
{
    auto& cap = proc.getCapture();
    juce::String s;

    if (cap.isOpen())
    {
        s << "OPEN: " << cap.getCurrentDeviceName() << "\n"
          << "Capture ch: " << cap.getCaptureChannelCount()
          << "  |  rate: " << juce::String (cap.getCaptureSampleRate(), 1) << " Hz"
          << "  |  buf: " << cap.getCaptureBufferSize()
          << "\nHost rate: " << juce::String (proc.getHostSampleRate(), 1) << " Hz"
          << "  |  PDC latency: " << cap.getReportedLatencySamples() << " samples"
          << "\nRing avail: " << cap.getRing().getAvailableToRead()
          << "  underruns: " << cap.getRing().getUnderruns()
          << "  overruns: " << cap.getRing().getOverruns();

        if (std::abs (cap.getCaptureSampleRate() - proc.getHostSampleRate()) > 1.0)
            s << "\nWARNING: sample rates differ — drift stub, match Roland panel to Live.";
    }
    else
    {
        s << "Closed. Select a TR-8S WASAPI input (prefer MULTI / 12-ch if listed) and Open.\n"
          << "Stereo endpoints (IN 1-2 … IN 11-12) only yield 2 channels each.\n"
          << "Devices listed: " << proc.getCapture().getDevices().size();
        if (cap.getLastError().isNotEmpty())
            s << "\nLast error: " << cap.getLastError();
    }

    statusLabel.setText (s, juce::dontSendNotification);
}
