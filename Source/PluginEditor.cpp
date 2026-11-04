#include "PluginEditor.h"

TR8SSideStreamEditor::TR8SSideStreamEditor (TR8SSideStreamProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (720, 560);

    titleLabel.setText ("TR-8S SideStream", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    hintLabel.setText ("Use “TR-8S (all stems)”. Depth≈PDC; reopen after changing buffer settings.",
                       juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions (12.5f));
    hintLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (hintLabel);

    addAndMakeVisible (deviceBox);
    addAndMakeVisible (refreshButton);
    addAndMakeVisible (openButton);
    addAndMakeVisible (closeButton);
    addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::topLeft);
    statusLabel.setFont (juce::FontOptions (12.0f));

    wasapiLabel.setText ("WASAPI buf", juce::dontSendNotification);
    wasapiLabel.attachToComponent (&wasapiBufBox, true);
    addAndMakeVisible (wasapiLabel);
    addAndMakeVisible (wasapiBufBox);
    wasapiBufBox.addItem ("256", 256);
    wasapiBufBox.addItem ("512", 512);
    wasapiBufBox.addItem ("1024", 1024);
    wasapiBufBox.addItem ("2048", 2048);
    wasapiBufBox.setSelectedId (proc.getCapture().getWasapiBufferSize(), juce::dontSendNotification);
    if (wasapiBufBox.getSelectedId() == 0)
        wasapiBufBox.setSelectedId (1024, juce::dontSendNotification);

    latencyLabel.setText ("Depth ms", juce::dontSendNotification);
    latencyLabel.attachToComponent (&latencySlider, true);
    addAndMakeVisible (latencyLabel);
    addAndMakeVisible (latencySlider);
    latencySlider.setRange (WasapiCapture::kMinLatencyMs, WasapiCapture::kMaxLatencyMs, 1.0);
    latencySlider.setValue (proc.getCapture().getLatencyMs(), juce::dontSendNotification);
    latencySlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 22);
    latencySlider.setSliderStyle (juce::Slider::LinearHorizontal);

    latencySlider.onValueChange = [this] { applyBufferSettingsFromUi(); };
    wasapiBufBox.onChange = [this] { applyBufferSettingsFromUi(); };

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

    bool selected = false;
    if (proc.getPreferredDevice().isNotEmpty())
    {
        for (int i = 0; i < deviceBox.getNumItems(); ++i)
        {
            if (deviceBox.getItemText (i).startsWith (proc.getPreferredDevice())
                || (proc.getPreferredDevice() == WasapiCapture::kAggregateName
                    && deviceBox.getItemText (i).startsWith (WasapiCapture::kAggregateName)))
            {
                deviceBox.setSelectedItemIndex (i, juce::dontSendNotification);
                selected = true;
                break;
            }
        }
    }

    if (! selected)
    {
        for (int i = 0; i < deviceBox.getNumItems(); ++i)
        {
            if (deviceBox.getItemText (i).startsWith (WasapiCapture::kAggregateName))
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

void TR8SSideStreamEditor::applyBufferSettingsFromUi()
{
    proc.getCapture().setLatencyMs ((int) latencySlider.getValue());
    const int id = wasapiBufBox.getSelectedId();
    if (id > 0)
        proc.getCapture().setWasapiBufferSize (id);
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
        fill = fill.removeFromBottom ((int) std::round (level * (float) area.getHeight()));
        g.setColour (level > 0.85f ? juce::Colours::orangered : juce::Colour (0xff3dd68c));
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
    titleLabel.setBounds (r.removeFromTop (26));
    r.removeFromTop (4);
    hintLabel.setBounds (r.removeFromTop (18));
    r.removeFromTop (10);

    auto row = r.removeFromTop (28);
    deviceBox.setBounds (row.removeFromLeft (row.getWidth() - 260));
    row.removeFromLeft (8);
    refreshButton.setBounds (row.removeFromLeft (80));
    row.removeFromLeft (6);
    openButton.setBounds (row.removeFromLeft (70));
    row.removeFromLeft (6);
    closeButton.setBounds (row.removeFromLeft (70));

    r.removeFromTop (10);
    auto settings = r.removeFromTop (28);
    settings.removeFromLeft (72);
    wasapiBufBox.setBounds (settings.removeFromLeft (90));
    settings.removeFromLeft (80);
    latencySlider.setBounds (settings);

    r.removeFromTop (10);
    statusLabel.setBounds (r.removeFromTop (140));
    r.removeFromTop (8);

    auto meters = r.removeFromTop (160);
    const int gap = 6;
    const int w = (meters.getWidth() - gap * (kMeters - 1)) / kMeters;
    for (int i = 0; i < kMeters; ++i)
        meterBounds[(size_t) i] = { meters.getX() + i * (w + gap), meters.getY(), w, meters.getHeight() - 18 };
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
    int aggregateIndex = -1;

    for (int i = 0; i < devices.size(); ++i)
    {
        const auto& d = devices.getReference (i);
        juce::String label = d.name;
        label += d.isAggregate ? "  [12 mono from IN 1-2…11-12]"
                              : ("  [" + d.typeName + "]");
        deviceBox.addItem (label, id);
        if (d.isAggregate)
            aggregateIndex = i;
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

    if (aggregateIndex >= 0)
        deviceBox.setSelectedItemIndex (aggregateIndex, juce::dontSendNotification);
    else if (deviceBox.getNumItems() > 0)
        deviceBox.setSelectedItemIndex (0, juce::dontSendNotification);
}

void TR8SSideStreamEditor::openSelectedDevice()
{
    applyBufferSettingsFromUi();

    auto name = deviceBox.getText();
    if (name.isEmpty())
        return;

    const int bracket = name.lastIndexOf ("  [");
    if (bracket > 0)
        name = name.substring (0, bracket);
    if (name.startsWith (WasapiCapture::kAggregateExclusiveName))
        name = WasapiCapture::kAggregateExclusiveName;
    else if (name.startsWith (WasapiCapture::kAggregateName))
        name = WasapiCapture::kAggregateName;

    proc.setPreferredDevice (name);
    const auto err = proc.getCapture().openDevice (name, proc.getHostSampleRate(), proc.getHostBlockSize());
    if (err.isNotEmpty())
        statusLabel.setText ("Open: " + err, juce::dontSendNotification);
    else
        updateStatusLabel();
    repaint();
}

void TR8SSideStreamEditor::updateStatusLabel()
{
    auto& cap = proc.getCapture();
    juce::String s;

    if (cap.isOpen())
    {
        s << "OPEN: " << cap.getCurrentDeviceName() << "\n";
        if (cap.isAggregateOpen())
            s << "Pairs: " << cap.getOpenPairSummary() << "\n";

        s << "ch " << cap.getCaptureChannelCount()
          << " | rate " << juce::String (cap.getCaptureSampleRate(), 1) << " Hz"
          << " | WASAPI " << cap.getCaptureBufferSize() << " (req " << cap.getWasapiBufferSize() << ")"
          << " | depth " << cap.getLatencyMs() << " ms\n"
          << "Host " << juce::String (proc.getHostSampleRate(), 1) << " Hz / block "
          << proc.getHostBlockSize()
          << " | PDC " << cap.getReportedLatencySamples() << " smp"
          << " | primed " << (cap.isPrimed() ? "yes" : "pre-roll…")
          << " | SRC " << juce::String (cap.getMasterRatio(), 4) << "\n"
          << "FIFO " << cap.getAvailableToRead() << " (mid " << (int) std::lround (cap.getTargetFillSamples() * 0.60)
          << " / depth " << cap.getTargetFillSamples() << ")"
          << " | underrun " << cap.getUnderruns()
          << " | overrun " << cap.getOverruns()
          << " | slip " << cap.getSlips() << "\n"
          << cap.getPairHealthSummary();

        if (std::abs (cap.getCaptureSampleRate() - proc.getHostSampleRate()) > 1.0)
            s << "\nWARNING: sample rates differ — set Roland panel = Live.";
    }
    else
    {
        s << "Closed. Prefer “TR-8S (all stems)” or “…exclusive”. Depth 150, WASAPI 1024.\n"
          << "Ableton buffer 512–1024 @ 48 kHz. After Open, status must show cb/s>0 on all pairs.\n"
          << "Devices: " << proc.getCapture().getDevices().size();
        if (cap.getLastError().isNotEmpty())
            s << "\nLast error: " << cap.getLastError();
    }

    statusLabel.setText (s, juce::dontSendNotification);
}
