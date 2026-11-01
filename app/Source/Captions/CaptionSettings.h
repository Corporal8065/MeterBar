#pragma once
#include <juce_core/juce_core.h>
#include <meterbar/Captions.hpp>

// LIVE CAPTIONS — SETTINGS (v0.23.0, spec section 1.7).
//
// App-local file, deliberately NOT part of PresetState: loading a
// meter preset must never start/stop transcription or move the
// subtitle window. The JSON holds no audio and no caption text.
struct CaptionSettingsData {
    meterbar::CaptionMode mode = meterbar::CaptionMode::Off;
    bool alwaysOnTop = true;
    bool hasPosition = false;
    int x = 0, y = 0, w = 900;
};

class CaptionSettings {
public:
    // Documents\MeterBar\Captions\settings.json
    static juce::File settingsFile();
    static CaptionSettingsData load();
    static void save(const CaptionSettingsData& data);
};
