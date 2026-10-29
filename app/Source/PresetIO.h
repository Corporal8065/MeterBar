#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <meterbar/State.hpp>
// JSON presets in <Documents>/MeterBar/Presets, via JUCE built-in JSON (no extra deps).
namespace presetio {
inline juce::File presetFolder() {
    auto f = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                 .getChildFile("MeterBar").getChildFile("Presets");
    f.createDirectory(); return f;
}
inline juce::var toVar(const meterbar::PresetState& s) {
    auto* o = new juce::DynamicObject();
    auto set = [&](const char* k, juce::var v){ o->setProperty(k, v); };
    set("name", juce::String(s.name));
    set("specEn", s.spectrogramEnabled); set("waveEn", s.waveformEnabled); set("loudEn", s.loudnessEnabled);
    set("stereoEn", s.stereometerEnabled); set("scopeEn", s.oscilloscopeEnabled); set("spectrumEn", s.spectrumEnabled);
    set("sgOrient", (int)s.spectrogramOrientation); set("fade", s.fade); set("linkedSpeed", s.linkedSpeed);
    set("waveCh1", (int)s.waveCh1); set("waveCh2", (int)s.waveCh2); set("waveHist", (int)s.waveHistory);
    set("loudMode", (int)s.loudnessMode);
    set("loudTargets", s.loudnessShowTargets); // v0.17
    set("stereoMode", (int)s.stereoMode); set("stereoColor", (int)s.stereoColor);
    set("stereoPointAuto", s.stereoPointAuto); set("stereoPointSize", s.stereoPointSize);
    set("scopeTrigger", (int)s.scopeTrigger); set("scopeDisplay", (int)s.scopeDisplay);
    set("scopeWinMs", s.scopeWindowMs); // v0.15
    set("spectrumStyle", (int)s.spectrumStyle); set("smoothing", s.spectrumSmoothingPct); set("tilt", s.spectrumTiltDb);
    set("dispTilt", s.spectrumDisplayTiltDb); // v0.15
    { juce::String flex; for (int i = 0; i < 6; ++i) { if (i) flex += ","; flex += juce::String(s.moduleFlex[(size_t)i], 3); }
      set("modFlex", flex); } // v0.15: module widths, comma-joined in display order
    set("fftSize", s.fftSize); set("fftScale", (int)s.fftScale);
    set("showRef", s.spectrumShowReference); set("showTarget", s.spectrumShowTarget);
    set("spectrumColorMode", (int)s.spectrumColorMode); // v0.13
    set("theme", juce::String(s.themeName)); set("colorMap", (int)s.colorMap);
    set("bgOpacity", s.backgroundOpacity); // v0.13
    set("bgStyle", (int)s.backgroundStyle); set("beatPulse", s.beatPulseEnabled); // v0.16
    set("animeClicks", s.animeClickSounds); set("mascotFlex", s.mascotFlex); set("sixthOct", s.spectrumSixthOctave); // v0.16
    set("audioSource", juce::String(s.audioSource)); set("audioDevice", juce::String(s.audioDeviceName));
    set("outputDevice", juce::String(s.outputDeviceName));
    set("outputDeviceId", juce::String(s.outputDeviceId)); // v0.13
    set("trimDb", s.trimDb); set("routingPair", s.routingPair); set("routingCh1", s.routingCh1); set("routingCh2", s.routingCh2);
    set("vsync", s.vsync); set("fps", s.fpsLimit);
    set("pinned", s.pinned); set("stick", s.stick); set("showToolbar", s.showToolbar); set("showHint", s.showRepositionHint);
    set("reserveSpace", s.reserveScreenSpace); // v0.18
    set("vizEnabled", s.visualizerEnabled); set("vizHue", s.visualizerBaseHue); // v0.20.0
    set("vizSat", s.visualizerSaturation); set("vizSpread", s.visualizerSpread);
    set("vizCycle", s.visualizerCycleSec);
    set("winX", s.windowX); set("winY", s.windowY); set("winW", s.windowW); set("winH", s.windowH);
    return juce::var(o);
}
inline meterbar::PresetState fromVar(const juce::var& v, const meterbar::PresetState& base) {
    meterbar::PresetState s = base; if (!v.isObject()) return s;
    auto g = [&](const char* k){ return v.getProperty(k, {}); };
    auto gb = [&](const char* k, bool d){ auto x = g(k); return x.isVoid() ? d : (bool)x; };
    auto gi = [&](const char* k, int d){ auto x = g(k); return x.isVoid() ? d : (int)x; };
    auto gf = [&](const char* k, float d){ auto x = g(k); return x.isVoid() ? d : (float)(double)x; };
    s.name = g("name").toString().toStdString();
    s.spectrogramEnabled = gb("specEn", s.spectrogramEnabled); s.waveformEnabled = gb("waveEn", s.waveformEnabled);
    s.loudnessEnabled = gb("loudEn", s.loudnessEnabled); s.stereometerEnabled = gb("stereoEn", s.stereometerEnabled);
    s.oscilloscopeEnabled = gb("scopeEn", s.oscilloscopeEnabled); s.spectrumEnabled = gb("spectrumEn", s.spectrumEnabled);
    s.spectrogramOrientation = (meterbar::Orientation)gi("sgOrient", (int)s.spectrogramOrientation);
    s.fade = gf("fade", s.fade); s.linkedSpeed = gf("linkedSpeed", s.linkedSpeed);
    s.waveCh1 = (meterbar::ChanSel)gi("waveCh1", (int)s.waveCh1); s.waveCh2 = (meterbar::ChanSel)gi("waveCh2", (int)s.waveCh2);
    s.waveHistory = (meterbar::HistoryMode)gi("waveHist", (int)s.waveHistory);
    s.loudnessMode = (meterbar::LoudnessMode)gi("loudMode", (int)s.loudnessMode);
    s.loudnessShowTargets = gb("loudTargets", s.loudnessShowTargets); // v0.17 (absent in old presets -> keeps default false)
    s.stereoMode = (meterbar::StereoMode)gi("stereoMode", (int)s.stereoMode);
    s.stereoColor = (meterbar::StereoColor)gi("stereoColor", (int)s.stereoColor);
    s.stereoPointAuto = gb("stereoPointAuto", s.stereoPointAuto); s.stereoPointSize = gf("stereoPointSize", s.stereoPointSize);
    s.scopeTrigger = (meterbar::ScopeTrigger)gi("scopeTrigger", (int)s.scopeTrigger);
    s.scopeDisplay = (meterbar::ScopeDisplay)gi("scopeDisplay", (int)s.scopeDisplay);
    s.scopeWindowMs = gf("scopeWinMs", s.scopeWindowMs); // v0.15 (absent in old presets -> default 100 ms)
    s.spectrumStyle = (meterbar::PresetState::SpectrumStyle)gi("spectrumStyle", (int)s.spectrumStyle);
    s.spectrumSmoothingPct = gf("smoothing", s.spectrumSmoothingPct); s.spectrumTiltDb = gf("tilt", s.spectrumTiltDb);
    s.spectrumDisplayTiltDb = gf("dispTilt", s.spectrumDisplayTiltDb); // v0.15
    { auto toks = juce::StringArray::fromTokens(g("modFlex").toString(), ",", "");
      for (int i = 0; i < 6 && i < toks.size(); ++i) s.moduleFlex[(size_t)i] = toks[i].getFloatValue(); } // v0.15
    s.fftSize = gi("fftSize", s.fftSize); s.fftScale = (meterbar::FreqScale)gi("fftScale", (int)s.fftScale);
    s.spectrumShowReference = gb("showRef", s.spectrumShowReference); s.spectrumShowTarget = gb("showTarget", s.spectrumShowTarget);
    s.spectrumColorMode = (meterbar::SpectrumColorMode)gi("spectrumColorMode", (int)s.spectrumColorMode); // v0.13 (old presets keep default)
    s.themeName = g("theme").toString().toStdString(); s.colorMap = (meterbar::ColorMapId)gi("colorMap", (int)s.colorMap);
    s.backgroundOpacity = gf("bgOpacity", s.backgroundOpacity); // v0.13
    s.backgroundStyle = (meterbar::BackgroundStyle)gi("bgStyle", (int)s.backgroundStyle); // v0.16 (absent -> recipe default)
    s.beatPulseEnabled = gb("beatPulse", s.beatPulseEnabled); s.animeClickSounds = gb("animeClicks", s.animeClickSounds); // v0.16
    s.mascotFlex = gf("mascotFlex", s.mascotFlex); s.spectrumSixthOctave = gb("sixthOct", s.spectrumSixthOctave); // v0.16
    s.audioSource = g("audioSource").toString().toStdString(); s.audioDeviceName = g("audioDevice").toString().toStdString();
    s.outputDeviceName = g("outputDevice").toString().toStdString();
    s.outputDeviceId = g("outputDeviceId").toString().toStdString(); // v0.13 (absent in old presets -> name fallback still works)
    s.trimDb = gf("trimDb", s.trimDb); s.routingPair = gi("routingPair", s.routingPair);
    s.routingCh1 = gi("routingCh1", s.routingCh1); s.routingCh2 = gi("routingCh2", s.routingCh2);
    s.vsync = gb("vsync", s.vsync); s.fpsLimit = gi("fps", s.fpsLimit);
    s.pinned = gb("pinned", s.pinned); s.stick = gb("stick", s.stick);
    s.reserveScreenSpace = gb("reserveSpace", s.reserveScreenSpace); // v0.18 (absent in old presets -> default off)
    s.visualizerEnabled = gb("vizEnabled", s.visualizerEnabled); // v0.20.0 (absent -> OFF)
    s.visualizerBaseHue = gf("vizHue", s.visualizerBaseHue);
    s.visualizerSaturation = gf("vizSat", s.visualizerSaturation);
    s.visualizerSpread = gf("vizSpread", s.visualizerSpread);
    s.visualizerCycleSec = gf("vizCycle", s.visualizerCycleSec);
    s.showToolbar = gb("showToolbar", s.showToolbar); s.showRepositionHint = gb("showHint", s.showRepositionHint);
    s.windowX = gi("winX", s.windowX); s.windowY = gi("winY", s.windowY); s.windowW = gi("winW", s.windowW); s.windowH = gi("winH", s.windowH);
    return s;
}
inline bool save(const meterbar::PresetState& s, const juce::String& fileName) {
    auto f = presetFolder().getChildFile(juce::File::createLegalFileName(fileName) + ".json");
    return f.replaceWithText(juce::JSON::toString(toVar(s), true));
}
inline bool load(const juce::File& f, meterbar::PresetState& into) {
    auto parsed = juce::JSON::parse(f.loadFileAsString()); if (parsed.isVoid()) return false;
    into = fromVar(parsed, into); return true;
}
} // namespace presetio
