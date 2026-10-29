#pragma once
#include <array>
#include <string>
#include "Fft.hpp"
#include "ColorMap.hpp"
#include "Loudness.hpp"
#include "Modules.hpp"
namespace meterbar {
enum class BackgroundStyle { CleanDark, MidnightKids, Flat, Pastel }; // v0.16 (Pastel appended for the Anime theme)
// Every user setting in SPEC §2–§7, with SPEC defaults. The JUCE app maps this
// 1:1 to JSON presets; the DSP core tests assert the defaults.
// NOTE (v0.16 item 1, recipe revised v0.17): the C++ defaults of this
// struct ARE Jackson's "Default" preset recipe — a fresh install (no
// saved state anywhere) constructs exactly this state: Midnight Kids
// theme + Midnight colour map, Clean Dark background at 15% opacity,
// Spectrum style Both, smoothing 50, display Tilt 1.15 dB/oct, analyser
// tilt 5.6 dB, Colour By = Level, scope window 100 ms, LUFS Momentary,
// loudness streaming-target lines OFF, beat pulse on, cute click sounds
// on, trim +10.18 dB, all modules enabled at equal widths, and the audio
// source = System Loopback on the OS-default output (never the test
// tone). Saved presets are separate files and are never touched by
// these defaults.
struct PresetState {
    std::string name = "Default";
    // Modules enable
    bool spectrogramEnabled = true, waveformEnabled = true, loudnessEnabled = true;
    bool stereometerEnabled = true, oscilloscopeEnabled = true, spectrumEnabled = true;
    // Spectrogram
    Orientation spectrogramOrientation = Orientation::Horizontal;
    float fade = 0.5f;                 // 0..1
    bool loopEnabled = false;          // present but disabled/greyed in UI
    // Linked
    float linkedSpeed = 5.0f;          // 0..10, shared spectrogram <-> waveform
    // Waveform
    ChanSel waveCh1 = ChanSel::Left; ChanSel waveCh2 = ChanSel::Off;
    HistoryMode waveHistory = HistoryMode::Off;
    // Loudness
    LoudnessMode loudnessMode = LoudnessMode::LufsMomentary;
    bool loudnessShowTargets = false;  // v0.17: streaming-target lines (Spotify/YT -14, Apple Music -16) beside the loudness bars — OFF in the Default recipe (Jackson corrected himself to off); the Integrated/Short-Term/True Peak lines always show
    // Stereometer
    StereoMode stereoMode = StereoMode::ScaledUnipolar;
    StereoColor stereoColor = StereoColor::Single;
    bool stereoPointAuto = true; float stereoPointSize = 1.0f; // 1..20
    // Oscilloscope
    ScopeTrigger scopeTrigger = ScopeTrigger::Pitch;
    ScopeDisplay scopeDisplay = ScopeDisplay::Single;
    float scopeWindowMs = 100.0f;      // v0.15: visible time window 10..200 ms (wider than the old fixed ~43 ms)
    // Spectrum
    enum class SpectrumStyle { Fft, Color, Bars, Both };
    SpectrumStyle spectrumStyle = SpectrumStyle::Both; // v0.17 recipe: bars + FFT line (was Fft)
    float spectrumSmoothingPct = 50.0f; // 0..100 (v0.17 recipe; SPEC's old default was 90)
    float spectrumTiltDb = 5.6f;        // -9..+9 analyser tilt (v0.17 recipe; was 4.5)
    int fftSize = 1024;                 // 1024..16384
    FreqScale fftScale = FreqScale::Log;
    bool spectrumShowReference = false, spectrumShowTarget = false;
    SpectrumColorMode spectrumColorMode = SpectrumColorMode::Level; // v0.13: level-heat default (Jackson's ask)
    float spectrumDisplayTiltDb = 1.15f;   // v0.15: DISPLAY-ONLY slope compensation, dB/octave 0..6 — never touches analyser data (v0.17 recipe: 1.15; was Pro-Q's 4.5)
    bool spectrumSixthOctave = false;      // v0.16: optional 1/6-octave perceptual smoothing of the drawn spectrum (display-only)
    // Appearance
    std::string themeName = "Midnight Kids"; // v0.12: Jackson's colourful default (was "Midnight Pink", still selectable)
    ColorMapId colorMap = ColorMapId::Midnight; // v0.15: was Plasma — Midnight IS the v0.14 palette as a LUT, so the default spectrogram look is unchanged while the selector now visibly drives it (saved presets keep their own stored map)
    float backgroundOpacity = 0.15f;       // v0.13: module/window background alpha 0.15..1 (desktop shows through below 1). v0.17 recipe: 15% — the slider/view floor, airy over the desktop
    // v0.16: background STYLE (Settings -> Appearance). CleanDark is the
    // default — it is part of Jackson's "Default" preset recipe (item 1).
    // Pastel is appended (value 3) as the Anime theme's paired background.
    BackgroundStyle backgroundStyle = BackgroundStyle::CleanDark;
    bool beatPulseEnabled = true;          // v0.16: onset-reactive border glow, default ON in the Default recipe (subtle)
    bool animeClickSounds = true;          // v0.16: cute click sounds — only ever audible in the Anime theme
    float mascotFlex = 0.55f;              // v0.16: mascot cell width (relative), resizable like a module in Anime theme
    // Audio (§6)
    // v0.17 recipe: first run hears the SYSTEM'S DEFAULT OUTPUT (loopback
    // of the OS-default render endpoint — empty outputDeviceName/Id makes
    // WindowsLoopback::pickDevice return GetDefaultAudioEndpoint). The
    // test tone is a diagnostic the user picks explicitly, never the
    // first-run source.
    std::string audioSource = "System Loopback"; // "Input" | "System Loopback" | "Test Tone"
    std::string audioDeviceName;           // chosen INPUT device (source = Input)
    std::string outputDeviceName;          // chosen OUTPUT device (source = System Loopback captures THIS device)
    std::string outputDeviceId;            // v0.13: WASAPI endpoint ID of the chosen output — loopback keys on THIS, name is display/fallback
    float trimDb = 10.18f;             // -24..+24 (v0.17 recipe: +10.18 dB exactly, Jackson's number)
    int routingPair = 0;               // stereo pair index
    int routingCh1 = 0, routingCh2 = 1;
    // Performance (§7)
    bool vsync = true; int fpsLimit = 60; // 15/30/60/120
    // Layout (v0.15): relative widths of the 6 modules in display order
    // (Spectrogram, Waveform, Loudness, Stereometer, Oscilloscope, Spectrum),
    // adjusted by dragging the dividers between modules. Persisted in presets.
    std::array<float, 6> moduleFlex = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
    // Shell (§1)
    bool pinned = false, stick = false, showToolbar = true, showRepositionHint = true;
    bool reserveScreenSpace = false; // v0.18: Windows AppBar reservation — while the bar is docked at a monitor's top edge, Windows shrinks that monitor's work area so maximised windows stop below the bar. Default OFF (Jackson's ask; it changes how OTHER windows behave, so it is strictly opt-in). The "Default" recipe does not set it.
    int windowX = 100, windowY = 40, windowW = 1100, windowH = 170;
};
} // namespace meterbar
