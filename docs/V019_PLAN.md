# MeterBar v0.19 — Prepared Plan (NOT built yet)

Prepared 2026-10-28 ~18:53 PT from Jackson's three-part burst:
(1) GitHub + auto-commit + a working Check For Updates / Update button,
(2) Settings is blinding white — make it pink,
(3) Fi (Fiona) wants a second app to fill the space on her monitor above the MeterBar strip.

Status: **plan only.** No GitHub repo created, nothing pushed/posted, no source edited,
no build started. Current shipping state this plan builds on: v0.18 kit delivered
(`~/workspace/your_files/meterbar-test-kit-v0.18.zip`, 18:45 PT). The app in
`~/workspace/meters-app` is **not a git repo yet**; `gh` is installed on the VM but is
**not logged into any GitHub host**.

---

## 1. Settings pink fix — the blinding-white culprit

### 1.1 What is actually white, and why (verified in source)

The Settings window's background is **never painted by the Settings code itself**:

- `SettingsContent` (`app/Source/Settings.h:9`) has **no `paint()` override at all**, and
  its control-builder helpers — `combo()` (:211), `slider()` (:219), `toggle()` (:226),
  `buttonRow()` (:232), `note()` (:239), `soundSourceCombo()` (:134) — set **no colours**
  on anything they create. The only colour set anywhere in Settings is the panel-header
  text (`Settings.h:116`).
- The visible window background is therefore whatever the `DocumentWindow` was last told:
  `MainComponent::applyTheme()` runs `settingsWin->setBackgroundColour(theme.panel)`
  (`app/Source/MainComponent.cpp:81`) on every theme change. **Settings inherits the
  main bar's theme panel colour wholesale:**
  - **Anime theme → panel `0xfffdf3f8`** (`app/Source/Theme.h:33`, bg `0xfff6e3ee`) —
    near-white pink. This is the blinding sheet.
  - **Daylight theme → panel `0xffffffff`** (`Theme.h:39`) — **pure white**.
  - Selecting Anime also force-switches the background style to **Pastel**
    (`MainComponent.cpp:626`), whose gradient is `0xfffdeef5 → 0xfff6d3e4`
    (`Theme.h:71`) — the same near-white family.
- Two further theme-independent problems, present **even on the default Midnight Kids
  setup** (whose own panel, `0xff231f31` at `Theme.h:46`, is correctly dark):
  1. **The app installs no custom LookAndFeel — zero `setLookAndFeel` calls in
     `app/` or `dsp/`.** Every Settings control is stock JUCE `LookAndFeel_V4`, so
     labels, tick marks, combo text and arrows render in V4's dark-scheme
     `defaultText` = **`0xffffffff` pure white**, combo/slider/toggle surfaces in
     V4 slate (`widgetBackground 0xff263238`, window `0xff323e44`), and nothing in
     Settings ever applies the active theme's own `text` colour — the light themes
     even define dark text (`Anime 0xff5a4356`, `Daylight 0xff181824`) that Settings
     never uses. Settings is a half-themed window: background from one place,
     controls from JUCE stock.
  2. **The native title bar.** `SettingsWindow` uses a native title bar
     (`Settings.h:251`, `setUsingNativeTitleBar(true)`), and nothing in the app ever
     sets `DWMWA_USE_IMMERSIVE_DARK_MODE` (the only `dwmapi.h` use is
     `DWMWA_CLOAKED` in `ScreenSpace.cpp`). On Windows with the default light app
     mode, that frame + title bar render white regardless of theme. The Debug window
     has the identical issue (`MainComponent.cpp:61`).

**One-sentence culprit:** Settings has no background or control colours of its own —
it inherits `theme.panel` via `MainComponent.cpp:81`, which is near-white
(`0xfffdf3f8`) in Anime and pure white (`0xffffffff`) in Daylight, and even on dark
themes its controls and Windows title bar fall back to unthemed JUCE/Windows whites.

### 1.2 The fix (exact changes, when the build is green-lit)

Decouple Settings from the bar theme entirely — Settings always uses one fixed,
soft, muted pink-dark palette, in every theme:

| Role | Colour | Used for |
|---|---|---|
| Settings base | `#2A1E2E` (0xff2a1e2e) | Window/content background fill |
| Panel / control surface | `#3A2438` (0xff3a2438) | Combo boxes, buttons, slider text boxes, panel blocks |
| Control well / track | `#241722` (0xff241722) | Slider tracks, scrollbar track, tick boxes |
| Border / outline | `#57304D` (0xff57304d) | Combo outlines, dividers, scrollbar hover |
| Body text | `#F6E3EE` (0xfff6e3ee) | Labels, combo text, toggle text (≈13:1 on base, ≈11.5:1 on surface) |
| Dim text (notes) | `#C9A3BB` (0xffc9a3bb) | `note()` lines (≈7:1 on base — AA pass) |
| Pink accent | `#E04C89` (0xffe04c89) | Slider thumbs, ticks, highlights, header accents (existing brand pink from `Theme.h`; ≈4.2:1 — large/bold/accent use only, never body text) |
| Soft pink | `#E77FB1` (0xffe77fb1) | Combo arrows, secondary accents (≈5.5:1 on base) |
| Popup menu | base `#2A1E2E`, highlighted bg `#E04C89`, highlighted text `#FFFFFF` | ComboBox dropdowns (`PopupMenu` colours) |

Concrete edit sites (no build in this pass — this is the spec for it):

1. `Settings.h` — add `SettingsContent::paint()` doing
   `g.fillAll(juce::Colour(0xff2a1e2e))`. This one method ends the inheritance bug
   for the content area in all themes.
2. `Settings.h` — in each existing builder helper (they already centralise control
   creation, so this is ~6 small sites): `combo()` (:211) sets ComboBox
   background/text/outline/arrow colours + popup menu colours; `slider()` (:219)
   sets background/thumb/track/text-box colours; `toggle()` (:226) sets
   text/tick colours; `buttonRow()` (:232) sets TextButton colours like the
   toolbar buttons already get in `applyTheme()`; `note()` (:239) sets
   `Label::textColourId` to the dim pink; panel headers (:114–116) keep the
   per-module accents (they read well on the new base).
3. `Settings.h:246` — `SettingsWindow` ctor background `0xff231f31` → `0xff2a1e2e`
   so the frame fill matches before/without `applyTheme()`.
4. `MainComponent.cpp:81` — **stop** overwriting the Settings window background
   with `theme.panel`; set the fixed `0xff2a1e2e` once. (Debug window at :80 can
   take the same one-line treatment — recommended, same bug class.)
5. Windows frame: after the Settings (and Debug) windows' peers exist, call
   `DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE /*20*/, TRUE)`
   under `#if JUCE_WINDOWS` (a small helper next to the existing `dwmapi.h` use in
   `ScreenSpace.cpp`), so the native title bar stops rendering white.
6. `Theme.h` (optional polish, Jackson's call — Decision 5): deepen the Pastel
   background style (`Theme.h:71`) from `0xfffdeef5 → 0xfff6d3e4` to
   `0xff4a2c40 → 0xff2a1e2e` so the Anime main-bar background also stops being
   near-white. Anime/Daylight bar panels themselves are left alone — the fix
   above means Settings no longer cares what the bar theme is.

Acceptance check for the build: open Settings in all five themes (Midnight Kids,
Midnight Pink, Deep Teal, Anime, Daylight) — Settings must look identical
(the pink-dark palette) in every one, title bar included, with no white surface
larger than a text glyph.

---

## 2. GitHub + auto-commit

**Recommendation: yes — with a private-first repo, and GitHub Releases (not raw
commits) as the thing the app updates from.**

### 2.1 Repo setup

- `git init` in `~/workspace/meters-app`, first commit = the exact v0.18 source
  state (so history starts from a verified, shipped kit).
- Repo created **private first** under Jackson's GitHub account. Milo does the
  initial commits and the CI shakedown privately; flip to **public** at the first
  release Jackson actually shares (one settings change, history preserved).
  Public is the recommended end state — see §3.3 for why the updater wants it.
- `.gitignore` from day one: `build/`, `build-app/` (includes the fetched JUCE
  `_deps` tree — never commit it; CMake re-fetches JUCE 8.0.4 from its release
  zip, `CMakeLists.txt:30-34`), `dist/`, `*.zip` (test kits stay a local/Discord
  hand-off, as now), build logs (`build*.txt`, `build-release-log.txt`),
  `*.user`, `.vs/`.
- **What Milo auto-commits — source only:** `app/`, `dsp/`, `tests/`,
  `CMakeLists.txt`, `MeterBar.iss`, `build*.bat`, `SPEC.md`, `docs/`, `assets/`
  (source art, not generated media JSON dumps). **Never** binaries, installers,
  zips, or build trees.
- **When:** after a version's existing verification gate passes — DSP test suite
  green + clean Linux rebuild + Xvfb smoke (the same gate every v0.12–v0.18 kit
  already went through). One commit per verified version, plus small
  `docs:`/`fix:` commits between versions if a plan/spec changes on its own.
- **Message convention:** `v0.19: <what changed>` for version commits;
  Conventional-Commits style (`feat:`, `fix:`, `docs:`) for anything else.
  Every commit body names the verification (e.g. "DSP 129/129, clean rebuild,
  Xvfb smoke 14s") so the git log doubles as the build log.
- `BUILD_LOG.md` keeps its job locally; commit messages mirror its headlines.

### 2.2 CI: GitHub Actions builds the Windows installer

The point of the repo is that **nobody has to build on Jackson's PC again**:

- Workflow `.github/workflows/build.yml`, `runs-on: windows-latest` (VS 2022 +
  CMake are preinstalled on that image; JUCE comes from the existing FetchContent
  URL — no git needed for JUCE).
- Steps: checkout → configure (`-DMETERBAR_BUILD_APP=ON -A x64`) → build Release
  → run `test_dsp` → install Inno Setup (`choco install innosetup`) → stage
  `dist\MeterBar.exe` exactly as `build-release.bat` does → `ISCC MeterBar.iss`
  → upload `MeterBar-Setup.exe` as a workflow artifact.
- On an ordinary push, the artifact is just a downloadable build (proof CI is
  green). On a **version tag** (see §2.3), the same job additionally creates a
  **GitHub Release** for that tag and attaches `MeterBar-Setup.exe` — that
  release is the update feed in §3.
- `build-release.bat` stays as the local fallback; CI mirrors it step-for-step
  so a local and a CI installer are the same product.

### 2.3 Versioning — one source of truth

Today the version is hard-coded in **four** places and they will drift:
`CMakeLists.txt:2` (`project(MeterBar VERSION 1.0.0)`), `MeterBar.iss`
(`MyAppVersion "1.0.0"`), `app/Source/Main.cpp` (`getApplicationVersion()`
returns `"1.0.0"`), and `app/Source/MainComponent.cpp:8` (`kVersion = "v1.0.0"`,
also baked into the current update-stub message at `MainComponent.cpp:636-637`).

Plan:
- **Single source:** the `VERSION` in `CMakeLists.txt`'s `project()` line
  (kit versions map to it directly: v0.19 → `0.19.0`).
- CMake generates a `MeterBarVersion.h` (`configure_file`) that `Main.cpp` and
  `MainComponent.cpp` read — deleting both hard-coded strings.
- Installer version passed, not edited: `ISCC /DMyAppVersion=<version>`
  (Inno supports overriding a `#define` from the command line); both
  `build-release.bat` and CI pass it from the same source.
- **Release rule:** a release happens **only** when a git tag `vX.Y.Z` matching
  the `VERSION` file is pushed; CI fails the release job if tag ≠ version.
  **Auto-commit ≠ auto-release:** Milo commits verified work freely, but a tag —
  and therefore anything users' apps can update to — is created only when
  Jackson says "ship it" (Decision 3).

### 2.4 Blockers that need Jackson (see also §5)

- `gh` on the VM is **not authenticated**. Jackson runs `gh auth login` once
  (browser flow, his account) in a session with Milo — Milo never handles his
  password — or creates the empty repo on github.com himself and hands Milo
  the URL after authenticating `gh`. Owner/name (placeholder above:
  `<owner>/meterbar`) comes from his account.
- Public-vs-private end state (Decision 2) — drives the updater design in §3.3.

---

## 3. Updater — Check For Updates + Update (reinstall)

The menu items already exist and currently dead-end in a stub:
Help menu and hamburger menu both carry "Check For Updates"
(`MainComponent.cpp:564`, `:611`), and the handler just shows
*"MeterBar v1.0.0 — this build has no update server configured. You are on the
version you built."* (`MainComponent.cpp:636-637`). This section replaces that
stub; no new UI surface is needed beyond one extra button in its dialog.

### 3.1 Check For Updates

- On click: background-thread GET (never block the UI thread; a small
  `juce::ThreadPoolJob` / `URL::createInputStream` off-thread) to
  `https://api.github.com/repos/<owner>/meterbar/releases/latest` with a
  `User-Agent: MeterBar/<version>` header (GitHub's API rejects requests
  without one).
- Parse `tag_name` and the `MeterBar-Setup.exe` asset's
  `browser_download_url` with `juce::JSON`. Compare the tag against the
  compiled-in version from §2.3 as numeric semver (not string compare).
- Three outcomes, three honest messages: *up to date* (with current version),
  *update available* (new version + the release notes body from the API +
  an **Update** button), *check failed* (offline/API down — say so, offer
  retry; the unauthenticated API limit of 60 requests/hour/IP is ample for a
  manual check).
- v0.19 scope is **manual check only** (the button Jackson asked about).
  An automatic once-per-day startup check is a listed later option
  (Decision 6), off by default if ever added.

### 3.2 Update button — download, hand off to Inno, exit, relaunch

1. Download `MeterBar-Setup.exe` from the release asset URL to
   `%TEMP%\MeterBar-Update\`, showing progress in the same dialog;
   verify the download completed at the asset's published size (the GitHub
   API returns it) before proceeding. (SHA-256 digest: GitHub now exposes an
   asset `digest` field — verify against it too when present.)
2. Launch the installer detached:
   `MeterBar-Setup.exe /SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART`
   — `/SILENT` = progress bar, no wizard clicks; the `/CLOSEAPPLICATIONS` +
   `/RESTARTAPPLICATIONS` pair uses Inno's Restart Manager so a locked
   `MeterBar.exe` can't block the file replacement.
3. **MeterBar exits itself** immediately after launching the installer
   (it must not be running while its own exe is replaced).
4. **Relaunch:** the existing `[Run]` entry in `MeterBar.iss` is
   `postinstall skipifsilent` — correct for a friend double-clicking the
   installer interactively, but it is *skipped* in silent mode. Add a second
   `[Run]` entry without `skipifsilent`, gated to silent installs
   (e.g. `Check: WizardSilent`), so an updated MeterBar **comes back on its
   own** at the new version. Interactive installs keep today's behaviour.
5. User state survives untouched by design: presets/settings live outside
   `{app}` (app-data/user documents), and the installer only replaces
   `{app}` files (`MeterBar.iss` `[Files]`, `ignoreversion`).
6. Known, accepted caveat (already documented for recipients): the installer
   is **unsigned**, so SmartScreen may still challenge a *first* install;
   silent in-app updates after that first install are the smooth path.
   A code-signing certificate is a paid, later decision — not v0.19.

### 3.3 Why public matters for this design

GitHub's `releases/latest` API and release-asset downloads work
**unauthenticated only for public repos.** A private repo would force the app
to carry a token — and any token embedded in a distributed exe is effectively
public the moment someone inspects the binary, so a "private repo + token in
app" design is a leak, not a safeguard. If Jackson wants the repo private
long-term, the workaround is a **tiny separate public "feed" repo** containing
only `latest.json` + the installer asset (no source) — workable, but it's a
second repo to keep in sync. Recommendation stands: private during setup,
**public at first shared release** — the app is free and given away anyway,
and public source is a trust feature for exactly the friends receiving it.

---

## 4. Fi's second app — filling the space above the strip

The screenshot problem: MeterBar is a thin strip at the bottom of a large
monitor; the rest of the screen is empty wallpaper. Fi wants an application
that lives in that space. Three concrete options, all sized to sit
*above/alongside* a MeterBar strip on a big display:

### Option A — "MeterWall": a full-screen audio-reactive visualizer wall ⭐
A borderless full-screen (or fill-this-monitor) ambient visualizer driven by
the same system sound MeterBar hears: a giant slow spectrogram waterfall,
aurora/plasma spectrum fields, and a beat-reactive glow, auto-cycling scenes,
with a small clock + now-playing-style track readout in one corner and the
mascot tucked into another. It is wallpaper that is alive whenever music is
playing — Discord listening parties included — and idles down to a dim
clock when the sound stops.
**Reuse (why it's cheap — roughly 70% existing code):** the exact WASAPI
loopback capture (`WindowsLoopback`), the DSP FFT/spectrum/spectrogram core,
`ColorMap`/`paletteGradient`, the theme system (including Anime/Pastel +
the mascot assets and cute click sounds), the JUCE app shell, and the whole
§2–§3 pipeline (same repo family, same CI, same updater pattern).
Two apps capturing loopback at once is fine — WASAPI loopback is shared-mode,
so MeterWall and MeterBar can read the same endpoint simultaneously.
**v1 contents:** 3 scenes (Waterfall spectrogram, Aurora spectrum field,
Bar/Plasma wall), scene auto-cycle + manual hotkey, fullscreen/fill-monitor
toggle (`F11`/`Esc`), source picker (same ONE-picker model as MeterBar
Settings), idle clock overlay, the §1 pink-dark settings palette from birth.

### Option B — "DeskBoard": a big ambient dashboard
Oversized clock + date, calendar/agenda block, a now-playing card, and
weather — an information wall rather than a visual one. Fills the space
usefully, but almost none of it reuses MeterBar: now-playing needs Windows
media-session (SMTC) integration, weather needs an API + key, calendar needs
a data source — three new integrations before anything looks good, and it
does nothing with the music that is the whole context of these screens.

### Option C — "Companion": a lofi mascot companion panel
A cozy animated panel built around the existing mascot art: she idles,
reacts to beats, hosts a pomodoro/focus timer, a small reminder/chat-bubble
area, rain/café ambience visuals. Reuses mascot assets, themes, and click
sounds, and it's the most *personal* option — but filling a whole monitor
with it needs a lot of new animation/art, it uses none of the proven DSP
core, and its value is charm rather than the music the screen is playing.

### Recommendation: **Option A — MeterWall.**
Fi's space problem exists *because the screen is a music screen* (MeterBar
is already on it), A is the only option that is native to a full monitor
rather than a window stretched to fit, and it's the only one whose expensive
parts — capture, FFT, colour language, themes, distribution — are already
built, verified, and (per §2–§3) about to be auto-built and auto-updating.
B is a fine *second* project once A exists (a DeskBoard corner widget can
even ship inside MeterWall later — the idle clock in v1 is its seed).
First build, when green-lit, follows the normal pattern: spec → Linux DSP
reuse check → JUCE shell with the 3 scenes → Windows test kit → (with §2 in
place) straight into the same GitHub/CI/updater lane.

---

## 5. Decisions Jackson must make

1. **GitHub account step:** run `gh auth login` once with Milo (browser flow,
   your account), *or* create the empty repo on github.com yourself and give
   Milo its URL. Also pick the repo name (suggestion: `meterbar`).
2. **Visibility end state:** flip the repo **public at first shared release**
   (recommended — makes the in-app updater work with no tokens), or keep it
   private forever and accept the extra public "feed" repo workaround (§3.3)?
3. **Release gate:** tags/releases only when you say **"ship it"** (recommended)
   — Milo auto-commits verified work but never auto-releases. Confirm, or
   say if you want every verified version commit to auto-release instead.
4. **Settings pink:** the fixed pink-dark Settings palette in §1.2 (same in
   every theme, pink `#E04C89` family on deep plum `#2A1E2E`) — good as spec'd,
   or do you want it brighter/more saturated pink?
5. **Pastel bar background:** also deepen the Anime/Pastel main-bar gradient
   (§1.2 item 6), or leave the bar's pale pastel as-is and fix only Settings?
6. **Auto-check for updates:** v0.19 manual button only (recommended), or also
   an automatic check on startup (at most once per day, with an off switch)?
7. **Fi's app:** MeterWall (Option A) as the build — yes/no, and if yes, is
   "MeterWall" the name or does Fi want to name it?
