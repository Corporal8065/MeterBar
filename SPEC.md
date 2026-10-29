# Meters App — Feature Spec v0.1
Source: Jackson's dictated spec, 2026-10-13 (screenshot: MiniMeters v1.0.30 toolbar, pink-on-dark theme)
Status: APPROVED 2026-10-13 — Jackson: "approve all, go ahead", then chose C++ (JUCE) and
confirmed he does not know C++ himself. Build must therefore be code-complete from Milo's side
with a one-click Windows build/run kit; Jackson's role is running builds and reporting
screenshots / behaviour (audio + visual testing only he can do on his Windows 11 PC:
Ryzen 7 9700X, 32 GB, RTX 4070 Ti Super).
Working order = highest expected value first: de-risk audio capture + FFT pipeline first,
then modules, then shell polish. Original product is MiniMeters by Direct; this is a
functionally similar tool with our own name, logo, code, and visual identity.
DECISION (2026-10-13): Stack = C++ / JUCE (not C#). Windows-first, cross-platform structure kept.
BUILD UPDATE (2026-10-13, Milo): Working name = "MeterBar" (placeholder). Jackson's palette
theme named "Midnight Pink". Phase A (pure C++ DSP core + unit tests) BUILT AND PASSING on the
Linux VM — 39/39 checks, see BUILD_LOG.md. Phase B (JUCE app: shell, all 6 modules, settings,
presets, test tone, WASAPI loopback source) code-complete; Windows build is via build.bat
(Jackson runs it; Milo cannot run Windows builds). See README_TEST.md for his test slices.
BUILD UPDATE 2 (2026-10-13 later, Milo): Phase B now also BUILT AND LINKED on the Linux VM
(full JUCE GUI compile + 12 s Xvfb smoke run, no crash). The Linux build caught 4 real code
bugs that were in the v0.1 kit source (CMake C language, two removed JUCE APIs, missing
juce_audio_utils module) — all fixed in-tree after the kit snapshots; see BUILD_LOG.md
"REAL CODE BUGS" entry. A re-zip from the current tree is needed for Jackson to get them.

## 1. Shell / Toolbar (top bar)
- Logo (ours, TBD) + version number (e.g. v1.0.0)
- Drag handle (grip) to move window; resizable edges/corners
- Quit, Minimize buttons
- Preset dropdown: New, Save, Save As, Open Preset Folder (in Explorer/Finder)
  - Presets = JSON files in user Presets folder; store ALL module settings + theme + layout + window state
- Reposition dropdown:
  - Position: At the Top / At the Bottom
  - Stick (dock to screen edge as a bar)
  - Move to Display > (list of monitors)
  - Pin Above Other Windows (always-on-top toggle)
- Record button: start/stop recording captured audio to WAV;
  plus clip-last-10s / 60s from the in-memory ring buffer (Jackson to confirm)
- Pause button: freeze analysis/rendering (audio engine keeps state clean on resume)
- Settings button: opens the larger Settings window (per-module panels, see §4)
- Hamburger (three-bars) menu, with submenus (> = submenu):
  - Help >
  - Settings >
  - Audio Source >
  - Theme >
  - Color Map >
  - Audio Capture >
  - Show Reposition Hint (toggle)
  - Show Toolbar (toggle)
  - Show Debug Window (toggle — FPS, device, sample rate, buffer stats)
  - Check For Updates
  - Quit

## 2. Modules (all enable/disable-able; order + pop-out in later phase)
### Spectrogram
- Sections: Main / FFT / Overlay
- Orientation: Horizontal / Vertical
- Linked Speed: 0–10 (linked = shared with Waveform, see §3)
- Loop: present but disabled/greyed out
- Fade: default 0.5, range 0–1

### Waveform
- Channel 1: Left / Right / Mid / Side
- Channel 2: Off / L / R / M / S
- History: Off / Fast / Slow
- Linked Speed: same control domain as Spectrogram

### Loudness
- Mode dropdown:
  - LUFS: Momentary
  - LUFS: Short Term
  - RMS: Fast
  - RMS: Slow
  - RMS +3: Fast
  - RMS +3: Slow
- (LUFS = K-weighted loudness per ITU-R BS.1770; RMS+3 = +3 dB-scaled RMS reading)

### Stereometer
- Main + Reference track sections
- Main mode dropdown:
  - Scaled - Unipolar
  - Scaled - Bipolar
  - Linear - Unipolar
  - Linear - Bipolar
  - Lissajous
- Color: Single (S) / RGB / Multi-band (M band)
- Point Size: Auto, slider starting at 1, max 20

### Oscilloscope
- Modes: Pitch (pitch-tracked trigger) / Free
- Display: Multi (multi-band) / Single

### Spectrum
- Sections: Main / Audio / Reference / Target
- Style: FFT / Color / Bars / Both
- Smoothing: 0–100%, default 90%
- Tilt: default 4.5 dB, range -9 to +9 dB
- FFT Size: default 1024; options 2048, 4096, 8192, 16384
- Linked section — FFT Scale: Mel / Log / Linear, default Log
  (Linked = FFT size/scale shared with Spectrogram's FFT section)

## 3. Linked controls (shared state, one value drives multiple modules)
- Linked Speed: Spectrogram ↔ Waveform
- Linked FFT: Spectrum FFT size + scale ↔ Spectrogram FFT

## 4. Settings window
- Larger window opened from Settings button / menu
- Per-module panels containing every control in §2, plus module Enable/Disable toggles:
  Spectrogram, Waveform, Loudness, Stereometer, Oscilloscope, Spectrum

## 5. Themes & Color Maps
- Multiple built-in color themes (user-selectable)
- Jackson's preferred palette sampled from his 2026-10-13 screenshot:
  - Background: #1e1b24 / panels #26232e, deepest #161519
  - Accent / text pink: ~#e04c89
  - Waveform gradient: teal #40aac0 -> yellow #fad927 -> orange #fe8a0a
  (Ship under our own theme name — do not use the original product's theme names/assets.)
- Color Maps (for Spectrogram / Spectrum Color modes): Plasma, Rainbow, Inferno,
  Viridis, Magma, Turbo + custom gradient editor (later phase)

## 6. Audio Source
- Audio Input selector (device + input / system-output loopback)
- Trim: gain up/down applied pre-analysis (range TBD, suggest -24 to +24 dB)
- Channel Routing: L/R pair select, and source channel 1/2 select (for multichannel devices)

## 7. Performance
- VSync: ON by default
- FPS Limit: Max 120; options 15 / 30 / 60 / 120
- Audio thread (lock-free ring buffer) fully separate from render thread (FPS-capped)

## 8. Hard / risky parts (flagged at spec time)
1. System-output capture differs per OS (Windows WASAPI loopback = easiest; macOS = 14.2+ only, fragile)
2. Stick/dock + Move to Display + always-on-top are OS window-manager behaviours — fiddly per OS
3. Pitch-tracked oscilloscope triggering — needs stable pitch/zero-cross detection
4. Audio Server DAW plugin (send audio from DAW to standalone) — separate VST3/AU/CLAP build, Phase 4
5. Testing loop: audio/DSP can be coded anywhere, but capture/render must be tested on Jackson's Windows PC

## 9. Proposed phasing
- Phase 0: Shell — toolbar, drag/resize, presets (JSON), themes, settings window skeleton, hamburger menu
- Phase 1: Audio engine (input + Windows loopback, trim, routing) + Spectrum + Spectrogram + Waveform
- Phase 2: Loudness (LUFS/RMS) + Stereometer + Oscilloscope
- Phase 3: Record/clip, pause, reposition/stick/pin, FPS/VSync, debug window, update check
- Phase 4 (optional): macOS port, Audio Server plugin versions

## 10. Open decisions for Jackson
1. Windows-only first? (Recommended: yes)
2. Name + logo — cannot be MiniMeters or close to it if this is ever sold
3. Personal clone vs sellable product? (Determines how close UI/trade dress may sit)
4. Stack: C#/.NET + NAudio + SkiaSharp (fastest Windows v1) vs JUCE/C++ (slower start, cross-platform + plugins later)
