# MeterBar — Jackson's test checklist

**For your Windows 11 PC.** (Any "Linux" mentions in BUILD_LOG.md are just where Milo tested the meter math before packaging this for you.)

You don't need to know any C++. Your job: build it, click through these slices,
and send back (a) a screenshot and (b) one line per slice: works / broken + what you saw.

## Build (once)
1. Install **Visual Studio 2022** → workload **"Desktop development with C++"** (if not already installed).
2. Double-click **`build.bat`** in this folder. First run downloads JUCE (~2–5 min), then builds.
3. If it fails, send me a screenshot of the *first* red error — not the whole log.
4. Success = `build-app\app\MeterBar_artefacts\Release\MeterBar.exe` exists. Double-click it.

## v0.18.1 — Reserve flicker FIX (do these FIRST, please)

**v0.18.1 fixes the reserve flicker you reported**: in v0.18 the reservation was re-checked from a timer ~4 times a second, and the check compared what MeterBar *asked* Windows for against what Windows *granted* — two numbers that could never match once Windows trimmed the granted strip, so the reservation was torn down and rebuilt several times a second. Every maximized window bounced between full height and below-the-bar height, and its tabs/close button kept moving out from under your mouse. The reservation is now **event-driven** (it only changes when the bar actually docks, undocks, resizes, or the toggle flips) with a **hysteresis band** so a pixel of wobble changes nothing.

**Retest (the exact scenario that broke):**
- [ ] **Reposition → At the Top**, click **Reserve** on, then **maximize a browser** on the same monitor: it should sit **still** below the bar — no flicker, no bouncing, top edge parked at the bar's bottom edge.
- [ ] Use the browser normally for a minute: scroll, switch tabs, hover the close button — nothing should move, and clicking/closing must work first try.
- [ ] Click **Reserve** off: everything returns — the browser can maximize over the full screen again, and any windows you had **Arrange**d go back where they were.
- [ ] Scatter floating windows, click **Arrange** once: they fit below the bar. Start dragging one of them and (while still holding it) have a second Arrange happen — the window you're holding must NOT be yanked. Click **Restore**: everything returns to its true original place, even if you Arranged twice.
- [ ] Debug line check: **Menu → Show Debug Window** → "Screen space (v0.18):" should read `appbar active - top strip NNN px reserved` while reserved, and `not registered` after Reserve off.

What changed under the hood (detail in BUILD_LOG v0.18.1): no timer touches other windows any more; Arrange is strictly one-shot per button press and skips any window you're actively dragging; arranged-window originals are written once and never overwritten, so Restore always knows the real home positions; quitting MeterBar now also unregisters and restores.

## v0.18 — Reserve screen space + Arrange windows

**NEW, your ask: MeterBar can now claim the top strip of your monitor**, so other windows stop sliding underneath it. Two parts, both **Windows-only** and both **off until you turn them on**:

**A. Reserve (the AppBar — the real fix)**
- [ ] **Reposition → At the Top** (bar docked at the top of a monitor), then click the toolbar's new **Reserve** button (or Settings → Window → "Reserve screen space…"). It lights up and stays on.
- [ ] Open a browser (or any app) and **maximize it**: it should now fill the screen *below* MeterBar — its top edge stops at the bar's bottom edge instead of hiding behind/under the bar. That's Windows itself shrinking the monitor's work area, the same trick the taskbar uses.
- [ ] Un-maximize and re-maximize: still stops below the bar. Drag MeterBar away from the top edge (or Reposition → At the Bottom): the reservation releases — maximize again and the window uses the full screen.
- [ ] Click **Reserve** off: reservation releases immediately. The state saves into presets like everything else.
- [ ] If you have 2 monitors: dock the bar at the top of the *other* monitor and maximize something there — the reservation follows the bar.

**B. Arrange / Restore (the one-shot sweep, for floating windows)**
- [ ] Scatter a few normal (not maximized) windows around so some of them poke up under/behind the docked bar. Click **Arrange**: every floating window on that monitor slides/resizes to fit **below** the bar. Nothing that was already fully below the bar should move.
- [ ] Click **Restore**: they all go back where they were.
- [ ] Maximized windows are deliberately *not* touched by Arrange (Reserve handles those). Minimized windows aren't touched either.

**C. Tell me what the Debug window says**
- [ ] **Menu → Show Debug Window**: there's a new line, **"Screen space (v0.18):"**. With Reserve on and the bar docked at top it should read `appbar active - top strip NNN px reserved`. If anything above misbehaves, a screenshot of that line tells me which stage we're in.

Honest limits: this is the first build with this feature and it can only be tested on your PC (Milo's machine can't run Windows window management). Apps running **as administrator** can ignore window moves from a normal app, and **fullscreen games** do whatever they want — both are Windows rules, not bugs. Mixed-DPI multi-monitor setups are the fiddliest case; if the reservation lands at a slightly wrong height on a scaled monitor, tell me the scaling % of that monitor and what the Debug line says.

## v0.17 — the Rainbow fix + your new Default recipe

**A. Spectrogram Rainbow check — FIXED (the colour lookup itself was broken)**
Honest story: in v0.15 the spectrogram started taking its colours from the selected Color Map's lookup table — and that table's lookup had a math bug: it read the table's **first (darkest) colour at every level below full blast**. So every map rendered as ONE hue getting brighter and darker — Rainbow was blue at every volume, which is exactly what you saw. (The automated test back then compared *different maps at one level* — their first colours differ, so it passed while the bug shipped. There's a proper regression test now: one map at five levels must give five colours.) The level sweep is also widened, so a cell's loudness travels the **whole** map, and the spectrum bars got the same treatment.
- [ ] Play a song. **Menu → Color Map → Rainbow**: the **spectrogram** now colours each cell by that frequency's loudness at that moment — quiet parts **blue/cyan**, middling **green → yellow**, the loudest bits **RED** (red is saved for the top). It should look unmistakably like a rainbow waterfall.
- [ ] Switch the map back to **Midnight Kids**: the familiar dark violet→red look returns (that's still the default).
- [ ] Spectrum too: **Settings → Spectrum → Colour By → "Color Map (classic)"** (Rainbow still selected): each bar's colour follows **its own height** through the same rainbow — short bars cool, tall bars red.

**B. Fresh-state Default check (your new recipe)**
Your saved settings are **not** touched by this update — the new Default only shows on a fresh install, or when loaded on purpose. To see it: rename `Documents\MeterBar\Presets` to `Presets-old` (just hides your saves; rename back after), launch, and eyeball:
- [ ] **Theme** Midnight Kids, **Color Map** Midnight Kids, background **Clean Dark** at **15% opacity** (desktop faintly visible through the panels)
- [ ] **Source = your default output** — NOT the test tone: play anything through your usual speakers/headphones and the meters follow it immediately
- [ ] **Trim +10.18 dB** (Settings → Audio Source)
- [ ] **Spectrum**: Style **Both** (bars + line together), **Smoothing 50**, **Display Tilt 1.15**, analyser **Tilt 5.6**, Colour By = Level
- [ ] **Oscilloscope** window **100 ms**; **Loudness** = LUFS Momentary, with Integrated / Short-Term / True Peak lines showing — and the two **Spotify/Apple target lines NOT showing** (that section is off now; the toggle lives in Settings → Loudness if you ever want them)
- [ ] **Beat Pulse on**, **Cute click sounds on** (only audible in the Anime theme), all six modules on at equal widths

## v0.15 — your six field fixes + a colour-map fix

**A. Spectrogram reaches red now**
- [ ] Play a loud song at the trim where the other meters peak properly (don't touch the trim for this). The **spectrogram** should now push into **red** in the loud frequencies — it has its own sensitivity mapping now (strong bins ≈ red, quiet parts stay dark/cool), independent of the other modules.

**B. Spectrum tilt (Pro Q style)**
- [ ] Play **white noise** (any generator/YouTube "white noise" works). At the default **Tilt** the spectrum should read **roughly flat**; now drag **Settings → Spectrum → "Tilt – Display Slope"** down to **0** — the left-heavy slope comes back. Back to 4.5 = flat-ish. (This tilt only changes how the spectrum *draws* — it doesn't touch the audio or any other module.)

**C. Oscilloscope window control**
- [ ] **Settings → Oscilloscope → "Window ms"**: drag it up toward 200 — much more of the waveform comes into view; down toward 10 zooms into a tiny slice. Default is now 100 ms (the old fixed view was ~43 ms).

**D. Loudness meter is vertical now**
- [ ] The **Loudness** module shows **L and R as vertical bars** (bottom = quiet, top = loud), with the big live number **right beside the bars**, moving with the sound. Peak ticks sit on each bar.

**E. Modules resize by dragging**
- [ ] Hover **between two modules** — the cursor changes and there are faint grip dots. **Drag the divider**: the two neighbouring modules trade width. Make the Spectrum huge, the Waveform tiny, whatever you like. (Widths save into presets when you save one; the window's own height still sets all module heights together.)

**F. Stereo meter is a proper vectorscope now**
- [ ] The **Stereometer** is now an L-vs-R vectorscope (the Ozone Imager look): a mono-ish source draws a **diagonal line**, wide stereo spreads off the diagonal. No more surround-style display. Correlation + balance numbers sit under the plot.

**G. Colour Map selector — FIXED (it changed nothing before)**
Honest story: the selector's setting only fed ONE hidden path (the spectrum's "Color Map" colour-by mode, which isn't the default). The spectrogram stopped listening to it back in v0.14, so on your normal setup switching maps changed literally nothing. Now the spectrogram (and the spectrum's Color Map mode) are driven by the selected map again, and there's a new default map, "Midnight Kids", which is exactly the palette you've been looking at — so nothing changes until you pick a different one.
- [ ] On **Midnight Kids** theme, play a song, then **Menu → Color Map** and click through Plasma → Rainbow → Inferno → Viridis → Magma → Turbo → Midnight Kids: the **spectrogram colours visibly change every time**.

**H. Pick a specific OUTPUT as the source (headphones / TV / speakers) — FIXED**
The trap: an old stock "Choose Input Device..." dialog in the menu could only ever list **microphones** (it was an input-only component) — it's **gone now**. Every source picker is the unified one: **Test Tone / Input Devices / Output Devices**, and picking an output captures what's playing through THAT exact output.
- [ ] Play music through your **headphones** (or TV/HDMI speakers). Open **Settings → Audio Source**: under **OUTPUT DEVICES** pick the headphones by name. Meters should dance to the music — with the mic irrelevant (tap the mic: nothing should change).
- [ ] Same check via **Menu → Audio Source → Output Devices**: pick a *different* output (e.g. your speakers); the meters should now follow what's playing through *that* one. The active one is ticked in the list.
- [ ] With an output picked, press **Rec**, let it run ~10 s, **Stop**, and play back the WAV from Documents\MeterBar: it must contain the **music**, not room sound from the mic. (Recording always follows whatever source is active — same path as the meters.)
- [ ] If an output you expect is missing from the list: the Settings panel now prints **"Loopback check: Windows reports N active output device(s)"** right under the picker, and the **Debug window** shows "Render endpoints found". If N is 0 or your device isn't in the list, Windows isn't reporting it as active — check Sound settings. Send Milo a screenshot of that line and he can tell which side the problem is on.

## v0.16 — your recipe as the Default, mastering readouts, and Anime Mode

**A. Fresh state opens in "Default" (your recipe)**
- [ ] Find your saved settings: rename the preset folder `Documents\MeterBar\Presets` to `Presets-old` (this just hides your saves — nothing is deleted; your own normal use is NOT affected by this update). Launch the app: it should open as the **"Default"** preset — Midnight Kids theme, **Clean Dark** background, spectrum Colour By = Level with Tilt 4.5, wide scope window, LUFS Momentary loudness, beat pulse on, all modules on at equal widths. Rename the folder back when you're done.

**B. Background Styles (Settings → Appearance)**
- [ ] Switch **Background Style** through **Clean Dark** (subtle near-black blue-grey gradient), **Midnight Kids** (colourful violet/plum gradient), **Flat** (the old solid colour), **Pastel** (soft pink). The Background Opacity slider works the same under every style.

**C. Loudness pack (mastering readouts, beside the vertical bars)**
- [ ] Play a mastered track end-to-end-ish (at least ~30 s). Beside the bars you now get **Integrated LUFS**, **Short-Term LUFS**, and **True Peak (dBTP)**, plus two target lines: **"Spotify/YT −14: at target / over by X / under by X"** and **"Apple Music −16: …"**, read against the Integrated value. Sanity: a commercial master should read roughly −14…−8 LUFS integrated with true peak around −1…0 dBTP. (Integrated restarts when you switch source — it's per-session.)

**D. Chroma spectrum + 1/6-octave smoothing (Settings → Spectrum)**
- [ ] Set **Colour By → Chroma** and play a piano or synth line: notes are coloured **by pitch class** — every A is the same hue in every octave, neighbouring notes are neighbouring hues. A chord lights up several colours at once.
- [ ] Toggle **1/6-octave smoothing**: the spectrum curve gets visibly calmer/less spiky (display only — nothing else changes).

**E. Beat Pulse (Settings → Appearance)**
- [ ] Play something drum-heavy: the module borders/backgrounds **breathe a soft glow on each hit** — subtle on purpose. Toggle **Beat Pulse** off: glow gone, meters unchanged.

**F. Stereo numbers**
- [ ] Under the vectorscope: **corr** now sits next to a **width %**. Play a mono source (or sum to mono): corr ≈ **+1.00**, width ≈ **0%**. A wide stereo mix: width climbs.

**G. Anime Mode (Menu → Theme → Anime)**
- [ ] Pick **Anime**: pastel pink/lavender look, the background switches to the soft **Pastel** gradient, and **she appears** in her own cell at the **end of the strip** (after the Spectrum) — drag her left edge to make her bigger or smaller, like a module.
- [ ] Play a drum-heavy song: she **bobs on the hits** and flips to the **excited face** when it gets loud (or right after a big hit); quiet parts = calm face.
- [ ] Click the toolbar buttons: **soft pops** (quiet, UI-level). Load a preset: a little **sparkle**. Switch the theme back to **Midnight Kids**: mascot **gone**, clicks **gone** — completely.
- [ ] Settings → Appearance has **"Cute click sounds (Anime theme only)"** if you want her silent but visible.

## Sharing it — give MeterBar to friends (new in v0.15)

You can now hand MeterBar to a friend as **one file** they double-click. No compiler, no scripts, no Visual Studio on their side.

1. On YOUR PC, double-click **`build-release.bat`** (in this kit folder). It builds the release version and stages it.
2. The first time only, it will ask you to install **Inno Setup** — a free, standard installer-maker — from **https://jrsoftware.org/isdl.php**. Install it once, then run `build-release.bat` again.
3. You get **`dist\MeterBar-Setup.exe`**. Send that one file to your friend.
4. They double-click it → normal installer (Program Files, Start Menu entry, optional desktop shortcut, clean uninstall via Windows Settings). Done.

Honest caveats, so nobody is surprised:
- **The exe is unsigned**, so on first run Windows shows a blue **"Windows protected your PC"** SmartScreen box. That's normal for free unsigned software: your friend clicks **More info → Run anyway** and it never asks again.
- The release build still runs **on your PC** (one click, same as your test builds) — Milo's Linux machine can't produce the Windows binary, so there's no way around that step. Your friend never builds anything.
- The app shows a tiny **"Made with JUCE"** splash for ~1.5 s when it starts (also in Help → About). That's the attribution the free JUCE license asks for when giving the app away — it's supposed to be there.
- The installer needs nothing else: the app is built so it doesn't need the Visual C++ Redistributable on your friend's machine. (If that ever changed, the installer script already knows how to bundle and silently install it — Milo would tell you to drop one extra file next to the build.)

## v0.14 — the REAL loopback fix + full-spectrum colour everywhere

**A. Loopback on the Realtek 2nd output (was: silent in every version so far)**
What was actually wrong — and it's a bigger mea culpa than v0.13's note: the whole Windows
loopback section of the program was never inside the app at all. That part of the code is
fenced off behind an on/off marker that only exists if the JUCE library's headers have been
loaded into the same file first — and that file loads none of them, so the marker read "off"
in every build, including on Windows, and a placeholder stub shipped instead: picking an
output device did nothing, the endpoint list was empty, and the Debug lines read blank.
v0.13's "locks on by endpoint ID" code was real, but it sat inside the fenced-off section —
and my v0.13 compiler check forced the marker on, so it proof-read code your build never
contained. v0.14 fences that section with the compiler's own Windows marker instead (every
Windows compiler sets it itself), and this time the check ran WITHOUT forcing anything:
under the exact settings your build uses, the old file contains zero loopback code and the
new one contains all of it. One more fix in the same path: asking Windows for the endpoint
list no longer unbalances the app's COM bookkeeping.
- [ ] Play music so it's coming through the **Realtek HD Audio 2nd output** (your headphones). Settings → Audio Source → Sound Source → pick **Realtek HD Audio 2nd output** under OUTPUT DEVICES. **Meters must move**, in time with what you hear.
- [ ] Menu → **Show Debug Window** and check: **Loopback wanted** = the Realtek 2nd-output name, **Loopback id** = a long `{…}` endpoint ID, **Loopback device** shows the same name with **(via endpoint id)**, **Loopback packets** counting UP past 0, **Loopback status** = `running`, **peak decoded** > 0.
- [ ] Also try Menu → Audio Source → Output Devices → the same entry.
- [ ] Your other sources should be untouched: mic input still works, Test Tone still works.
- [ ] **If it's still silent:** 📸 send me the Debug screenshot again. New honesty rule: if loopback was requested but isn't capturing, the status line now starts with **NOT CAPTURING -** and names the stage that failed, instead of sitting blank while the Source line implies it's live.

**B. Full-spectrum colour in every trace module (new)**
One colour language everywhere now — quiet = violet/teal, loud climbs green → yellow →
orange → red:
- [ ] **Spectrogram**: play something dynamic. It should sweep the whole palette (cool colours for quiet parts, hot for loud) instead of the old all-red/maroon wash. Silence/quiet stays dark.
- [ ] **Waveform**: loud columns should run hot (orange/red peaks), quiet columns cool — colour follows each column's peak, not one flat orange.
- [ ] **Oscilloscope**: loud passages should visibly redden; quiet wiggles stay teal/green.
- [ ] **Spectrum → Colour By → Level**: a loud tone should no longer paint the whole thing pink/red — the peak goes red, but mid levels should show green/yellow and quiet parts violet/teal.
- [ ] Frequency / Blend / Color Map (classic) modes still there — tell me which Level look you prefer as the default.

**C. Transparent backgrounds (from v0.13 — still worth a pass)**
- [ ] Settings → **Appearance** → **Background Opacity**: drag it down toward ~40%. The module backgrounds (behind the oscilloscope, spectrum, meters…) should go translucent and your desktop should show through a little; the traces, bars and text stay solid. Drag back to 100% = the old solid look.
- Note: this is translucency, not a blur — the desktop shows through as-is, not frosted. A real backdrop blur needs Windows-specific tricks outside this app's window system; say the word if you want me to attempt it separately.

## Still open from v0.12 — worth a pass if you haven't yet

**D. Record → Stop (was: freeze/crash)**
- [ ] Pick any source with sound, click **Rec**, let it run ~10 s, click **Stop**. The app must stay responsive and the button must flip back to **Rec**.
- [ ] Repeat 3 times in a row. Then play the WAVs in `Documents\MeterBar\record-*.wav` — they should sound like what was playing.
- [ ] While recording, switch the sound source (Menu → Audio Source → pick something else). New behaviour: the take ends cleanly instead of recording the new source at the wrong speed — button returns to **Rec**. That's intended, not a bug.

**E. Window drag / resize (was: mouse stays grabbed)**
- [ ] Drag the window by the top bar (left mouse button, anywhere that isn't a button). Release — the mouse must be free immediately.
- [ ] Drag each window edge to resize a few times. Mouse must never stay grabbed.
- [ ] **Right-click** the top bar: the window must NOT move (only the left button drags).

## Slice 1 — It lives (Test Tone, no audio setup needed)
The app should open as a dark bar with colourful module headers, already on the **Test Tone** source. (v0.12's new default look is the **Midnight Kids** theme — deep dark background, saturated teal/green/yellow/orange/pink accents; each module's title wears its own colour.)
- [ ] All 6 modules visible: Spectrogram, Waveform, Loudness, Stereometer, Oscilloscope, Spectrum
- [ ] Every meter is **moving** (test tone = 220+440 Hz swell)
- [ ] Spectrum shows clear peaks; Loudness shows a number around −15 to −5
- [ ] Drag the **top bar** (anywhere that isn't a button) — the whole window moves
- [ ] **_** minimises, **Quit** closes
📸 Send: one screenshot of the whole bar.

## Slice 2 — Your real audio
- [ ] Menu (☰) → **Audio Source → Choose Input Device…** → pick your USB mic/interface → set source to **Input**, clap/talk: meters react
- [ ] Set source to **System Loopback (Windows)**, play Spotify/YouTube: meters react to the music
- [ ] Play a reference track and watch Spectrum vs a Spotify tab — roughly similar shape?
📸 Send: screenshot while music plays + tell me which source worked (Input / Loopback / both / neither + any error text).

## Slice 3 — Shell features
- [ ] **Preset → Save As** "Test1" → change a setting → **Preset → Test1** reloads it; **Open Preset Folder** opens Explorer
- [ ] **Reposition → At the Bottom**, then **Stick**, then **Pin Above Other Windows** (open Ableton over it — bar stays on top with Pin on)
- [ ] **Reposition → Move to Display** (if you have 2 monitors)
- [ ] **Pause** freezes the meters; **Resume** restarts them
- [ ] **Rec** records; play the WAV from `Documents\MeterBar\record-*.wav`
- [ ] Menu → **Audio Capture → Clip Last 10 Seconds** → check `Documents\MeterBar\clip10s-*.wav`
📸 Send: just tell me which of these failed, if any.

## Slice 4 — Settings window (the big one)
Click **Settings**. For each panel, move one control and check the meter changes:
- [ ] Spectrogram: Orientation Horizontal→Vertical flips it; Fade slider changes trail brightness
- [ ] Waveform: Channel 1 → Side (should go quiet-ish on mono-ish music); History → Fast
- [ ] Loudness: cycle all 6 modes — numbers change, none say NaN or freeze
- [ ] Stereometer: Lissajous mode; Point Size slider (uncheck Auto first)
- [ ] Oscilloscope: Pitch vs Free; Multi vs Single
- [ ] Spectrum: FFT Size 1024→16384 (peaks get sharper); Smoothing 90→0 (jumpy); Tilt −9→+9 (shape tilts); Colour By cycles Level/Frequency/Blend/Color Map
- [ ] Appearance: Background Opacity 100→40 (desktop shows through the module backgrounds) → back to 100
- [ ] Audio Source: Trim +12 dB — everything gets louder/higher; back to 0
- [ ] Performance: FPS 15 (visibly choppy) → 60; the **Enabled** toggle on any module hides/shows it
📸 Send: screenshot of the Settings window + any control that did nothing.

## Slice 5 — Look & debug
- [ ] Menu → **Theme** → cycle Midnight Kids (new default) / Midnight Pink / Deep Teal / Daylight
- [ ] Menu → **Color Map** → Plasma → Rainbow: spectrogram colours change
- [ ] Menu → **Show Debug Window**: note the sample rate + UI fps it reports
📸 Send: the debug numbers + which theme you like best.

## Known v1 limits (not bugs — tell me if any of these matter to you)
- WASAPI loopback uses the first stereo pair of the render device.
- Background translucency (Appearance → Background Opacity) has no backdrop blur — the desktop shows through unfrosted.
- VSync toggle is stored in presets; the Windows software renderer doesn't expose a real vsync switch (fps limit is the effective control).
- No DAW plugin (Audio Server) yet — that's Phase 4 in SPEC.md.
