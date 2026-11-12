#pragma once
#include <array>
#include <string>
#include "Fft.hpp"
#include "ColorMap.hpp"
#include "Loudness.hpp"
#include "Modules.hpp"
#include "Performance.hpp"
namespace meterbar {
enum class BackgroundStyle { CleanDark, MidnightKids, Flat, Pastel }; // v0.16 (Pastel appended for the Anime theme)
// Every user setting in SPEC §2–§7, with SPEC defaults. The JUCE app maps this
// 1:1 to JSON presets; the DSP core tests assert the defaults.
// NOTE (v0.16 item 1, recipe revised v0.17): the C++ defaults of this
// struct ARE Jackson's "Default" preset recipe — a fresh install (no
// saved state anywhere) constructs exactly this state: Midnight Kids
// theme + Midnight colour map, Midnight Kids background style at 15%
// opacity, Spectrum style Both, smoothing 50, display Tilt 1.15 dB/oct, analyser
// tilt 5.6 dB, Colour By = Level, scope window 100 ms, LUFS Momentary,
// loudness streaming-target lines OFF, beat pulse on, cute click sounds
// on, trim +27 dB, visualizer background ON (Field style), all modules enabled at equal widths, and the audio
// source = System Loopback on the OS-default output (never the test
// tone). Saved presets are separate files and are never touched by
// these defaults. (v0.28.0 recipe revision, Jackson's word 2026-11-02:
// trim +10.18 -> +27 dB, background style Clean Dark -> Midnight Kids,
// visualizer OFF/Bubbles -> ON/Field. v0.28.6 recipe revision, Jackson
// 2026-11-03: Stereometer colour Single -> Rainbow, point size 1 -> 2.)
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
    StereoColor stereoColor = StereoColor::Rainbow; // v0.28.6 recipe (Jackson, 2026-11-03): Rainbow is the default, not the purple Single — a DEFAULTS change only: saved presets/settings carry their own stored value and are never rewritten by this
    bool stereoPointAuto = true; float stereoPointSize = 2.0f; // 1..20; v0.28.6 recipe (Jackson, 2026-11-03): default point size 2 (was 1)
    // Oscilloscope
    ScopeTrigger scopeTrigger = ScopeTrigger::Free; // v0.28.13 (Jackson, 2026-11-12): the FRESH-INSTALL default is Free (free-run) — a defaults change only: a saved scopeTrigger in settings/presets loads over this and is never rewritten by it
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
    // v0.16: background STYLE (Settings -> Appearance). CleanDark is a
    // selectable style; v0.28.0 the Default recipe's pick is
    // MidnightKids (Jackson's word 2026-11-02; FEEDBACK.md named it
    // "Midnight Kits" — Midnight Kids is the exact in-app style name).
    // Pastel is appended (value 3) as the Anime theme's paired background.
    BackgroundStyle backgroundStyle = BackgroundStyle::MidnightKids;
    bool beatPulseEnabled = true;          // v0.16: onset-reactive border glow, default ON in the Default recipe (subtle)
    int beatTimeSignature = 0;             // v0.26.0: auto-BPM downbeat marking meter — 0 = 4/4, 1 = 3/4, 2 = 6/8; switched from the chip at the waveform's bottom-right, drives the core BeatTracker's meter
    // v0.20.0 (item l): visualizer background mode — the pure model is
    // meterbar::Visualizer (Visualizer.hpp); these are its palette
    // controls, mirrored here like every other preset field. OFF by
    // default; when ON the window background goes black and the
    // particle layer renders behind the modules.
    bool linkEnabled = false;              // v0.20.0 (k): family linking — opt-in, default OFF
    bool linkBackground = true;            // v0.21.3: share background (style/opacity/effect) with linked bars — default ON; the visible toggle Jackson asked for
    bool visualizerEnabled = true;         // v0.28.0 recipe: ON by default (was OFF through v0.27.x)
    int visualizerStyle = 3;               // v0.28.1: UNDERTOW is the default style (was Field/2 through v0.28.0). Jackson grading v0.28.0 could not see the background layer and asked for it to "auto detect the thing" — the layer is signal-driven by design (Governor: silence -> Still), so shipping Undertow as the default IS the auto-engage; no manual mode selection. v0.21.5: 0 = Bubbles (particle engine), 1 = Lines (flowing ribbons) — Jackson's second style, chosen beside the toggle in Settings; rides the link packet as bgEffect 1 vs 3 (BLK4). v0.25.0: 2 = Field (wandering vortices), bgEffect 4 (BLK5). v0.28.0: 3 = Undertow (five-band council design; local only — over the link it presents as Bubbles via the shared 3-band broadcast). v0.28.0 recipe: Field (2) is the default style.
    // v0.28.0: Undertow grading hooks (visualizer-council DESIGN.md
    // build notes: "the prototype should expose" them so Jackson's
    // grading settles the council's open numbers). Defaults ARE the
    // committed design: both bloom scales on, pre-arm on, kick
    // release τ 65 ms.
    bool undertowBloomQuarter = true;      // quarter-res bloom scale
    bool undertowBloomEighth = true;       // eighth-res halo scale
    bool undertowPreArm = true;            // T2-5 pre-arm ceiling
    float undertowKickTauMs = 65.0f;       // 40..120 ms — the §(b) number grading may move
    float visualizerBaseHue = 0.83f;       // 0..1 (family pink)
    float visualizerSaturation = 0.85f;    // 0..1
    float visualizerSpread = 0.75f;        // 0..1 of the hue wheel per burst stroke
    float visualizerCycleSec = 24.0f;      // palette drift period; 0 = static
    bool animeClickSounds = true;          // v0.16: cute click sounds — only ever audible in the Anime theme
    float mascotFlex = 0.55f;              // v0.16: mascot cell width (relative), resizable like a module in Anime theme
    // Audio (§6)
    // v0.17 recipe: first run hears the SYSTEM'S DEFAULT OUTPUT (loopback
    // of the OS-default render endpoint — empty outputDeviceName/Id makes
    // WindowsLoopback::pickDevice return GetDefaultAudioEndpoint). The
    // test tone is a diagnostic the user picks explicitly, never the
    // first-run source.
    std::string audioSource = "System Loopback"; // "Input" | "System Loopback" | "Test Tone"
    int appSource = 0;                 // v0.27.0 (item 8): within System Loopback — 0 = System (whole endpoint mix), 1 = Spotify only, 2 = Tidal only (per-app process loopback; silence while the app is not running, never a silent fallback to system audio); v0.27.2: 3 = Spotify (Spicetify) — same Spotify.exe + resolver, explicit label; v0.28.7: 4 = Pinned app only (pinnedExe below, same process loopback + watcher attach); appended so persisted 0/1/2 keep their meaning
    std::string pinnedExe;             // v0.28.7: the pinned app's exe NAME (e.g. "Ableton Live 12 Suite.exe"); empty = no pin. Matched by name, case-insensitively (meterbar/PinSource.hpp), exactly like the Spotify/Tidal resolver. Rides presets with appSource; the live pin ALSO persists in Documents/MeterBar/pin-source.json (the perf-auto.json idiom), loaded at startup before the first applySource
    std::string audioDeviceName;           // chosen INPUT device (source = Input)
    std::string outputDeviceName;          // chosen OUTPUT device (source = System Loopback captures THIS device)
    std::string outputDeviceId;            // v0.13: WASAPI endpoint ID of the chosen output — loopback keys on THIS, name is display/fallback
    float trimDb = 27.0f;              // -48..+48 since v0.20.3 (was -24..+24; v0.17 recipe: +10.18 dB exactly; v0.28.0 recipe: +27 dB, Jackson's word 2026-11-02)
    int routingPair = 0;               // stereo pair index
    int routingCh1 = 0, routingCh2 = 1;
    // Performance (§7)
    bool vsync = true; int fpsLimit = 60; // 15/30/60/120
    int performanceMode = 2;               // v0.28.2 (APPENDED — no prior persisted meaning): 0 = Full, 1 = Low-End, 2 = Auto (starts Full, latches to Low-End after ~3 s of sustained fps sag, and a stepped-down session PERSISTS — the next launch starts stepped down; Performance.hpp). DEFAULT IS AUTO (urgent change, Jackson's field evidence 2026-11-03 ~11:28 PT: on his laptop the app is too laggy after ~10 s to even open Settings, so a mode that must be switched on FROM Settings can never rescue that machine; Auto needs no one's click. His desktop never sags, so Auto there simply stays at Full.)
    // Layout (v0.15): relative widths of the 6 modules in display order
    // (Spectrogram, Waveform, Loudness, Stereometer, Oscilloscope, Spectrum),
    // adjusted by dragging the dividers between modules. Persisted in presets.
    std::array<float, 6> moduleFlex = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
    // Shell (§1)
    bool pinned = false, stick = false, showToolbar = true, showRepositionHint = true;
    bool reserveScreenSpace = false; // v0.18: Windows AppBar reservation — while the bar is docked at a monitor's top edge, Windows shrinks that monitor's work area so maximised windows stop below the bar. Default OFF (Jackson's ask; it changes how OTHER windows behave, so it is strictly opt-in). The "Default" recipe does not set it. v0.28.9: no longer a separate control — Stick drives this field (applyStickReserveMerge keeps the pair in sync on every write and load).
    int windowX = 100, windowY = 40, windowW = 1100, windowH = 170;
};
} // namespace meterbar
