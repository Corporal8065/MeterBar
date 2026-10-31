# MeterBar — REQUESTS (per-item status ledger)

Verification rule (Jackson, 2026-10-29): every item is DONE only with
proof — the full DSP suite green after the change, new checks for new
behaviour, and errors recorded here with their actual text, never hidden.

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
