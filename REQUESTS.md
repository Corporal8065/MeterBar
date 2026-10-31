# MeterBar — REQUESTS (per-item status ledger)

Verification rule (Jackson, 2026-10-29): every item is DONE only with
proof — the full DSP suite green after the change, new checks for new
behaviour, and errors recorded here with their actual text, never hidden.

## v0.21.5 (2026-10-31) — Visualizer round: melted blur, calmed whites, Lines style (Jackson's asks)

Jackson (2026-10-31 14:17 PT, with reference frames at 14:32 PT):
(1) background blur increased so the colours smudge together into
a wash — the dots should melt into each other, not read as
distinct dotted circles; (2) the white bulbs calmed — clusters of
near-opaque grey-white discs are the loudest element in the bar;
give them the same movement strength/smoothing as the colourful
dots so they sit at the same energy as the rest of the field;
(3) a NEW "Lines" visualizer style alongside Bubbles (not
replacing it): smooth, hand-drawn-feeling flowing lines/ribbons,
one per frequency band (low/mid/high), spring-smoothed, thickness
swelling with the band's level, near-still when quiet.

- **Blur melt — DONE (all three bars, identical).** The composite
  kernel goes 7/2.8 -> 15/5.5 and the crisp pass backs off
  0.45 -> 0.30 (drawVisualizerBackground), so the blurred wash is
  what reads and the dots smudge into it.
- **Whites calmed — DONE (all three bars, identical, core
  Visualizer.hpp).** Three changes to the ambient convergers:
  velocity ease-out 0.995 -> 0.985 per frame (the burst dots' own
  value — they streamed hotter and piled up at the centre);
  brightness envelope gains the burst dots' fade-OUT over the
  last 30% of life (they used to sit at full brightness until the
  respawn pop); peak capped at kAmbientPeak = 0.55 of full white
  (they rendered at a full 255). The silence gate is untouched:
  no energy, no whites. New checks pin the cap and the fade-out;
  the v0.20.3 gate check's brightness threshold was retuned to
  the new scale (intent unchanged, recorded in the test).
- **Lines style — DONE (all three bars, identical).** New pure
  core model VisualizerLines (Visualizer.hpp): three ribbons at
  the burst anchors, each band's level a damped spring
  (k=42, d=9) toward the band energy, centreline = two detuned
  sines + a baked per-point wobble (hand-drawn, never a machine
  sine), half-thickness swelling with the level and breathing
  along the line; phase creep and amplitude collapse when quiet
  (near-still), and the particle field's activity gate fades the
  ribbons out in silence. Rendered as filled paths through the
  same group field map + blur composite as the dots. Selectable
  beside the existing visualizer controls (MeterBar: "Visualizer
  style" combo next to the toggle in Settings -> Appearance;
  siblings: beside "Background effect"), persisted like the other
  appearance settings (MeterBar PresetIO "vizStyle"; siblings'
  SettingsStore "vizStyle").
- **Link carrying — the style is shared background state, carried
  like the effect.** BLK4 (magic bumped in all three cores; slot
  layout unchanged, the bgEffect ordinal's domain widened):
  1 = visualizer Bubbles, 3 = visualizer Lines. A style change in
  any linked bar adopts in the others with the effect, so the
  linked strip never mixes Bubbles on one bar with Lines on the
  next. MeterBar publishes 1/3 by its style and adopts 1/3 as
  visualizer-on with that style (a peer's glow, 2, still lands as
  visualizer-on, style untouched — MeterBar has no glow).
  Mixed BLK3/BLK4 binaries never link (the family's magic rule).
- Honest limit: the LOOK is verified only on Jackson's PC. The
  evidence here is the code, the checks, and his frames.

## v0.21.4 (2026-10-31) — Arrange takes maximized windows; Recordings submenu (Jackson's asks)

Jackson (2026-10-31 15:00 PT, verbatim): "the arrange does not
work when the application is already meximized" and "also add a
section in the menu drop down to view all recordings".

- **Arrange + maximized — DONE (all three bars, identical).** Root
  cause, in the code: the EnumWindows callback skipped IsZoomed
  windows outright, so a maximized app was never arranged. Now a
  zoomed window participates through its NORMAL rect
  (GetWindowPlacement rcNormalPosition): the core plan
  (planArrangeWindow in Arrange.hpp) fits that rect, the window is
  snapshotted BEFORE being touched (normal rect + wasMaximized,
  write-once as ever), un-maximized (SW_RESTORE), then fitted.
  Restore honours the flag (planRestoreWindow): snapshot rect
  applied, then SW_MAXIMIZE — never stranded restored-down. The
  v0.18.1 invariants stand: one-shot per press, write-once
  snapshots, GUI_INMOVESIZE windows still skipped. (The Settings
  Window note that said Arrange skips maximised windows is
  updated.) Windows TU verified by the MinGW syntax+type check;
  real AppBar/arrange behaviour is field-verified on Jackson's PC.
- **Recordings submenu — DONE.** Hamburger menu gains a
  "Recordings" submenu beside Audio Capture, rebuilt from a fresh
  scan of Documents\MeterBar on every open: every record-*.wav
  take plus clip*.wav clips (labelled "Clip — "), newest first,
  filename + file size, capped at the 25 newest, with an "Open
  Recordings Folder" item; empty folder shows a disabled "No
  recordings yet". Clicking an entry opens it with the OS default
  handler. The take currently being written is omitted
  (MainComponent now tracks it in recordingFile_; the filter/sort/
  cap logic is core planRecordingsMenu, suite-graded).

## v0.21.3 (2026-10-31) — visible Link background toggle + effect in the packet (Jackson's ask)

Jackson (2026-10-31 14:57 PT, verbatim): "still need a visual UI
way of changing the size of each thing thats going by for the
ticker bar and to link th ebackground". (The Sizes half is
TickerBar-only, v0.5.6.)

- **Link background — DONE (with TickerBar v0.5.6 / SessionBar
  v0.4.5).** Diagnosis and protocol fix as in TickerBar's entry:
  style/opacity already propagated; the EFFECT selection was not
  in the packet, and no visible control existed. LinkSlot gains
  bgEffect (appended), magic BLK2 -> BLK3. MeterBar has no 3-way
  effect: it publishes visualizerEnabled as effect 1/0 and adopts
  effect != 0 as visualizer-on (a peer's glow lands as the
  visualizer — MeterBar's only effect). Visible control: a
  persisted "Link background" toggle (state.linkBackground,
  default ON) in Settings > Appearance under the Visualizer
  toggle; the client gates publish + adoption on it, and turning
  it on re-stamps the current background for peers.

## v0.21.2 (2026-10-31) — GitHub link in Settings; Reserve moves into the Reposition menu (Jackson's asks)

Jackson's asks (2026-10-31 14:13 PT, paraphrased faithfully): (a) a
GitHub link inside each bar's Settings, next to the version display,
opening the app's own repo; (b) all three bars end up with the same
Reposition dropdown (MeterBar's is the template); (c) the standalone
Reserve button leaves the button row and becomes a ticked "Reserve
Screen Space" item in the Reposition dropdown, toggling the same
state through the same path, with existing users' settings carrying
over untouched.

- **GitHub link — DONE.** MeterBar's Settings had no version display
  at all, so this adds an About panel (after Performance): the
  version line composed from the generated single-source
  `meterbar::kVersionString`, plus a GitHub button that opens
  https://github.com/Corporal8065/MeterBar in the default browser.
- **Reserve into the dropdown — DONE.** `reserveBtn` is deleted
  (member, ctor wiring, toolbar bounds, visibility lists, and the
  syncScreenSpace toggle mirror). The Reposition menu gains a ticked
  "Reserve Screen Space" item (new idReserve) that flips
  `engine.state.reserveScreenSpace` and calls `syncScreenSpace()` —
  exactly what the button did. Same persisted key, so a user who had
  Reserve on still has it on. The button row closes up in resized()
  (Arrange | Restore now sit directly left of Link/Preset). The
  Settings Window-panel Reserve toggle and its note now point at the
  menu item instead of the removed button.
- **Reposition menu** — unchanged here (it is the template the
  siblings were ported to).

## v0.21.1 (2026-10-31) — link applies get a visibility floor (Jackson's "stuck somewhere i cannot see" report)

Jackson's report (2026-10-31, #crypto-book, about SessionBar —
same defect here by construction): "Session bar is also for me
still stuck somewhere i cannot see upon load up and have no idea
how to fix plz help".

Root cause: the v0.20.1 clamp guards the two places a SAVED rect
is applied — the startup restore and preset apply. It never
covered the family LINK: with Link on, stack plans, drag follows,
snap settles and group docks move the bar AFTER startup through
the link client's setBounds callback, unguarded. A stale stack
request in the shared block, or a peer docked against space no
monitor owns any more, could move the just-restored bar off-screen
within a tick of launch — and the position-save feed then saved
the off-screen rect, so every later launch restored (clamped) and
re-lost it the same way.

- **Visibility floor in core WindowGeom — DONE.** New pure rule,
  identical in all three repos: `rectMeaningfullyVisible` (at
  least 100x40 px of the rect inside ONE display work area — the
  rect itself when it is smaller than that) and
  `ensureRectVisible`. A meaningfully-visible rect — a bar on a
  second monitor, a straddle — is returned untouched; below the
  floor the rect keeps its size and lands on the primary work
  area, centred horizontally, near the top. 12 new checks: fully
  off-screen rescued, second display untouched, above-floor
  partial untouched, sliver and sunk rects rescued, straddle
  untouched, no-areas no-op. Suite 1006 → 1018 checks, 0 failures.
- **Link setBounds funnel guarded — DONE.** This bar's
  `linkClient_.setBounds` callback — the single funnel every
  remote apply passes through — now runs its target through
  `ensureRectVisible` with the live displays' user areas before
  touching the window. The fix landed first in SessionBar v0.4.2
  (the reported bar) and is ported identically here and in
  TickerBar v0.5.2.
- **Version 0.21.0 → 0.21.1** in the single-source place: root
  CMakeLists.txt. Proof: full DSP suite green at 1018 checks, 0
  failures; the edited app TU passes a full g++ type-check vs
  JUCE 8.0.4. Windows CI + Jackson's eyes remain the visual proof.

## v0.21.0 (2026-10-31) — one linked background across the bars (Jackson's ask)

Jackson's words (dictated, 2026-10-31): "the addition to MeterBar
where we change the background graphics needs to be applied to
not only MeterBar but when you connect the other 2 softwares to
all link and instead use the combination of the 3 resolutions as
the sizing for it, and then when sound is played with specific
frequencies and or transients that we have set up will do its
thing."

- **Group field map — DONE in code, core-proven.** New pure core
  in `BarLink.hpp` (identical in all three repos): `fieldMap` /
  `fieldToLocal` / `fieldDotVisible`. When the bars are linked,
  the visualizer's unit field spans the GROUP rect — the union of
  the linked bars' screen rects, i.e. the combination of their
  resolutions — and each bar renders only its own slice of it.
  Bursts anchor at the centre of the COMBINED strip and the
  ambient convergers stream toward that same centre across the
  seams. 17 new checks pin the seam arithmetic, the three-bar
  middle third, vertical stacks, degenerate-rect fallback and
  dot culling. Suite 989 → 1006 checks, 0 failures.
- **Render — DONE in code.** This bar is the audio source: its
  visualizer (driven by its own spectrum, the same bands it
  broadcasts over BarLink) now maps through the field map in
  `drawVisualizerBackground`. Unlinked (or linked but alone) the
  map is the bar-local identity and the v0.20.3 render runs
  unchanged; dot radius is deliberately NOT remapped — physical
  dot size is preserved, only positions come from the group.
  The edited translation unit passes a full g++ syntax/type
  check against the JUCE 8.0.4 headers (0 errors). The Windows
  CI build after push, and Jackson's eyes on the linked strip,
  are the remaining proof — nothing visual was verified here.
- **Siblings.** TickerBar v0.5.0 and SessionBar v0.4.0 render the
  same field from this bar's broadcast; SessionBar also received
  the feel-pass render treatment it missed in the v0.4.0 round
  (its own REQUESTS record).

## Landed on main 2026-10-29 (SessionBar v0.2.1 fix round — no MeterBar bump/tag tonight; releases tomorrow)

Shared-core corrections from Jackson's SessionBar field test, applied
identically in all three repos:

- **Group-dock cross-axis alignment:** `groupDockRect` moved a
  stacked follower along the dock axis only, so a bar stacked under
  a sideways-moved peer kept its own x and no longer sat under it.
  Stacking now aligns left edges (top/bottom stacks) / top edges
  (left/right stacks). New core checks pin it.
- **Background sync hardening:** the publish/adopt decisions moved
  into the pure core (`BgSyncState`, `bgIsLocalChange`,
  `bgAdoption`) with tests, and the app client was rewired onto
  them. Defects fixed from the v0.2.0 client: a follower echoing an
  adopted background back as its own change (bgSeq bump), the
  settings feed force-publishing (bgSeq bump) on every interaction
  whether or not the background changed, and adoption being
  postponed indefinitely behind a live peer drag. **Plus a
  MeterBar-only host defect: MeterBar never called
  `notifyLocalSettings()` — its background publishes rode on the
  link client's 30 Hz poll alone. The settings onChanged now calls
  it, like the other two bars.** Core suite: 926 → 949 checks,
  0 failures. The live two-bar field test is still the end-to-end
  proof (Link must be ON in both bars).

## v0.20.0 (2026-10-29)

- (i) **Exact spectrum frequency mapping everywhere — DONE.** The
  spectrum view already used the core's `freqToX` walk (since v0.12);
  the SPECTROGRAM still indexed bins linearly, so its frequency axis
  disagreed with the spectrum's under Log/Mel scales. Fix: the walk
  now lives in the core as `SpectrumAnalyser::binAtX` (single source
  of truth), and the spectrogram builds its frequency-axis LUT from
  it, honouring the selected FFT scale. Proof: core checks for all
  three scales — edges, monotonicity, and freqToX round-trips.
- (i) **Oscilloscope Multi = true 3-band split — DONE.** The old
  Multi redrew the same left channel faintly over the main trace
  (one signal twice). Now: core `splitScopeBands` (cascaded one-pole
  crossovers at 250 Hz / 4 kHz; bands sum back to the input exactly),
  computed per frame in MainComponent and drawn as three labelled
  lanes (LOW / MID / HIGH). Proof: core checks — 100 Hz / 1 kHz /
  8 kHz sines each dominate the expected band; reconstruction error
  < 1e-5.
- (l) **Visualizer background mode — DONE (build); NEEDS JACKSON'S
  VISUAL FIELD TEST for tuning.** Pure-core model
  (`dsp/include/meterbar/Visualizer.hpp`, deterministic, hard cap
  600 particles): white particles converge on the centre; per-band
  energies (core `spectrumBandEnergies`: <250 / 250–4k / >4k Hz)
  crossing the trigger burst rainbow brush strokes at band anchors
  that expand and dissolve into their dots. Rendering: half-res
  image (capped 480×270), Gaussian-blurred glow layer + crisp dots,
  black base, behind the modules. Settings → Appearance: toggle
  (default OFF) + base hue / saturation / palette spread / cycle
  period sliders, all persisted in presets. Proof: core checks for
  the cap (asserted every frame of a 600-frame loud run), ambient
  convergence (per-particle inward motion), burst triggering,
  band-energy attribution, and palette rules (saturation 0 = grey,
  ambient always white, hue wrap).
- (j) **UI parity — N/A as a change:** MeterBar is the reference the
  other bars converge to; this release instead exports its mapping
  and reserve conventions (already shared) and gains the family
  REQUESTS ledger.
- (k) **Inter-app linking — DONE (build); NEEDS A LIVE TWO-BAR FIELD
  TEST.** Pure core `meterbar/BarLink.hpp` (protocol + snap /
  follow / group-dock geometry, unit-checked) + app-layer
  `BarLink.h` (Windows named shared memory `Local\BarFamilyLinkV1`;
  honest stub elsewhere) + on-bar **Link** toggle (persisted in
  presets as `linkEnabled`, default OFF). A linked follower stacks
  against this bar's reserve-docked edge; background style + opacity
  propagate across linked bars. Proof: DSP suite geometry checks +
  app TU syntax check; live multi-process behaviour needs two bars
  on Windows — field test still owed.

## Errors recorded during this work

- Three of the new DSP checks initially failed against my own test
  expectations, not the code: binAtX(0) returns the last sub-minF bin
  (the view LUT's exact semantics — assertion corrected to pin THAT),
  a round-trip bin above the 20 kHz display max clamps by design
  (test bins moved inside the range), and the visualizer's population
  mean distance is stationary BY DESIGN (converged particles respawn
  at the rim; the test now measures per-particle inward motion).
  All recorded here rather than silently adjusted; final suite:
  913 checks, 0 failures (271 at HEAD — the +642 includes 600
  per-frame cap assertions in the loud-run loop).
- The full app build shares this VM's environment gap (no
  libxrandr-dev for JUCE's nested juceaide build); the changed app
  TUs were verified with `g++ -fsyntax-only` against JUCE 8.0.4 —
  clean after one real fix (JUCE 8 has no `Image::copyImage`;
  `duplicateIfShared()` is the correct call). CI builds the app on
  windows-latest.
