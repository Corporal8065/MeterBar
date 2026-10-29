# MeterBar — BUILD LOG (dated, honest: only what was actually observed)

## 2026-10-13 — Phase A: DSP core (Linux VM: g++ 13.3, cmake 3.28)
- Structure: `dsp/include/meterbar/` header library (namespace `meterbar`), ZERO external deps:
  RingBuffer (SPSC stereo, clip-10s/60s), Fft (iterative radix-2, Hann, smoothing, tilt,
  Log/Mel/Linear warp), ColorMap (Plasma/Rainbow/Inferno/Viridis/Magma/Turbo generated LUTs +
  palette gradient), Loudness (BS.1770 K-weight biquads — published 48k coefficients, re-derived
  shelf/HP for other rates; LUFS momentary 400ms / short-term 3s; RMS fast 125ms / slow 1s; RMS+3),
  Modules (WaveformHistory off/fast/slow + linked speed, SpectrogramHistory with fade model,
  Stereometer correlation/Lissajous/projections for all 5 modes, Oscilloscope autocorrelation
  pitch estimate + capture), State (PresetState covering every SPEC §2–§7 setting with SPEC defaults).
- First test run: 39 checks, 3 failures — all diagnosed and fixed:
  (1) FFT normalisation was half the correct factor (peak read −12.67 dB for a −6 dBFS sine);
      fixed to 4/N Hann scaling.
  (2) RMS slow test fed only 1 s into a 1 s ballistic (read −5.00); test now settles 6 s. Code was right.
  (3) Log-scale expectation in the test itself was wrong (0.666 vs true 0.566). Test fixed.
- FINAL (observed): `cmake -S . -B build && cmake --build build && ./build/test_dsp` →
  **39 checks, 0 failures, exit 0.** Evidence lines:
  FFT peak bin=85 (exact, 1 kHz @ 4096/48k), −6.65 dB (off-bin scallop, expected);
  RMS slow full-scale sine = −3.021 dBFS (theory −3.01); RMS+3 delta exactly +3.000;
  LUFS momentary = short-term = −22.97 for a −23 dBFS 997 Hz stereo sine;
  correlation mono = +1.000, anti-phase = −1.000; scope pitch 220.2 Hz on a 220 Hz sine;
  ring clip: last-1s extract frame-exact.

## 2026-10-13 — Phase B: JUCE app (code written; build status below)
- Files: app/CMakeLists.txt (juce_add_gui_app), Source/Main.cpp (borderless window, no title bar),
  MainComponent (toolbar per SPEC §1: drawn logo mark + "MeterBar" + v1.0.0, :::: grip drag via
  ComponentDragger, Preset/Reposition/Settings/Menu/Min/Quit, Rec + Pause; hamburger menu with
  Help/Settings/Audio Source/Theme/Color Map/Audio Capture submenus + Show Reposition Hint /
  Show Toolbar / Show Debug Window / Check For Updates / Quit), ModuleViews (all 6 renderers),
  Settings (Settings window, per-module panels + enable toggles + Audio + Performance panels),
  AudioEngine (JUCE device input with routing pair + ch1/ch2, Test Tone generator thread,
  loudness fan-out, WAV recording via ThreadedWriter, clip 10/60 s export),
  WindowsLoopback (real WASAPI loopback under #if JUCE_WINDOWS; stub elsewhere),
  PresetIO (JSON presets via JUCE JSON in Documents/MeterBar/Presets).
- FPS limit: UI Timer at 15/30/60/120. VSync setting stored in presets; JUCE's software
  renderer exposes no vsync switch — fps limit is the effective control (noted in README_TEST).
- Linux GUI build attempt: see next entry (JUCE configure was still running at log time).
- WINDOWS STATUS: **untested.** Nothing in Phase B has been compiled for Windows yet.
  Jackson's first action is build.bat (see README_TEST.md, Build section).

## 2026-10-13 (later) — v0.1 kit on Windows + Linux GUI verification + REAL BUGS FOUND
- v0.1 kit result (reported via parent): Jackson ran build.bat on Windows 11 and the CMD
  window flashed and vanished. Root cause was the .bat itself (LF-only line endings +
  non-ASCII em-dashes break cmd.exe IF-block parsing), NOT the app code. Parent rewrote
  build.bat (pure ASCII, CRLF, VS2022-bundled CMake discovery via vswhere, build-log.txt,
  guaranteed pause) and shipped v0.2. Milo did not touch build.bat (on-disk copy is the
  parent's CRLF version, verified).
- Linux GUI build verification (this VM) — final outcome: **FULL BUILD SUCCEEDS.**
  `cmake -DMETERBAR_BUILD_APP=ON` + `cmake --build build-app` produced
  `build-app/app/MeterBar_artefacts/Release/MeterBar` (ELF x86-64, 11.9 MB).
  Smoke run under Xvfb: app stayed in its message loop for the full 12 s test window
  (killed by timeout, exit 124 — i.e. no startup crash; ALSA warnings are just the VM
  having no sound hardware). test_dsp in the app tree: **39 checks, 0 failures.**
- One VM-only blocker first: configure died in juceaide with
  `fatal error: X11/extensions/Xrandr.h: No such file or directory` — missing dev packages
  on this VM only; fixed with apt (libxrandr-dev, libxinerama-dev, libxext-dev,
  libxcursor-dev). Not a code bug; Windows unaffected.
- **REAL CODE BUGS the Linux build caught — these were in the source as snapshotted for
  the v0.1 kit (and in v0.2 unless its snapshot post-dates the fixes). All four would
  break the Windows build the same way. Fixed in the tree on 2026-10-13, AFTER the kit
  snapshots — they need a re-zip (v0.3) to reach Jackson:**
  1. Root `CMakeLists.txt`: `project(... LANGUAGES CXX)` only — CMake generate failed
     with "Missing variable is: CMAKE_C_COMPILE_OBJECT" (JUCE targets contain C sources).
     Fix: `LANGUAGES C CXX`.
  2. `MainComponent.cpp`: called `juce::DocumentWindow::minimiseWindow()`, which does not
     exist in JUCE 8. Fix: minimise via `getPeer()->setMinimised(true)`.
  3. `MainComponent.cpp`: used removed API `juce::ModalComponentManager::CallbackFunction::create`.
     Fix: `juce::ModalCallbackFunction::create`.
  4. `MainComponent.cpp`: used `juce::AudioDeviceSelectorComponent` without the module —
     "expected type-specifier" at the `new` expression. Fix: link `juce::juce_audio_utils`
     in app/CMakeLists.txt AND `#include <juce_audio_utils/juce_audio_utils.h>` in MainComponent.h.
- After fixes 1–4: configure clean (11 s), full build clean, no errors.
- WARNING for future kit zips: exclude `build/` and `build-app/` (~390 MB: Linux JUCE
  clone + ELF binary) — source-only zips, build.bat recreates everything on Windows.
- WINDOWS STATUS (unchanged): the GUI has still never been compiled/run on Windows by
  anyone. First Windows run of the FIXED tree is the next milestone.

## Known limits / follow-ups (v1)
- WASAPI loopback: first stereo pair only; no 16-bit-extensible subformat detection beyond float/int16.
- Spectrum view x-mapping uses a power warp approximation (log ≈ ^2.2) rather than exact
  per-pixel inverse warp; DSP core freqToX is exact and tested — view can adopt it later.
- Spectrogram renders by sampling cells from history (CPU cost scales with window size);
  a juce::Image cache would be the optimisation if Jackson reports lag.
- Oscilloscope "Multi" draws the scope plus a faint raw trace (true 3-band split is a follow-up).
- Stereometer "Reference" panel and spectrum Reference curve are reserved/toggle-only in v1.
- Check For Updates is informational only (no update server exists).

## 2026-10-13 — Kit v0.5: git-free JUCE download (Windows evidence)
- Jackson's first real Windows run (build-log-2.txt, 12:21 PT): VS2022 Community found via
  vswhere, bundled CMake + MSVC 19.44 + SDK 10.0.26100 all OK; configure died ONLY on
  "could not find git for clone of juce-populate" — his PC has no git.
- Fix: FetchContent switched from GIT_REPOSITORY to URL (github tag archive zip, no hash pin).
- Verified on this VM in a fresh build dir: configure exit 0, juce-src populated from the zip,
  juceaide built, "Configuring done". No git involved anywhere in the chain now.

## 2026-10-13 — Kit v0.6: Windows BUILD SUCCEEDED (app), test-only fix
- Jackson's Windows run #2 (build-log-3.txt, 12:30 PT): configure DONE (124.9s, zip JUCE),
  MSBuild compiled everything, final line: MeterBar.vcxproj -> ...\Release\MeterBar.exe.
  THE APP BUILDS ON WINDOWS. Only failure: tests/test_dsp.cpp C2065 'M_PI' undeclared x5 —
  MSVC does not define M_PI without _USE_MATH_DEFINES (g++ does). App code already had a guard.
- Fix: _USE_MATH_DEFINES + #ifndef M_PI fallback in test_dsp.cpp. Linux retest: 39/0.

## 2026-10-13 — Kit v0.7: field-test fixes from Jackson's first Windows run
Field report (12:43 PT): exe works, mic reaches Spectrum, pause/resume OK. Four issues:
1. Oscilloscope trace in first ~10% of width — CONFIRMED bug: pitch capture wrote ~1 period
   then zero-padded 2048. Fix: tile the zero-cross-aligned segment across the full buffer.
2. Reposition-to-top / Stick lock the window, buttons dead — moves now deferred via
   MessageManager::callAsync out of the popup-menu callback; grip drag rewritten manual
   (ComponentDragger removed); resizable corner grabber disabled (border resize only),
   which also addresses the resize cursor-grab report.
3. Waveform looked dead on mic audio — it was linear-scaled (mic peaks ~0.06 = 1-2 px)
   while spectrum/spectrogram/stereometer are dB-scaled or normalized. Renderer now sqrt.
4. Loudness: engine value + peak added to the Debug window to split engine vs display
   if it still looks flat next round.
Verified here: full Linux rebuild clean, DSP 39/0. Windows re-test = Jackson's next run.

## 2026-10-13 — Kit v0.8: Jackson's three feature asks
1. Whole top bar drags the window; grip ("::::") button removed. Labels no longer
   intercept clicks so drags can start on the logo/version/hint too. Buttons unaffected.
2. Settings window content now sits in a Viewport with a vertical scrollbar —
   all panels reachable regardless of window height.
3. Real device pickers: Settings > Audio Source has Input Device + Output Device
   dropdowns listing every device the OS reports (current type scanned at open);
   Menu > Audio Source has matching Input/Output Device submenus. Selecting applies
   AudioDeviceSetup immediately and re-runs applySource; input choice persists in
   the preset's audioDeviceName.
Verified here: Linux rebuild clean, DSP 39/0, Xvfb smoke alive.

## 2026-10-13 — Kit v0.9: UNIFIED sound source (Jackson's correction)
Jackson clarified the model: ONE picker for the program's ears, grouped
INPUT DEVICES / OUTPUT DEVICES — picking any device makes it THE source.
Mic picked = meters hear the mic; speakers picked = meters hear what's playing
through those speakers. v0.8's separate in/out dropdowns did NOT do this (the
output dropdown only set JUCE's unused playback device; loopback was default-only).
Changes:
- Settings > Audio Source: single "Sound Source" ComboBox with section headings
  (Test Tone / INPUT DEVICES / OUTPUT DEVICES), same list in Menu > Audio Source.
- WindowsLoopback: start(cb, deviceName) — enumerates render endpoints and
  activates loopback on the one whose friendly name matches (exact/substring),
  default endpoint only as fallback or when name is empty.
- state.outputDeviceName added + persisted in presets; applySource Input branch
  now honors a saved input device name.
- setDeviceByName unified: input pick sets source=Input; output pick sets
  source=System Loopback on that device.
Verified here: Linux rebuild clean, DSP 39/0, smoke alive. WASAPI per-device path
is Windows-only code — Jackson's swap test (mic vs speakers) is the real check.

## 2026-10-13 — Kit v0.10: loopback decode, record crash, colors, icon
Field report (13:45 PT): speakers (Realtek) loopback silent; test tone + laptop
input fine; record->stop crashes; spectrogram faint blue; wants MiniMeters-style
color behaviour + an exe icon.
1. Loopback decode bug: mix formats were classified "float if extensible+32-bit" —
   Realtek reports PCM in a 32-bit extensible container, decoding to near-silence.
   Now: SubFormat GUID decides float vs PCM; PCM 16/24/32 decoded properly.
   Debug window now shows the activated loopback endpoint name + packet count.
2. Record-stop crash: audio thread could write into the ThreadedWriter while the
   UI freed it. recording_ is atomic; teardown under recLock held during writes.
3. Colors: spectrogram gets a dedicated heat ramp (deep red low -> orange ->
   yellow -> white, gamma-lifted, highs tinted hotter, fully opaque); waveform
   colour now follows the sqrt amplitude curve; scope trace is multicolour
   segments coloured by amplitude; spectrum level dots are colormap-coloured in
   every style.
4. Icon: 5-bar logo (teal/green/yellow/orange/pink on dark, pink edge) generated
   as icon_big/icon_small PNG + multi-size meterbar.ico; wired via ICON_BIG/SMALL.
Verified here: Linux rebuild clean, DSP 39/0, smoke alive. Windows-only paths
(loopback decode, record/stop race) need Jackson's re-test.

## 2026-10-13 — Kit v0.11: recorder rewrite, native drag, loopback diagnostics
Field report (14:04 PT) on v0.10: record->stop STILL froze then crashed; window
drag grabbed the mouse again; Realtek speakers loopback still silent.
- Recorder REWRITTEN: ThreadedWriter/TimeSliceThread design deleted. A dedicated
  RecorderThread owns the WAV writer end-to-end (writes + closes on its own
  thread after draining a bounded queue). Audio path only copies into the queue.
  Stop = flag off, signal, join(4s). No shared object can be freed under a writer.
- Drag: Windows now uses the OS native move loop (ReleaseCapture +
  WM_NCLBUTTONDOWN/HTCAPTION on the peer HWND); hand-rolled drag remains only
  as the non-Windows fallback. Spectrogram also renders via a half-res cached
  image (one blit) to cut UI-thread load during moves.
- Loopback diagnostics extended: Debug window now shows activated endpoint,
  packet count, decoded format label (F32/PCM16/PCM24/PCM32) and the peak
  sample the capture thread has decoded. peak>0 with flat meters = downstream;
  peak=0 = capture/decode at the endpoint. Jackson to send that screenshot.
Verified here: Linux rebuild clean, DSP 39/0, smoke alive.

## 2026-10-28 — Kit v0.12: bug-finish audit + UI colour pass
Jackson's ask: "finish all the bugs for the MiniMeters software + make the UI
more pleasing and colourful" (he likes the "midnight kids" palette). Full
source audit of the three v0.10/v0.11 field bugs + a theme pass. Below,
"fixed (code-proven)" = the flaw is demonstrable from the source and the fix
removes it; "needs Jackson's re-test" = Windows behaviour only his PC shows.

### (a) Record -> stop — FIXED (code-proven), 2 flaws + 1 coherence fix
1. **Use-after-free window on recorder_ (the v0.11 hole).** feedRecorder()
   (runs on the audio/loopback/test-tone threads) read the recorder_
   unique_ptr and called push() with NO synchronisation, while the UI thread
   could reset() it in stopRecording() — a check-then-use race exactly on the
   record->stop path. Fix: the pointer hand-off now happens under a dedicated
   recPtrLock (held only for the short push; lock order recPtrLock ->
   recorder queue lock, never reversed); stopRecording() moves the thread
   out under the lock BEFORE signalling/joining, so feeders see null and skip.
2. **Join-timeout destruction.** stopThread(4000)'s return was ignored: if
   the writer thread ever outlived the timeout, reset() would destroy the
   object (writer, queue lock, queue) under its still-running run() loop.
   Fix: on timeout the small thread object is intentionally LEAKED instead
   (it exits alone once its bounded <=400-block queue drains). The drain is
   bounded local disk writes, so this path should never trigger — but it can
   no longer crash if it does.
3. **Wrong-rate WAV on source switch.** The writer's sample-rate header is
   fixed at record start; switching source mid-record kept writing the new
   source into the old-rate file. Fix: applySource() now ends any active
   recording first, and the Rec button label/colour re-syncs from the engine
   every UI frame (it can no longer claim "Stop" for a finished take).
   Also re-checked clean: double-stop (null-safe), stop-while-starting
   (start publishes under the lock before recording_ flips true),
   quit-while-recording (destructor -> shutdownRecorder -> same path).

### (b) Window drag grabbing — audit: no hand-rolled grab path left on Windows
The Windows move is fully OS-owned since v0.11 (ReleaseCapture +
WM_NCLBUTTONDOWN/HTCAPTION); the corner grabber has been disabled since v0.7
(border resize is JUCE-stock and ends on mouse-up); module views and toolbar
labels don't intercept clicks; Reposition moves are deferred out of the menu
callback since v0.7. Remaining fixes made anyway:
- **Right/middle-button drag (code-proven flaw):** mouseDown started a move
  for ANY mouse button. Now left-button only (all platforms, incl. the
  non-Windows fallback drag, which is the only hand-rolled drag left).
- Stale hint text ("drag the grip" — the grip was removed in v0.8) fixed.
NEEDS JACKSON'S RE-TEST: confirm drag + edge-resize never keep the mouse.

### (c) Realtek loopback silence — root cause STILL UNCONFIRMED (no Debug
screenshot ever arrived); code hardened at every fragile point found:
- **No HRESULT was checked anywhere (code-proven).** A failed Activate left
  a null client that GetMixFormat then dereferenced; a failed Start left the
  packet loop spinning forever on a dead client = "silent" with no evidence.
  Now every stage (Activate/GetMixFormat/Initialize/GetService/Start and the
  packet-loop calls) is checked, and the failing stage + HRESULT lands in a
  new Debug line: "Loopback status".
- **ReleaseBuffer after failed GetBuffer (code-proven API violation)** —
  only legal after a successful GetBuffer; AUDCLNT_E_DEVICE_INVALIDATED now
  ends the loop with status "device lost (GetBuffer)" instead of spinning.
- **Format stride from wBitsPerSample (fragile).** For extensible formats
  wBitsPerSample is the CONTAINER size; stride now comes from nBlockAlign/
  nChannels (authoritative), decode uses container width with valid bits
  left-justified per the extensible spec, and the format label says so:
  e.g. "PCM24in32 ch2 @48000" (was just "PCM24").
- **Endpoint pick could take the wrong twin.** First-hit substring match
  (v0.9) could grab the wrong "Speakers (...)" twin; now an exact-name pass
  over all endpoints runs before any substring match.
- **Diagnostic string race (code-proven).** activeName/fmtLabel were plain
  std::strings written by the capture thread, read by the UI thread. Now
  mutex-guarded; sample rate is atomic; the peak max uses a CAS loop.
NEEDS JACKSON'S RE-TEST: Realtek speakers loopback + the Debug screenshot
(device / format / status / peak decoded) if it is still silent.

### Extra latent bug found by the sweep — FIXED (code-proven)
- **Settings "Enabled" checkboxes always showed ON.** panel() passed the
  bool* POINTER to setToggleState(bool) — a non-null pointer converts to
  true, so a disabled module showed enabled (and a click then wrote the
  wrong state). Now dereferenced. Jackson: worth one glance — disable a
  module, reopen Settings, the box should now show unticked.
- Spectrum view now uses the DSP core's exact freqToX mapping (tested)
  instead of the ^2.2 power approximation the v1 notes flagged — Log/Mel/
  Linear scales all honoured, peaks land where the scale says.

### UI pass ("Midnight Kids", Jackson's palette)
- paletteGradient extended 3 -> 5 stops: teal -> green -> yellow -> orange
  -> pink (the icon/logo family). This ONE ramp is the level language in
  waveform, scope, stereo bands and now the loudness bar fill.
- New default theme "Midnight Kids" (deep violet-black bg #17141F, panel
  #231F31, warm text, pink brand accent + teal secondary); "Midnight Pink"
  kept selectable (State default + the DSP default-check updated to match —
  deliberate product change, Jackson's direction; saved presets keep their
  own theme). Theme struct gained border/grid colours + per-module accents:
  Spectrum pink, Spectrogram orange, Waveform teal, Loudness green,
  Stereometer violet, Oscilloscope yellow.
- Module chrome: rounded soft borders, header = accent dot + accent small-
  caps title; Settings panel headers wear the same module accents;
  spectrum gets faint -30/-60 dB gridlines and its trace in module pink;
  loudness bar fills with the palette gradient + a bright peak tick (red
  when the peak is within 1 dB of full scale); stereo "Single" colour is now
  the violet module accent; scope Multi overlay uses the teal secondary.
- Toolbar: 2px palette-gradient strip under the bar; logo mark is now the
  icon's 5 bars; Rec turns red while recording; Settings/Debug window
  backgrounds follow the theme. NO layout or behaviour changes beyond this.

Verified here: Linux rebuild clean (fresh ELF, only pre-existing JUCE Font
deprecation warnings), DSP 39/0 in both build trees, Xvfb smoke alive 14 s.
VM note: the X11 -dev packages had vanished from this VM again (same blocker
as 2026-10-13) — restored from the Ubuntu pool .debs (libxrandr/xinerama/
xcursor/xfixes -dev); VM-only, Windows unaffected.
WINDOWS STATUS: v0.12 needs Jackson's re-test — README_TEST.md leads with
the three re-test slices (record->stop x3 + source-switch, drag/resize +
right-click, Realtek loopback + Debug screenshot).

## 2026-10-28 — Kit v0.12.1: Windows compile fix (NOMINMAX)
Jackson's first v0.12 Windows run (build-log-4.txt, 10:58 PT) split the
world cleanly: the VS C++ workload install is GOOD (vswhere found VS2022
Community, bundled CMake ran, configure + juceaide + test_dsp all built) —
then the MeterBar target died in MainComponent.cpp at lines 182/192/198/203:
error C2589 "'(' : illegal token on right side of '::'" (plus cascading
C2059/C2660 from the mangled lines).
- **Root cause:** the classic windows.h min/max MACRO collision. JUCE pulls
  in windows.h, which (without NOMINMAX) defines min and max as macros;
  the preprocessor eats the `(` of every std::min/std::max call, leaving
  `std:: <macro-expanded junk>` -> C2589. The flagged lines are exactly
  std::max/std::min calls (fft need/start, waveform pushBlock offsets,
  stereometer count). My Linux/GCC verification build CANNOT see this bug
  class — no windows.h, no macros — which is how v0.12 shipped with it.
- **The fix (one line, global):** NOMINMAX added to the MeterBar target's
  target_compile_definitions in app/CMakeLists.txt. Per-call-site fixes
  would be whack-a-mole: grep shows std::min/std::max/std::clamp across
  MainComponent.cpp, ModuleViews.h, WindowsLoopback.cpp and the dsp
  headers (ColorMap/Fft/RingBuffer/Loudness/Modules) as included by the
  app — the target-level define covers every one of those TUs at once.
- **Trap sweep (same bug class, before shipping):** NO bare min(/max(
  calls, NO identifiers named near/far, NO identifier named ERROR, NO
  numeric_limits::max/min (the other classic C2589 trigger), NO other
  common windows.h macro victims (CreateFile/GetCurrentTime/DrawText/
  GetMessage/small/interface — only the word "small" in comments).
  NOMINMAX is the only fix this codebase needs.
- **test_dsp untouched on purpose:** it compiled fine in Jackson's own
  Windows log (pure DSP, includes no windows.h/JUCE) — no churn.
- Verified here: Linux rebuild clean — app objects recompiled with the
  new define (flags.make carries NOMINMAX; MainComponent.cpp.o rebuilt)
  and a fresh ELF linked (one gmake clock-skew hiccup initially left the
  old binary in place; forced an explicit target relink and mtime-verified
  the new binary). DSP 39/0 in both build trees. Xvfb smoke alive 14 s.
- **Honest split:** none of that proves MSVC. The MSVC proof is Jackson
  re-running build.bat from the v0.12.1 kit — if MainComponent.cpp now
  compiles past line 182, the diagnosis and fix are confirmed on the only
  machine that can confirm them. The three v0.12 field re-tests
  (record->stop, drag, loopback) still ride on that same run.

## 2026-10-28 — Kit v0.13: headphone loopback fix + transparency + spectrum colours
Jackson's v0.12.1 field report: the NOMINMAX fix WORKED (v0.12.1 compiled and
ran on his PC — the v0.12 re-tests for record/drag are still unreported). New
report: Settings sound source = his headphones, the "Realtek HD Audio 2nd
output" render endpoint — meters silent, other sources fine. Plus two asks:
transparent section backgrounds (with blur if possible) and a spectrum whose
colours follow level ("the higher it is the redder it is") / frequency.

### (a) Loopback on a non-default endpoint — FIXED by construction + diagnosed
Root cause (code-level, most probable): capture was keyed on TEXT. The picker
names came from JUCE's device-type list (whose names are NOT the WASAPI
endpoint friendly names — a DirectSound-style description like "Realtek HD
Audio 2nd output" matches no endpoint friendly name), and WindowsLoopback
re-matched that text against endpoints by exact/substring name. On no match
it SILENTLY fell back to the DEFAULT endpoint — so picking the 2nd-output
headphones captured the default speakers instead: silent meters for exactly
that source, everything else working. The v0.12 Debug lines could not prove
it (no wanted-name/ID was shown), and no Debug screenshot ever arrived, so
the fix removes the fragile step instead of guessing further:
- NEW `WindowsLoopback::enumerateRenderEndpoints()` (Windows): the real
  ACTIVE render endpoints (friendly name + MMDevice endpoint ID).
- The Settings picker AND the hamburger menu now build their OUTPUT lists
  from that enumeration; a pick stores `outputDeviceId` in state/presets.
- Capture activates by ID first (`IMMDeviceEnumerator::GetDevice`) — no
  name matching on the critical path. Name pick remains as fallback for
  old presets (no ID stored), default-endpoint only as last resort.
- Debug window is now decisive either way: "Loopback wanted" (the pick),
  "Loopback device (via endpoint id / exact name / partial name /
  default (NO MATCH for chosen name))", "Loopback id", plus the existing
  format/status/packets/peak lines. Window widened + font shrunk so the
  long endpoint IDs stay on one line.
- Regression surface held: input path, Test Tone, recorder, the v0.12
  HRESULT/stride/exact-name hardening all untouched. applySource already
  re-opened capture per switch; it now re-opens it onto the right endpoint.

### (b) Transparency — translucency YES, blur NO (stated plainly)
- New Appearance setting: Background Opacity (15–100%, default 100% =
  unchanged). The top-level window is now non-opaque with a fully clear
  background, and every BACKGROUND fill (window bg, toolbar strip base,
  each module's panel, the loudness bar track, the stereo plot square)
  paints with that alpha — the desktop shows through the sections while
  traces, bars and text stay solid.
- NO backdrop blur: a real DWM acrylic/blur-behind needs platform hacks
  outside JUCE's window model; faking it was not an option. README says so
  and offers it as a separate attempt if Jackson wants it.
- Settings/Debug windows stay opaque on purpose (utility windows).

### (c) Spectrum colours — level heat is the new default
- DSP core gains `spectrumColor(mode, level01, freq01)` + `spectrumHeat()`:
  Level mode (DEFAULT) maps bin level through violet -> teal -> green ->
  yellow -> orange -> hot RED (every stop from the midnight-kids family
  except the deliberate true-red top). Frequency mode = the palette
  gradient across the axis (teal lows -> pink highs). Blend = the average.
  ColorMap mode preserves the classic selected-ColorMap behaviour exactly.
- Settings > Spectrum gains "Colour By" (4 modes); bars and FFT dots use
  it; the pink accent trace still strokes on top in Fft/Both styles.
- DSP suite extended: 39 -> 46 checks (heat endpoints, frequency gradient
  direction, blend-between-pures, + the three new state defaults).

### Verification (this VM, observed on disk — not by status report)
- DSP: **46 checks, 0 failures** in BOTH build trees.
- Full clean app rebuild: fresh configure + build, exit 0, fresh ELF
  (11,947,720 bytes). NOMINMAX confirmed in the app target's flags.make.
- **Windows-branch compiler coverage (NEW):** the `#if JUCE_WINDOWS` half
  of WindowsLoopback.cpp — all the COM code incl. the new enumeration /
  GetDevice / GetId paths — passed a full mingw-w64 syntax+type check
  (-fsyntax-only -DJUCE_WINDOWS=1), exit 0. MinGW's headers are not MSVC's,
  but this is the first time the Windows-only code has met ANY compiler
  before Jackson's build; v0.12 shipped that class of code unchecked and
  v0.12.1 was the price. MSVC trap sweep also re-run: clean (no bare
  min/max, no numeric_limits extremes, no macro-victim identifiers).
- Xvfb smoke: alive the full 14 s, no errors (ALSA warnings = VM has no
  sound hardware, expected).
- VM environment note (cost ~40 min, VM-only, Windows unaffected): this
  VM's non-home filesystem reverted mid-session — apt's package lists and
  the X11 -dev packages installed minutes earlier vanished, and one stale
  background delivery falsely claimed "BUILD SUCCEEDED" for a tree that
  did not exist. Caught by verifying artifacts on disk; the successful
  build ran as ONE atomic chain (apt update + install + build) so no
  revert window could open between install and compile.

### What only Jackson's Windows run can prove
- The headphones/2nd-output loopback actually capturing (COM behaviour on
  his Realtek driver). If still silent, the new Debug screenshot settles
  it in one look: wanted vs device + via + id + status + peak.
- The transparent window compositing under DWM (JUCE layered-window path)
  and the exact look of the level-heat spectrum.
README_TEST.md leads with these three re-tests (A loopback, B opacity,
C colour), with the v0.12 record/drag re-tests kept as D/E (still open).

## 2026-10-28 — Kit v0.14: THE loopback root cause (compiled out since birth) + full-spectrum colour
Jackson's v0.13 field screenshot was decisive and it acquitted the v0.13
fix while convicting something older. He IS on v0.13 (Settings shows the
Background Opacity slider + Colour By: Level; Sound Source = "Realtek HD
Audio 2nd output (Realtek(R) Audio)"). But the Debug window shows:
Device = Microphone (USB Advanced Audio Device), Source = System Loopback,
**Loopback wanted EMPTY, Loopback device "(via )" EMPTY, Loopback id
EMPTY, packets 0, format empty, peak decoded 0.000, status empty** — while
buffers/ring advance on a steady 220.2 Hz trace at peak 0.674 (the startup
test tone's fingerprint: 445184 ring frames / 1739 buffers = exactly
256.0 frames per buffer, and 0.674 ≈ the tone's 0.675 theoretical max —
the meters were frozen on the last test-tone frame, not hearing the mic).

### (a) ROOT CAUSE — the WASAPI implementation was never in the binary
`WindowsLoopback.cpp` guards its entire real implementation behind
`#if JUCE_WINDOWS`, but that TU (and its header) deliberately includes NO
JUCE header — it is std-only. `JUCE_WINDOWS` is defined ONLY inside JUCE's
own headers (juce_TargetPlatform.h) or as a command-line -D; the app
build defines neither for this TU (verified: flags.make CXX_DEFINES has
JUCE_MODULE_AVAILABLE_* / NOMINMAX / etc., no JUCE_WINDOWS). An undefined
macro in `#if` is 0, so **every build — MSVC on Windows included — has
compiled the stub branch since the guard was introduced**: start()
returned false and stored nothing, enumerateRenderEndpoints() returned
{}, every diagnostic returned empty/0. Every field observation fits:
the picker fell back to JUCE device-type names (hence the picked name
is JUCE's string, endpoint ID empty), applySource DID run on the pick
(state was written, tone stopped) but the stub start() did nothing, and
loopback has been "silent" in EVERY field test (v0.10, v0.11, v0.12,
v0.13) because it has never existed in a shipped binary. The v0.10–v0.13
hardening (HRESULT checks, SubFormat decode, endpoint-ID activation)
was all real code — inside the dead branch. And v0.13's MinGW check
**forced -DJUCE_WINDOWS=1**, type-checking code the real build never
compiles: a false verification, recorded here so it is never repeated.
PROOF (this VM, Windows-target compiler, NO -D flags — the real build's
exact conditions): preprocessing the SHIPPED v0.13 WindowsLoopback.cpp
yields **0** occurrences of CoCreateInstance and 0 of the real branch's
"NO MATCH" string; the fixed file yields 6 and 1. The other
`#if JUCE_WINDOWS` sites (Settings.h, MainComponent.cpp) were audited
and are SAFE — those TUs include JUCE headers before the guard.

### (b) The fix
- The guard now tests the compiler's own predefined target macro:
  `#if defined(_WIN32)` (MSVC and MinGW both predefine _WIN32; no header
  needed), via a named METERBAR_LOOPBACK_WINDOWS define with the whole
  story in a comment at the site so the trap is visible to the next editor.
- One more latent break in the same path, fixed while it was open:
  enumerateRenderEndpoints() counted CoInitializeEx's S_FALSE ("thread
  already has an apartment" — the JUCE message thread does) as didInit
  and CoUninitialized it, unbalancing the message thread's COM count on
  every picker build. Now only S_OK owes a CoUninitialize. (This code
  had also never run on Windows before.)
- Debug truthfulness: statusInfo() with no capture returns "not
  started" instead of ""; the non-Windows stub returns "loopback
  unavailable (non-Windows build)"; and MainComponent's Debug status
  line is prefixed **"NOT CAPTURING - "** whenever the source is System
  Loopback but the capture thread is not in its running state — the
  Source label can no longer imply live loopback over a dead capture.
- Regression surface held: the pick path (Settings onChange -> state
  {audioSource, outputDeviceName, outputDeviceId} -> applySource ->
  loopback.start(name, id)), the menu path (setDeviceByName resolves
  the ID from the enumeration), presets (outputDeviceId round-trips),
  input path, recorder, test tone — all untouched. With the guard fixed,
  enumeration returns the real endpoints, so the v0.13 ID-keyed
  activation finally executes as designed; no restart is needed on a
  pick (applySource already re-opens capture per switch).

### (c) Full-spectrum colour in all four trace modules (Jackson's ask)
One level language everywhere: the DSP `spectrumHeat` ramp (violet ->
teal -> green -> yellow -> orange -> hot red), midnight-kids family.
- **Spectrum Level mode range fixed (DSP `spectrumLevelHeatT`):** the
  v0.13 view fed the heat ramp a LINEAR map of the -90..0 dB window, so
  programme bins (top of the window, tilt included) landed at 0.85–1.0
  and a loud tone rendered almost entirely pink/red (his screenshot).
  The level now goes through a 1.35 power curve: -60 dB -> 0.23 (teal),
  -30 dB -> 0.58 (green/yellow), -12 dB -> 0.82 (orange), true red only
  near 0 dB. ColorMap (classic) mode keeps the classic linear level.
- **Spectrogram:** the private maroon-red ramp (deep red -> orange ->
  white, in ModuleViews since 2026-10-13) is replaced by DSP
  `spectrogramHeat` — the same full palette with the low end pulled
  toward black so silence stays dark; the cell colour is level-pure
  (the old 0.55-gamma + frequency hot-tint removed).
- **Waveform:** per-column colour is now spectrumHeat(sqrt peak)
  instead of the palette gradient — loud columns climb to true red.
- **Oscilloscope:** segment colour is now spectrumHeat(amp^0.8)
  instead of the palette gradient — loud passages visibly redden.
- DSP suite extended 46 -> **56 checks** (level-curve anchors +
  monotonicity, spectrogram darkness-at-silence / green mid / red top).

### Verification (this VM, observed on disk)
- DSP: **56 checks, 0 failures in BOTH build trees.**
- Full clean app rebuild: fresh configure (437 s) + build exit 0, fresh
  ELF (11,947,592 bytes); NOMINMAX confirmed in the new tree's flags.
- **Corrected Windows-branch coverage:** MinGW full syntax+type check
  of WindowsLoopback.cpp passes **WITHOUT -DJUCE_WINDOWS** (the macro
  is not forced anywhere this time), plus the A/B preprocess proof in
  (a) above. MSVC trap sweep on all changed files: clean.
- Xvfb smoke: alive the full 14 s, no errors.

### What only Jackson's Windows run can prove
- The Realtek 2nd-output loopback actually capturing — the v0.10–v0.13
  WASAPI code (HRESULT checks, format decode, ID activation) runs for
  the FIRST time in this build; any runtime behaviour of his Realtek
  driver is unproven until he runs it. README_TEST leads with it:
  pick the 2nd output, play audio through it, meters must move AND the
  Debug screenshot must show Loopback wanted/id filled, (via endpoint
  id), packets > 0, status "running". If anything in the chain fails,
  the Debug status now names the failing stage (or NOT CAPTURING).
- The exact look of the four colour passes under real programme material.
README_TEST.md's v0.14 section leads with the loopback re-test, then the
colour pass; v0.12 record/drag re-tests remain open below it.

## 2026-10-28 — Kit v0.15: Jackson's six field fixes + colour-map bug + distribution
v0.14 field test PASSED (capture works for the first time; mic live, all
modules responding to real music). Jackson's six feedback items, plus his
colour-map bug report (added as item 7, a defect), plus a distribution
package so he can give the app to friends as a one-click install.

### 1. Spectrogram sensitivity — its OWN mapping (DSP spectrogramLevelT)
At Jackson's working trim the spectrogram never reached red. Cause: one
spectrogram cell is one FFT bin; strong music bins sit at -30..-12 dB,
while the view mapped cells linearly over -90..-10 dB, so programme cells
landed mid-palette. Fix: spectrogramLevelT() — dedicated window floor
-75 dB / ceiling -8 dB, 0.85 gamma. Anchors (measured in the suite):
-12 dB -> t 0.949, spectrogramHeat = red (r>200, r>g); -30 dB -> 0.713
(yellow); -70 dB -> 0.110 and rendered <40 RGB (dark). No other module's
gain or mapping touched; the full-palette look is kept.

### 2. Stereometer — conventional stereo vectorscope
The old mode projections (Scaled/Linear x Unipolar/Bipolar) read like a
surround display. drawStereo now plots a Lissajous L-vs-R vectorscope
(Ozone-Imager style) from the stored Mid/Side via the new DSP helper
StereometerEngine::lissajousPoint(m, s) = {x = R, y = L}: mono draws the
main diagonal, anti-phase the opposite one (both guides drawn faintly).
Correlation + balance readouts kept (the DSP computation was untouched);
the Settings "Main Mode" combo is removed (the projections are retired
from the view; StereoMode/project() remain in the DSP core + tests).

### 3. Oscilloscope window control (scopeWindowMs, default 100 ms)
Was a fixed 2048-sample capture (~43 ms) — Jackson: "I only see a very
small amount of it". New state scopeWindowMs (10..200 ms slider in
Settings > Oscilloscope); MainComponent sizes the capture from it
(256..16384 samples, re-prepares the engine only when it changes) and
captures from the ring's newest windowMs of audio. Default 100 ms is
~2.3x wider than the old view. Persisted in presets (scopeWinMs).

### 4. Spectrum display Tilt (Pro-Q style, DISPLAY-ONLY)
New state spectrumDisplayTiltDb (default +4.5 dB/oct, slider 0..6,
Settings > Spectrum "Tilt - Display Slope"). DSP helpers in Fft.hpp:
spectrumDisplayTiltCompDb(freq, tilt) = tilt * log2(f/1kHz), clamped
+/-30 dB, and applySpectrumDisplayTilt() for vectors. drawSpectrum adds
the compensation to the DRAWN values only — analyser process() output
(the data the spectrogram and everyone else consume) is never tilted by
it; the pre-existing analyser tilt (spectrumTiltDb, -9..+9) is untouched
and its slider is now labelled "(analyser...)" so the two can't be
confused. Suite check: a synthetic equal-per-octave (pink-like) spectrum
falling exactly -4.5 dB/oct draws at slope -4.500 (tilt 0), 0.000
(default 4.5), +1.500 (max 6) — flatter with tilt, as specified.

### 5. Loudness meter — VERTICAL
drawLoudness rewritten: L and R as vertical -60..0 dB columns (segmented
palette fill, per-channel peak tick, L/R labels under the bars) and the
live numeric readout (selected mode value + mode name + peak) placed
immediately to the RIGHT of the bars. Per-channel levels are display
ballistics computed in prepareFrame (fast attack, 0.5 dB/frame decay,
peak hold 0.15 dB/frame decay) into new AnalysisData fields; the engine
loudness value feeding the number is unchanged.

### 6. Resizable modules (moduleFlex + splitters)
New state moduleFlex[6] (display order, all 1.0 default = the old equal
layout). MainComponent::layoutModules distributes width by flex; a
ModuleSplitter component (left-right cursor, grip dots) sits on each
visible gap; dragging trades width inside the adjacent pair only (pair
sum preserved, min flex 0.35 each). Heights still follow the window
height — stated plainly in README. Persistence: sizes ride in presets
(modFlex key, plus the new scopeWinMs/dispTilt keys; old presets load
with defaults). Session-persistent otherwise — also stated in README.

### 7. COLOUR MAP BUG (Jackson's report — a defect, fixed)
Audit under real build conditions (grep over the shipped tree): state ->
frame.cmap wiring was intact (constructor + applyTheme on menu pick),
but the ONLY consumer of cmap in all views was drawSpectrum's Colour By
= ColorMap mode. The spectrogram's v0.14 palette pass replaced its
ramp with the fixed spectrumHeat palette and silently dropped the map
feed; waveform/scope also use the fixed palette (by v0.14 design). So
on Midnight Kids + default Level mode, Menu -> Color Map changed
nothing visible anywhere. Jackson's report was exactly right.
Fix: (a) the spectrogram colours its cells from the SELECTED map again
via new DSP spectrogramMapColour(cm, t) (LUT + the same low-end
darkening); (b) a new ColorMapId::Midnight — the v0.14 spectrumHeat
palette sampled into a LUT — APPENDED as value 6 so saved preset ints
0..5 keep their meaning, and it is the new default map, so the approved
spectrogram look is unchanged until another map is picked; (c) the menu
gains the 7th entry. Waveform/scope keep the fixed heat language (they
were never map consumers in any version's design intent beyond v1
notes). Suite check: spectrogramMapColour Plasma vs Rainbow at t=0.5
differ by a summed channel delta of 279 (> 60 required); Midnight LUT
endpoints = spectrumHeat ends; silence under any map renders dark.

### 8. Distribution package (Jackson: give it to friends, one click)
- Static MSVC runtime: root CMakeLists sets
  CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  (CMP0091, before any target) — /MT release, so MeterBar.exe needs NO
  VC++ Redistributable on a recipient machine. (Linux cannot prove /MT;
  Jackson's build log / the exe running on a clean machine is the proof.
  The dynamic fallback below exists if it ever conflicts.)
- MeterBar.iss (Inno Setup 6): installs dist\MeterBar.exe to Program
  Files, Start Menu + optional desktop shortcut, clean uninstall. If
  dist\vc_redist.x64.exe exists at installer-compile time, ISPP #if
  bundles it and [Run] installs it silently (/install /quiet
  /norestart) — the dynamic-link fallback path, pre-wired.
- build-release.bat (NEW, separate from build.bat which is
  byte-identical to v0.14, md5 5f5e724b...): own CMake discovery (never
  calls build.bat — it pauses), builds Release, stages dist\MeterBar.exe
  (+ vc_redist if Jackson drops one beside it), compiles the installer
  via ISCC if present; if ISCC is absent it still produces dist\ and
  prints the one-line instruction + the free Inno Setup URL
  (https://jrsoftware.org/isdl.php). Pure ASCII + CRLF like build.bat.
- JUCE attribution: brief "Made with JUCE" startup splash (1.5 s,
  click-dismiss, JUCE's own SplashScreen — self-deleting, held by raw
  pointer per JUCE's example, never owned) + the same line in
  Help -> About. README's "Sharing it" section explains it, the
  SmartScreen unsigned-exe warning (More info -> Run anyway), and that
  the release build still runs on Jackson's PC.

### Verification (this VM, observed on disk)
- DSP: **78 checks, 0 failures in BOTH build trees** (56 + 17 v0.15
  checks + 5 colour-map checks = 78; the spectrogram/tilt/stereo/state
  checks are itemised above).
- Full clean rebuild (fresh build/ and build-app/ trees): configure +
  build exit 0, fresh ELF (29,731,952 bytes); NOMINMAX still in the app
  target's flags.make. All checks ran under the real build's conditions
  — no define was forced anywhere (the v0.14 lesson); WindowsLoopback
  is untouched this version.
- Xvfb smoke: alive the full 14 s (startup splash path included).
- MSVC trap sweep over all changed files: clean.
- build.bat byte-identical to v0.14 (md5 unchanged).

### What only Jackson's Windows run can prove
- The six fixes under real programme material on his PC; the splitter
  drag feel; /MT actually taking effect in his MSVC build (check: the
  release exe runs, and build-release.bat + Inno Setup producing a
  working MeterBar-Setup.exe — ISCC is not installed on this VM, so the
  .iss is written and reviewed but not compile-tested here).
README_TEST.md leads with the v0.15 re-tests (A-G) and the new
"Sharing it" section; older sections kept below.

### 9. ITEM 8 (added 2026-10-28 ~15:26 PT, Jackson: "last one" before
distribution) — the source picker could only offer microphones
Jackson's field report on v0.14: "the audio input selection he can reach
lets him pick only MICROPHONES" — he could not pick a specific OUTPUT
(headphones, TV speakers) as the loopback source. Full audit of every
source-selection surface, in the shipped code (file/line evidence):
- Settings > Audio Source (Settings.h soundSourceCombo): genuinely
  unified — Test Tone / INPUT DEVICES / OUTPUT DEVICES, outputs from
  WindowsLoopback::enumerateRenderEndpoints() (real COM enumeration,
  EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE), _WIN32-guarded since
  the v0.14 fix). Sound.
- Hamburger > Audio Source submenu: same unified two-section list with
  the active source ticked (including when it is an output). Sound.
- Hamburger > Audio Source > "Choose Input Device..." ->
  MainComponent::showDeviceSelector(): JUCE's stock
  AudioDeviceSelectorComponent(engine.deviceManager, 1, 8, 0, 0, ...) —
  1..8 INPUT channels, ZERO output channels: an input-only component BY
  CONSTRUCTION that could never list an output, sitting in the source
  menu under a label that says "Input Device". Changes made in it also
  rewired the shared deviceManager behind the app's back (state desync).
  This was the surface matching Jackson's report word for word.
Fix: showDeviceSelector() DELETED (function, declaration, menu item,
handler) — the unified pickers are now the only source surfaces.
Requirement-3 instrumentation: the Settings Audio Source panel prints
"Loopback check: Windows reports N active output device(s)" (the count
the picker itself saw), and the Debug window gained "Render endpoints
found (loopback sources): N" (refreshed ~2 s while open) — a screenshot
settles any recurrence in one look.
Recorder verified in code (no fix needed): AudioEngine::pushStereo is
the single ingest for device input, the loopback callback AND the test
tone, and feedRecorder() is called from pushStereo; clip export reads
the same ring. Recording/clips therefore follow whichever source is
active — no input-only assumption exists in the record path.
Verification here: app rebuild exit 0 under real build conditions (no
define forced anywhere), Xvfb smoke alive 14 s, no live references to
the removed component remain. The Windows behaviour (endpoint list
populated, loopback on the picked endpoint) is proven by re-test H in
README_TEST on Jackson's PC.

## 2026-10-28 — Kit v0.16: Default recipe, background styles, loudness pack,
spectrum perception, beat pulse, stereo width, Anime Mode
Built on the v0.15 tree (items 1-8 + distribution). Same kit discipline:
build.bat byte-identical (md5 5f5e724b...), clean rebuild, no define ever
forced, MSVC trap sweep clean.

### 1. "Default" preset = Jackson's recipe
PresetState's C++ defaults ARE the first-run state (the app constructs
its state from them; saved presets are separate files, only loaded when
picked — nothing about this update touches anything saved on Jackson's
PC). The defaults now encode the recipe exactly, and a DSP check asserts
every element: name "Default", Midnight Kids theme, BackgroundStyle
CleanDark at opacity 1.0, spectrum Colour By Level, display Tilt +4.5,
colour map Midnight (v0.15 spectrogram sensitivity rides along), scope
window 100 ms, loudness LUFS Momentary, beat pulse ON, all six modules
enabled at equal flex. Most elements were already the v0.15 defaults;
the new work is the background style + beat pulse defaults and the
asserted recipe check.

### 2. Background Styles (Settings > Appearance)
New state backgroundStyle (enum BackgroundStyle, PresetIO "bgStyle").
One painter (Theme.h fillStyledBackground) serves the window background
and every module panel: CleanDark = near-black blue-grey vertical
gradient (#272c36 -> #101216); MidnightKids = violet -> plum -> deep
violet gradient in the palette family; Flat = the pre-v0.16 flat fill,
byte-for-byte the old behaviour; Pastel (item 7) = pale-pink gradient.
Opacity composes exactly as before (the style is painted at the
opacity-clamped alpha).

### 3. Loudness pack (DSP LoudnessEngine)
- Integrated LUFS: BS.1770 gating over 100 ms K-weighted block energies
  (absolute gate -70 LUFS, relative gate -10 LU under the ungated
  value, two passes; storage capped at 1 h of blocks). Suite: a constant
  -13 dBFS 1 kHz stereo sine integrates to -12.97 LUFS == its momentary
  value (gating keeps all blocks on constant programme). Resets when the
  engine re-prepares (i.e. on input-device change / app start).
- Short-Term LUFS: the engine's existing 3 s window, now displayed.
- True Peak: 16-tap Blackman-windowed-sinc interpolation at 4x over the
  raw stream (per channel). First attempt used 4-point Lagrange and
  UNDER-READ the test crest by ~1 dB (0.884 for a true 1.0) — replaced,
  not shipped. Suite: a 12 kHz sine phased so samples land at +/-0.7071
  (sample peak -3.01 dB) reconstructs to true peak 1.0004 = 0.00 dBTP.
- UI: Integrated / Short-Term / True Peak lines sit with the numbers
  beside the v0.15 vertical bars, plus "Spotify/YT -14: at target /
  over by X / under by X" and "Apple Music -16: ..." rows read against
  the integrated value.

### 4. Spectrum perception options
- Chroma Colour By (SpectrumColorMode::Chroma, appended value 4):
  DSP chromaRgb(freq, level) — hue = pitch class (12 hues, one per note
  name, octave-invariant), brightness follows level. Suite: A4 vs A#4
  channel delta 92 (>60 required); A4 == A5 exactly.
- 1/6-octave smoothing toggle (display-only): DSP smoothSixthOctave —
  each bin averaged over +/-1/12 octave. Suite: bin-to-bin variance of a
  +/-10 dB zigzag spectrum falls from 58000 to 253 (<40% required... it
  lands at <1%). The view applies it to a copy at draw time, before the
  display tilt; analyser data untouched.

### 5. Beat Pulse
DSP BeatPulse (Modules.hpp): per-block mean-square vs a slow (~0.7 s)
trailing baseline; fires when energy > 2.4x baseline after a 180 ms
refractory and a 0.6 s warm-up (warm-up added so steady material
starting from cold cannot false-fire while the baseline converges);
envelope decays with a ~120 ms time constant. Suite: a click train over
a quiet bed fires 6/6 clicks; a 2 s steady tone fires 0. Fed on the
common ingest in AudioEngine::pushStereo (256-sample sub-blocks) — all
sources drive it. UI: ModuleView paints a subtle accent fill (alpha
<= 0.055) + border brighten proportional to the envelope when the
Appearance toggle is on (default on).

### 6. Stereo numeric readout
StereoFrame gains widthPct = 100 * rms(Side)/rms(Mid), capped 200,
computed in analyse() from the same samples as correlation. Suite:
mono -> corr +1.00 / width 0%; polarity-inverted R -> corr -1.00 /
width 200%. The vectorscope's bottom line now reads
"corr X.XX width N% bal Y.YY" (the corr readout itself landed in v0.15;
width is the v0.16 addition).

### 7. Anime Mode (theme, NOT the Default preset)
- Theme "Anime" in themeByName + the theme menu (5 entries): pastel
  chrome (soft pink accent #e77fb1, baby blue #8fb8e8, cream panels,
  plum text #5a4356 for contrast). Meter DATA keeps the heat colour
  language in every theme, Anime included.
- "Pastel" Background Style (#fdeef5 -> #f6d3e4); picking Anime from
  the theme menu pairs it automatically (a preset storing its own
  style is respected — the pairing is a menu-pick behaviour only).
- Mascot: Jackson's transparent cutouts (anime-mascot-calm-clear.png /
  anime-mascot-excited-clear.png — the amended assets, NOT the
  pink-background originals) embedded with juce_add_binary_data
  (target MeterBarAssets; generated symbols animemascotcalmclear_png /
  animemascotexcitedclear_png, verified in the generated BinaryData.h).
  MascotView draws her directly on the background (soft ellipse shadow,
  no card) in her OWN cell after the last module; the cell participates
  in the splitter system (state mascotFlex, default 0.55, PresetIO
  "mascotFlex") — drag her left edge to resize. Hidden in all other
  themes (visibility is keyed on themeName == "Anime" in applyTheme).
- Bob/swap triggers (MainComponent timer): a BeatPulse onset-count
  change starts a 130 ms sine hop (-5 px peak) and a 600 ms excited
  window; excited also holds while short-term loudness > -12 LUFS;
  calm otherwise.
- Cute clicks: three WAVs synthesized by assets/synth_clicks.py (kept
  in-tree as provenance; NO external samples) — pop: 70 ms sine glide
  880->320 Hz, exp decay, peak 0.50; bloop: 90 ms glide 300->720 Hz,
  peak 0.45; sparkle: two pings 1568/2093 Hz 60 ms apart, 160 ms total,
  peak 0.40; all 44.1 kHz/16-bit/mono, embedded via the same
  BinaryData target. Playback: ClickSounds (own output-only
  AudioDeviceManager + AudioSourcePlayer + MixerAudioSource, lazily
  initialised on first click, player gain 0.5 + buffer gain 0.6 —
  UI-feedback level). The capture device path is never touched.
  Trigger points: pop on toolbar button clicks, bloop on switching
  INTO Anime, sparkle on preset load. Gate: theme == Anime AND the
  "Cute click sounds" toggle (Settings > Appearance, default on);
  applyTheme hushes (drops queued inputs) the moment the theme leaves
  Anime — switching themes silences them completely.

### Verification (this VM, observed)
- DSP suite: 103 checks, 0 failures in BOTH build trees (78 v0.15 + 25
  v0.16: recipe 11, loudness pack 5, chroma 2, smoothing 1, beat pulse 2,
  width/corr 4). Full clean rebuild from scratch trees, exit 0; fresh
  ELF 32,496,200 bytes (~2.7 MB of embedded assets over v0.15).
- Xvfb smoke: alive the full 14 s. MSVC trap sweep clean. NOMINMAX
  still in the app flags.
- Kit: v0.16 zip carries assets/ build inputs (the two -clear mascot
  PNGs, three click WAVs, synth_clicks.py). The pink-background
  originals and the media-generation duplicates in assets/ are NOT
  build inputs and are not in the kit.
### What only Jackson's Windows run can prove
- Anime look/mascot/click levels on his machine (the click output path
  opens the default playback device — his machine is the first real
  test of that lane; capture is untouched by construction); the Default
  recipe on a genuinely fresh profile; LUFS-I/TP against a known master.
README_TEST.md leads with the v0.16 re-tests (A-G).

## 2026-10-28 — Kit v0.17: the REAL colour-map root cause (LUT index) + Jackson's revised Default recipe

Jackson's field report on v0.16: "I can't quite seem to get the actual
spectrogram to be RGB... when I hover over the spectrogram it's not
rainbowy like the rest of the things." Clarified the same evening: he
wants LEVEL-DRIVEN colour through the rainbow — "the more volume on
that frequency at that time, the more red it is, given from the
spectrum of colors" — on the spectrogram AND available on the spectrum
bars.

### (A) Root cause — ColorMap::at() never left index 0
`lut_[(size_t)std::clamp(t01, 0.f, 1.f) * 255]` — the cast binds to the
clamped FLOAT before the multiplication: (size_t)(0.577f) == 0, so
at() returned lut_[0] (the map's darkest colour) for EVERY level below
1.0, and only an exactly-full-scale input ever reached lut_[255].
Measured BEFORE the fix (real analyser row, music-like signal, 508
content cells): Rainbow spectrogram = (0,0,157) at -40 dB and
(0,0,219) at -20 dB — blue at every musical level; hue shares over the
row: 100% cyan/blue band, 0% red/orange/yellow/green. The spectrum's
Color Map mode had the same bug via the same at(). Why v0.15 shipped
it: its wiring test compared DIFFERENT maps at ONE level (plasma vs
rainbow at t=0.5) — their index-0 colours differ, so the delta check
passed (279 was in fact the FIXED code's number) while no test ever
compared one map at two levels. v0.17 adds that regression block:
Rainbow at(0/.25/.5/.75/1) must be blue/cyan/green/yellow/red.
Fix: multiply first, then cast. Blast radius: exactly the two
map-driven surfaces (spectrogram cells, spectrum Color Map mode);
Midnight's approved look was spectrumHeat-direct in v0.14 and the
map path in v0.15/16 rendered its violet start colour at all levels —
the fix restores the intended full ramp there too.

### (A2) Sweep widening — level now travels the whole map
With at() fixed, two compressions remained against the clarified
acceptance (full sweep, red only at the top):
- spectrogramLevelT: was floor -75 / ceiling -8 dB with a 0.85 gamma
  (-40 dB -> t 0.58, yellow-green — the cool half only appeared below
  -55 dB). Now LINEAR, floor -70, ceiling -4: equal dB steps take
  equal colour steps; every v0.15 test anchor still holds.
- Spectrum Color Map mode fed the LUT a plain linear -90..0 level.
  New DSP spectrumMapColour(cm, db) feeds it spectrumLevelHeatT — the
  spectrum's own level language — so a bar's colour follows its height
  through the selected map (view: drawSpectrum ColorMap branch).
Acceptance, measured by the DSP suite (Rainbow):
- Spectrogram: -40 dB = (0,129,25) hue 132 deg, -20 dB = (199,193,0)
  hue 58 deg, -6 dB = (248,31,0) hue 8 deg — cool -> warm -> red.
- Spectrum:    -40 dB = (0,255,50), -20 dB = (214,255,0),
  -6 dB = (255,92,0).
- Hue travel across the spectrogram map path t=0.05..1.0: 230 deg.
- Midnight at the same levels stays in family: -40 = (31,101,78)
  teal-green, -6 = (247,84,26) hot orange-red; silence still dark.

### (B) Default recipe v0.17 (PresetState C++ defaults; Jackson dictated)
- Background opacity 1.0 -> 0.15 (15%).
- Spectrum style Fft -> Both (bars + line).
- Spectrum smoothing 90 -> 50.
- Display tilt 4.5 -> 1.15 dB/oct. Analyser tilt 4.5 -> 5.6 dB.
- Loudness streaming-target lines: were drawn unconditionally in
  v0.16; now behind new state loudnessShowTargets (PresetIO key
  "loudTargets", absent in old presets -> default), Default OFF per
  Jackson's self-correction. Settings -> Loudness gains the toggle.
  Integrated/Short-Term/True Peak lines still always show. (The
  spectrum's own Target section toggle was already off and stays off.)
- Audio source first-run: "Test Tone" -> "System Loopback" with empty
  outputDeviceName/Id = the OS-default render endpoint
  (WindowsLoopback::pickDevice: "default (no device chosen)"). The
  test tone is never a first-run source again.
- Trim 0 -> +10.18 dB exactly.
- Unchanged from v0.16: Midnight Kids theme, Midnight map, Clean Dark
  background, beat pulse on, cute click sounds on, scope 100 ms,
  LUFS Momentary, Colour By Level, all modules on, equal flex.
- His same-breath retraction (cut the sub at 50) was NOT actioned: no
  low-frequency cutoff anywhere changed.
- Saved state/presets untouched by construction (defaults only
  construct fresh state; old preset files load over them key by key).

### Verification (this VM, observed — real build conditions, no forced defines)
- DSP suite: 129 checks, 0 failures (103 v0.16 + recipe/assertion
  updates + 6 LUT regression + 12 Rainbow acceptance/recipe-new).
- Full clean rebuild from scratch trees (fresh JUCE fetch included),
  exit 0; fresh ELF 32,502,176 bytes; Xvfb smoke alive the full 14 s
  (with the new loopback first-run default on the stub platform).
- MinGW syntax+type check of WindowsLoopback.cpp WITHOUT any forced
  define: clean. MSVC trap sweep clean (NOMINMAX still in app flags).
- build.bat BYTE-IDENTICAL (md5 5f5e724b5adf6455d88a6be2a440f611).
  build-release.bat is the lead's post-field-fix version (goto-based
  CMake discovery, :have_cmake/:scan labels) — kept verbatim, md5
  72ac7a3aad4530c3c0f101a3cdc6764a; both .bat ASCII + CRLF.
- Kit zip: same 36-entry layout as v0.16, every entry byte-verified
  against the tree.
### What only Jackson's Windows run can prove
- The Rainbow spectrogram/spectrum on real music through his outputs;
  the fresh-state Default on a genuinely fresh profile; MSVC's opinion
  of the tree (as always).
README_TEST.md leads with the v0.17 re-tests (A: Rainbow on both
surfaces; B: fresh-state Default eyeball list).

## 2026-10-28 — Kit v0.18: Reserve screen space (AppBar) + Arrange windows (Jackson's ask, approved ~18:28 PT)

Jackson's ask: with the bar pinned at the top, other windows slide under
it. He wanted a button that makes the windows on the monitor fit the
screen WITHOUT getting in the meter's way. Two mechanisms, as scoped:

### 1. RESERVE — the bar becomes a Windows AppBar (primary)
New service `ScreenSpace` (app/Source/ScreenSpace.h/.cpp), following the
WindowsLoopback idiom exactly: std-only header, pimpl, real Windows
implementation + honest stub ("unavailable (non-Windows build)").
- State `reserveScreenSpace` (PresetState, PresetIO key "reserveSpace",
  absent in old presets -> off). **Default OFF, and the Default recipe
  does not set it** — it changes how OTHER windows behave, so it is
  strictly opt-in. Everything else in the v0.17 recipe is untouched.
- Controls, all mirroring the one state flag: toolbar **Reserve**
  toggle button (sitting with the Reposition/pin family) and
  Settings > Window (new panel) toggle.
- While ON **and** the window's top edge is flush with its monitor's
  top (<= 2 px, from GetWindowRect vs GetMonitorInfo — PHYSICAL pixels
  throughout, because AppBar rects are physical and per-monitor DPI
  would make a JUCE logical rect the wrong size): SHAppBarMessage
  ABM_NEW (callback message registered) -> ABM_QUERYPOS -> ABM_SETPOS
  with uEdge = ABE_TOP and height = the window's own height. Windows
  then shrinks that monitor's work area; maximised windows stop below
  the bar. ABN_POSCHANGED is handled via a SetWindowSubclass proc that
  re-queries and re-sets the stored rect. Monitor moves and height
  changes re-register cleanly (remove + new); toggle-off, un-docking,
  app exit (MainComponent dtor AND ScreenSpace dtor) all ABM_REMOVE.
- Sync discipline: MainComponent::syncScreenSpace() runs on every state
  change and ~4x/sec from the UI timer; reserveTop is idempotent for an
  unchanged rect, so the poll is what makes dock/move/resize take
  effect without hooking window events. The Debug window gained a
  "Screen space (v0.18):" line carrying ScreenSpace::statusInfo() —
  the field test reads its state from there.

### 2. ARRANGE — one-shot sweep for floating windows (secondary)
Toolbar **Arrange** / **Restore** buttons + the same pair in
Settings > Window. Arrange enumerates the top-level windows on the
bar's own monitor (EnumWindows; skips the bar itself, the shell,
minimised, cloaked via DWMWA_CLOAKED, and tool windows — and MAXIMISED
windows on purpose: the AppBar owns those), plans every move FIRST
(moving mid-enumeration can disturb it), then SetWindowPos with
SWP_NOZORDER|SWP_NOACTIVATE. The strip cleared is the bar's bottom
edge measured from the monitor's top. Originals are snapshotted per
sweep; Restore puts them back in reverse order.
- The fitting rule is a PURE function, `meterbar::arrangeFit`
  (dsp/include/meterbar/Arrange.hpp): keep size unless bigger than
  the free area, slide the minimum distance inside, already-fitting
  windows returned unchanged, degenerate inputs unchanged.

### The v0.14 lesson, applied on purpose
ScreenSpace.cpp includes NO JUCE header (it is driven by void* HWNDs),
so its guard tests the compiler's predefined `_WIN32` — never
JUCE_WINDOWS — with the whole story in a comment at the site.
Verification under the REAL build's conditions (no define forced
anywhere):
- g++-mingw-w64-x86-64 (13.2.0, installed via apt on this VM) full
  syntax+type check of ScreenSpace.cpp: CLEAN. It caught one real bug
  first — a paren typo in the DWMWA_CLOAKED condition (extra `)`),
  fixed before anything else ran. WindowsLoopback.cpp re-checked:
  CLEAN (untouched).
- A/B preprocess proof: the status string "appbar active" (exists
  only in the real branch) appears 1x in the Windows-target
  preprocessed output and 0x in the Linux preprocessed output — the
  branch is provably compiled in for Windows targets and out for
  Linux, the exact A/B the v0.14 fix used. SHAppBarMessage appears
  7x in the Windows-target output.
- MSVC trap sweep over the new/changed files: clean; NOMINMAX is
  defined inside ScreenSpace.cpp as well as target-wide.

### Verification (this VM, observed on disk)
- DSP suite: **146 checks, 0 failures in BOTH build trees**
  (129 + 17: sixteen arrangeFit checks — unchanged-when-fitting,
  strip overlap, oversize shrink, all four edge pull-ins, strip 0 /
  strip-overflow / negative, second-monitor and negative-coordinate
  origins, exact-fit, degenerate — plus the reserveScreenSpace
  default-off state check).
- Full clean rebuild from fresh trees (fresh JUCE fetch included):
  exit 0, fresh ELF 32,562,512 bytes; NOMINMAX confirmed in the app
  target's flags.make. Xvfb smoke: alive the full 14 s.
- build.bat BYTE-IDENTICAL (md5 5f5e724b5adf6455d88a6be2a440f611);
  build-release.bat untouched, still the fixed goto-flow version
  (md5 72ac7a3aad4530c3c0f101a3cdc6764a); both ASCII + CRLF.
- VM note: the X11 -dev packages had vanished from this VM again
  (same juceaide Xrandr.h blocker as 2026-10-13/v0.12) — restored via
  apt inside the same atomic chain as the app build. VM-only.

### What only Jackson's Windows run can prove
- The AppBar behaviour itself: SHAppBarMessage semantics, Explorer's
  work-area handling, the subclass callback, per-monitor DPI on his
  actual monitors (incl. any scaled display), and the arrange sweep
  against his real window set. NONE of that can execute on this Linux
  VM — stated plainly: the Windows code is compiler-proven and
  preprocess-proven, not runtime-proven. README_TEST v0.18 leads with
  the tests (A reserve, B arrange/restore, C the Debug line).
- Known Windows rules, not bugs: windows owned by elevated (admin)
  apps can't be moved by the sweep, and fullscreen games ignore both
  mechanisms.
- Kit rebuild from the SHIPPED zip in a scratch tree: test_dsp
  146/0 and the full app build exit 0 there too (fresh ELF,
  32,562,512 bytes — same size as the in-tree build); all 39 zip
  entries byte-verified against the tree.

## 2026-10-28 — Kit v0.18.1: Reserve flicker FIX (Jackson's field report, ~19:03 PT)

Jackson's report on v0.18, verbatim in substance: dock the bar at the
top, windows move below it — "it does the job but then flickers between
the two states, permanently moving the tabs and stopping the user from
interacting with the UI including closing it from the UI."

### ROOT CAUSE (diagnosed from the shipped v0.18 code; three defects
that compound, the first is the engine)

1. **The idempotence check compared the request against the GRANT, so
   it could never come back equal — and a ~4 Hz timer kept paying for
   it.** MainComponent.cpp (v0.18) timerCallback re-ran
   syncScreenSpace() every ~250 ms. ScreenSpace::reserveTop judged
   "already registered" by comparing the requested rect (`want`, from
   the window's live GetWindowRect) against `g_appBar.applied` — the
   rect stored from `abd.rc` AFTER ABM_QUERYPOS/ABM_SETPOS, i.e. the
   rect Windows GRANTED. ABM_QUERYPOS is allowed to trim that rect
   (e.g. the bottom, to fit the space left by other bars), and reserveTop
   itself only restored left/top from `want` before SETPOS — the code
   knew the system rewrites the rect, then compared against the
   rewritten value anyway. Once trimmed, `want != applied` forever, so
   EVERY poll fell into the `releaseTop()` + full re-register branch:
   ABM_REMOVE (work area expands → every maximised window jumps to
   full height) followed by ABM_NEW + ABM_SETPOS (work area shrinks →
   they jump back below the bar), ~4 times a second. That is the
   flicker, the "moving tabs", and title-bar controls (including the
   close button) sliding out from under the cursor mid-click.
   The single-threshold dock test in syncScreenSpace
   (`abs(wr.top - mon.top) <= 2`, no hysteresis) was the second feed:
   any 1 px rect wobble across that line alternated reserveTop /
   releaseTop directly, no QUERYPOS trim needed.
2. **The ABN_POSCHANGED handler could re-trigger itself.**
   ScreenSpace.cpp's appBarSubclassProc answered ABN_POSCHANGED with
   an unconditional ABM_QUERYPOS + ABM_SETPOS. An ABM_SETPOS generates
   fresh ABN_POSCHANGED notifications, so the handler could keep the
   work area recalculating in a loop between polls as well.
3. **Arrange records were destructible, and nothing restored on
   toggle-off / quit.** arrange() began with `impl_->saved.clear()`
   ("each sweep snapshots fresh originals"): a second sweep recorded
   POST-first-sweep positions as "home", and windows absent from the
   latest sweep were forgotten — Restore could never take them back
   (the report's "permanently moving"). The sweep also had no guard
   for a window in an interactive move/size loop, so it could yank
   the window the user was holding. And MainComponent's destructor /
   the Reserve toggle-off path called releaseTop() only — arranged
   windows were left wherever the sweep put them.

### THE FIX
- **Event-driven reserve, decided by a tested state machine.** New
  pure DSP header `dsp/include/meterbar/ReserveState.hpp`:
  `meterbar::ReserveMachine` takes (toggle, gap, bar rect, monitor)
  observations and returns Register / Update / Release ONLY on
  transitions. Hysteresis: dock at gap <= 2 px; once reserved, release
  only at gap > 8 px; the 3..8 px band is a dead zone in both
  directions. Identical repeats return None forever. syncScreenSpace()
  now just feeds the machine and performs the one returned action; it
  is called from real events only — the toggle, Settings/preset
  changes, reposition commands, MainComponent::resized(), and the
  window's moved()/resized() notifications (MainWindow in Main.cpp
  forwards them via notifyWindowGeometryChanged()). **The timer poll
  is deleted** — timerCallback does no screen-space work.
- **ScreenSpace: idempotence judged on the REQUEST.** AppBarState
  gained `requested` (what the caller asked for) alongside `applied`
  (what Windows granted); reserveTop's early-out compares against
  `requested` only. The QUERYPOS/SETPOS pair is factored into
  setAppBarPosIfChanged(): it re-SETPOSes only when the granted rect
  actually differs from `applied` — the ABN_POSCHANGED handler now
  routes through it, so a no-change notification costs one QUERYPOS
  and zero window movement (defect 2 closed).
- **Arrange is strictly one-shot and write-once.** It still runs only
  from the Arrange buttons (no timer path exists). The enumeration
  now skips any window whose thread reports GUI_INMOVESIZE for it
  (a window being dragged/resized is never yanked). Snapshots merge
  instead of clearing: once a window's original rect is recorded it
  is never overwritten by a later sweep; only restore() clears the
  record, and Restore stays available after any sweep. Turning
  Reserve OFF now performs releaseTop() + restore() together, and
  ~MainComponent does releaseTop() + restore() — after either, no
  code path moves another window until the user presses Arrange.
- Regression surface held: all other v0.17/v0.18 behaviour unchanged;
  build.bat and build-release.bat BYTE-IDENTICAL to v0.18 (md5
  5f5e724b5adf6455d88a6be2a440f611 / 72ac7a3aad4530c3c0f101a3cdc6764a,
  ASCII + CRLF).

### Verification (this VM, observed on disk)
- DSP suite: **178 checks, 0 failures in BOTH build trees**
  (146 + 32 new ReserveMachine checks: toggle-off silence, dead-zone
  non-docking, dock -> Register once, in-band jitter x8 -> None with
  state held, geometry change -> exactly one Update, monitor change
  -> one Update, undock -> Release, dead-zone non-re-register after
  release, toggle-off -> one Release, degenerate rect never
  registers).
- Full app rebuild exit 0, fresh ELF 32,562,800 bytes; Xvfb smoke
  alive the full 14 s (ALSA warnings only — VM has no sound hardware).
- MinGW (g++-mingw-w64-x86-64 13) full syntax+type check of the
  changed ScreenSpace.cpp: CLEAN, with NO define forced (the v0.14
  discipline). A/B preprocess proof re-run: "appbar active" appears
  1x in the Windows-target output, 0x in the Linux output.
  MSVC trap sweep over the new/changed files: no std::min/max, no
  numeric_limits extremes, no macro-victim identifiers.
- Kit rebuild from the SHIPPED v0.18.1 zip in a scratch tree: full
  app build exit 0 there too (fresh ELF) and test_dsp 178/0; all 40
  zip entries byte-verified against the tree (39 from v0.18 + the new
  ReserveState.hpp). VM environment saga during that scratch build,
  all VM-only and none of it code: /tmp is a 512 MB tmpfs and filled
  mid-build (gcc temp files died with "No space left"), then orphaned
  cc1plus children of a killed parallel build held ~3.5 GB and got
  the next compiler OOM-killed on a JUCE unity TU. Scratch tree moved
  onto the home filesystem with TMPDIR redirected and the orphans
  killed by PID; the identical sources then built clean end to end.

### What only Jackson's Windows run can prove
- That the flicker is gone in the exact field scenario (README_TEST
  v0.18.1 leads with it: reserve on, maximise a browser, it must sit
  still; toggle off, everything returns). The diagnosis is code-proven
  (the loop is demonstrable in the v0.18 source) and the fix removes
  every identified re-assertion path, but SHAppBarMessage behaviour
  on his monitors remains runtime-unproven from this Linux VM, as
  with every version. Known Windows rules unchanged: elevated (admin)
  windows and fullscreen games sit outside both mechanisms.

## 2026-10-28 — Kit v0.19: Settings pink fix + single-source version + updater + git/GitHub setup
Built from docs/V019_PLAN.md (Jackson approved the plan's recommendations
19:03 PT: private-first repo, tag-gated releases, manual-only update
check, the §1.2 palette as spec'd, Settings-only colour scope). Built on
the v0.18.1 tree; reserve/arrange behaviour untouched.

### 1. Settings pink fix (Jackson: "blinding when I hit Settings")
Root cause (verified in the v0.18.1 source): SettingsContent had NO
paint() and set NO colours on any control it built — the window simply
inherited theme.panel via MainComponent::applyTheme (near-white
0xfffdf3f8 in the Anime theme, pure white 0xffffffff in Daylight), its
controls fell back to stock JUCE LookAndFeel_V4 whites/slates in every
theme, and its native title bar rendered white on Windows because
nothing ever set DWMWA_USE_IMMERSIVE_DARK_MODE.
- Settings is now DECOUPLED from the bar theme: one fixed pink-dark
  palette in every theme (settingsPalette in Settings.h: base #2A1E2E,
  surface #3A2438, well #241722, border #57304D, body text #F6E3EE
  ~13:1 on base, dim #C9A3BB, accent #E04C89, soft pink #E77FB1).
  SettingsContent::paint() fills the base; the six builder helpers
  (combo/slider/toggle/buttonRow/note/soundSourceCombo) plus the
  panel() Enabled toggle set explicit colours; panel headers keep
  their per-module accents (they read well on the new base).
- A SettingsLookAndFeel (LookAndFeel_V4 with the palette seeded into
  every relevant colour ID, incl. PopupMenu and ScrollBar) is attached
  to the Settings content + viewport ONLY — the main bar has no
  setLookAndFeel call, as before. (Verified: ComboBox popups take
  their L&F from the combo's getLookAndFeel() in JUCE 8, so the scoped
  L&F reaches the dropdowns too.)
- SettingsWindow's ctor background is now the palette base, and
  MainComponent::applyTheme re-pins that fixed base for the Settings
  window instead of theme.panel (the Debug window still follows the
  theme — same bug class, deliberately out of this pass's scope).
- Windows title bar: SettingsWindow::visibilityChanged() calls
  DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE) on the native
  peer once it exists. The guard tests the compiler's _WIN32 (never
  JUCE_WINDOWS — the v0.14 lesson), and the HWND expression
  (getPeer()->getNativeHandle()) is the identical one MainComponent
  already ships in MSVC-proven code (lines 148/191/266).
- NOT changed (scope): the Anime/Pastel/Daylight main-bar themes and
  gradients are exactly as v0.18.1.

### 2. Single-source version (was: six hard-coded "1.0.0"s)
- Root CMakeLists project(VERSION) is now THE source, set to 0.19.0,
  with configure_file generating generated/MeterBarVersion.h
  (meterbar::kVersionString). Main.cpp getApplicationVersion(), the
  splash version line, MainComponent's toolbar kVersion and the About
  box all consume it.
- Places the plan did NOT list, found by grep and folded in:
  app/CMakeLists.txt carried TWO more — juce_add_gui_app VERSION
  "1.0.0" and JUCE_APPLICATION_VERSION_STRING="1.0.0" — both now
  ${PROJECT_VERSION}; and the About box had its own literal
  "MeterBar v1.0.0" separate from kVersion. Full-source grep for
  "1.0.0" after the change finds NO live version string — only a
  historical comment at the old kVersion site, a SemVer doc example,
  one SemVer test case, and historical docs (SPEC.md's MiniMeters
  screenshot note + this log).
- MeterBar.iss: MyAppVersion is now an #ifndef fallback ("0.19.0");
  build-release.bat reads the version out of CMakeLists.txt
  (findstr, tokens=3) and passes ISCC /DMyAppVersion=%MBVER%. That is
  the ONE justified build-release.bat change (md5 now
  84b1268491c5af6287d01fd742c5f26b, still pure ASCII + CRLF);
  build.bat BYTE-IDENTICAL (md5 5f5e724b5adf6455d88a6be2a440f611).

### 3. Updater (replaces the "no update server configured" stub)
- Config: app/Source/UpdaterConfig.h — ONE constant,
  mbupdate::kGitHubRepo, EMPTY by default on purpose. Empty => Check
  For Updates shows the honest "updates are not configured yet — no
  check was performed" message and makes NO network request.
- Configured behaviour (Updater.h): a detached worker thread (touches
  only a shared_ptr state; MainComponent polls it from the existing UI
  timer — no UI object is ever touched off-thread) GETs
  api.github.com/repos/<repo>/releases/latest with a MeterBar
  User-Agent, parses tag_name/body + the MeterBar-Setup.exe asset
  (URL, size, digest) with JUCE JSON, and compares versions with the
  new pure DSP header meterbar/SemVer.hpp (numeric compare —
  "0.9.0" < "0.19.0"; unit-tested, incl. the string-compare trap).
  Outcomes: up-to-date / check-failed (says why) / update-available
  (notes + Update button).
- Update: downloads to %TEMP%\MeterBar-Update\MeterBar-Setup.exe via
  URL::downloadToFile (polled in the same timer), verifies the
  published size and the SHA-256 digest when GitHub provides one
  (juce_cryptography now linked for SHA256), launches the installer
  /SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART, then
  quits the app. Verification failure = the installer is NOT run and
  the user is told so. Manual check only — no startup auto-check.
- MeterBar.iss gains the relaunch [Run] entry the plan specified:
  no skipifsilent, Check: WizardSilent — silent (in-app) updates
  relaunch the new version; interactive installs keep the existing
  postinstall entry's behaviour.

### 4. Git + GitHub (local only — the remote waits on Jackson)
- git init (branch main) in meters-app; repo-local identity
  (Milo (MeterBar build) <milo@localhost> — a local placeholder, no
  global identity existed). .gitignore excludes build trees (incl.
  the FetchContent JUCE cache), dist/, zips, build logs, .vs/*.user,
  and the assets/media-generation-* dumps; source art is tracked.
- Commit 1 (319c2c6) "baseline: v0.18.1 tree as shipped" — the clean
  v0.18.1 tree + the plan doc + .gitignore, BEFORE any v0.19 edit.
- .github/workflows/build-windows.yml: windows-latest; triggers on
  tags v* (plus branch pushes / manual dispatch for artifacts);
  reads the version from CMakeLists.txt and FAILS a tag build whose
  tag != version; configure -> build Release -> DSP suite ->
  choco install innosetup -> stage dist -> ISCC /DMyAppVersion ->
  upload artifact always, and attach MeterBar-Setup.exe to the tag's
  GitHub Release on tag builds (softprops/action-gh-release).
  YAML validated by parsing. NOT run anywhere yet (no remote exists).
- NO remote was created, nothing was pushed, gh auth was not run:
  creating the repo needs Jackson's own GitHub login (plan §5.1).

### Verification (this VM, observed on disk)
- DSP suite: **197 checks, 0 failures** (178 baseline + 19 new
  SemVer checks), in the fresh working tree and (below) from the
  shipped kit's extracted tree.
- Full clean rebuild from scratch trees (fresh JUCE fetch): configure
  + build exit 0, fresh ELF 32,711,768 bytes, zero errors in the log;
  the REAL build's flags.make carries
  JUCE_APPLICATION_VERSION_STRING="0.19.0" and NOMINMAX, and the
  generated MeterBarVersion.h carries 0.19.0. Xvfb smoke: alive the
  full 14 s (ALSA warnings only).
- Windows-branch proof for the NEW Windows-only code (the DWM call):
  JUCE 8 has REMOVED MinGW support (juce_TargetPlatform.h #warning),
  so JUCE's own headers cannot be parsed by MinGW at all — a full-TU
  MinGW check of Settings.h is impossible for reasons unrelated to
  this code (the v0.13–v0.18 MinGW checks covered std-only TUs, which
  still stand untouched). Adapted proof, no define forced anywhere:
  (a) the exact Win32 calls (windows.h + dwmapi.h +
  DwmSetWindowAttribute + the immersive-dark-mode constant) pass a
  MinGW full syntax+type check in a standalone TU — CLEAN;
  (b) A/B preprocess of the real Settings TU: DwmSetWindowAttribute
  appears 2x in the MinGW (Windows-target) preprocessed output and
  0x in the Linux one — the branch is selected exactly under Windows
  targets; (c) the JUCE-side expression it wraps
  (getPeer()->getNativeHandle()) is byte-identical to MSVC-proven
  code already shipped in MainComponent.cpp. The updater itself has
  NO Windows-gated code (JUCE URL/ChildProcess/SHA256 APIs are
  cross-platform) and compiled in the Linux build above.
- MSVC trap sweep over all new/changed files: clean (no bare
  min/max, no numeric_limits extremes, no near/far/ERROR identifiers,
  no windows.h macro victims).
- VM note (VM-only, repeated from v0.18.1): /tmp is a 512 MB tmpfs
  and filled during the first fresh-build attempt (compiler temp
  files); the successful build ran with TMPDIR redirected to the
  home filesystem, exactly as in the v0.18.1 scratch build.

### What only Jackson's Windows run can prove
- Settings actually rendering pink-dark with a dark title bar, in
  Anime/Daylight/Midnight Kids (DWM behaviour is his Windows').
- The 0.19.0 strings in the built exe (toolbar, About, splash) and
  the installer version from build-release.bat's new ISCC /D line
  (ISCC is not installed on this VM; the .iss changes are written
  and reviewed, not compile-tested).
- The honest-unconfigured Check For Updates message. The configured
  path (real GitHub check -> download -> silent install -> relaunch)
  can only run once the repo exists and a release is published.
README_TEST.md leads with the v0.19 re-tests (A settings in three
themes incl. title bar, B version everywhere, C the honest Check
message, D the standing v0.18.1 reserve re-test).

## 2026-10-28 — Kit v0.19.1: edge-aware reserve (MiniMeters semantics)

**Why.** Jackson field-tested v0.18.1's flicker fix and came back
with the real requirement, in his words after the screenshot: "you
did what i asked but it wasn't quite what i had in mind." His bar is
docked at the **bottom** of his screen, and the reserve he wants is
MiniMeters': Reserve ON + maximize an app → the app's maximum size
shrinks so it stops **at the bar** instead of sliding under it. The
v0.18 line reserved **ABE_TOP only**, gated on the bar being flush
with the monitor's **top** — a correct AppBar, on the wrong edge for
his layout, so in his setup Reserve silently did nothing at all.
v0.19.1 generalizes the reservation to the edge the bar is actually
docked to. Nothing else changes: the v0.18.1 event-driven/hysteresis
discipline is untouched, Reserve stays default OFF and out of the
Default preset, and v0.19's pink Settings, single-source version,
and updater are exactly as shipped (this build bumps the one
version line to 0.19.1).

### The fix — four layers, all edge-parameterized
- **ReserveMachine (DSP).** The observation now carries the gap to
  **all four** monitor edges plus the monitor rect
  (`update(toggleOn, EdgeGaps, barRect, monitorRect, monitorId)`).
  Docked edge = the bar's nearest edge when its gap is ≤ 2 px;
  release still requires the gap to the **registered** edge to
  exceed 8 px (same hysteresis, per-edge; 3–8 px dead zone both
  directions). One deliberate restriction, learned from the test
  bench: **docking is orientation-limited** — a wide bar (w ≥ h,
  MeterBar's shape) can only dock Top/Bottom, a tall bar only
  Left/Right. Without it, a full-width bar sitting at the bottom is
  incidentally flush with the left/right edges (gap 0) and "nearest
  edge" would happily reserve a strip the size of the whole screen.
  The registered rect is a **full edge strip** of the monitor
  (top/bottom: full width × thickness to the bar's far edge;
  left/right: full height × same), computed by a new pure
  `stripRect()`. A bar dragged straight from one docked edge to
  another yields a new **Switch** action (release old edge +
  register new, exactly once each); a monitor change
  mid-reservation re-registers on the new monitor. The `ReserveEdge`
  enum lives in Arrange.hpp (the lower-level header) so both DSP
  headers share it.
- **arrangeFit (DSP).** Generalized from "fit below a top strip" to
  "fit into the monitor minus a strip on a given edge"
  (`arrangeFreeArea()` + edge-aware
  `arrangeFit(window, monitor, edge, stripPx)`), same clamping math
  (shrink oversize to the free area, pin inside it). The original
  3-arg form is kept as a wrapper; a unit test asserts it returns
  the Top result exactly, so v0.18 arrange outcomes are identical
  by construction.
- **ScreenSpace (Windows TU).** `reserveTop()` →
  `reserveEdge(hwnd, edge, rect…)` registering
  ABE_TOP/ABE_BOTTOM/ABE_LEFT/ABE_RIGHT as told; the AppBar state
  records the live edge; `setAppBarPosIfChanged()` re-anchors after
  ABM_QUERYPOS per edge (the edge-adjacent coordinate + the strip's
  start along the monitor; the system may trim only the far side);
  idempotence is judged on **requested rect AND edge** (the v0.18.1
  requested-vs-applied fix, extended); ABN_POSCHANGED still routes
  through the guarded setter; `releaseTop()` → `release()`; the
  Debug status line now names the edge: `appbar active - BOTTOM
  strip 120 px reserved`. `arrange()` takes the edge and clears
  that edge's strip. The `_WIN32` guard discipline and the
  v0.14-lesson comments are unchanged — the file still contains
  zero JUCE.
- **MainComponent / Settings copy.** `syncScreenSpace()` computes
  the four edge gaps from GetWindowRect/GetMonitorInfo and feeds
  the machine; Register/Update register the machine's
  `requestedRect()` strip on `machine.edge()`; Switch performs
  release-then-register; Release keeps the v0.18.1 semantics
  (toggle-off additionally restores arranged windows
  synchronously; quit path unchanged). `doArrangeWindows()` picks
  the bar's nearest edge by the same gap math and clears that
  edge's strip — a bottom-docked bar now arranges windows into the
  space **above** it. Settings' Window panel no longer says "top":
  the toggle is "Reserve screen space when docked at a screen
  edge", the note covers all four edges and the drag-to-another-
  edge behavior, and the Arrange button reads "Arrange windows
  clear of the bar". The Debug status label reads "Screen space
  (v0.19.1)". No new files, no CMake source-list changes, no
  updater or .github/ file touched.

### Tests — DSP suite 197 -> 250 checks, 0 failures (+53 net)
- The v0.18.1 reserve block converted to the four-gap API on the
  top edge (identical scenarios and expected actions).
- New: `stripRect` on all four edges incl. dead-zone-float
  thickness (gap + bar size); **bottom docking** end-to-end (dead
  zone no-dock, Register, requested strip = full width × bar
  height anchored to the monitor bottom, repeats None, dead-zone
  wobble held, height growth → exactly one Update with the strip
  following, drag-clear → Release); **left and right docking**
  (+ toggle-off Release-once on the left); nearest-edge ties
  (exact tie → Top); the orientation rule (a full-width bar at the
  bottom registers **Bottom**, never a side; a tall bar at a
  corner registers its side); **edge switch** bottom→top = exactly
  one Switch, still reserved, new edge/rect, repeats None, then
  toggle-off = one Release; arrangeFit per edge (bottom: window
  poking under the bar pulled up / oversize capped / exact fit
  untouched / strip overflow unchanged; left/right mirrors;
  negative-coordinate monitor; 3-arg == Top-form equality).
- One real bug caught by these tests before any of it shipped: the
  first `nearestEdge` draft had no orientation limit and the
  bottom-dock block's full-width bar (side gaps 0) registered as a
  **left** dock — 5 failing checks, the machine was fixed, not the
  tests. A second catch in the trap sweep: a local variable named
  `near` in the machine (a windows.h macro-victim class) — renamed
  before it could bite.

### Verification
- In-tree: DSP 250/0 in both build trees; full app build exits 0;
  fresh ELF (32,711,808 B) with
  `JUCE_APPLICATION_VERSION_STRING="0.19.1"` in its flags and
  NOMINMAX present; Xvfb smoke alive the full 14 s (ALSA noise
  only).
- MinGW g++-mingw-w64-x86-64 13.2.0 full syntax+type check of
  ScreenSpace.cpp **clean, no forced defines**. (MinGW and the X11
  -dev packages had both vanished from the VM again and were
  apt-restored in the same chain before building — the standing
  VM note.)
- A/B preprocess proof: `"appbar active"` 1× Windows target /
  0× Linux.
- MSVC trap sweep over all changed files clean after the `near`
  rename (no bare min(/max(, no numeric_limits extremes, no
  near/far/ERROR identifiers, no windows.h macro victims).
- `build.bat` **byte-identical to v0.18** (md5
  5f5e724b5adf6455d88a6be2a440f611); `build-release.bat` untouched
  (it reads the version from CMakeLists, so ISCC gets 0.19.1 with
  no edit).
- Confirmed on the kit as shipped: extracted to a scratch dir on
  the home filesystem, full configure + DSP suite **250 checks,
  0 failures** + full app build exit 0; all 44 zip entries
  byte-verified against the tree.

### What only Jackson's Windows run can prove
- AppBar registration behavior on a real Windows desktop is
  **code- and preprocess-proven, not runtime-proven** — this VM is
  Linux. Jackson's field test is the arbiter, and README_TEST leads
  with it in his own layout: bar at the bottom → Reserve →
  maximize a browser → it stops above the bar; drag the bar to
  the top → the reservation moves; Reserve off → everything
  restores.
- One deliberate trade-off carried from v0.18.1: if Windows refuses
  a registration, nothing retries until the next real
  geometry/toggle event (retry loops are the bug class that caused
  the flicker).

## 2026-10-28 — v0.19.2: updater wired to Corporal8065/meterbar (launch check, release notes, private-repo fallback)

Jackson's expansion of the v0.19 updater scope, in his words via the
main chat: check on launch AND a manual button -> version + release
notes shown in-app -> "Download & install" -> fetch, install,
relaunch. Plus the private-repo handling agreed the same evening.

### What changed
- **Version 0.19.1 -> 0.19.2**, single source as always: the root
  CMakeLists `project(VERSION)` line only. The generated
  MeterBarVersion.h regenerates to 0.19.2; the exe's embedded version
  string is "0.19.2" (verified with `strings` on the built ELF — an
  early build attempt in this session produced a stale 0.19.1 binary
  whose splash still read v0.19.1; that binary was discarded and the
  rebuild verified before anything else proceeded). The Inno
  fallback literal in MeterBar.iss was bumped to 0.19.2 per that
  file's own instruction (build-release.bat / CI pass the real
  version via /DMyAppVersion, so the fallback only serves a bare
  manual ISCC run). No other hard-coded version literals exist in
  app code (grep-verified; remaining "0.19.x" strings in the tree
  are historical comments and SemVer test data).
- **UpdaterConfig.h**: kGitHubRepo = "Corporal8065/meterbar" (was
  empty). The header comment was rewritten to state the private-repo
  reality plainly (see "Mode verification" below).
- **Check on launch**: MainComponent's constructor starts the same
  background worker check, flagged as a launch check. pollUpdater
  presents launch results NON-modally and selectively: up-to-date,
  offline, error and private-repo outcomes are completely silent at
  launch; a newer release lights a pink "Update!" indicator button
  in the toolbar (brand pink #E04C89 in every theme), and clicking
  it opens the same update panel as the manual flow. No dialog ever
  appears at startup.
- **Release-notes panel**: presentUpdateResult's newer-release path
  now opens showUpdatePanel — version line, the release's FULL notes
  in a scrollable read-only text box (no more 600-char truncation),
  "Download & install" / "Later". The download -> size check ->
  SHA-256 check -> silent Inno launch -> quit sequence is the
  unchanged v0.19 machinery.
- **Private-repo fallback (first-class state)**: ReleaseInfo gained
  `authBlocked` + `httpStatus`. The check worker captures the HTTP
  status via JUCE's InputStreamOptions::withStatusCode. 401/403/404
  (GitHub's answer to unauthenticated calls on a private repo) sets
  authBlocked; the manual flow then shows "Automatic update check
  isn't available while the repository is private." with an "Open
  releases page" button that opens
  https://github.com/Corporal8065/meterbar/releases in the default
  browser. The same state fires if a release IS visible but the
  installer download is refused with 401/403/404. No token or
  credential exists anywhere in the app. One code path: if the repo
  is made public, the identical build performs the full flow.
- **Release-JSON parsing moved into the DSP core**:
  dsp/include/meterbar/ReleaseParse.hpp (pure std, SemVer-pattern)
  extracts tag_name, body and ONLY the MeterBar-Setup.exe asset's
  URL/size/digest from the API response, with full JSON string
  unescaping. Updater.h now consumes it instead of juce::JSON, which
  is what makes the notes extraction unit-testable.
- Untouched, deliberately: ScreenSpace.cpp/.h, ReserveState.hpp,
  Settings.h (reserve code and the pink Settings palette are
  byte-identical to v0.19.1 — verified by empty `git diff` on those
  paths). build.bat byte-identical (md5 5f5e724b5adf6455d88a6be2a440f611);
  build-release.bat untouched (it reads the version from CMakeLists).

### Mode verification — what was and was NOT proven (honest statement)
- **Neither live mode was verified against GitHub from this VM.**
  The repo is private and this VM holds no GitHub credential for
  it, so a live check here cannot see the real repo's releases, and
  no claim is made that the private-mode check "works" against the
  live API — Jackson's Windows run (README_TEST v0.19.2 section) is
  the live proof of the private-repo branch.
- What WAS proven here: (1) the fallback branch logic — the status
  classification (401/403/404 -> auth) is a pure function with 7 DSP
  checks; (2) the field extraction the public-mode flow depends on
  (tag, notes unescaping, asset selection/bounding) has 14 DSP
  checks on representative GitHub-shaped JSON; (3) the full app
  compiles and runs with the repo constant set (Xvfb smoke below),
  i.e. the launch check fires on startup and the app stays healthy
  when the network answer is an error/unreachable — silent, as
  designed. The public-mode end-to-end (notes panel -> download ->
  install -> relaunch) remains provable only once a release is
  visible to an unauthenticated caller.

### Verification (in-tree, this commit)
- DSP suite: **271 checks, 0 failures** (250 at v0.19.1 + 14
  ReleaseParse + 7 httpStatusBlocksAuth).
- Full app build exit 0; fresh ELF 32,741,784 B; `strings` shows
  "0.19.2" as the only 0.19.x version string.
- Xvfb smoke: alive the full 14 s, no output beyond ALSA noise —
  with the launch check active against an unreachable/private feed.
- MSVC trap sweep over all changed/added lines: clean (no bare
  min(/max(, no near/far/ERROR identifiers, no numeric_limits traps).
- No new platform APIs: browser launch is juce::URL; the changed
  app files compile in the real Linux JUCE build (stronger than the
  forced-macro MinGW checks this project retired in v0.14).

## 2026-10-28 — docs: fancy README + screenshot gallery (companion commit to v0.19.2)

- README.md at the repo root, product-page style: icon + one-line
  description, build-status badge for the build-windows workflow
  (an HTML comment in the file notes, honestly, that the badge
  lights up after the first push + Actions run), features, Install
  (with the SmartScreen "More info -> Run anyway" note for the
  unsigned installer), Updating (incl. the private-repo behaviour),
  Build-from-source, Gallery, and "The family" (TickerBar and
  SessionBar as separate repos).
- Gallery (docs/images/): the app icon, the social-preview image,
  the clear mascot cutout, and two REAL screenshots — the bar
  docked at the bottom of an actual Windows desktop (cropped from
  Jackson's 2026-10-28 field screenshot) and the pink Settings
  palette running in the Linux verification build under Xvfb
  (captioned as exactly that). The other evening screenshots in the
  media library were reviewed and excluded (they don't show
  MeterBar); the tiny thumbnail of Fi's monitor was excluded on
  quality/provenance. Per-theme Windows shots that cannot be
  produced honestly from this VM are a clearly-marked
  "screenshots wanted" list in the README — NO fabricated UI
  screenshots were generated or included.
- Final-tree verification for the pair of commits: clean rebuild
  from scratch (fresh DSP tree in-tree: 271 checks, 0 failures;
  full app build from a pristine scratch copy of the tree: exit 0,
  built ELF's only 0.19.x string is "0.19.2"; DSP in the scratch
  copy also 271/0). The shipped kit is built from this committed
  tree and re-verified from its own extraction (see the release
  report; kit file: meterbar-test-kit-v0.19.2.zip).

## 2026-10-29 — v0.20.0: exact mapping everywhere, true 3-band scope, visualizer background

- **(i) One frequency mapping for every view.** The spectrum view has
  used the core's tested `freqToX` walk since v0.12 — but the
  SPECTROGRAM still mapped image rows/columns straight onto bin
  indices, so under the default Log scale its frequency axis was
  linear while the spectrum's was logarithmic: the same tone sat at
  different heights in the two views. The walk now lives in the core
  as `SpectrumAnalyser::binAtX` and the spectrogram builds its
  frequency LUT from it (FFT scale honoured). Reported plainly: the
  spectrum view itself was NOT still approximate — the spectrogram
  was the remaining offender.
- **(i) Oscilloscope "Multi" is now a true 3-band split.** It used
  to draw the scope buffer, then the same left channel again
  faintly — one signal twice wearing a "multi" label. New core
  `splitScopeBands` (one-pole crossovers at 250 Hz / 4 kHz; the
  bands sum back to the input exactly, pinned by test) feeds three
  stacked lanes: LOW / MID / HIGH.
- **(l) Visualizer background mode — NEEDS JACKSON'S VISUAL FIELD
  TEST.** Behind a Settings → Appearance toggle (default OFF):
  black background; white particles converge on the centre; the
  three frequency bands (core `spectrumBandEnergies`) trigger
  rainbow brush-stroke bursts that expand and dissolve into their
  constituent dots; the layer renders at half resolution (capped
  480×270) with a Gaussian-blur glow pass under the crisp dots.
  Palette controls: base hue, saturation, palette spread, cycle
  period — persisted in presets. Particle count is hard-capped at
  600 (asserted every frame in the tests) for a 16 GB gaming PC.
  The model is unit-tested; the LOOK (densities, speeds, blur
  radius, trigger threshold) can only be tuned on Jackson's screen
  — that field test is explicitly still owed.
- **Theme parity:** MeterBar is the family's theme reference; this
  release verifies the other two bars' defaults are value-for-value
  identical to its Midnight Kids palette (see their build logs).
- Version single-sourced as ever (root CMakeLists project VERSION
  → 0.20.0; installer fallback literal updated to match). No tag
  created by this work — the existing build-windows workflow will
  build the release when a tag is pushed.

Kit file: `meterbar-test-kit-v0.20.0.zip` — same layout as v0.19.2,
plus `REQUESTS.md` and `dsp/include/meterbar/Visualizer.hpp`.

Verified (Linux VM):
- **DSP suite: 913 checks, 0 failures** (HEAD baseline rebuilt
  separately: 271 checks, 0 failures). The +642 delta includes 600
  per-frame particle-cap assertions in one loud-run loop; the rest
  are binAtX (3 scales: edges/monotonic/round-trips), band-split
  dominance + exact reconstruction, and the visualizer model suite.
  Three new checks initially failed on MY expectations, not the
  code — details and the corrected semantics are recorded in
  REQUESTS.md, not hidden.
- **Changed app TUs syntax-checked clean** against JUCE 8.0.4
  headers (one real fix on the way: JUCE 8 removed
  `Image::copyImage`; the blur layer now uses
  `duplicateIfShared()`). The full CMake app build could not run on
  this VM — the nested juceaide build needs libxrandr-dev, which is
  not installed (environment gap, also recorded in REQUESTS.md);
  CI builds the app on windows-latest.

Honest limits: the visualizer's tuning constants are first-pass
values chosen conservatively; expect one tuning round after
Jackson sees it live. Linking (P4 item k) is NOT in this release.
