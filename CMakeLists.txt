cmake_minimum_required(VERSION 3.22)

project(TR8SSideStream VERSION 0.4.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# System JUCE install used by OceanNoise / NoiseGen on this machine
add_subdirectory("C:/Program Files/JUCE" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

juce_add_plugin(TR8SSideStream
    COMPANY_NAME "Tyron"
    COMPANY_WEBSITE "https://localhost"
    COMPANY_EMAIL ""
    BUNDLE_ID "com.tyron.tr8s-sidestream"
    # Ableton requires Instrument-category VST3s to expose an event (MIDI) input
    # bus or it fails with: "plugin has instrument category, but no valid event input bus".
    # We ignore MIDI in processBlock; the bus exists so Live will load us on a MIDI track.
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_COPY_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3"
    PLUGIN_MANUFACTURER_CODE Tyro
    PLUGIN_CODE Tr8s
    FORMATS VST3 Standalone
    PRODUCT_NAME "TR-8S SideStream"
    VST3_CATEGORIES "Instrument|Synth"
    DESCRIPTION "WASAPI side-stream capture of Roland TR-8S INDIV stems into the host")

juce_generate_juce_header(TR8SSideStream)

target_sources(TR8SSideStream
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/WasapiCapture.cpp
        Source/WasapiCapture.h
        Source/RingBuffer.h)

target_compile_definitions(TR8SSideStream
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_ASIO=0
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=0
        JUCE_ALSA=0
        JUCE_JACK=0
        JUCE_USE_WINRT_MIDI=0)

target_link_libraries(TR8SSideStream
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
