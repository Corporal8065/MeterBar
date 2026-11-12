#include "MainComponent.h"
#include <meterbar/WindowGeom.hpp> // v0.20.1: preset/startup position clamp
#include <meterbar/Recordings.hpp> // v0.21.4: Recordings submenu model
#include <meterbar/CrashWatch.hpp> // v0.28.3: crash-record naming/notice logic (core)
#include "CrashHandler.h"          // v0.28.3: hard-crash handler (installed in Main.cpp)
#if JUCE_WINDOWS
 #include <windows.h> // native window move (v0.11)
#endif
using namespace meterbar;
// v0.19: the version string comes from the GENERATED header — root
// CMakeLists project(VERSION) is the single source of truth. (Before
// this it was a hard-coded "v1.0.0" literal here, one of six places.)
static const juce::String kVersion = juce::String("v") + meterbar::kVersionString;
// v0.28.3: the app's Documents folder — crash records, capture log,
// presets and recordings all live here; "Open Log Folder" opens it.
static juce::File meterBarDocsDir() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MeterBar");
}
static void openLogFolder() { auto dir = meterBarDocsDir(); dir.createDirectory(); dir.startAsProcess(); }
// v0.28.4: WER HARVEST (family crash-log contract amendment,
// 2026-11-03 ~14:08 PT). The in-process crash handler
// (CrashHandler.h) NEVER fires for heap-corruption deaths — ntdll
// fast-fail goes straight to WER — so for exactly the crash class
// behind the per-app capture crash, the only record is the dump
// WER's LocalDumps wrote to %LOCALAPPDATA%\CrashDumps (the dumps
// Jackson has been sending by hand). On launch, copy each NEW
// MeterBar dump from there into Documents\MeterBar under a
// crash-record name — timestamp = the dump's last-write time, so
// the stem sorts with the crash — plus a .txt sibling composed by
// the core, so the next-launch notice below and Open Log Folder
// surface it exactly like a handler-written record. Copied dumps
// are keyed in wer-harvested.txt (core CrashWatch) and never
// copied twice; the source files are left untouched.
static void harvestWerCrashDumps() {
#if JUCE_WINDOWS
    const auto localApp = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", juce::String());
    if (localApp.isEmpty()) return;
    juce::File srcDir = juce::File(localApp).getChildFile("CrashDumps");
    if (!srcDir.isDirectory()) return;
    auto dir = meterBarDocsDir(); dir.createDirectory();
    auto logFile = dir.getChildFile("wer-harvested.txt");
    std::string logText = logFile.loadFileAsString().toStdString();
    std::vector<std::string> existing;
    for (auto& f : dir.findChildFiles(juce::File::findFiles, false))
        existing.push_back(f.getFileName().toStdString());
    for (auto& f : srcDir.findChildFiles(juce::File::findFiles, false)) {
        const std::string name = f.getFileName().toStdString();
        if (!meterbar::isWerDumpName(name)) continue;
        if (f.getSize() <= 0) continue; // a dump WER failed to write carries nothing
        const auto mt = f.getLastModificationTime();
        meterbar::CrashTimeParts parts;
        parts.year = mt.getYear(); parts.month = mt.getMonth() + 1; parts.day = mt.getDayOfMonth();
        parts.hour = mt.getHours(); parts.minute = mt.getMinutes(); parts.second = mt.getSeconds();
        const std::string key = meterbar::werHarvestKey(name, (long long) f.getSize(), parts);
        if (meterbar::werHarvestKeySeen(logText, key)) continue;
        const std::string stem = meterbar::harvestStem(existing, parts);
        if (!f.copyFileTo(dir.getChildFile(juce::String(stem + ".dmp")))) continue;
        existing.push_back(stem + ".dmp");
        // The .txt is what the notice keys on. The handler never
        // ran for this crash — no exception code exists to quote —
        // so the record says where the dump came from and lets the
        // .dmp speak for the rest.
        const std::string snap =
            "source: harvested from %LOCALAPPDATA%\\CrashDumps\\" + name + " on launch \xe2\x80\x94 "
            "Windows Error Reporting wrote this dump; the in-process crash handler did not "
            "fire (fast-fail crash class).\nexception details: inside the .dmp.\n";
        auto rec = meterbar::crashRecordText(meterbar::kVersionString, "harvested record (see .dmp)",
                                             meterbar::crashTimeText(parts), 0ul, "n/a", snap);
        dir.getChildFile(juce::String(stem + ".txt")).replaceWithText(juce::String(rec));
        existing.push_back(stem + ".txt");
        logFile.appendText(juce::String(key + "\n"));
        logText += key + "\n";
    }
#endif
}
MainComponent::MainComponent() {
    presetio::loadLastState(engine.state); // v0.28.8: restore the last state BEFORE anything reads it (theme below, applySource later, MainWindow's placement after) — the close-save prompt's other half. loadPinPersist() below still runs after this and re-overrides the pin fields: the pin file stays the pin authority.
    setOpaque(false); // v0.13: lets translucent background fills show the desktop (window is non-opaque too)
    theme = themeByName(engine.state.themeName);
    frame.cmap = meterbar::ColorMap(engine.state.colorMap);
    // logo: our own mark — three vertical bars in palette gradient (drawn in paint) + wordmark
    logoLabel.setText("MeterBar", juce::dontSendNotification);
    logoLabel.setFont(juce::Font(15.f, juce::Font::bold));
    versionLabel.setText(kVersion, juce::dontSendNotification); versionLabel.setFont(11.f);
    hintLabel.setText("Drag the top bar to move - Reposition menu docks the bar", juce::dontSendNotification); hintLabel.setFont(10.f);
    for (auto* c : { (juce::Component*)&logoLabel, (juce::Component*)&versionLabel, (juce::Component*)&hintLabel }) {
        addAndMakeVisible(c);
        c->setInterceptsMouseClicks(false, false); // clicks fall through to the toolbar drag (v0.8)
    }
    for (auto* b : { &recBtn, &pauseBtn, &settingsBtn, &burgerBtn, &linkBtn }) addAndMakeVisible(b);
    // v0.22.0: the icon buttons carry tooltips — with the text labels
    // gone, the hover text is what names each glyph.
    recBtn.setTooltip("Record: start / stop recording the current source (WAV in Documents\\MeterBar)");
    pauseBtn.setTooltip("Pause / resume audio");
    settingsBtn.setTooltip("Settings");
    burgerBtn.setTooltip("Menu: presets, arrange / restore windows, reposition, minimize, quit");
    // v0.22.1: the glyph buttons wear the emoji typeface (see
    // IconButtonLookAndFeel) — everything else keeps the stock font.
    for (auto* b : { &linkBtn, &recBtn, &pauseBtn, &settingsBtn, &burgerBtn }) b->setLookAndFeel(&iconLnf_);
    // v0.26.1: the toolbar labels wear the text-pop look
    // (TextPop.h) — same pictured backing as the module text,
    // over the translucent toolbar strip. v0.28.0: the Update!
    // button does NOT any more — its label read doubled through
    // the pop (Jackson, 2026-11-02 21:54 PT); it keeps the stock
    // look and draws once.
    for (auto* l : { &logoLabel, &versionLabel, &hintLabel }) l->setLookAndFeel(&labelLnf_);
    // v0.28.6: TRIM slider on the bar (Jackson's ask) — the same
    // PresetState::trimDb as Settings > Audio Source > Trim dB
    // (-48..+48, Settings.h), surfaced beside the wordmark. No text
    // box (toolbar room); the tooltip carries the live value's
    // meaning and the Settings slider remains the precise editor.
    // The frame timer mirrors state -> slider (see timerCallback),
    // so the two surfaces can never disagree.
    trimLabel_.setText("Trim", juce::dontSendNotification);
    trimLabel_.setFont(juce::Font(10.f, juce::Font::bold));
    trimLabel_.setLookAndFeel(&labelLnf_);
    trimLabel_.setInterceptsMouseClicks(false, false); // like the other toolbar labels: drags fall through to the window drag
    addAndMakeVisible(trimLabel_);
    trimSlider_.setRange(-48.0, 48.0, 0.01);
    trimSlider_.setValue(engine.state.trimDb, juce::dontSendNotification);
    trimSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    trimSlider_.setTooltip("Input trim (dB) — the same setting as Settings > Audio Source > Trim dB; drag to trim the meters' input");
    trimSlider_.onValueChange = [this]{ engine.state.trimDb = (float)trimSlider_.getValue(); };
    addAndMakeVisible(trimSlider_);
    // v0.20.0 (k): family LINK toggle + client wiring. Geometry leads
    // via notifyWindowGeometryChanged. v0.2.1: the settings feed is
    // wired too (see onChanged below) — MeterBar was the only bar not
    // calling notifyLocalSettings, leaving its background publishes to
    // the client's 30 Hz poll alone; the poll remains as backstop.
    linkBtn.setClickingTogglesState(true);
    linkBtn.setToggleState(engine.state.linkEnabled, juce::dontSendNotification);
    linkBtn.setTooltip("Link: snap, drag, dock and share the background with TickerBar / SessionBar when their Link is on too");
    linkBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop);
        engine.state.linkEnabled = linkBtn.getToggleState(); };
    linkClient_.getBounds = [this]{ return getScreenBounds(); };
    linkClient_.setBounds = [this](juce::Rectangle<int> r){
        if (auto* tl = getTopLevelComponent()) {
            // v0.21.1: EVERY remote apply (stack / follow / snap /
            // group-dock) passes the visibility floor (core
            // WindowGeom, ensureRectVisible). Before this, the link
            // could move the bar off-screen a tick after the
            // (clamped) startup restore, and the position-save feed
            // kept it there — the defect behind Jackson's
            // 2026-10-31 "stuck somewhere i cannot see upon load
            // up" report on SessionBar, shared here by construction.
            // Meaningfully-visible targets pass through untouched.
            std::vector<meterbar::IntRect> areas;
            for (auto& d : juce::Desktop::getInstance().getDisplays().displays) {
                auto ua = d.userArea;
                areas.push_back({ ua.getX(), ua.getY(), ua.getWidth(), ua.getHeight() });
            }
            auto safe = meterbar::ensureRectVisible(
                { r.getX(), r.getY(), r.getWidth(), r.getHeight() }, areas);
            tl->setBounds(safe.x, safe.y, safe.w, safe.h);
        }
    };
    linkClient_.getBg = [this]{
        // BLK4: the visualizer style rides the effect ordinal (1 Bubbles / 3 Lines).
        // BLK5 (v0.25.0): style 2 (Field) rides ordinal 4.
        // Drag-lag fix (2026-10-31): effect 2 (a peer's glow) has NO
        // MeterBar-native emission — the visualizer mapping can only
        // produce 0/1/3/4. Adopting a glow packet records
        // publishedEffect=2 in the link bookkeeping while an unshadowed
        // getBg keeps emitting 1/3, so the publish gate misreads the
        // adoption as a NEW local change and MeterBar re-publishes its
        // interpretation — vetoing the peer's glow (the peer adopts
        // the visualizer back within a tick) and re-firing adoption
        // work on both bars at every link join and every peer glow
        // change. While the shadow is set, pass the adopted ordinal
        // through; the display mapping in applyBg is unchanged.
        const int effect = linkBgEffectShadow_ >= 0 ? linkBgEffectShadow_
            : (engine.state.visualizerEnabled ? (engine.state.visualizerStyle == 1 ? 3 : engine.state.visualizerStyle == 2 ? 4 : 1) : 0);
        BarLinkClient::Bg out{ (int)engine.state.backgroundStyle, engine.state.backgroundOpacity, effect };
        // BLK7 (DESIGN §2): theme — MeterBar's five menu themes map
        // 0-4 directly onto the family ordinal list; an unknown name
        // publishes -1 (none). Scheme — colorMap -> family ordinal:
        // 0->1, 1->5, 2->2, 3->3, 4->4, 5->6, 6->7.
        {
            const char* names[] = { "Midnight Kids", "Midnight Pink", "Deep Teal", "Daylight", "Anime" };
            out.theme = -1;
            for (int i = 0; i < 5; ++i) if (engine.state.themeName == names[i]) out.theme = i;
            static const int toFamily[] = { 1, 5, 2, 3, 4, 6, 7 };
            const int cm = (int)engine.state.colorMap;
            out.scheme = (cm >= 0 && cm < 7) ? toFamily[cm] : -1;
            out.palHue = engine.state.visualizerBaseHue; out.palSat = engine.state.visualizerSaturation;
            out.palSpread = engine.state.visualizerSpread; out.palCycle = engine.state.visualizerCycleSec;
        }
        return out;
    };
    linkClient_.applyBg = [this](const BarLinkClient::Bg& bg){
        engine.state.backgroundStyle = (meterbar::BackgroundStyle)juce::jlimit(0, 3, bg.style); // v0.20.1: Pastel (3) is a family style too
        engine.state.backgroundOpacity = juce::jlimit(0.15f, 1.0f, bg.opacity);
        // BLK4: MeterBar's only effect is the visualizer — a peer's
        // Bubbles (1) or Lines (3) lands as visualizer-on WITH that
        // style; a peer's glow (2) lands as visualizer-on keeping
        // the current style (MeterBar has no glow to show).
        if (bg.effect == 1) { engine.state.visualizerEnabled = true; engine.state.visualizerStyle = 0; }
        else if (bg.effect == 3) { engine.state.visualizerEnabled = true; engine.state.visualizerStyle = 1; }
        else if (bg.effect == 4) { engine.state.visualizerEnabled = true; engine.state.visualizerStyle = 2; } // BLK5: Field
        else engine.state.visualizerEnabled = bg.effect != 0;
        linkBgEffectShadow_ = (bg.effect == 2) ? 2 : -1; // 2 (glow) is the only ordinal getBg cannot emit natively — see getBg
        // BLK7 (DESIGN §3): follower apply order — background (above),
        // then theme, then scheme, then palette.
        // Theme: family ordinal 0-4 maps directly to MeterBar's menu
        // themes; ordinal 5 (Clean Dark) has no MeterBar theme — keep
        // our own theme for that field and adopt everything else.
        if (bg.theme >= 0 && bg.theme <= 4) {
            const char* names[] = { "Midnight Kids", "Midnight Pink", "Deep Teal", "Daylight", "Anime" };
            engine.state.themeName = names[bg.theme];
            applyTheme(); // the existing theme path (DESIGN §3); NOTE: applyTheme does not touch backgroundStyle, so no Anime->Pastel menu pairing leaks in
        }
        // Scheme: family ordinal -> colorMap (inverse of getBg's map);
        // family 0 (Family) has no MeterBar counterpart — keep own.
        {
            static const int toLocal[] = { -1, 0, 2, 3, 4, 1, 5, 6 }; // family 0..7 -> colorMap (-1 = keep own)
            if (bg.scheme >= 0 && bg.scheme < 8 && toLocal[bg.scheme] >= 0) {
                engine.state.colorMap = (meterbar::ColorMapId)toLocal[bg.scheme];
                frame.cmap = meterbar::ColorMap(engine.state.colorMap);
            }
        }
        // Palette: plain field assignments (read live per frame).
        engine.state.visualizerBaseHue = bg.palHue;
        engine.state.visualizerSaturation = bg.palSat;
        engine.state.visualizerSpread = bg.palSpread;
        engine.state.visualizerCycleSec = bg.palCycle;
        repaint();
    };
    linkClient_.bgLinkEnabled = [this]{ return engine.state.linkBackground; }; // v0.21.3
    linkClient_.getDockedEdge = [this]{ return reserveMachine.reserved() ? (int)reserveMachine.edge() : -1; };
    linkClient_.isLinked = [this]{ return engine.state.linkEnabled; };
    // v0.20.1 (item 17): broadcast the band energies + level the
    // visualizer already computes, so linked siblings can render the
    // reactive glow from the same music.
    linkClient_.getAudio = [this](float out[5]) {
        out[0] = latestBands_[0]; out[1] = latestBands_[1]; out[2] = latestBands_[2]; out[3] = latestLevel_;
        out[4] = latestBalance_; // BLK5 (v0.25.0): stereo balance for the siblings' distributed emitters
    };
    // BLK7 (DESIGN §7): pulse EMIT. Host side tracks the engine's
    // onset count; the callback reports the CURRENT pulse (the link
    // tick stamps the slot when this seq advances). Threshold: the
    // UI Beat Pulse renders frame.beatGlow when it exceeds 0.01
    // (ModuleViews) — the same threshold gates emission here.
    // Colour: theme.accent — the theme colour source the beat-glow
    // render derives from (module accents are theme accents).
    linkClient_.getPulse = [this](unsigned& seq, int& colourRgb, float& strength) {
        seq = pulseSeq_; colourRgb = pulseColour_; strength = pulseStrength_;
    };
    // BLK7 (DESIGN §6b/§6d): Link All host surface.
    linkClient_.onLinkAllForming = [this]{
        if (!linkAllGeomSnapshot_) { linkAllGeom_ = getScreenBounds(); linkAllGeomSnapshot_ = true; }
    };
    linkClient_.setLinkEnabled = [this](bool on){
        engine.state.linkEnabled = on;
        linkBtn.setToggleState(on, juce::dontSendNotification);
    };
    linkClient_.getLinkAllArea = [this]{
        // DESIGN §6a: bounds of the display containing MeterBar's
        // current rect (MeterBar IS this app), else our own display.
        auto own = getScreenBounds();
        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
            if (d.totalArea.contains(own.getCentre())) return d.totalArea;
        return juce::Desktop::getInstance().getDisplays().getMainDisplay().totalArea;
    };
    // BLK7 (DESIGN §6d): appearance snapshot on the link OFF->ON edge
    // (any cause), restore on ON->OFF; Link All geometry snapshot
    // restores on the same OFF edge. In-memory only (MeterBar has no
    // settings file; restoring writes the app-native fields back).
    linkClient_.onLinkEdge = [this](bool on){
        auto& st = engine.state;
        if (on) {
            if (!linkAppearanceSnapshot_) {
                linkSnapBg_ = BarLinkClient::Bg{ (int)st.backgroundStyle, st.backgroundOpacity, 0 };
                linkSnapBg_.theme = [&]{ const char* n[] = {"Midnight Kids","Midnight Pink","Deep Teal","Daylight","Anime"}; for (int i=0;i<5;++i) if (st.themeName==n[i]) return i; return -1; }();
                linkSnapColorMap_ = (int)st.colorMap;
                linkSnapVizEnabled_ = st.visualizerEnabled; linkSnapVizStyle_ = st.visualizerStyle;
                linkSnapPal_[0] = st.visualizerBaseHue; linkSnapPal_[1] = st.visualizerSaturation;
                linkSnapPal_[2] = st.visualizerSpread; linkSnapPal_[3] = st.visualizerCycleSec;
                linkAppearanceSnapshot_ = true;
            }
        } else {
            if (linkAppearanceSnapshot_) {
                st.backgroundStyle = (meterbar::BackgroundStyle)juce::jlimit(0, 3, linkSnapBg_.style);
                st.backgroundOpacity = juce::jlimit(0.15f, 1.0f, linkSnapBg_.opacity);
                if (linkSnapBg_.theme >= 0 && linkSnapBg_.theme <= 4) {
                    const char* n[] = {"Midnight Kids","Midnight Pink","Deep Teal","Daylight","Anime"};
                    st.themeName = n[linkSnapBg_.theme];
                }
                st.colorMap = (meterbar::ColorMapId)linkSnapColorMap_;
                st.visualizerEnabled = linkSnapVizEnabled_; st.visualizerStyle = linkSnapVizStyle_;
                st.visualizerBaseHue = linkSnapPal_[0]; st.visualizerSaturation = linkSnapPal_[1];
                st.visualizerSpread = linkSnapPal_[2]; st.visualizerCycleSec = linkSnapPal_[3];
                linkBgEffectShadow_ = -1;
                linkAppearanceSnapshot_ = false;
                applyTheme(); repaint();
            }
            if (linkAllGeomSnapshot_) {
                if (auto* tl = getTopLevelComponent()) tl->setBounds(linkAllGeom_);
                linkAllGeomSnapshot_ = false;
            }
        }
    };
    linkClient_.start();
    // v0.22.0: Preset / Reposition / Arrange / Restore / Minimize /
    // Quit have no toolbar buttons any more — they are items in the
    // hamburger menu (showHamburgerMenu), calling the same handlers.
    // Reserve — the AppBar reservation toggle — had already moved
    // into the Reposition menu at v0.21.2 as the ticked "Reserve
    // Screen Space" item; engine.state.reserveScreenSpace is
    // unchanged, so existing users keep it.
    burgerBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); showHamburgerMenu(); };
    settingsBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); toggleSettings(); }; // v0.28.6: toggle — the open/close logic + capture hygiene live in toggleSettings()
    pauseBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); engine.paused = !engine.paused.load(); pauseBtn.setButtonText(engine.paused ? juce::String::fromUTF8(meterbar::glyphPlayUtf8()) : juce::String::fromUTF8(meterbar::glyphPauseUtf8())); }; // v0.22.0: glyph swap replaces the Pause/Resume text swap; v0.22.2: explicit UTF-8 bytes (see meterbar/Glyphs.hpp)
    recBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); toggleRecord(); };
    // v0.19.2: the update indicator lives in the toolbar but starts
    // hidden and stays hidden until a check actually finds a newer
    // release — it is the (only) visible trace of the launch check.
    addAndMakeVisible(updateBtn); updateBtn.setVisible(false);
    updateBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop);
        if (pendingRelease.ok && pendingRelease.newer) showUpdatePanel(pendingRelease); };
    // modules in display order
    auto add = [&](ModuleView::Kind k){ auto m = std::make_unique<ModuleView>(k); m->data = &frame; m->flexIndex = (int)modules.size(); addAndMakeVisible(m.get()); modules.push_back(std::move(m)); };
    add(ModuleView::Kind::Spectrogram); add(ModuleView::Kind::Waveform); add(ModuleView::Kind::Loudness);
    // v0.26.0: the waveform's time-signature chip click cycles the
    // meter (4/4 -> 3/4 -> 6/8) and re-meters the beat tracker.
    for (auto& m : modules) if (m->kind == ModuleView::Kind::Waveform)
        m->onSigCycle = [this] {
            engine.state.beatTimeSignature = (engine.state.beatTimeSignature + 1) % 3;
            engine.applyBeatMeter();
            uiClick(ClickSounds::Which::Pop);
        };
    add(ModuleView::Kind::Stereometer); add(ModuleView::Kind::Oscilloscope); add(ModuleView::Kind::Spectrum);
    // v0.15: splitters between modules (drag to resize a module). Created
    // after the modules so they sit on top; layoutModules() positions and
    // binds them. One per gap — at most modules-1 are ever visible.
    // v0.16: one extra splitter for the mascot cell's leading edge.
    addAndMakeVisible(mascot); mascot.setVisible(false); // Anime theme only (applyTheme drives); added before the splitters so they paint on top
    for (int i = 0; i < 6; ++i) { auto s = std::make_unique<ModuleSplitter>(); addAndMakeVisible(s.get()); splitters.push_back(std::move(s)); }
    // v0.24.0: the docked caption strip — added after the modules so
    // it paints above them; hidden until docked captions turn on
    // (syncCaptionDock drives its visibility + the bar's growth).
    addAndMakeVisible(captionStrip_); captionStrip_.setVisible(false);
    spectro.prepare(256, 220); waveHist.prepare(512); scope.prepare(2048);
    settingsWin = std::make_unique<SettingsWindow>();
    settingsWin->content.st = &engine.state;
    settingsWin->content.deviceManager = &engine.deviceManager;
    settingsWin->content.onChanged = [this]{ linkBgEffectShadow_ = -1; applyTheme(); layoutModules(); repaint(); syncScreenSpace(); linkClient_.notifyLocalSettings(); }; // v0.2.1: bg publish feed (was missing). Drag-lag fix: any LOCAL settings change drops the adopted-effect shadow — local intent wins and MeterBar publishes its true values again
    settingsWin->content.onSourceChanged = [this]{ engine.applySource(); savePinPersist(); }; // v0.28.7: every source change rewrites the pin file from live state
    settingsWin->content.appSourceStatus = [this]{ return engine.appSourceState(); }; // v0.27.0 (item 8)
    settingsWin->content.onFpsChanged = [this]{ applyFps(); };
    settingsWin->content.onBgLinkToggled = [this]{ linkClient_.notifyBgLinkChanged(); }; // v0.21.3
    settingsWin->content.onArrange = [this]{ doArrangeWindows(); };   // v0.18: Settings > Window buttons
    settingsWin->content.onRestore = [this]{ doRestoreWindows(); };   // v0.18
    settingsWin->content.onStickToggled = [this](bool b){ setStick(b); }; // v0.28.9: Settings' Stick lands in the same merged action as the menu's
    settingsWin->content.build();
    debugWin = std::make_unique<HideOnCloseWindow>("MeterBar Debug", juce::Colour(0xff231f31), juce::DocumentWindow::closeButton); // v0.28.2: X hides the window (stock DocumentWindow's X did nothing)
    debugLabel.setFont(12.5f); // v0.13: endpoint IDs are long — smaller font + wider window keeps them on one line
    debugWin->setContentNonOwned(&debugLabel, true); debugWin->setSize(470, 262); debugWin->setUsingNativeTitleBar(true);
    loadPinPersist(); // v0.28.7: BEFORE the first applySource below — a saved pin comes up pinned (attached or waiting), like it was left
    loadPerfPersist(); // v0.28.2: BEFORE the first applyFps — a persisted stepped-down Auto starts at the Low-End cap from frame one
    engine.applySource(); applyTheme(); applyFps();
    // v0.28.3: NEXT-LAUNCH CRASH NOTICE (family crash-log contract).
    // The crash handler (CrashHandler.h, installed in Main.cpp)
    // leaves crash-*.txt + .dmp in Documents\MeterBar; a crash file
    // without a ".seen" sibling earns exactly one notice, however
    // many launches it survives. v0.28.4: harvestWerCrashDumps()
    // first brings in any WER LocalDumps records the handler could
    // not write (fast-fail classes), under the same naming — the
    // scan below then covers both provenances uniformly. Deferred
    // via callAsync so the bar is fully up before the dialog
    // appears. Either button marks every unseen crash seen — the
    // notice never repeats for the same crash file.
    juce::MessageManager::callAsync([this]{
        harvestWerCrashDumps(); // v0.28.4: WER dumps land in the folder BEFORE the unseen scan below, so the notice counts them too
        auto dir = meterBarDocsDir();
        std::vector<std::string> names;
        for (auto& f : dir.findChildFiles(juce::File::findFiles, false))
            names.push_back(f.getFileName().toStdString());
        auto unseen = meterbar::unseenCrashes(names);
        if (unseen.empty()) return;
        auto count = (int)unseen.size();
        auto w = std::make_shared<juce::AlertWindow>("MeterBar crashed",
            "MeterBar crashed " + juce::String(count) + (count == 1 ? " time" : " times") +
            " since you last saw this notice.\nThe crash record" + (count == 1 ? " is" : "s are") +
            " in Documents\\MeterBar (crash-*.txt + .dmp).\nOpen the folder to send them to Milo?",
            juce::AlertWindow::WarningIcon);
        w->addButton("Open Log Folder", 1); w->addButton("Dismiss", 0);
        w->enterModalState(true, juce::ModalCallbackFunction::create([unseen](int r){
            for (auto& n : unseen) {
                auto seen = meterBarDocsDir().getChildFile(juce::String(meterbar::seenMarkerFor(n)));
                seen.create();
            }
            if (r == 1) openLogFolder();
        }));
    });
    // v0.23.0: Live Captions initialise AFTER the first applySource
    // (the controller reads the engine's caption epoch/source) and
    // after the window exists (the overlay's placement rule wants
    // the bar's screen bounds).
    captions_.initialise(engine, [this] { return getScreenBounds(); });
    setSize(engine.state.windowW, engine.state.windowH);
    syncScreenSpace(); // v0.18: a preset loaded at startup may already ask for the reservation
    // v0.19.2: silent background check on launch — non-blocking, no
    // dialog. Up-to-date, offline, private-repo and error outcomes
    // are all silent at launch; only a NEWER release surfaces, as
    // the toolbar "Update!" indicator (see pollUpdater).
    if (mbupdate::configured()) startUpdateCheck(true);
}
MainComponent::~MainComponent() { captions_.shutdown(); stopTimer(); screenSpace.release(); screenSpace.restore(); if (engine.isRecording()) engine.stopRecording(); }
void MainComponent::applyTheme() {
    theme = themeByName(engine.state.themeName);
    frame.cmap = meterbar::ColorMap(engine.state.colorMap);
    logoLabel.setColour(juce::Label::textColourId, theme.accent);
    versionLabel.setColour(juce::Label::textColourId, theme.dim);
    hintLabel.setColour(juce::Label::textColourId, theme.dim);
    // v0.28.6: the bar trim slider wears the theme like the rest of the toolbar
    trimLabel_.setColour(juce::Label::textColourId, theme.dim);
    trimSlider_.setColour(juce::Slider::thumbColourId, theme.accent);
    trimSlider_.setColour(juce::Slider::trackColourId, theme.dim.withAlpha(0.55f));
    trimSlider_.setColour(juce::Slider::backgroundColourId, theme.deepest);
    for (auto* b : { &recBtn, &pauseBtn, &settingsBtn, &burgerBtn, &linkBtn }) {
        b->setColour(juce::TextButton::buttonColourId, theme.panel);
        b->setColour(juce::TextButton::buttonOnColourId, theme.deepest);
        b->setColour(juce::TextButton::textColourOffId, theme.accent);
        b->setColour(juce::TextButton::textColourOnId, theme.text);
    }
    // v0.19.2: the update indicator is brand pink in every theme —
    // it must read as "something new", not as another toolbar button.
    updateBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffe04c89));
    updateBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffe04c89));
    updateBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffffff));
    updateBtn.setColour(juce::TextButton::textColourOnId, juce::Colour(0xffffffff));
    if (debugWin) debugWin->setBackgroundColour(theme.panel);
    // v0.19: Settings does NOT follow the bar theme any more — it has a
    // fixed pink-dark palette of its own (Settings.h, settingsPalette).
    // This line previously overwrote the Settings background with
    // theme.panel on every theme change: near-white in Anime, pure
    // white in Daylight. It now re-pins the fixed palette base instead.
    if (settingsWin) settingsWin->setBackgroundColour(settingsPalette::base());
    // v0.16: the mascot exists only in the Anime theme, and leaving Anime
    // silences the cute clicks completely (queued sounds dropped too).
    mascot.setVisible(engine.state.themeName == "Anime");
    if (engine.state.themeName != "Anime") clicks.hush();
    layoutModules();
    repaint();
}
// v0.16: cute clicks fire ONLY in the Anime theme with the toggle on —
// in every other theme this is a no-op, so call sites stay unconditional.
void MainComponent::uiClick(ClickSounds::Which w) {
    if (engine.state.themeName == "Anime" && engine.state.animeClickSounds) clicks.play(w);
}
void MainComponent::adjustMascotFlex(int dxPx) {
    // Same pair-trade idea as adjustFlex, but the mascot's partner is the
    // group of visible modules as a whole: convert px to flex against the
    // strip width and trade with the total module flex (each module keeps
    // its relative share; the mascot keeps a sane minimum/maximum).
    auto& st = engine.state;
    auto area = getLocalBounds(); if (st.showToolbar) area.removeFromTop(30);
    if (area.getWidth() <= 0) return;
    float totalFlex = st.mascotFlex; for (auto& m : modules) if (m->isVisible()) totalFlex += st.moduleFlex[(size_t)m->flexIndex];
    if (totalFlex <= 0.f) return;
    float dw = (float)dxPx * totalFlex / (float)area.getWidth();
    float scale = 1.0f;
    float newMascot = juce::jlimit(0.25f, 2.5f, st.mascotFlex + dw);
    float delta = newMascot - st.mascotFlex;
    if (std::abs(delta) > 1e-6f) {
        float modTotal = totalFlex - st.mascotFlex;
        if (modTotal > 0.f) scale = (modTotal - delta) / modTotal;
        for (auto& m : modules) if (m->isVisible())
            st.moduleFlex[(size_t)m->flexIndex] = std::max(0.35f, st.moduleFlex[(size_t)m->flexIndex] * scale);
        st.mascotFlex = newMascot;
    }
    layoutModules(); repaint();
}
// v0.28.2 PERFORMANCE MODE: all policy lives in the core
// (meterbar/Performance.hpp); here we only resolve mode + latch.
meterbar::EffectiveLimits MainComponent::perfLimits() {
    auto& st = engine.state;
    if (st.performanceMode != perfModeShadow_) { // an explicit mode choice is the ONLY step-up path: it re-arms Auto and clears the persisted latch
        perfModeShadow_ = st.performanceMode;
        perfAuto_.reset();
        savePerfPersist(false);
    }
    return meterbar::effectiveLimits(
        meterbar::perfLowActive(st.performanceMode, perfAuto_.latched()),
        st.fpsLimit, st.undertowBloomQuarter, st.undertowBloomEighth);
}
// v0.28.2: the Auto latch's persistence (CaptionSettings' idiom —
// a small JSON file under Documents/MeterBar). Written when the
// latch trips; cleared on any explicit Performance Mode change.
namespace { juce::File perfPersistFile() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("MeterBar").getChildFile("perf-auto.json");
} }
void MainComponent::loadPerfPersist() {
    perfModeShadow_ = engine.state.performanceMode; // adopt, don't "change"
    const auto f = perfPersistFile();
    if (!f.existsAsFile()) return;
    const auto v = juce::JSON::parse(f.loadFileAsString());
    if (v.isObject() && (bool)v.getProperty("steppedDown", false))
        perfAuto_.forceLatched();
}
void MainComponent::savePerfPersist(bool steppedDown) {
    const auto f = perfPersistFile();
    f.getParentDirectory().createDirectory();
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty("schema", 1);
    o->setProperty("steppedDown", steppedDown);
    f.replaceWithText(juce::JSON::toString(juce::var(o), true));
}
// v0.28.7: the pin's persistence — the perf-auto.json idiom (a
// small JSON file under Documents/MeterBar, CaptionSettings'
// pattern). The pin ALSO rides presets via PresetIO, but presets
// are explicit user saves; this file is what makes the pin (and
// whether pinned mode was on) survive an ordinary restart.
// Written on every source change; read once at startup.
namespace { juce::File pinPersistFile() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("MeterBar").getChildFile("pin-source.json");
} }
void MainComponent::loadPinPersist() {
    const auto f = pinPersistFile();
    if (!f.existsAsFile()) return;
    const auto v = juce::JSON::parse(f.loadFileAsString());
    if (!v.isObject()) return;
    engine.state.pinnedExe = v.getProperty("pinnedExe", "").toString().toStdString();
    if ((bool)v.getProperty("pinActive", false) && !engine.state.pinnedExe.empty())
        engine.state.appSource = 4;
}
void MainComponent::savePinPersist() {
    const auto f = pinPersistFile();
    f.getParentDirectory().createDirectory();
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty("schema", 1);
    o->setProperty("pinnedExe", juce::String(engine.state.pinnedExe));
    o->setProperty("pinActive", engine.state.appSource == 4);
    f.replaceWithText(juce::JSON::toString(juce::var(o), true));
}
// v0.28.7: the pinned-source watcher, stepped ~1 s from
// timerCallback. While pinned mode is off it only keeps the
// settle stamp fresh (and the watch belief reset), so entering
// pinned mode starts clean. Attach goes through
// AudioEngine::applySource — the same path a picker selection
// takes, activation included; detach is a bare loopback stop.
// Pinned mode NEVER falls back: an absent exe is the waiting
// state (silence), not a cue to hear something else.
void MainComponent::pollPinnedSource(juce::int64 nowMs) {
    auto& s = engine.state;
    const bool pinnedMode = s.audioSource == "System Loopback" && s.appSource == 4 && !s.pinnedExe.empty();
    if (!pinnedMode) { pinWatch_ = meterbar::PinWatch{}; pinLastActionMs_ = nowMs; return; }
    const bool running = WindowsLoopback::isProcessRunning(s.pinnedExe);
    const bool capturing = engine.loopbackStatus() == "running";
    const bool settle = nowMs - pinLastActionMs_ >= 6000;
    switch (pinWatch_.step(running, capturing, settle)) {
        case meterbar::PinAction::Attach:
            pinLastActionMs_ = nowMs; engine.applySource(); break;
        case meterbar::PinAction::Detach:
            pinLastActionMs_ = nowMs; engine.stopLoopback(); break;
        case meterbar::PinAction::None: break;
    }
}
void MainComponent::applyFps() { int fps = perfLimits().fpsCap; startTimerHz(fps > 0 ? fps : 60); }
// v0.18 RESERVE SCREEN SPACE (Jackson's ask): while the toggle is ON and
// the bar sits flush at a monitor edge, register the window as a
// Windows AppBar so the OS shrinks that monitor's work area and maximised
// windows stop at the bar instead of sliding under it. All rects here
// are PHYSICAL pixels straight from GetWindowRect / GetMonitorInfo —
// AppBar rects are physical, and a JUCE logical rect would be wrong by
// the monitor's DPI scale factor.
//
// v0.18.1 EVENT-DRIVEN (the flicker fix): v0.18 re-ran this sync from the
// UI timer ~4x/sec and acted on a single 2 px dock threshold; combined
// with ScreenSpace comparing the request against the system-GRANTED
// rect, the AppBar could be torn down and rebuilt several times a
// second and every maximised window bounced with it. Now this function
// only FEEDS one observation to meterbar::ReserveMachine (hysteresis:
// dock at <= 2 px, release only past 8 px, actions are transitions) and
// performs the single action the machine returns. It is called on real
// events only: the toggle itself, Settings/preset changes, reposition
// commands, and the window's own moved/resized notifications (MainWindow
// forwards them via notifyWindowGeometryChanged, resized() below calls
// it too). No timer touches other windows' space any more.
//
// v0.19.1 EDGE-AWARE (MiniMeters semantics): the observation now carries
// the gap to ALL FOUR monitor edges and the monitor rect; the machine
// reserves whichever edge the bar is docked to — Jackson's bar lives at
// the BOTTOM, where the v0.18 line's top-only reservation never fired —
// and reports a Switch when the bar is dragged straight from one docked
// edge to another (release the old edge, register the new, once each).
// The rect registered is the machine's full edge strip
// (requestedRect), not the bar's own rect.
//
// v0.28.9 STICK = RESERVE (Jackson's dock-merge ruling): the toggle
// this machine is fed (engine.state.reserveScreenSpace) is now driven
// by the Stick control — doStick() keeps the two state fields merged
// (meterbar::applyStickReserveMerge) and docks the bar first, so this
// function sees the same single flag it always did. The observation
// rect is the bar's VISIBLE rect (see below).
void MainComponent::syncScreenSpace() {
#if JUCE_WINDOWS
    auto* w = findParentComponentOfClass<juce::DocumentWindow>();
    HWND hwnd = (w && w->getPeer()) ? static_cast<HWND>(w->getPeer()->getNativeHandle()) : nullptr;
    if (hwnd) {
        RECT wr{}; GetWindowRect(hwnd, &wr);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof mi; GetMonitorInfoW(mon, &mi);
        // v0.28.9: the machine judges the bar's VISIBLE rect (DWM
        // extended frame bounds), not the raw window rect — the
        // window rect includes the invisible resize border, which
        // ate the machine's 2 px dock band whole (the reservation
        // only engaged at one very specific dragged spot) and padded
        // the reserved strip thicker than the visible bar. Fallback
        // to the window rect if the DWM query ever refuses.
        meterbar::IntRect rect{ wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top };
        { meterbar::IntRect vis; if (screenSpace.windowVisibleRect(hwnd, vis)) rect = vis; }
        const meterbar::IntRect monRect{ mi.rcMonitor.left, mi.rcMonitor.top,
                                         mi.rcMonitor.right - mi.rcMonitor.left,
                                         mi.rcMonitor.bottom - mi.rcMonitor.top };
        const meterbar::EdgeGaps gaps{ std::abs(rect.y - monRect.y),
                                       std::abs(monRect.bottom() - rect.bottom()),
                                       std::abs(rect.x - monRect.x),
                                       std::abs(monRect.right() - rect.right()) };
        const long long monId = (long long)(uintptr_t)mon;
        auto registerNow = [this, hwnd] {
            const meterbar::IntRect strip = reserveMachine.requestedRect();
            screenSpace.reserveEdge(hwnd, reserveMachine.edge(), strip.x, strip.y, strip.w, strip.h);
        };
        switch (reserveMachine.update(engine.state.reserveScreenSpace, gaps, rect, monRect, monId)) {
            case meterbar::ReserveAction::Register:
            case meterbar::ReserveAction::Update:
                registerNow();
                break;
            case meterbar::ReserveAction::Switch:
                screenSpace.release(); // old edge off...
                registerNow();         // ...new edge on — exactly once each
                break;
            case meterbar::ReserveAction::Release:
                screenSpace.release();
                // Toggle-OFF is a full stop: the reservation is removed
                // AND any arranged windows go home in the same action.
                // After this, no code path moves another window until
                // the user presses Arrange again. (An un-dock release —
                // toggle still on — leaves the arrange record alone.)
                if (!engine.state.reserveScreenSpace) screenSpace.restore();
                break;
            case meterbar::ReserveAction::None:
                break;
        }
    }
#else
    screenSpace.release();
#endif
    // The toolbar Link toggle mirrors state whichever surface
    // changed it. (Reserve's old toolbar mirror went with its
    // button in v0.21.2; since v0.28.9 the Reposition menu's Stick
    // item reads the merged stick state fresh each time it opens.)
    if (linkBtn.getToggleState() != engine.state.linkEnabled)
        linkBtn.setToggleState(engine.state.linkEnabled, juce::dontSendNotification);
}
void MainComponent::notifyWindowGeometryChanged() { syncScreenSpace(); linkClient_.notifyLocalGeometry(); }
// v0.18 ARRANGE: slide every other window on this bar's monitor clear
// of the bar. The strip the sweep clears is the bar's far edge measured
// from the monitor edge it is docked to — so it does the right thing
// even if the bar is floating a little off the edge. (The fitting math
// is meterbar::arrangeFit; ScreenSpace does the enumeration.)
// v0.19.1: the docked edge is the bar's nearest edge by the same gap
// math the reserve machine uses — a bottom-docked bar (Jackson's)
// arranges windows into the area ABOVE it, not below it.
void MainComponent::doArrangeWindows() {
#if JUCE_WINDOWS
    auto* w = findParentComponentOfClass<juce::DocumentWindow>();
    HWND hwnd = (w && w->getPeer()) ? static_cast<HWND>(w->getPeer()->getNativeHandle()) : nullptr;
    if (hwnd) {
        RECT wr{}; GetWindowRect(hwnd, &wr);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof mi; GetMonitorInfoW(mon, &mi);
        // v0.28.9: visible rect, same as syncScreenSpace (strip is
        // measured to the visible bar's far edge, not the frame's).
        meterbar::IntRect rect{ wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top };
        { meterbar::IntRect vis; if (screenSpace.windowVisibleRect(hwnd, vis)) rect = vis; }
        const meterbar::IntRect monRect{ mi.rcMonitor.left, mi.rcMonitor.top,
                                         mi.rcMonitor.right - mi.rcMonitor.left,
                                         mi.rcMonitor.bottom - mi.rcMonitor.top };
        const meterbar::EdgeGaps gaps{ std::abs(rect.y - monRect.y),
                                       std::abs(monRect.bottom() - rect.bottom()),
                                       std::abs(rect.x - monRect.x),
                                       std::abs(monRect.right() - rect.right()) };
        const meterbar::ReserveEdge edge = meterbar::ReserveMachine::nearestEdge(gaps, rect);
        int strip = 0;
        switch (edge) {
            case meterbar::ReserveEdge::Top:    strip = rect.bottom() - monRect.y; break;
            case meterbar::ReserveEdge::Bottom: strip = monRect.bottom() - rect.y; break;
            case meterbar::ReserveEdge::Left:   strip = rect.right() - monRect.x; break;
            case meterbar::ReserveEdge::Right:  strip = monRect.right() - rect.x; break;
        }
        screenSpace.arrange(hwnd, edge, strip);
        return;
    }
#endif
    screenSpace.arrange(nullptr, meterbar::ReserveEdge::Top, 0); // stub: records the honest "unavailable" status
}
void MainComponent::doRestoreWindows() { screenSpace.restore(); }
void MainComponent::rebuildSettings() {
    // v0.28.2 SETTINGS MOUSE CONFINEMENT: build() destroys and
    // recreates EVERY Settings control. While the window is VISIBLE
    // a control may hold a live drag/capture (slider, scrollbar,
    // combo) — destroying it mid-gesture orphans exactly the capture
    // state the confinement feeds on. A re-open while visible now
    // just brings the window to the front; the rebuild still happens
    // on every open-from-hidden, which is the normal flow.
    if (settingsWin->isVisible()) return;
    settingsWin->content.build();
}
// v0.28.6: SETTINGS TOGGLE (Jackson's feel-fixes ask: "if the
// settings bar is already open, clicking settings again should
// close the settings"). Both open paths — the toolbar gear and
// the hamburger menu's Settings item — route through here, so the
// toggle behaves identically from either. Closing is a plain
// hide. Opening keeps the exact v0.28.2 flow: rebuild-from-hidden
// (rebuildSettings' own visible-guard stands) + the mouse-capture
// hygiene for the bar's window (this click's mouse-down capture is
// still held while onClick runs; JUCE releases it in the peer's
// mouse-up path, after dispatch — free it now so opening Settings
// can never inherit a live capture).
void MainComponent::toggleSettings() {
    if (settingsWin->isVisible()) { settingsWin->setVisible(false); return; }
    rebuildSettings(); settingsWin->setVisible(true); settingsWin->toFront(true);
    settingsPalette::releaseCaptureIfHeldBy(this); settingsPalette::resetMouseSources();
}
void MainComponent::paint(juce::Graphics& g) {
    // v0.13: background fills honour the Background Opacity setting; at 1.0
    // (default) this paints exactly as before, below 1.0 the desktop shows
    // through the bar's background and the gaps between modules.
    const float bgOp = juce::jlimit(0.15f, 1.0f, engine.state.backgroundOpacity);
    // v0.16: the window background paints the selected Background Style
    // (Flat = the old flat theme.bg fill, exactly).
    // v0.20.0: with the visualizer ON, the background is the visualizer
    // layer instead (black base + blurred glow + particles) — module
    // panels still paint over it at the Background Opacity setting.
    if (engine.state.visualizerEnabled) drawVisualizerBackground(g);
    else fillStyledBackground(g, getLocalBounds().toFloat(), engine.state.backgroundStyle, theme, bgOp);
    if (engine.state.showToolbar) {
        g.setColour(theme.panel.withAlpha(bgOp)); g.fillRect(0, 0, getWidth(), 30);
        // v0.12: a thin palette-gradient strip under the toolbar — the icon
        // family's teal -> green -> yellow -> orange -> pink, left to right.
        const int segs = 64;
        for (int i = 0; i < segs; ++i) { g.setColour(paletteGrad((float)i / (segs - 1)));
            g.fillRect((float)i * getWidth() / segs, 28.f, (float)getWidth() / segs + 1.f, 2.f); }
    }
    // logo mark: the icon's 5 bars (teal/green/yellow/orange/pink)
    auto r = logoLabel.getBounds().toFloat(); float bx = r.getX() - 27;
    static const float heights[5] = { 7.f, 12.f, 16.f, 11.f, 8.f };
    for (int i = 0; i < 5; ++i) { g.setColour(paletteGrad(0.25f * i));
        g.fillRect(bx + i * 5.f, r.getCentreY() - heights[i] / 2, 3.2f, heights[i]); }
}
void MainComponent::resized() {
    auto& st = engine.state;
    auto area = getLocalBounds();
    if (st.showToolbar) {
        auto bar = area.removeFromTop(30);
        logoLabel.setBounds(bar.removeFromLeft(128).withTrimmedLeft(30));
        versionLabel.setBounds(bar.removeFromLeft(46));
        // v0.22.0: the icon-only cluster — Link | Rec | Pause |
        // Settings | Menu, uniform 44 px glyph buttons. Everything
        // else that used to sit here is in the Menu now.
        burgerBtn.setBounds(bar.removeFromRight(44)); settingsBtn.setBounds(bar.removeFromRight(44));
        pauseBtn.setBounds(bar.removeFromRight(44)); recBtn.setBounds(bar.removeFromRight(44));
        linkBtn.setBounds(bar.removeFromRight(44));
        // v0.28.6: the TRIM cluster (label + slider) takes the first
        // slice of the leftover middle strip — but only while the
        // strip can spare ~190 px; narrower than that the cluster
        // hides rather than crushing the hint / update surfaces
        // (the Settings slider remains the trim editor there).
        if (bar.getWidth() >= 190) {
            trimLabel_.setBounds(bar.removeFromLeft(30));
            trimSlider_.setBounds(bar.removeFromLeft(130).reduced(0, 4));
            trimLabel_.setVisible(true); trimSlider_.setVisible(true);
        } else { trimLabel_.setVisible(false); trimSlider_.setVisible(false); }
        // v0.19.2: the update indicator takes a slice of the leftover
        // middle strip (where the hint label lives) — only while lit.
        if (updateAvailable) { updateBtn.setBounds(bar.removeFromLeft(78)); updateBtn.setVisible(true); }
        else updateBtn.setVisible(false);
        if (st.showRepositionHint) { hintLabel.setBounds(bar); hintLabel.setVisible(true); } else hintLabel.setVisible(false);
    } else {
        for (auto* b : { &recBtn, &pauseBtn, &settingsBtn, &burgerBtn }) b->setVisible(false);
        logoLabel.setVisible(false); versionLabel.setVisible(false); hintLabel.setVisible(false);
        trimLabel_.setVisible(false); trimSlider_.setVisible(false); // v0.28.6: no toolbar, no trim cluster
        updateBtn.setVisible(false);
    }
    if (st.showToolbar) for (auto* b : { &recBtn, &pauseBtn, &settingsBtn, &burgerBtn }) b->setVisible(true);
    logoLabel.setVisible(st.showToolbar); versionLabel.setVisible(st.showToolbar);
    // v0.24.0: the docked caption strip owns the bottom band of the
    // (grown) bar; layoutModules() leaves that band out of its area.
    if (captionStripInHeight_)
        captionStrip_.setBounds(0, getHeight() - kCaptionStripHeight, getWidth(), kCaptionStripHeight);
    layoutModules();
    syncScreenSpace(); // v0.18.1: a resize is a reserve-machine event (bar height / position may have changed)
}
// v0.24.0: which edge caption-dock growth anchors at — the edge the
// reserve machine currently holds when the bar is reserved (growth
// must not move the registered edge), else the nearest edge by the
// native gaps (the bar's own dock test's inputs), else Bottom (the
// family bars' home edge).
meterbar::ReserveEdge MainComponent::captionAnchorEdge() {
    if (reserveMachine.reserved()) return reserveMachine.edge();
#if JUCE_WINDOWS
    auto* w = findParentComponentOfClass<juce::DocumentWindow>();
    HWND hwnd = (w && w->getPeer()) ? static_cast<HWND>(w->getPeer()->getNativeHandle()) : nullptr;
    if (hwnd) {
        RECT wr{}; GetWindowRect(hwnd, &wr);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(mon, &mi)) {
            const int gapTop = std::abs(wr.top - mi.rcMonitor.top);
            const int gapBottom = std::abs(mi.rcMonitor.bottom - wr.bottom);
            return gapBottom <= gapTop ? meterbar::ReserveEdge::Bottom : meterbar::ReserveEdge::Top;
        }
    }
#endif
    return meterbar::ReserveEdge::Bottom;
}
// v0.24.0: the docked-caption reconciliation, run every UI tick
// after captions_.poll(). Content flows to the strip continuously
// (change-detected inside it); the WINDOW only moves on a state
// transition — docked captions turning on grows the bar by the
// strip height, turning off (menu Off, presentation flip, or the
// controller's own error auto-off) shrinks it back. The growth is
// applied through the top level's setBounds, so it rides the exact
// feeds every other geometry change rides: MainWindow::resized ->
// notifyWindowGeometryChanged -> the reserve machine (which reads
// the grown rect and Updates the strip to the new real thickness)
// and the link client (which leads the change to linked followers;
// stack plans and group-dock read the new rect). No side channel,
// no cached height anywhere.
void MainComponent::syncCaptionDock() {
    const bool want = captions_.dockedActive();
    if (want) {
        const auto c = captions_.dockedContent();
        captionStrip_.setContent(c.previous, c.current, c.currentFinal, c.badge, c.pill);
    }
    if (want == captionStripInHeight_) {
        captionStrip_.setVisible(want);
        return;
    }
    auto* tl = getTopLevelComponent();
    if (tl == nullptr) return; // window not up yet — the next tick retries
    const auto b = tl->getBounds();
    const meterbar::IntRect cur{ b.getX(), b.getY(), b.getWidth(), b.getHeight() };
    const auto nb = want ? meterbar::captionGrownBarRect(cur, kCaptionStripHeight, captionAnchorEdge())
                         : meterbar::captionShrunkBarRect(cur, kCaptionStripHeight, captionAnchorEdge());
    captionStripInHeight_ = want;
    tl->setBounds(nb.x, nb.y, nb.w, nb.h);
    captionStrip_.setVisible(want);
    captionStrip_.toFront(false);
}
// v0.8: the WHOLE top bar drags the window (buttons still click normally —
// their events never reach here; labels let clicks fall through).
// v0.20.2: the drag now uses juce::ComponentDragger — SessionBar's proven
// path (freeze-audit suspect 2). v0.11 handed the drag to the OS itself
// (SendMessageW WM_NCLBUTTONDOWN / HTCAPTION — a synchronous modal move
// loop inside click dispatch); if the press/release pairing got confused
// (press begins on a fall-through label, release lands over a button or
// another window) capture state could be left wrong and the buttons dead
// until another full drag cycle — this app's own "stuck mouse capture,
// dead buttons" field history. ComponentDragger never enters an OS modal
// loop, and SessionBar ships it with no stuck-capture report.
void MainComponent::mouseDown(const juce::MouseEvent& e) {
    // v0.12: only the LEFT button may start a window move — a right/middle
    // press on the bar previously also entered the drag path.
    if (!e.mods.isLeftButtonDown()) return;
    // v0.28.9: while STUCK the bar cannot be dragged (Jackson's
    // 2026-11-03 ruling, carried into the dock merge) — Stick off
    // first. The flag alone never enforced this before: Stick only
    // snapped the bar to the top and the drag stayed live.
    if (engine.state.stick) return;
    if (engine.state.showToolbar && e.eventComponent == this && e.getPosition().y < 30) {
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) {
            windowDragger.startDraggingComponent(w, e);
            winDragging = true;
        }
    }
}
void MainComponent::mouseDrag(const juce::MouseEvent& e) {
    if (winDragging) {
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            windowDragger.dragComponent(w, e, nullptr);
    }
}
void MainComponent::mouseUp(const juce::MouseEvent&) { winDragging = false; }
void MainComponent::adjustFlex(int leftFlexIdx, int rightFlexIdx, int dxPx) {
    // Convert the pixel drag into a flex delta using the CURRENT geometry,
    // then trade width inside the pair only (pair sum preserved, each side
    // keeps a sane minimum so a module can never be dragged to nothing).
    auto& st = engine.state;
    float totalFlex = 0.f; for (auto& m : modules) if (m->isVisible()) totalFlex += st.moduleFlex[(size_t)m->flexIndex];
    auto area = getLocalBounds(); if (st.showToolbar) area.removeFromTop(30);
    if (captionStripInHeight_) area.removeFromBottom(kCaptionStripHeight); // v0.24.0: same content area as layoutModules
    if (totalFlex <= 0.f || area.getWidth() <= 0) return;
    float dw = (float)dxPx * totalFlex / (float)area.getWidth();
    float pair = st.moduleFlex[(size_t)leftFlexIdx] + st.moduleFlex[(size_t)rightFlexIdx];
    float newL = juce::jlimit(0.35f, pair - 0.35f, st.moduleFlex[(size_t)leftFlexIdx] + dw);
    st.moduleFlex[(size_t)leftFlexIdx] = newL;
    st.moduleFlex[(size_t)rightFlexIdx] = pair - newL;
    layoutModules(); repaint();
}
void MainComponent::layoutModules() {
    auto area = getLocalBounds(); if (engine.state.showToolbar) area.removeFromTop(30);
    if (captionStripInHeight_) area.removeFromBottom(kCaptionStripHeight); // v0.24.0: the docked strip's band is not module space
    std::vector<ModuleView*> vis;
    auto& st = engine.state;
    for (auto& m : modules) {
        bool en = (m->kind == ModuleView::Kind::Spectrogram && st.spectrogramEnabled) ||
                  (m->kind == ModuleView::Kind::Waveform && st.waveformEnabled) ||
                  (m->kind == ModuleView::Kind::Loudness && st.loudnessEnabled) ||
                  (m->kind == ModuleView::Kind::Stereometer && st.stereometerEnabled) ||
                  (m->kind == ModuleView::Kind::Oscilloscope && st.oscilloscopeEnabled) ||
                  (m->kind == ModuleView::Kind::Spectrum && st.spectrumEnabled);
        m->setVisible(en); if (en) vis.push_back(m.get());
    }
    for (auto& s : splitters) s->setVisible(false);
    // v0.16: in the Anime theme the mascot cell joins the strip AFTER the
    // last module, with its own flex width and its own leading splitter.
    const bool showMascot = mascot.isVisible();
    if (vis.empty() && !showMascot) return;
    // v0.15: widths follow PresetState::moduleFlex (equal by default, so a
    // fresh state lays out exactly as before); a splitter sits on each gap.
    float totalFlex = 0.f; for (auto* m : vis) totalFlex += st.moduleFlex[(size_t)m->flexIndex];
    if (showMascot) totalFlex += st.mascotFlex;
    if (totalFlex <= 0.f) totalFlex = (float)vis.size();
    int x = area.getX();
    size_t spIdx = 0;
    auto placeSplitter = [&](int gapX, std::function<void(int)> drag) {
        if (spIdx >= splitters.size()) return;
        auto* sp = splitters[spIdx++].get();
        sp->setBounds(gapX - 5, area.getY(), 10, area.getHeight());
        sp->onDrag = std::move(drag);
        sp->setVisible(true); sp->toFront(false);
    };
    for (size_t i = 0; i < vis.size(); ++i) {
        bool last = (i + 1 == vis.size()) && !showMascot;
        int wpx = last ? (area.getRight() - x)
                       : (int)std::lround(area.getWidth() * st.moduleFlex[(size_t)vis[i]->flexIndex] / totalFlex);
        vis[i]->setBounds(x, area.getY(), std::max(10, wpx - 2), area.getHeight());
        if (i + 1 < vis.size()) {
            int li = vis[i]->flexIndex, ri = vis[i + 1]->flexIndex;
            placeSplitter(x + wpx, [this, li, ri](int dx){ adjustFlex(li, ri, dx); });
        }
        x += wpx;
    }
    if (showMascot) {
        if (!vis.empty()) placeSplitter(x, [this](int dx){ adjustMascotFlex(-dx); });
        int wpx = area.getRight() - x;
        mascot.setBounds(x, area.getY(), std::max(10, wpx - 2), area.getHeight());
    }
}
// v0.20.0: render the visualizer model into a half-res image (capped
// 480x270 — the particle layer never needs full res under a blur),
// blur a copy of it with a Gaussian convolution, and composite:
// black -> blurred glow -> crisp dots on top. The model itself is
// the capped pure core (<= 600 particles), so per-frame cost is the
// image work only.
// v0.28.0: render the Undertow model into the half-res layer.
// Local field map only — Undertow is a local section (over the
// link it presents as Bubbles via the shared 3-band broadcast,
// which stays untouched). Every alpha is pre-scaled by the
// model's display gain (visual limiter x silence gate): the
// limiter scales alphas only, never timing (DESIGN §(f)).
void MainComponent::drawUndertowLayer(juce::Graphics& ig, int iw, int ih) {
    auto own = getScreenBounds();
    const auto fm = meterbar::fieldMap(
        meterbar::IntRect{ own.getX(), own.getY(), own.getWidth(), own.getHeight() },
        meterbar::IntRect{ own.getX(), own.getY(), own.getWidth(), own.getHeight() }, iw, ih);
    const float sc = (float)ih / 270.f; // canonical-height scale (model radii/widths are px at 270)
    const float gain = undertow_.displayGain();
    auto col = [](meterbar::VizRgb c) { return juce::Colour(c.r, c.g, c.b); };
    const auto& lv = undertow_.levels();
    constexpr int kEdge = meterbar::FiveBandLevels::kEdgePoints;
    // ---- ground membrane: fill shaped by the 64-point sub/low
    // edge profile, a stroked top edge, and the kick crest — the
    // edge lifted by the spike envelope through the render-only
    // easeOutBack mapping (bounce lives in the mapping, never in
    // the signal; there is no spring in this section). ----
    {
        const float mh = undertow_.membraneHeight();
        juce::Path fill, edge;
        for (int i = 0; i < kEdge; ++i) {
            const float u = (float)i / (float)(kEdge - 1);
            const float h = mh * (0.30f + 0.70f * lv.edge[i]);
            float px, py;
            meterbar::fieldToLocal(fm, u, 1.f - h, px, py);
            if (i == 0) { fill.startNewSubPath(px, (float)ih); fill.lineTo(px, py); edge.startNewSubPath(px, py); }
            else { fill.lineTo(px, py); edge.lineTo(px, py); }
            if (i == kEdge - 1) fill.lineTo(px, (float)ih);
        }
        fill.closeSubPath();
        ig.setColour(col(meterbar::UndertowModel::bandColour(0)).withAlpha(undertow_.membraneFillAlpha() * 0.55f * gain));
        ig.fillPath(fill);
        ig.setColour(col(meterbar::UndertowModel::membraneEdgeColour())
                         .withAlpha(juce::jmin(1.f, undertow_.membraneFillAlpha() + 0.2f) * gain));
        ig.strokePath(edge, juce::PathStrokeType(1.5f * sc));
        const float spike = undertow_.kickSpikeDisplay();
        if (spike > 0.01f) {
            juce::Path crest;
            for (int i = 0; i < kEdge; ++i) {
                const float u = (float)i / (float)(kEdge - 1);
                const float h = mh * (0.30f + 0.70f * lv.edge[i]) + spike * 0.10f * (0.4f + 0.6f * lv.edge[i]);
                float px, py;
                meterbar::fieldToLocal(fm, u, 1.f - h, px, py);
                if (i == 0) crest.startNewSubPath(px, py); else crest.lineTo(px, py);
            }
            ig.setColour(col(meterbar::UndertowModel::membraneEdgeColour())
                             .withAlpha(juce::jmin(1.f, spike) * 0.85f * gain));
            ig.strokePath(crest, juce::PathStrokeType(2.f * sc));
        }
    }
    // ---- echo rings: follow-through for confirmed hits, born at
    // the distributed emitters on the membrane line ----
    for (auto& r : undertow_.rings()) {
        if (!r.active) continue;
        float cx, cy;
        meterbar::fieldToLocal(fm, r.x, 0.76f, cx, cy);
        const float rad = (r.age / 0.280f) * 0.32f * (float)iw;
        auto rc = col(meterbar::UndertowModel::membraneEdgeColour());
        if (r.warm) rc = rc.interpolatedWith(col(meterbar::UndertowModel::ringWarmColour()), 0.10f); // beat 1: ~10% toward the warm note
        ig.setColour(rc.withAlpha(0.18f * r.amp * (1.f - r.age / 0.280f) * gain));
        ig.drawEllipse(cx - rad, cy - rad, rad * 2.f, rad * 2.f, 1.5f * sc);
    }
    // ---- trails helper: segments oldest -> newest; a segment's
    // width/alpha ride the DESIGN §(c) tapers at its age, and the
    // birth alpha factor (x0.55) is applied by the caller. ----
    auto drawTrail = [&](const meterbar::UndertowTrail& tr, juce::Colour c, float headW, float alpha) {
        for (int k = tr.n - 1; k >= 1; --k) {
            float x0, y0, x1, y1;
            meterbar::fieldToLocal(fm, tr.x[k], tr.y[k], x0, y0);
            meterbar::fieldToLocal(fm, tr.x[k - 1], tr.y[k - 1], x1, y1);
            const float age = tr.age[k];
            ig.setColour(c.withAlpha(alpha * 0.55f * meterbar::tailAlphaFactor(age)));
            ig.drawLine(x0, y0, x1, y1, headW * meterbar::tailWidthFactor(age));
        }
    };
    // ---- low orbs: discs drifting against the swarm ----
    // v0.28.9: each orb wears its OWN palette colour (was: all 12 in
    // the one slate); Halo-effect orbs get a wide soft under-disc
    // (the bloom pass blurs it into a glow); orbs parked by the
    // Low-End orb cap (alpha 0) are skipped entirely.
    for (auto& o : undertow_.orbs()) {
        if (o.alpha <= 0.001f || o.radius <= 0.f) continue;
        float px, py;
        meterbar::fieldToLocal(fm, o.x, o.y, px, py);
        const float r = o.radius * sc;
        const auto oc = col(o.colour);
        drawTrail(o.trail, oc, r * 2.f, o.alpha * gain);
        if (o.effect == meterbar::UndertowOrbEffect::Halo) {
            const float hr = r * 2.3f;
            ig.setColour(oc.withAlpha(o.alpha * 0.22f * gain));
            ig.fillEllipse(px - hr, py - hr, hr * 2.f, hr * 2.f);
        }
        ig.setColour(oc.withAlpha(o.alpha * gain));
        ig.fillEllipse(px - r, py - r, r * 2.f, r * 2.f);
    }
    // ---- mid swarm: warm-white inward swirl, the hero ----
    for (auto& p : undertow_.swarm()) {
        if (!p.alive) continue;
        float px, py;
        meterbar::fieldToLocal(fm, p.x, p.y, px, py);
        const float r = p.size * sc;
        drawTrail(p.trail, col(meterbar::UndertowModel::bandColour(2)), r * 2.f, p.alpha * gain);
        ig.setColour(col(meterbar::UndertowModel::bandColour(2)).withAlpha(p.alpha * gain));
        ig.fillEllipse(px - r, py - r, r * 2.f, r * 2.f);
    }
    // ---- high ticks: short falling streaks, transient-born ----
    for (auto& t : undertow_.ticks()) {
        if (!t.alive) continue;
        float px, py;
        meterbar::fieldToLocal(fm, t.x, t.y, px, py);
        drawTrail(t.trail, col(meterbar::UndertowModel::bandColour(3)), 1.2f * sc, t.alpha * gain);
        const float vl = std::sqrt(t.vx * t.vx + t.vy * t.vy);
        const float dx = vl > 1e-6f ? t.vx / vl : 0.f, dy = vl > 1e-6f ? t.vy / vl : 1.f;
        ig.setColour(col(meterbar::UndertowModel::bandColour(3)).withAlpha(t.alpha * gain));
        ig.drawLine(px, py, px - dx * t.len * sc, py - dy * t.len * sc, 1.2f * sc);
    }
    // ---- presence dust: level-written points, no follower ----
    {
        const int n = undertow_.dustCount();
        ig.setColour(col(meterbar::UndertowModel::bandColour(4)).withAlpha(undertow_.dustAlpha() * gain));
        for (int i = 0; i < n; ++i) {
            float ux, uy, px, py;
            undertow_.dustPoint(i, ux, uy);
            meterbar::fieldToLocal(fm, ux, uy, px, py);
            ig.fillEllipse(px - sc, py - sc, 2.f * sc, 2.f * sc);
        }
    }
}
// v0.28.0: the thresholded bright-pass bloom (DESIGN §(d)) —
// only luminance above 0.75 blooms, through a soft knee; the
// quarter-res scale blurs with the house 9-tap sigma 2.75 kernel,
// the eighth-res scale is a blur OF that blur. Post-only: this
// result never feeds back into the model. Rebuilt at <=30 Hz.
void MainComponent::rebuildUndertowBloom(int iw, int ih) {
    const int qw = std::max(1, iw / 2), qh = std::max(1, ih / 2);
    const int ew = std::max(1, iw / 4), eh = std::max(1, ih / 4);
    if (!vizBloomQ.isValid() || vizBloomQ.getWidth() != qw || vizBloomQ.getHeight() != qh)
        vizBloomQ = juce::Image(juce::Image::RGB, qw, qh, true);
    if (!vizBloomE.isValid() || vizBloomE.getWidth() != ew || vizBloomE.getHeight() != eh)
        vizBloomE = juce::Image(juce::Image::RGB, ew, eh, true);
    {   // bright-pass + 2x2 downsample in one pass
        juce::Image::BitmapData src(vizCache, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst(vizBloomQ, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < qh; ++y) {
            for (int x = 0; x < qw; ++x) {
                float rr = 0, gg = 0, bb = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(iw - 1, x * 2 + dx), sy = std::min(ih - 1, y * 2 + dy);
                    const auto* p = reinterpret_cast<const juce::PixelRGB*>(src.getPixelPointer(sx, sy));
                    rr += p->getRed(); gg += p->getGreen(); bb += p->getBlue();
                }
                rr *= 0.25f; gg *= 0.25f; bb *= 0.25f;
                const float lum = (0.2126f * rr + 0.7152f * gg + 0.0722f * bb) / 255.f;
                const float k = lum > 0.75f ? (lum - 0.75f) / 0.25f : 0.f;
                auto* q = reinterpret_cast<juce::PixelRGB*>(dst.getPixelPointer(x, y));
                q->setARGB(0xFF, (juce::uint8)(rr * k), (juce::uint8)(gg * k), (juce::uint8)(bb * k)); // PixelRGB's component setter is setARGB (alpha ignored)
            }
        }
    }
    {   // quarter-res blur (9 taps / sigma 2.75, the house kernel)
        juce::Image blurred = vizBloomQ; blurred.duplicateIfShared();
        juce::ImageConvolutionKernel kernel(9);
        kernel.createGaussianBlur(2.75f);
        kernel.applyToImage(blurred, vizBloomQ, vizBloomQ.getBounds());
        vizBloomQ = blurred;
    }
    {   // eighth-res: downsample the blurred quarter, blur again
        juce::Image::BitmapData src(vizBloomQ, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst(vizBloomE, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < eh; ++y) {
            for (int x = 0; x < ew; ++x) {
                float rr = 0, gg = 0, bb = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(qw - 1, x * 2 + dx), sy = std::min(qh - 1, y * 2 + dy);
                    const auto* p = reinterpret_cast<const juce::PixelRGB*>(src.getPixelPointer(sx, sy));
                    rr += p->getRed(); gg += p->getGreen(); bb += p->getBlue();
                }
                auto* q = reinterpret_cast<juce::PixelRGB*>(dst.getPixelPointer(x, y));
                q->setARGB(0xFF, (juce::uint8)(rr * 0.25f), (juce::uint8)(gg * 0.25f), (juce::uint8)(bb * 0.25f));
            }
        }
        juce::Image blurred = vizBloomE; blurred.duplicateIfShared();
        juce::ImageConvolutionKernel kernel(9);
        kernel.createGaussianBlur(2.75f);
        kernel.applyToImage(blurred, vizBloomE, vizBloomE.getBounds());
        vizBloomE = blurred;
    }
}
void MainComponent::drawVisualizerBackground(juce::Graphics& g) {
    auto bounds = getLocalBounds();
    if (bounds.isEmpty()) return;
    // v0.25.1: LINKED, the base under the visualizer is the SHARED
    // styled background at the shared opacity, and the blurred wash
    // is drawn at that opacity too — so the parent's translucency
    // continues into the linked siblings instead of every bar
    // sitting on its own opaque black base. Unlinked/alone the base
    // is the historical opaque black and the composite is v0.21.5's,
    // unchanged. (group/own are computed again below for the field
    // map; the values are the same.)
    {
        auto grp = linkClient_.groupRect();
        auto ownR = getScreenBounds();
        if (grp.isEmpty() || ownR.isEmpty()) grp = ownR;
        if (grp != ownR)
            fillStyledBackground(g, bounds.toFloat(), engine.state.backgroundStyle, theme,
                                 juce::jlimit(0.15f, 1.0f, engine.state.backgroundOpacity));
        else
            g.fillAll(juce::Colours::black);
    }
    // v0.28.2 PERFORMANCE MODE: the layer caps come from the core
    // limits (Full: the historical 480x270; Low-End: 320x180).
    int iw, ih;
    meterbar::perfVizLayerSize(bounds.getWidth(), bounds.getHeight(), perfLimits(), iw, ih);
    if (!vizCache.isValid() || vizCache.getWidth() != iw || vizCache.getHeight() != ih) {
        vizCache = juce::Image(juce::Image::RGB, iw, ih, true);
        vizBlurFrame_ = 0; // v0.23.1: layer resized — force a blur recompute below (frame counter reset)
    }
    meterbar::VisualizerSettings vs;
    vs.baseHue = engine.state.visualizerBaseHue;
    vs.saturation = engine.state.visualizerSaturation;
    vs.paletteSpread = engine.state.visualizerSpread;
    vs.cycleSeconds = engine.state.visualizerCycleSec;
    // v0.21.0: LINKED, the particle field spans the GROUP — the
    // union of the linked bars' screen rects — and this bar draws
    // only its slice of it (core fieldMap), so the bursts and
    // convergers play across the whole linked strip instead of
    // restarting at this bar's own centre. Unlinked or alone the
    // map is the bar-local one and the render is v0.20.3's,
    // unchanged. (The siblings render the same field from this
    // bar's BarLink audio broadcast.)
    auto group = linkClient_.groupRect();
    auto own = getScreenBounds();
    if (group.isEmpty() || own.isEmpty()) group = own;
    const auto fm = meterbar::fieldMap(
        meterbar::IntRect{ own.getX(), own.getY(), own.getWidth(), own.getHeight() },
        meterbar::IntRect{ group.getX(), group.getY(), group.getWidth(), group.getHeight() }, iw, ih);
    {
        juce::Graphics ig(vizCache);
        ig.fillAll(juce::Colours::black);
        if (engine.state.visualizerStyle == 1) {
            // v0.21.5 LINES: three filled ribbons from the core
            // model — top edge along centreY - halfThick, back
            // along centreY + halfThick — mapped through the same
            // group field map as the dots, so a linked strip reads
            // as one set of lines across the seams. Alpha is the
            // model's silence gate: quiet = nothing drawn.
            const float alpha = vizLines.visibility();
            if (alpha > 0.01f) {
                constexpr int n = meterbar::VisualizerLines::kPoints;
                for (int band = 0; band < 3; ++band) {
                    auto c = vizLines.colorFor(band, vs);
                    juce::Path path;
                    for (int i = 0; i < n; ++i) {
                        float px, py;
                        meterbar::fieldToLocal(fm, (float)i / (float)(n - 1),
                                               vizLines.centreY(band, i) - vizLines.halfThick(band, i), px, py);
                        if (i == 0) path.startNewSubPath(px, py); else path.lineTo(px, py);
                    }
                    for (int i = n - 1; i >= 0; --i) {
                        float px, py;
                        meterbar::fieldToLocal(fm, (float)i / (float)(n - 1),
                                               vizLines.centreY(band, i) + vizLines.halfThick(band, i), px, py);
                        path.lineTo(px, py);
                    }
                    path.closeSubPath();
                    ig.setColour(juce::Colour(c.r, c.g, c.b).withAlpha(alpha * 0.85f));
                    ig.fillPath(path);
                }
            }
        } else if (engine.state.visualizerStyle == 2) {
            // v0.25.0 FIELD: the vortex dots, drawn through the
            // exact same dot path (and the same half-res + blur
            // envelope) as Bubbles.
            // v0.27.0 (item 10): SCREEN BURSTS first (under the
            // dots) — three different-colour soft discs with their
            // own random transparency when a frame's transients
            // sat in ~2 bands (core VisualizerField decides).
            for (auto& sb : vizField.screenBursts()) {
                const float env = std::sin(3.14159265f * juce::jlimit(0.f, 1.f, sb.age / std::max(0.01f, sb.life)));
                auto bc = meterbar::vizHsvToRgb(sb.hue, vs.saturation, 0.85f);
                ig.setColour(juce::Colour(bc.r, bc.g, bc.b).withAlpha(sb.alpha * env * vizField.visibility()));
                float bx, by;
                meterbar::fieldToLocal(fm, sb.x, sb.y, bx, by);
                const float br = sb.radius * (float)iw * 0.5f;
                ig.fillEllipse(bx - br, by - br, br * 2.f, br * 2.f);
            }
            // v0.27.0 (item 17): the SUB BODY — one big, heavy,
            // slow object low in the field that plays only when
            // the sub plays. Its radius and glow ride the core's
            // inertial spring level (never the raw band), drawn
            // as a deep-coloured mass with a brighter rim.
            if (vizField.subLevel() > 0.03f) {
                const float sl = juce::jlimit(0.f, 1.f, vizField.subLevel());
                float sx, sy;
                meterbar::fieldToLocal(fm, 0.5f, 0.88f, sx, sy);
                const float sr2 = (0.05f + 0.13f * sl) * (float)iw;
                auto sc = meterbar::vizHsvToRgb(vs.baseHue + 0.55f, juce::jmin(1.f, vs.saturation + 0.1f), 0.30f + 0.45f * sl);
                ig.setColour(juce::Colour(sc.r, sc.g, sc.b).withAlpha(0.55f * sl * vizField.visibility()));
                ig.fillEllipse(sx - sr2, sy - sr2, sr2 * 2.f, sr2 * 2.f);
                ig.setColour(juce::Colour(sc.r, sc.g, sc.b).brighter(0.6f).withAlpha(0.8f * sl * vizField.visibility()));
                ig.drawEllipse(sx - sr2, sy - sr2, sr2 * 2.f, sr2 * 2.f, 1.5f + 2.5f * sl);
            }
            // v0.27.0 (item 12): each dot's TAIL is one continuous
            // TAPERED STREAK along its motion path — a polyline
            // through the 4 trail stamps + the head, stroked in
            // segments of decreasing width and brightness (thick
            // at the head, thinning to the oldest point). NOT
            // ghost balls (the v0.26.0 render Jackson replaced).
            const float sizeScale = vizField.sizeScale(); // v0.27.0 (item 9): stereo-image bubble size
            for (auto& p : vizField.particles()) {
                float px, py;
                meterbar::fieldToLocal(fm, p.x, p.y, px, py);
                const float r = p.size * 1.35f * sizeScale;
                if (!meterbar::fieldDotVisible(px, py, r, iw, ih)) continue; // another bar's slice of the field
                if (p.trailSet) {
                    // points oldest -> head
                    float xs[5], ys[5];
                    for (int k = 0; k < 4; ++k) meterbar::fieldToLocal(fm, p.trailX[3 - k], p.trailY[3 - k], xs[k], ys[k]);
                    xs[4] = px; ys[4] = py;
                    for (int sgm = 0; sgm < 4; ++sgm) {
                        auto tc = vizField.trailColorFor(p, vs, 3 - sgm);
                        ig.setColour(juce::Colour(tc.r, tc.g, tc.b));
                        const float wdt = r * (0.35f + 0.55f * (float)sgm / 3.f); // tapers toward the tail tip
                        ig.drawLine(xs[sgm], ys[sgm], xs[sgm + 1], ys[sgm + 1], wdt);
                    }
                }
                auto c = vizField.colorFor(p, vs);
                ig.setColour(juce::Colour(c.r, c.g, c.b));
                ig.fillEllipse(px - r, py - r, r * 2.f, r * 2.f);
            }
        } else if (engine.state.visualizerStyle == 3) {
            // v0.28.0 UNDERTOW: the five-band field, rendered from
            // the core model only — the renderer applies no
            // smoothing of its own to any audio envelope (DESIGN
            // principle 4: hold, don't interpolate).
            drawUndertowLayer(ig, iw, ih);
        } else {
            for (auto& p : visualizer.particles()) {
                auto c = visualizer.colorFor(p, vs);
                ig.setColour(juce::Colour(c.r, c.g, c.b));
                float px, py;
                meterbar::fieldToLocal(fm, p.x, p.y, px, py);
                const float r = p.size * 1.35f; // v0.20.3: plumper dots — softer, rounder blobs (TickerBar v0.4.0 sibling)
                if (!meterbar::fieldDotVisible(px, py, r, iw, ih)) continue; // another bar's slice of the field
                ig.fillEllipse(px - r, py - r, r * 2.f, r * 2.f);
            }
        }
    }
    // v0.28.0 UNDERTOW composite: the generic blur wash below is
    // NOT thresholded, and DESIGN §(d) forbids bloom smearing the
    // near-black field — so this style takes its own path: label
    // safe zones are dimmed in the layer itself (x0.35, which also
    // masks the bright-pass there), a thresholded bright-pass
    // bloom composites at the two committed scales (total wash
    // <= 0.55), then the crisp layer at 0.30 and the house scrim.
    // Module labels paint after this function returns, so text
    // never blooms (DESIGN principle 8).
    if (engine.state.visualizerStyle == 3) {
        {
            juce::Graphics ig2(vizCache);
            // v0.28.1: the safe-zone dim drops 0.65 -> 0.35. At 0.65
            // the dim ate most of the field wherever a module sat —
            // and modules tile most of the bar — which is part of
            // why the layer read as absent in the main view while
            // the bare Field view looked fine.
            ig2.setColour(juce::Colours::black.withAlpha(0.35f));
            const float sx = (float)iw / (float)std::max(1, bounds.getWidth());
            const float sy = (float)ih / (float)std::max(1, bounds.getHeight());
            for (auto& r : utSafeLocal_)
                ig2.fillRect((float)r.getX() * sx, (float)r.getY() * sy,
                             (float)r.getWidth() * sx, (float)r.getHeight() * sy);
        }
        const bool groupedU = group != own;
        const float bgOpU = juce::jlimit(0.15f, 1.0f, engine.state.backgroundOpacity);
        const float washOpU = groupedU ? bgOpU : 1.0f;
        // v0.28.2 PERFORMANCE MODE: the effective bloom switches are
        // the core limits' — under Low-End both scales are off, so
        // the dual convolution rebuild is skipped entirely, not just
        // its composite.
        const auto plU = perfLimits();
        if (plU.bloomQuarter || plU.bloomEighth) { if ((utBloomFrame_++ % 2) == 0) rebuildUndertowBloom(iw, ih); } // <=30 Hz (DESIGN §(d))
        if (plU.bloomQuarter && vizBloomQ.isValid()) {
            g.setOpacity(0.45f * washOpU);
            g.drawImage(vizBloomQ, bounds.toFloat());
        }
        if (plU.bloomEighth && vizBloomE.isValid()) {
            g.setOpacity(0.30f * washOpU);
            g.drawImage(vizBloomE, bounds.toFloat());
        }
        // v0.28.1 VISIBILITY FIX (the invisible-background bug,
        // Jackson's v0.28.0 grade): the crisp pass rises 0.30 ->
        // 0.85 and the 0.18 black scrim over the whole layer is
        // GONE. The v0.28.0 stack multiplied out to almost nothing
        // by the time it reached the screen: model alphas (dust
        // 0.18 x level, orbs ~0.1-0.28, membrane fill x0.55) x the
        // Governor's display gain x crisp 0.30 x scrim ~0.82 left
        // typical elements at 1-3% effective alpha over black —
        // under the modules' own translucent panels, invisible.
        // The Field style survived the same 0.30 crisp pass only
        // because its elements draw at near-full alpha. The
        // Governor's luminance budget is still enforced inside the
        // core model (its estimate caps ~0.18), so the layer stays
        // a background; what changed is that the renderer no longer
        // throws away two-thirds of what the model produced.
        g.setOpacity(groupedU ? 0.85f * bgOpU : 0.85f);
        g.drawImage(vizCache, bounds.toFloat());
        g.setOpacity(1.0f);
        return;
    }
    // v0.23.1 (LAG FIX — audit Finding 3): the blur used to be a full
    // 15x15 Gaussian over the entire half-res layer EVERY frame (at
    // cap ~130k px x 225 taps of scalar code on the message thread).
    // With the visualizer + background sync on, that constant cost is
    // what starves mouse-move events, so the whole linked stack lags
    // while dragging. Now the convolution runs on a QUARTER-res copy
    // (half the half-res layer again: capped 240x135) with the
    // spatially equivalent kernel (9 taps / sigma 2.75 == the old
    // 15 / 5.5 at half scale), and only every 2nd frame — the result
    // is held between recomputes and upscaled on draw. A wash this
    // soft loses nothing visible at quarter res or ~30 Hz; the crisp
    // particle pass below still renders every frame, so the motion
    // stays fluid. Convolution cost drops to roughly 1/20th.
    {
        const int bw = juce::jmax(1, iw / 2), bh = juce::jmax(1, ih / 2);
        const bool blurSizeChanged = !vizBlurSrc.isValid() || vizBlurSrc.getWidth() != bw || vizBlurSrc.getHeight() != bh
                                  || !vizBlur.isValid() || vizBlur.getWidth() != bw || vizBlur.getHeight() != bh;
        if (blurSizeChanged) {
            vizBlurSrc = juce::Image(juce::Image::RGB, bw, bh, true);
            vizBlur = juce::Image(juce::Image::RGB, bw, bh, true);
        }
        if (blurSizeChanged || (vizBlurFrame_++ % 2) == 0) {
            {
                juce::Graphics bg(vizBlurSrc);
                bg.drawImage(vizCache, 0, 0, bw, bh, 0, 0, iw, ih); // downsample
            }
            vizBlur = vizBlurSrc;
            vizBlur.duplicateIfShared(); // the convolution reads source while writing dest — they must not share pixels
            juce::ImageConvolutionKernel kernel(9);
            kernel.createGaussianBlur(2.75f); // v0.23.1: == v0.21.5's 15/5.5 melt, at quarter res
            kernel.applyToImage(vizBlur, vizBlurSrc, vizBlur.getBounds());
        }
    }
    // v0.25.1: grouped, the wash + crisp passes ride the shared
    // opacity (the twins' composite); ungrouped the wash is full
    // and the crisp pass sits at 0.30.
    // v0.27.0 (item 11): FOGGY GLASS — the blur's strength now
    // falls off RADIALLY: the wash composites at reduced strength
    // across the whole frame, then the centre region composites
    // AGAIN at full strength, so the middle reads as thick fog
    // diffusing toward clearer edges. The crisp pass is drawn
    // scaled ~4% up about the centre — a small radial displacement
    // of the background sampling that reads as slight refraction
    // through the fogged layer. The convolution itself is still
    // the quarter-res kernel above (performance approach kept).
    const bool grouped = group != own;
    const float bgOp = juce::jlimit(0.15f, 1.0f, engine.state.backgroundOpacity);
    const float washOp = grouped ? bgOp : 1.0f;
    g.setOpacity(0.55f * washOp);
    g.drawImage(vizBlur, bounds.toFloat());
    g.setOpacity(washOp);
    {
        const int cw = juce::roundToInt(bounds.getWidth() * 0.55f), chh = juce::roundToInt(bounds.getHeight() * 0.55f);
        const int dx = (bounds.getWidth() - cw) / 2, dy = (bounds.getHeight() - chh) / 2;
        const int sw = juce::roundToInt(vizBlur.getWidth() * 0.55f), sh = juce::roundToInt(vizBlur.getHeight() * 0.55f);
        const int sx = (vizBlur.getWidth() - sw) / 2, sy = (vizBlur.getHeight() - sh) / 2;
        g.drawImage(vizBlur, dx, dy, cw, chh, sx, sy, sw, sh);
    }
    g.setOpacity(grouped ? 0.30f * bgOp : 0.30f); // v0.21.5: crisp pass backed off further (was 0.45) so the wash, not the dots, is what reads
    g.drawImage(vizCache, bounds.toFloat().expanded(bounds.getWidth() * 0.02f, bounds.getHeight() * 0.02f)); // v0.27.0: refraction displacement
    g.setOpacity(1.0f);
    g.setColour(juce::Colours::black.withAlpha(0.18f)); // v0.20.3: dark scrim — the moodier tone
    g.fillRect(bounds);
}
void MainComponent::timerCallback() {
    // v0.28.8: the close-save baseline — the state as opened,
    // captured on the first tick (MainWindow has placed the window
    // by now; the stamp normalises the caption strip back out, so
    // the baseline and any later comparison speak the same rect).
    if (!openBaselineCaptured_) {
        syncWindowRectToState();
        openBaseline_ = engine.state;
        openBaselineCaptured_ = true;
    }
    pollUpdater(); // v0.19: surface a finished background update check / download on the message thread
    captions_.poll(); // v0.23.0: Live Captions — drain helper events, track pause/source/epoch edges, drive the overlay
    syncCaptionDock(); // v0.24.0: docked presentation — feed the strip; grow/shrink the bar on state transitions
    auto now = juce::Time::getCurrentTime().toMilliseconds();
    // v0.18.1: NO screen-space work happens here any more. The v0.18
    // ~4x/sec timer poll of syncScreenSpace() was the flicker's engine;
    // the reservation is event-driven now (see syncScreenSpace).
    if (lastFrameMs) { double inst = 1000.0 / std::max<juce::int64>(1, now - lastFrameMs); uiFpsMeasured = 0.9 * uiFpsMeasured + 0.1 * inst; }
    // v0.28.2 PERFORMANCE MODE (Auto): feed the core latch the
    // measured fps against the user's own target. On the frame it
    // trips, retime the UI timer to the Low-End cap at once; the
    // latch then holds for the session (Performance.hpp).
    if (engine.state.performanceMode == 2 && lastFrameMs) {
        const double dt = (double)std::max<juce::int64>(1, now - lastFrameMs) / 1000.0;
        const bool was = perfAuto_.latched();
        perfAuto_.update(uiFpsMeasured, engine.state.fpsLimit > 0 ? engine.state.fpsLimit : 60, dt);
        if (!was && perfAuto_.latched()) { savePerfPersist(true); applyFps(); } // v0.28.2: the step-down persists — next launch starts stepped down
    }
    lastFrameMs = now;
    ++perfFrame_;
    // v0.28.7: pinned-source watcher — a ~1 s step inside the
    // frame timer (process enumeration is a Toolhelp snapshot;
    // once a second is plenty for an app appearing/disappearing).
    if (now - pinLastTickMs_ >= 1000) { pinLastTickMs_ = now; pollPinnedSource(now); }
    // v0.28.6: mirror the trim STATE into the bar slider — the
    // Settings slider and preset loads write the state directly,
    // so the bar surface follows them here (never while the user
    // is dragging the bar slider itself).
    if (!trimSlider_.isMouseButtonDown()
        && std::abs(trimSlider_.getValue() - (double)engine.state.trimDb) > 0.001)
        trimSlider_.setValue(engine.state.trimDb, juce::dontSendNotification);
    // v0.28.3: keep the crash handler's state snapshot current
    // (throttled — every ~90 frames) with the last state the app
    // can honestly know. The handler itself never formats; if the
    // process dies, the crash record carries this snapshot.
    if (perfFrame_ % 90 == 0) {
        juce::String snap;
        snap << "audio source: " << engine.state.audioSource << " (app source " << engine.state.appSource << ")\n";
        if (engine.state.appSource == 4) snap << "pinned exe: " << engine.state.pinnedExe << "\n"; // v0.28.7: pin forensics ride the crash snapshot
        snap << "capture status: " << engine.loopbackStatus() << "\n";
        snap << "performance mode: " << engine.state.performanceMode << (perfAuto_.latched() ? " (auto stepped down)\n" : "\n");
        crashhandler::setStateSnapshot(snap.toRawUTF8());
    }
    if (!engine.paused.load()) prepareFrame();
    repaint();
    // v0.12: the Rec button mirrors the engine every frame — a recording can
    // now also end via a source switch (applySource stops it), and the label
    // must never claim "Stop" for a take that already ended.
    {
        bool rec = engine.isRecording();
        const char* want = rec ? "Stop" : "Rec";
        if (recBtn.getButtonText() != want) recBtn.setButtonText(want);
        recBtn.setColour(juce::TextButton::textColourOffId, rec ? theme.bad : theme.accent);
    }
    // v0.16: Beat Pulse envelope into the frame + the mascot's animation
    // triggers. A fresh onset starts a ~130 ms bob hop and a ~600 ms
    // excited window; she also stays excited while the short-term level
    // runs hot (above about -12 LUFS). Calm otherwise.
    frame.beatGlow = engine.beatLevel();
    // BLK7 (DESIGN §7): pulse emit bookkeeping — on a fresh onset
    // whose level clears the UI Beat Pulse threshold (0.01, the
    // ModuleViews render gate), advance the host pulse seq.
    {
        const uint64_t oc = engine.beatOnsets();
        const float lvl = engine.beatLevel();
        if (oc != pulseOnsetCount_) {
            pulseOnsetCount_ = oc;
            if (lvl > 0.01f) {
                pulseSeq_ += 1;
                pulseStrength_ = juce::jlimit(0.f, 1.f, lvl);
                pulseColour_ = (int)(theme.accent.getARGB() & 0x00FFFFFFu);
            }
        }
    }
    if (mascot.isVisible()) {
        uint64_t oc = engine.beatOnsets();
        if (oc != lastOnsetCount) { lastOnsetCount = oc; bobStartMs = now; excitedUntilMs = now + 600; }
        double tBob = (double)(now - bobStartMs);
        mascot.bobPx = (bobStartMs > 0 && tBob < 130.0)
            ? (float)(-5.0 * std::sin(3.14159265358979 * tBob / 130.0)) : 0.f;
        mascot.excited = (now < excitedUntilMs) || (engine.loudnessShortTerm() > -12.0);
        mascot.repaint();
    }
    if (debugWin->isVisible()) {
        // v0.14: the status line must never let "Source: System Loopback"
        // imply capture is live when it isn't. If loopback was requested
        // but the capture thread is not in its running state, say so in
        // words: NOT CAPTURING - <actual stage/status>.
        juce::String lbStatus = engine.loopbackStatus();
        if (engine.state.audioSource == "System Loopback" && lbStatus != "running")
            lbStatus = "NOT CAPTURING - " + lbStatus;
        // v0.15 item 8: how many render endpoints (loopback sources) the
        // app can actually see — refreshed ~every 2 s while Debug is open.
        // If this reads 0 on a machine WITH speakers/headphones, Windows
        // is not reporting them active; a screenshot of this line settles
        // any "outputs don't show up" report in one look.
        if (debugEpsCount < 0 || now - lastEpsCheckMs > 2000) {
            debugEpsCount = (int)WindowsLoopback::enumerateRenderEndpointsCached().size(); // v0.20.2: cached — this poll must not re-run the COM sweep every ~2 s
            lastEpsCheckMs = now;
        }
        debugLabel.setText("Device: " + engine.currentDeviceName() +
            "\nSource: " + juce::String(engine.state.audioSource) +
            "\nSample rate: " + juce::String(engine.activeSampleRate.load(), 0) + " Hz" +
            "\nBuffers received: " + juce::String(engine.buffersReceived.load()) +
            "\nRing frames: " + juce::String(engine.ring.totalWritten()) +
            "\nUI fps: " + juce::String(uiFpsMeasured, 1) + " (limit " + juce::String(engine.state.fpsLimit) + ", VSync " + (engine.state.vsync ? "on" : "off") + ")" +
            "\nPerformance: " + juce::String(engine.state.performanceMode == 1 ? "Low-End" : engine.state.performanceMode == 2 ? (perfAuto_.latched() ? "Auto (stepped down)" : "Auto") : "Full") + " (effective fps cap " + juce::String(perfLimits().fpsCap) + ")" +
            "\nFFT size: " + juce::String(engine.state.fftSize) + "  Paused: " + (engine.paused ? "yes" : "no") +
            "\nLoudness (engine): " + juce::String(engine.loudnessValue(), 1) + "  Peak lin: " + juce::String(engine.peakValue(), 3) +
            "\nLoopback wanted: " + engine.loopbackWantedName() +
            "\nLoopback device: " + engine.loopbackDeviceName() + "  (via " + engine.loopbackPickHow() + ")" +
            "\nLoopback id: " + engine.loopbackDeviceId() +
            "\nLoopback packets: " + juce::String(engine.loopbackPackets()) +
            "\nLoopback format: " + engine.loopbackFormat() + "  peak decoded: " + juce::String(engine.loopbackPeak(), 3) +
            "\nLoopback status: " + lbStatus +
            "\nScreen space (v0.19.1): " + juce::String(screenSpace.statusInfo()) +
            "\nRender endpoints found (loopback sources): " + juce::String(debugEpsCount),
            juce::dontSendNotification);
    }
}
void MainComponent::prepareFrame() {
    auto& st = engine.state;
    // v0.15: the oscilloscope's visible window is user-controlled
    // (scopeWindowMs, default 100 ms — much wider than the old fixed
    // 2048-sample ~43 ms view Jackson found too cramped).
    double srNow = engine.activeSampleRate.load();
    int scopeSamples = juce::jlimit(256, 16384, (int)std::lround(st.scopeWindowMs * 0.001 * srNow));
    int need = std::max({ st.fftSize, 8192, scopeSamples + 256 });
    bufL.assign(need, 0.f); bufR.assign(need, 0.f);
    int got = engine.ring.readLatest(bufL.data(), bufR.data(), need);
    (void)got;
    frame.st = &st; frame.theme = theme; frame.sampleRate = engine.activeSampleRate.load();
    // Spectrum (mono = Mid) — re-prepare analyser when FFT size changes
    if (!analyserReady || lastFftSize != st.fftSize) { analyser.prepare(st.fftSize); lastFftSize = st.fftSize; analyserReady = true;
        spectro.prepare(analyser.numBins(), 220); }
    analyser.setSmoothing(st.spectrumSmoothingPct / 100.f); analyser.setTiltDb(st.spectrumTiltDb);
    mono.assign(st.fftSize, 0.f);
    int start = std::max(0, need - st.fftSize);
    for (int i = 0; i < st.fftSize; ++i) mono[i] = 0.5f * (bufL[start + i] + bufR[start + i]);
    frame.spectrumDb = analyser.process(mono.data(), st.fftSize);
    // Spectrogram row
    spectro.pushRow(frame.spectrumDb, st.linkedSpeed); frame.spectrogram = &spectro;
    // Waveform history from the newest 512 samples, in small blocks
    waveHist.pushBlock(bufL.data() + std::max(0, need - 512), bufR.data() + std::max(0, need - 512), 512,
                       st.waveCh1, st.waveCh2, st.waveHistory, st.linkedSpeed);
    frame.waveCh1 = waveHist.ch1(); frame.waveCh2 = waveHist.ch2();
    // v0.26.0: the auto-BPM grid, copied out of the engine for the
    // waveform / spectrogram / scope overlays. The overlay
    // timebases mirror the histories' own advance rules: the
    // waveform advances round(1 + linkedSpeed) columns per frame,
    // the spectrogram max(0.1, linkedSpeed) * 0.2 rows per frame
    // (SpectrogramHistory::pushRow's accumulator), frames run at
    // fpsLimit — so a beat's age maps to the same pixels the
    // histories themselves scroll by.
    frame.beatLocked = engine.beatLocked();
    frame.beatBpm = (float)engine.beatBpmValue();
    frame.beatConfidence = engine.beatConfidence();
    frame.beatSig = st.beatTimeSignature;
    // v0.26.1: no bar count crosses into the frame any more — the
    // displays mark beats without asserting beat 1. The scope's
    // dot takes the any-beat envelope instead of the downbeat one.
    frame.beatPulseEnv = engine.beatGridEnv();
    frame.beatPhase = engine.beatPhaseValue(); // v0.27.0 (item 13)
    {
        float ages[64]; int counts[64];
        const int nb = engine.collectBeats(30.f, ages, counts, 64);
        frame.beatAges.assign(ages, ages + nb);
        // v0.28.1: the counts cross now, alongside the core's
        // evidence-proven downbeat (0 = unproven -> overlays draw
        // every beat alike, the v0.26.1 rule). They exist only to
        // emphasise a proven downbeat; never drawn as numbers.
        frame.beatCounts.assign(counts, counts + nb);
        frame.beatDownbeat = engine.beatDownbeatCount();
    }
    // v0.28.2: overlay timebases run at the EFFECTIVE frame rate
    // (the Low-End cap genuinely slows the timer, so the beat-age
    // mapping must slow with it; in Full this is st.fpsLimit itself).
    frame.waveSecPerColumn = 1.0f / ((float)std::max(1, perfLimits().fpsCap) * (float)std::max(1, (int)std::lround(1 + st.linkedSpeed)));
    frame.sgSecPerRow = 1.0f / ((float)std::max(1, perfLimits().fpsCap) * std::max(0.1f, st.linkedSpeed) * 0.2f);
    frame.sgResScale = perfLimits().sgScale; // v0.28.6: spectrogram cache resolution follows Performance Mode (1.0 Full / 0.5 Low-End)
    // Loudness + stereo + scope
    frame.loudDb = engine.loudnessValue(); frame.peakLin = engine.peakValue();
    // v0.16 loudness pack readouts (integrated is gated BS.1770; true peak
    // is the oversampled inter-sample peak — see Loudness.hpp).
    // Drag-lag fix (2026-10-31): integratedLufs() re-runs two gated
    // passes over the whole 100 ms block history every call — a history
    // that grows to 36,000 entries (1 h) — and this ran once per UI
    // frame. The integrated readout is a session integral; recompute
    // at 4 Hz and serve the cached value between recomputes.
    {
        const long long nowMs = (long long)juce::Time::getMillisecondCounterHiRes();
        if (nowMs - lastLoudIntegratedMs_ >= 250) {
            lastLoudIntegratedMs_ = nowMs;
            cachedLoudIntegrated_ = engine.loudnessIntegrated();
        }
        frame.loudIntegrated = cachedLoudIntegrated_;
    }
    frame.loudShortTerm = engine.loudnessShortTerm();
    frame.loudTruePeakDb = engine.loudnessTruePeakDb();
    // v0.15: per-channel display levels for the VERTICAL loudness bars —
    // fast-attack / gentle-decay ballistics over the newest 2048 samples,
    // plus per-channel peak holds. Display-only; the engine's loudness
    // value above is untouched.
    {
        int ln = std::min(2048, need); double msL = 0, msR = 0; float pkL = 0, pkR = 0;
        for (int i = need - ln; i < need; ++i) {
            msL += (double)bufL[(size_t)i] * bufL[(size_t)i]; msR += (double)bufR[(size_t)i] * bufR[(size_t)i];
            pkL = std::max(pkL, std::abs(bufL[(size_t)i])); pkR = std::max(pkR, std::abs(bufR[(size_t)i]));
        }
        float instL = msL > 0 ? (float)(10.0 * std::log10(msL / ln)) : -120.f;
        float instR = msR > 0 ? (float)(10.0 * std::log10(msR / ln)) : -120.f;
        float ipkL = pkL > 0 ? (float)(20.0 * std::log10(pkL)) : -120.f;
        float ipkR = pkR > 0 ? (float)(20.0 * std::log10(pkR)) : -120.f;
        dispDbL = instL > dispDbL ? instL : std::max(instL, dispDbL - 0.5f);
        dispDbR = instR > dispDbR ? instR : std::max(instR, dispDbR - 0.5f);
        dispPeakDbL = std::max(ipkL, dispPeakDbL - 0.15f);
        dispPeakDbR = std::max(ipkR, dispPeakDbR - 0.15f);
        frame.loudDbL = dispDbL; frame.loudDbR = dispDbR;
        frame.loudPeakDbL = dispPeakDbL; frame.loudPeakDbR = dispPeakDbR;
    }
    StereometerEngine se; int sn = std::min(need, 4096);
    frame.stereo = se.analyse(bufL.data() + need - sn, bufR.data() + need - sn, sn);
    frame.latestL.assign(bufL.end() - 2048, bufL.end()); frame.latestR.assign(bufR.end() - 2048, bufR.end());
    if (scopeSamples != lastScopeSamples) { scope.prepare(scopeSamples); lastScopeSamples = scopeSamples; }
    // v0.28.2 PERFORMANCE MODE: under Low-End limits the pitch
    // autocorrelation (the expensive half of capture in Pitch mode)
    // runs every 3rd frame; between runs the engine's buffer and the
    // cached frequency are served as-is. Full quality: every frame,
    // exactly as before.
    float f = cachedScopeFreq_;
    if (st.scopeTrigger != meterbar::ScopeTrigger::Pitch
        || meterbar::perfScopeCaptureDue(perfFrame_, perfLimits())) {
        scope.capture(bufL.data() + (need - scopeSamples), scopeSamples, st.scopeTrigger, f, frame.sampleRate);
        cachedScopeFreq_ = f;
    }
    frame.scopeBuf = scope.buffer(); frame.scopeFreq = f;
    // v0.20.0 (item i): the scope's Multi display is a TRUE 3-band
    // split of the captured buffer (core splitScopeBands) — computed
    // once per frame here, drawn by ModuleView in three lanes.
    if (st.scopeDisplay == meterbar::ScopeDisplay::Multi) {
        auto bands = meterbar::splitScopeBands(frame.scopeBuf, frame.sampleRate);
        frame.scopeLow = std::move(bands.low);
        frame.scopeMid = std::move(bands.mid);
        frame.scopeHigh = std::move(bands.high);
    }
    // v0.20.0 (item l): advance the visualizer model from the frame's
    // own spectrum (per-band energies, core helper). dt follows the
    // UI timer's real cadence (fpsLimit), clamped to sane bounds.
    // v0.20.1: the broadcast stash — computed every frame from the
    // same spectrum, whether or not the local visualizer is on.
    latestBands_ = meterbar::spectrumBandEnergies(frame.spectrumDb, st.fftSize, frame.sampleRate);
    latestLevel_ = juce::jlimit(0.f, 1.f, (float)((frame.loudShortTerm + 50.0) / 45.0));
    latestBalance_ = juce::jlimit(-1.f, 1.f, frame.stereo.balance); // v0.25.0: distributed-emitter steering
    if (st.visualizerEnabled) {
        auto e = latestBands_;
        meterbar::VisualizerSettings vs;
        vs.enabled = true;
        vs.baseHue = st.visualizerBaseHue;
        vs.saturation = st.visualizerSaturation;
        vs.paletteSpread = st.visualizerSpread;
        vs.cycleSeconds = st.visualizerCycleSec;
        const float dt = (float)juce::jlimit(0.004, 0.1, 1.0 / std::max(1, st.fpsLimit));
        // v0.21.5: advance whichever style is selected — the
        // particle engine (Bubbles) or the ribbon model (Lines).
        // v0.25.0: style 2 = Field (wandering vortices); Bubbles
        // now also receives the stereo balance for its distributed
        // emitters.
        if (st.visualizerStyle == 1) vizLines.update(dt, e.data());
        // v0.26.0: the Field also hears the beat — the grid's
        // downbeat envelope once it locks, the Beat Pulse envelope
        // until then — so its objects impulse on the downbeats.
        // v0.27.0: it also hears the STEREO IMAGE (width, item 9)
        // and the TRUE SUB energy (bins under ~45 Hz, item 17).
        else if (st.visualizerStyle == 2) vizField.update(dt, e.data(), vs, latestBalance_,
                                                          engine.beatLocked() ? engine.beatDownbeatEnv()
                                                                              : engine.beatLevel() * 0.8f,
                                                          juce::jlimit(0.f, 1.f, frame.stereo.widthPct / 100.f),
                                                          meterbar::subBandEnergy(frame.spectrumDb, st.fftSize, frame.sampleRate));
        else if (st.visualizerStyle == 3) {
            // v0.28.0 UNDERTOW (visualizer council DESIGN.md). Two
            // paths, never derived from each other: Path A timing
            // arrives from the engine's five-band onset latch (fed
            // on the audio ingest in 256-sample sub-blocks); Path B
            // levels come from a DEDICATED 2048-pt analyser run
            // here on the newest samples — unsmoothed, untilted,
            // never the display spectrum above. The model advances
            // on REAL elapsed dt: the fixed dt the other styles use
            // is banned for this section (DESIGN build notes), so
            // the envelope wall-clock rates are true.
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            float rdt = lastUtMs_ > 0 ? (float)((nowMs - lastUtMs_) / 1000.0) : 0.016f;
            lastUtMs_ = nowMs;
            rdt = juce::jlimit(0.001f, 0.1f, rdt);
            if (!utAnalyserReady_) { utAnalyser_.prepare(2048); utAnalyser_.setSmoothing(0.f); utAnalyser_.setTiltDb(0.f); utAnalyserReady_ = true; }
            const int un = std::min(2048, need);
            utMono_.assign(2048, 0.f);
            for (int i = 0; i < un; ++i)
                utMono_[(size_t)(2048 - un + i)] = 0.5f * (bufL[(size_t)(need - un + i)] + bufR[(size_t)(need - un + i)]);
            auto& rawDb = utAnalyser_.process(utMono_.data(), 2048);
            auto lv = meterbar::fiveBandLevelsFromRawDb(rawDb, 2048, frame.sampleRate);
            // The Governor's level input: short-term loudness,
            // which tracks programme RMS closely enough for the
            // Still/silence rules (digital silence reads -120 and
            // hard-zeros the gate, as designed).
            const float overallDb = frame.loudShortTerm > -119.0 ? (float)frame.loudShortTerm : -120.f;
            // Label safe zones (DESIGN principle 8): every visible
            // module's header strip + bottom readout strip, plus
            // the toolbar strip — unit space for the model's spawn
            // rejection, local px for the renderer's dimming.
            std::vector<meterbar::UndertowSafeRect> safeU;
            utSafeLocal_.clear();
            const float W = (float)std::max(1, getWidth()), H = (float)std::max(1, getHeight());
            auto addSafe = [&](juce::Rectangle<int> r) {
                if (r.isEmpty()) return;
                utSafeLocal_.push_back(r);
                safeU.push_back({ r.getX() / W, r.getY() / H, r.getRight() / W, r.getBottom() / H });
            };
            if (st.showToolbar) addSafe({ 0, 0, getWidth(), 30 });
            for (auto& mv : modules) {
                if (!mv->isVisible()) continue;
                auto b = mv->getBounds();
                addSafe({ b.getX(), b.getY(), b.getWidth(), 15 });
                addSafe({ b.getX(), b.getBottom() - 13, b.getWidth(), 13 });
            }
            undertow_.setSafeRects(safeU);
            { auto pl = perfLimits(); undertow_.setSwarmCap(pl.swarmCap); undertow_.setDustCap(pl.dustCap); undertow_.setOrbCap(pl.orbCap); } // v0.28.2: Performance Mode caps (Full values == the model's own constants); v0.28.9: orb cap rides along
            undertow_.preArmEnabled = st.undertowPreArm;
            undertow_.setKickTau(st.undertowKickTauMs / 1000.f);
            meterbar::UndertowBeatFeed bf; engine.undertowBeatFeed(bf);
            meterbar::UndertowOnsetFeed of; engine.undertowOnsets(of);
            undertow_.update(rdt, lv, overallDb, bf, of);
            // Pre-arm ceiling write-back: the model may lower a
            // band's detection threshold for one beat position —
            // it can never fire, time, or scale an event (T2-5).
            for (int b = 0; b < 5; ++b) engine.setUndertowThresholdScale(b, undertow_.thresholdScale(b));
        }
        else visualizer.update(dt, e.data(), vs, latestBalance_);
    }
}
// ---------------- Menus ----------------
enum { idNew=1, idSave, idSaveAs, idOpenFolder, idLoadBase=1000,
       idTop=2001, idBottom, idStick, idPin, idReserve, idDisplayBase=2100,
       idAbout=3001, idOpenSettings, idSrcTone, idSrcInput, idSrcLoop, idThemeBase=3100, idMapBase=3200,
       idCapPause, idCapRecord, idClip10, idClip60, idHint, idToolbar, idDebug, idUpdates, idQuit,
       idArrange=6001, idRestore, idMinimize, // v0.22.0: compressed toolbar items in the hamburger menu
       idInDevBase=3300, idOutDevBase=3400,
       idRecBase=5000, idOpenRecFolder=5100, idRecEmpty=5101, idOpenLogFolder=5102, // v0.21.4: Recordings submenu; v0.28.3: Open Log Folder (crash records live in the same Documents\MeterBar)
       idLcOff=7001, idLcTranscribe, idLcTranslate, idLcTop, idLcMove, idLcResetPos, idLcSetup, idLcStatus,
       idLcPresentOverlay, idLcPresentDocked,
       idLinkAllTop=8001, idLinkAllBottom, // BLK7: Link All submenu (DESIGN §6e as amended 2026-11-03: option B only)
       idPinStatus=8101 }; // v0.28.7: disabled pin-status row in the Audio Source submenu
// v0.28.8: stamp the top level's live bounds into the state's
// window rect. The state's rect is the BAR rect: while the docked
// caption strip is in the height, the strip is un-grown again with
// the app's own inverse (captionShrunkBarRect, at the same anchor
// the growth used), so a stamped rect never double-counts the
// strip when it is restored later.
void MainComponent::syncWindowRectToState() {
    auto* tl = getTopLevelComponent();
    if (tl == nullptr) return;
    auto b = tl->getBounds();
    if (captionStripInHeight_) {
        auto bar = meterbar::captionShrunkBarRect(
            { b.getX(), b.getY(), b.getWidth(), b.getHeight() }, kCaptionStripHeight, captionAnchorEdge());
        b = { bar.x, bar.y, bar.w, bar.h };
    }
    engine.state.windowX = b.getX(); engine.state.windowY = b.getY();
    engine.state.windowW = b.getWidth(); engine.state.windowH = b.getHeight();
}
// v0.28.8: THE save function — one path for every preset save in
// the app (SessionBar v0.4.21 parity; Jackson's refinement,
// 2026-11-03: save, save as, and the close prompt go "all through
// the same kind of function"). The Presets menu's Save / Save As
// and the close prompt's Save / Save As all call performSave();
// callers differ only in what they do with the answer. Every save
// also refreshes the last-state record — the analogue of
// SessionBar's saveSettings() inside its performSave — with the
// live rect stamped first: a preset includes position. done(true)
// = state saved (preset + last-state); done(false) = the name
// prompt was cancelled and no preset was written.
void MainComponent::performSave(bool saveAs, std::function<void(bool)> done) {
    syncWindowRectToState();
    presetio::saveLastState(engine.state);
    auto& st = engine.state;
    if (!saveAs && st.name != "Default" && !st.name.empty()) {
        presetio::save(st, juce::String(st.name));
        done(true);
        return;
    }
    promptForPresetNameAsync([this, done](bool named) {
        if (!named) { done(false); return; }
        presetio::save(engine.state, juce::String(engine.state.name));
        presetio::saveLastState(engine.state); // persist the (new) active name at once
        done(true);
    });
}
void MainComponent::savePresetInteractive(bool saveAs) {
    performSave(saveAs, [](bool) {}); // the menu's Save / Save As — the same function the close prompt uses
}
void MainComponent::promptForPresetNameAsync(std::function<void(bool)> done) {
    auto w = std::make_shared<juce::AlertWindow>("Save Preset", "Preset name:", juce::AlertWindow::NoIcon);
    w->addTextEditor("name", juce::String(engine.state.name == "Default" ? "My Preset" : engine.state.name));
    w->addButton("Save", 1);
    w->addButton("Cancel", 0);
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, done](int r) {
        if (r == 1) {
            auto name = w->getTextEditorContents("name").trim();
            if (name.isNotEmpty()) { engine.state.name = name.toStdString(); done(true); return; }
        }
        done(false);
    }));
}
// v0.28.8: the close-time save prompt (Jackson's ruling,
// 2026-11-03, shipped in SessionBar v0.4.20: save on close ONLY if
// something changed since the app opened; a save restores the same
// position and size on the next open). JUCE 8 has no modal loop,
// so the gate is async in the family's enterModalState idiom:
// requestClose() either quits at once (unchanged) or opens the
// prompt, and the prompt's answer completes the quit itself.
void MainComponent::requestClose() {
    if (closePromptOpen_) return; // the prompt is already up — a second request adds nothing
    // Stamp the live rect first: the comparison (and any save)
    // must see the window as it is NOW, not as last written.
    syncWindowRectToState();
    if (!openBaselineCaptured_ || !meterbar::changedSinceOpen(openBaseline_, engine.state)) {
        // Unchanged since open (or quit inside the first tick):
        // silent close — the last-state record still refreshes, so
        // the next open restores exactly this state.
        presetio::saveLastState(engine.state);
        juce::JUCEApplication::getInstance()->quit();
        return;
    }
    closePromptOpen_ = true;
    auto w = std::make_shared<juce::AlertWindow>("Close MeterBar",
        "The settings, the active preset, or the bar's position/size changed since you opened MeterBar.\n\nSave the current state before closing?",
        juce::AlertWindow::QuestionIcon);
    // Exactly three actions (Jackson's refinement, 2026-11-03):
    // Save / Save As / Continue Without Saving. There is
    // deliberately no Cancel button — dismissing the prompt (Esc)
    // still aborts the close, it just isn't a fourth action.
    w->addButton("Save", 1);
    w->addButton("Save As...", 2);
    w->addButton("Continue Without Saving", 3);
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        closePromptOpen_ = false;
        // Both saves route through the ONE save function; a
        // cancelled name prompt inside it vetoes the close
        // (saved=false).
        if (r == 1) performSave(false, [this](bool saved) { if (saved) juce::JUCEApplication::getInstance()->quit(); });
        else if (r == 2) performSave(true, [this](bool saved) { if (saved) juce::JUCEApplication::getInstance()->quit(); });
        else if (r == 3) { presetio::saveLastState(openBaseline_); juce::JUCEApplication::getInstance()->quit(); } // discard: the last state reverts to the open baseline; no preset file is touched
        // r == 0 (prompt dismissed): stay open — no quit is issued
    }));
}
void MainComponent::loadPresetFile(const juce::File& f) {
    if (presetio::load(f, engine.state)) {
        engine.applySource(); engine.applyBeatMeter(); applyTheme(); applyFps(); layoutModules(); repaint(); syncScreenSpace(); uiClick(ClickSounds::Which::Sparkle);
        // v0.20.1 (item 2 audit): presets always SAVED the window
        // position (winX/winY/winW/winH ride in PresetState) but
        // loading one never MOVED the window — Jackson's requirement
        // is that a preset includes position. Apply it now, clamped
        // into a visible work area (core WindowGeom).
        if (auto* tl = getTopLevelComponent()) {
            std::vector<meterbar::IntRect> areas;
            for (auto& d : juce::Desktop::getInstance().getDisplays().displays) {
                auto ua = d.userArea;
                areas.push_back({ ua.getX(), ua.getY(), ua.getWidth(), ua.getHeight() });
            }
            auto clamped = meterbar::clampRectToWorkAreas(
                { engine.state.windowX, engine.state.windowY, engine.state.windowW, engine.state.windowH }, areas);
            // v0.24.0: a preset's saved size is the USER's bar size —
            // while the docked strip is in the height, the applied
            // rect is the preset rect grown by the strip, anchored
            // like any dock growth, so the strip never eats module
            // space the preset promised.
            if (captionStripInHeight_)
                clamped = meterbar::captionGrownBarRect(clamped, kCaptionStripHeight, captionAnchorEdge());
            tl->setBounds(clamped.x, clamped.y, clamped.w, clamped.h);
        }
    }
}
// v0.22.0: the old Preset dropdown, verbatim, as a submenu builder —
// the hamburger callback handles the ids (same actions as the old
// showPresetMenu callback). filesOut keeps the listed files alive
// for the load-range id check in that callback.
juce::PopupMenu MainComponent::buildPresetSubMenu(std::vector<juce::File>& filesOut) {
    juce::PopupMenu m;
    m.addItem(idNew, "New Preset"); m.addItem(idSave, "Save"); m.addItem(idSaveAs, "Save As...");
    m.addItem(idOpenFolder, "Open Preset Folder"); m.addSeparator();
    auto files = presetio::presetFolder().findChildFiles(juce::File::findFiles, false, "*.json");
    filesOut.assign(files.begin(), files.end());
    int i = 0; for (auto& f : files) m.addItem(idLoadBase + (i++), f.getFileNameWithoutExtension());
    return m;
}
void MainComponent::doTopBottom(bool top) {
    // v0.28.2 ADDENDUM (Jackson, 2026-11-03: Reposition must stretch
    // to the full width of THE SCREEN THE BAR IS ON): resolve the
    // display from the window's current rect, not always the primary
    // display. Primary remains the fallback if the current rect
    // resolves to no display.
    // v0.28.9 (Jackson's dock-merge ruling): the target is the
    // display's TOTAL area, not its user area — flush to the edge,
    // x = monitor left, width = the monitor's full width, height
    // unchanged (core: meterbar::dockedBarRect). User-area semantics
    // subtracted the taskbar and any live AppBar reservation, so a
    // reposition with the reservation up landed the bar below its
    // own reserved strip and short of the screen's edges.
    auto* w = findParentComponentOfClass<juce::DocumentWindow>();
    const auto curRect = w ? w->getBounds() : getScreenBounds();
    auto& displays = juce::Desktop::getInstance().getDisplays();
    auto disp = displays.getDisplayForRect(curRect);
    if (!disp) disp = displays.getPrimaryDisplay();
    if (!disp) return; auto ta = disp->totalArea;
    const auto target = meterbar::dockedBarRect(
        { curRect.getX(), curRect.getY(), curRect.getWidth(), getHeight() },
        { ta.getX(), ta.getY(), ta.getWidth(), ta.getHeight() }, top);
    if (w) {
        w->setBounds(target.x, target.y, target.w, target.h);
    }
    syncScreenSpace(); // v0.18: docking to the top is what arms the reservation (v0.18.1: the window's moved/resized events feed it too)
}
void MainComponent::doStick() { setStick(!engine.state.stick); }
void MainComponent::setStick(bool on) {
    // v0.28.9 STICK = RESERVE (Jackson's dock-merge ruling): the one
    // control. ON: dock flush + full width at the bar's nearest
    // edge (top or bottom) and register the AppBar reservation for
    // exactly the bar's height there — the two state fields are kept
    // merged (applyStickReserveMerge) so presets, the last-state
    // record and the reserve machine all read the one decision. OFF:
    // reservation released (syncScreenSpace's Release path also
    // restores arranged windows, as the old Reserve toggle-off did);
    // the bar stays where it sits, free to drag again.
    engine.state.stick = on;
    meterbar::applyStickReserveMerge(engine.state.stick, engine.state.reserveScreenSpace);
    if (engine.state.stick) {
        auto* w = findParentComponentOfClass<juce::DocumentWindow>();
        const auto curRect = w ? w->getBounds() : getScreenBounds();
        auto& displays = juce::Desktop::getInstance().getDisplays();
        auto disp = displays.getDisplayForRect(curRect);
        if (!disp) disp = displays.getPrimaryDisplay();
        // Nearest horizontal edge of the bar's own monitor wins; a
        // bar exactly mid-screen sticks to the top (its historical
        // home). If no display resolves, doTopBottom's own fallback
        // chain handles the top.
        bool toTop = true;
        if (disp) {
            const int gapTop = curRect.getY() - disp->totalArea.getY();
            const int gapBottom = disp->totalArea.getBottom() - curRect.getBottom();
            toTop = gapTop <= gapBottom;
        }
        doTopBottom(toTop); // docks + syncs the reservation in
    } else {
        syncScreenSpace(); // toggle-off: the machine releases
    }
}
void MainComponent::doMoveToDisplay(int idx) {
    auto disp = juce::Desktop::getInstance().getDisplays().displays[idx];
    juce::Rectangle<int> target(disp.userArea.getCentreX() - getWidth() / 2, disp.userArea.getY(), getWidth(), getHeight());
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setBounds(target);
    else setBounds(target);
}
// v0.22.0: the old Reposition dropdown, verbatim, as a submenu
// builder (ticks read live state at open). Its actions run in the
// hamburger callback, which DEFERS them via callAsync exactly as
// the old menu did — field report (2026-10-13): moving the window
// from inside a menu callback left it stuck at the top with dead
// buttons (stuck mouse capture during menu dismissal on Windows).
juce::PopupMenu MainComponent::buildRepositionSubMenu() {
    juce::PopupMenu m; auto& st = engine.state;
    m.addItem(idTop, "At the Top"); m.addItem(idBottom, "At the Bottom");
    m.addItem(idStick, "Stick", true, st.stick); m.addItem(idPin, "Pin Above Other Windows", true, st.pinned);
    // v0.28.9: the separate "Reserve Screen Space" item is GONE —
    // Stick IS the reservation now (Jackson's dock-merge ruling).
    juce::PopupMenu disp; auto displays = juce::Desktop::getInstance().getDisplays().displays;
    for (int i = 0; i < displays.size(); ++i) disp.addItem(idDisplayBase + i, "Display " + juce::String(i + 1));
    m.addSubMenu("Move to Display", disp);
    return m;
}
void MainComponent::toggleRecord() {
    if (engine.isRecording()) { engine.stopRecording(); recordingFile_ = juce::File{}; recBtn.setToggleState(false, juce::dontSendNotification); return; }
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MeterBar");
    dir.createDirectory();
    auto f = dir.getChildFile("record-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".wav");
    if (engine.startRecording(f)) { recordingFile_ = f; recBtn.setToggleState(true, juce::dontSendNotification); } // v0.22.0: the lit 🔴 replaces the Rec/Stop text swap
}
void MainComponent::exportClip(double seconds) {
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MeterBar");
    dir.createDirectory();
    auto f = dir.getChildFile("clip" + juce::String((int)seconds) + "s-" + juce::String(clipCounter++) + ".wav");
    engine.writeClip(seconds, f);
}
// v0.9 UNIFIED sound source: picking ANY device makes it THE source.
// Input device -> source "Input" on that device. Output device -> source
// "System Loopback" capturing that exact output (speakers/headphones/etc).
void MainComponent::setDeviceByName(bool input, const juce::String& name) {
    if (input) {
        engine.state.audioSource = "Input"; engine.state.audioDeviceName = name.toStdString();
        auto setup = engine.deviceManager.getAudioDeviceSetup();
        setup.inputDeviceName = name; setup.useDefaultInputChannels = true;
        engine.deviceManager.setAudioDeviceSetup(setup, true);
    } else {
        engine.state.audioSource = "System Loopback"; engine.state.outputDeviceName = name.toStdString();
        // v0.13: resolve the endpoint ID for that exact name so loopback
        // capture keys on identity, not on a later name re-match.
        engine.state.outputDeviceId.clear();
#if JUCE_WINDOWS
        for (auto& ep : WindowsLoopback::enumerateRenderEndpointsCached()) // v0.20.2: the picker that led here just enumerated — resolve from the cache
            if (juce::String(ep.name) == name) { engine.state.outputDeviceId = ep.id; break; }
#endif
    }
    engine.applySource();
}
// v0.15 item 8: showDeviceSelector() was REMOVED. It wrapped JUCE's stock
// AudioDeviceSelectorComponent configured (1, 8, 0, 0) — 1-8 input
// channels, ZERO output channels — so it could only ever list
// microphones, yet it sat in the Audio Source menu as "Choose Input
// Device...". That dialog was Jackson's field report: "the input
// selection only lets me pick microphones". Worse, changes made there
// rewired the shared deviceManager without updating the app's source
// state. The unified pickers (Settings > Audio Source, and the Audio
// Source submenu of the hamburger menu) are now the ONLY source
// surfaces: both list Input Devices AND Output Devices (WASAPI render
// endpoints), and picking an output starts loopback on that endpoint.
void MainComponent::showHamburgerMenu() {
    auto& st = engine.state; juce::PopupMenu m;
    juce::PopupMenu help; help.addItem(idAbout, "About MeterBar"); help.addItem(idUpdates, "Check For Updates");
    m.addSubMenu("Help", help);
    // v0.22.0: the compressed toolbar — the old Preset and Reposition
    // dropdowns as submenus (built fresh here, so ticks and the
    // preset list read live state), plus Arrange / Restore items.
    // Minimize joins Quit at the bottom (below).
    std::vector<juce::File> presetFiles;
    m.addSubMenu("Presets", buildPresetSubMenu(presetFiles));
    m.addItem(idArrange, "Arrange Windows");
    m.addItem(idRestore, "Restore Windows");
    m.addSubMenu("Reposition", buildRepositionSubMenu());
    // BLK7 Link All (DESIGN §6e, amended: option B ONLY — two items,
    // no auto-edge entry): one click links all three bars and stacks
    // them MeterBar -> TickerBar -> SessionBar at the chosen edge.
    { juce::PopupMenu la; la.addItem(idLinkAllTop, "Link All \u2192 Top"); la.addItem(idLinkAllBottom, "Link All \u2192 Bottom");
      m.addSubMenu("Link All", la); }
    m.addSeparator();
    juce::PopupMenu settings; settings.addItem(idOpenSettings, "Open Settings Window"); m.addSubMenu("Settings", settings);
    juce::PopupMenu src; src.addItem(idSrcTone, "Test Tone", true, st.audioSource == "Test Tone");
    // v0.9 unified: pick ANY device below and it becomes the sound source —
    // an input device = that mic/line-in; an output device = loopback of
    // whatever plays through those speakers/headphones.
    juce::StringArray inNames, outNames;
    if (auto* type = engine.deviceManager.getCurrentDeviceTypeObject()) {
        type->scanForDevices();
        inNames = type->getDeviceNames(true); outNames = type->getDeviceNames(false);
    }
#if JUCE_WINDOWS
    // v0.13: on Windows the OUTPUT list is the real WASAPI endpoint list —
    // the names are the endpoints' own friendly names (JUCE's device-type
    // names need not match them, which is how a picked device could capture
    // the wrong endpoint). setDeviceByName resolves the ID from the name.
    {
        auto eps = WindowsLoopback::enumerateRenderEndpoints();
        if (!eps.empty()) { outNames.clear(); for (auto& ep : eps) outNames.add(juce::String(ep.name)); }
    }
#endif
    auto setupNow = engine.deviceManager.getAudioDeviceSetup();
    juce::PopupMenu inDev, outDev;
    for (int i = 0; i < inNames.size(); ++i)
        inDev.addItem(idInDevBase + i, inNames[i], true,
                      st.audioSource == "Input" && inNames[i] == setupNow.inputDeviceName);
    for (int i = 0; i < outNames.size(); ++i)
        outDev.addItem(idOutDevBase + i, outNames[i], true,
                       st.audioSource == "System Loopback" && outNames[i] == juce::String(st.outputDeviceName));
    src.addSubMenu("Input Devices (mic / line-in)", inDev);
    src.addSubMenu("Output Devices (speakers - hear what's playing)", outDev);
    // v0.28.7: while the pinned app is the capture source, the
    // Audio Source menu SAYS so — a disabled status row (the Live
    // Captions pattern) carrying the live pin state: running, or
    // waiting for the pinned exe to start.
    if (st.appSource == 4) { src.addSeparator(); src.addItem(idPinStatus, engine.appSourceState(), false); }
    m.addSubMenu("Audio Source", src);
    juce::PopupMenu themeM; const char* themes[] = {"Midnight Kids","Midnight Pink","Deep Teal","Daylight","Anime"};
    for (int i = 0; i < 5; ++i) themeM.addItem(idThemeBase + i, themes[i], true, st.themeName == themes[i]);
    m.addSubMenu("Theme", themeM);
    juce::PopupMenu mapM; const char* maps[] = {"Plasma","Rainbow","Inferno","Viridis","Magma","Turbo","Midnight Kids"};
    for (int i = 0; i < 7; ++i) mapM.addItem(idMapBase + i, maps[i], true, (int)st.colorMap == i);
    m.addSubMenu("Color Map", mapM);
    juce::PopupMenu cap; cap.addItem(idCapPause, engine.paused ? "Resume Audio" : "Pause Audio");
    cap.addItem(idCapRecord, engine.isRecording() ? "Stop Recording" : "Begin Recording");
    cap.addItem(idClip10, "Clip Last 10 Seconds"); cap.addItem(idClip60, "Clip Last 60 Seconds");
    m.addSubMenu("Audio Capture", cap);
    // v0.23.0: Live Captions — radio-style mode rows (the tick is the
    // SELECTED mode; the controller's runtime state is separate and
    // the disabled status row reports it). Built fresh on every open,
    // like the rest of this menu.
    {
        juce::PopupMenu lc;
        const auto cmode = captions_.mode();
        lc.addItem(idLcOff, "Off", true, cmode == meterbar::CaptionMode::Off);
        lc.addItem(idLcTranscribe, "Captions", true, cmode == meterbar::CaptionMode::Transcribe);
        lc.addItem(idLcTranslate, "Translate to English", true, cmode == meterbar::CaptionMode::Translate);
        lc.addSeparator();
        lc.addItem(idLcTop, "Always on Top", true, captions_.overlayAlwaysOnTop());
        lc.addSeparator();
        // v0.24.0: the presentation flip (Jackson's ask) — captions in
        // their own window, or docked as a strip inside the bar. The
        // overlay-only actions below disable while docked.
        const bool docked = captions_.presentationDocked();
        lc.addItem(idLcPresentOverlay, "Separate Caption Window", true, !docked);
        lc.addItem(idLcPresentDocked, "Docked in MeterBar", true, docked);
        lc.addItem(idLcMove, "Move Caption Window", !docked);
        lc.addItem(idLcResetPos, "Reset Caption Position", !docked);
        lc.addSeparator();
        if (captions_.setupNeeded()) lc.addItem(idLcSetup, "Set Up Caption Runtime...");
        lc.addItem(idLcStatus, captions_.statusText(), false);
        m.addSubMenu("Live Captions", lc);
    }
    // v0.21.4: Recordings — rebuilt from a FRESH folder scan on every
    // open (no cached list); the core plan filters/sorts/caps and
    // drops the take currently being written.
    juce::PopupMenu rec; std::vector<juce::File> recFiles;
    {
        auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MeterBar");
        std::vector<meterbar::RecordingFileInfo> infos;
        for (auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.wav")) {
            meterbar::RecordingFileInfo info;
            info.name = f.getFileName().toStdString();
            info.mtimeMs = f.getLastModificationTime().toMilliseconds();
            info.sizeBytes = f.getSize();
            infos.push_back(info);
        }
        auto plan = meterbar::planRecordingsMenu(infos, engine.isRecording() ? recordingFile_.getFileName().toStdString() : std::string{});
        if (plan.empty()) rec.addItem(idRecEmpty, "No recordings yet", false);
        else for (size_t i = 0; i < plan.size(); ++i) {
            auto label = juce::String(plan[i].clip ? "Clip — " : "") + plan[i].name
                       + "  (" + juce::File::descriptionOfSizeInBytes(plan[i].sizeBytes) + ")";
            rec.addItem(idRecBase + (int)i, label);
            recFiles.push_back(dir.getChildFile(juce::String(plan[i].name)));
        }
    }
    rec.addSeparator();
    rec.addItem(idOpenRecFolder, "Open Recordings Folder");
    m.addSubMenu("Recordings", rec);
    m.addSeparator();
    m.addItem(idHint, "Show Reposition Hint", true, st.showRepositionHint);
    m.addItem(idToolbar, "Show Toolbar", true, st.showToolbar);
    m.addItem(idDebug, "Show Debug Window", true, debugWin->isVisible());
    m.addItem(idUpdates, "Check For Updates");
    m.addItem(idOpenLogFolder, "Open Log Folder"); // v0.28.3: crash records + capture log (family crash-log contract: reachable outside Settings)
    m.addSeparator(); m.addItem(idMinimize, "Minimize"); m.addItem(idQuit, "Quit");
    const int numDisplays = juce::Desktop::getInstance().getDisplays().displays.size();
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(burgerBtn), [this, inNames, outNames, recFiles, presetFiles, numDisplays](int r){
        auto& s = engine.state;
        if (r == idAbout) juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "MeterBar",
            "MeterBar " + kVersion + "\nA compact audio metering bar: Spectrum, Spectrogram, Waveform,\nLoudness, Stereometer, Oscilloscope.\nBuilt with JUCE + a custom DSP core.\nMade with JUCE (juce.com) — free-license attribution.");
        else if (r == idOpenSettings) { toggleSettings(); } // v0.28.6: same toggle as the toolbar gear
        else if (r == idSrcTone) { s.audioSource = "Test Tone"; engine.applySource(); }
        else if (r >= idInDevBase && r < idInDevBase + inNames.size()) setDeviceByName(true, inNames[r - idInDevBase]);
        else if (r >= idOutDevBase && r < idOutDevBase + outNames.size()) setDeviceByName(false, outNames[r - idOutDevBase]);
        else if (r >= idThemeBase && r < idThemeBase + 5) { const char* t[] = {"Midnight Kids","Midnight Pink","Deep Teal","Daylight","Anime"};
            s.themeName = t[r - idThemeBase];
            // v0.16: Anime arrives with its paired Pastel background
            // (a preset that stores its own background style keeps it —
            // this pairing applies to the menu pick only).
            if (s.themeName == "Anime") s.backgroundStyle = BackgroundStyle::Pastel;
            applyTheme(); uiClick(ClickSounds::Which::Bloop); }
        else if (r >= idMapBase && r < idMapBase + 7) { s.colorMap = (ColorMapId)(r - idMapBase); applyTheme(); }
        else if (r == idCapPause) pauseBtn.triggerClick();
        else if (r == idCapRecord) toggleRecord();
        else if (r == idClip10) exportClip(10);
        else if (r == idClip60) exportClip(60);
        else if (r == idLcOff) captions_.setMode(meterbar::CaptionMode::Off);
        else if (r == idLcTranscribe) captions_.setMode(meterbar::CaptionMode::Transcribe);
        else if (r == idLcTranslate) captions_.setMode(meterbar::CaptionMode::Translate);
        else if (r == idLcTop) captions_.setOverlayAlwaysOnTop(!captions_.overlayAlwaysOnTop());
        else if (r == idLcPresentOverlay) captions_.setPresentation(false);
        else if (r == idLcPresentDocked) captions_.setPresentation(true);
        else if (r == idLcMove) captions_.moveCaptionWindow();
        else if (r == idLcResetPos) captions_.resetCaptionPosition();
        else if (r == idLcSetup) captions_.launchSetup();
        else if (r == idOpenRecFolder) { auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MeterBar"); dir.createDirectory(); dir.startAsProcess(); }
        else if (r >= idRecBase && r < idRecBase + (int)recFiles.size()) recFiles[(size_t)(r - idRecBase)].startAsProcess();
        else if (r == idHint) { s.showRepositionHint = !s.showRepositionHint; resized(); }
        else if (r == idToolbar) { s.showToolbar = !s.showToolbar; resized(); repaint(); }
        else if (r == idDebug) debugWin->setVisible(!debugWin->isVisible());
        else if (r == idUpdates) checkForUpdates();
        else if (r == idOpenLogFolder) openLogFolder();
        else if (r == idLinkAllTop || r == idLinkAllBottom) {
            // BLK7 (DESIGN §6a): area = bounds of the display
            // containing MeterBar's current rect (this app), via the
            // client callback; defer past popup dismissal like the
            // Reposition actions (menu-capture discipline).
            const int edge = (r == idLinkAllBottom) ? 1 : 0;
            juce::MessageManager::callAsync([this, edge]{
                auto area = linkClient_.getLinkAllArea ? linkClient_.getLinkAllArea() : getScreenBounds();
                linkClient_.requestLinkAll(edge, area);
            });
        }
        // v0.22.0: the compressed toolbar items. Preset actions are
        // the old dropdown's, unchanged; Arrange / Restore call the
        // same handlers the buttons did.
        else if (r == idNew) { auto keepTheme = engine.state.themeName; engine.state = PresetState(); engine.state.themeName = keepTheme; engine.applySource(); applyTheme(); applyFps(); layoutModules(); }
        else if (r == idSave) savePresetInteractive(false);
        else if (r == idSaveAs) savePresetInteractive(true);
        else if (r == idOpenFolder) presetio::presetFolder().revealToUser();
        else if (r >= idLoadBase && r < idLoadBase + (int)presetFiles.size()) loadPresetFile(presetFiles[(size_t)(r - idLoadBase)]);
        else if (r == idArrange) doArrangeWindows();
        else if (r == idRestore) doRestoreWindows();
        else if (r == idTop || r == idBottom || r == idStick || r == idPin
                 || (r >= idDisplayBase && r < idDisplayBase + numDisplays)) {
            // Reposition actions defer until the popup has fully
            // closed — the old dropdown's discipline (see
            // buildRepositionSubMenu).
            juce::MessageManager::callAsync([this, r]{
                if (r == idTop) doTopBottom(true); else if (r == idBottom) doTopBottom(false);
                else if (r == idStick) doStick();
                else if (r == idPin) { engine.state.pinned = !engine.state.pinned;
                    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setAlwaysOnTop(engine.state.pinned); }
                else if (r >= idDisplayBase) doMoveToDisplay(r - idDisplayBase);
            });
        }
        else if (r == idMinimize) { if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                                        if (auto* peer = w->getPeer()) peer->setMinimised(true); }
        else if (r == idQuit) juce::JUCEApplication::getInstance()->systemRequestedQuit();
    });
}

// =====================================================================
// UPDATER — v0.19 built the feed + hand-off; v0.19.2 wires it to the
// real repo (UpdaterConfig.h: Corporal8065/meterbar) and adds the
// launch check, the release-notes panel and the private-repo state.
//
// One code path, two honest modes, decided by GitHub's answers:
//  - Repo PUBLIC: check -> notes -> download -> verify -> install.
//  - Repo PRIVATE (current state): the unauthenticated check gets
//    401/403/404, and the UI presents the private-repo state with an
//    "Open releases page" button instead of pretending to check.
//    The app carries no token — see UpdaterConfig.h.
// =====================================================================
void MainComponent::startUpdateCheck(bool fromLaunch) {
    if (updateCheckPending) return; // one check at a time, whoever asked
    updateState = std::make_shared<mbupdate::SharedState>();
    updateCheckPending = true;
    updateCheckFromLaunch = fromLaunch;
    mbupdate::checkLatestAsync(updateState, meterbar::kVersionString);
}
void MainComponent::checkForUpdates() {
    if (!mbupdate::configured()) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Updates",
            "MeterBar " + kVersion + " — updates are not configured yet.\n"
            "This build has no update feed set, so no check was performed.\n"
            "You are on the version you built.");
        return;
    }
    if (updateCheckPending) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Updates",
            "An update check is already running — its result will appear when it finishes.");
        return;
    }
    startUpdateCheck(false);
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Updates",
        "Checking GitHub for the latest MeterBar release...\n"
        "The result will pop up here when the check finishes.");
}
void MainComponent::pollUpdater() {
    // A finished background check: present its result exactly once.
    if (updateCheckPending && updateState && updateState->done.load(std::memory_order_acquire)) {
        updateCheckPending = false;
        const bool wasLaunch = updateCheckFromLaunch;
        updateCheckFromLaunch = false;
        const auto r = updateState->result;
        if (wasLaunch) {
            // Launch checks are SILENT unless there is something to
            // act on: up-to-date, offline, error and private-repo
            // outcomes show nothing at all. A newer release lights
            // the toolbar indicator — never a dialog at startup.
            if (r.ok && r.newer) {
                pendingRelease = r;
                updateAvailable = true;
                resized(); // lay out (and reveal) the indicator
                repaint();
            }
        } else {
            presentUpdateResult(r);
        }
    }
    // A finished installer download: verify, hand off to Inno, quit.
    if (updateDownloadPending && updateDownload && updateDownload->isFinished()) {
        updateDownloadPending = false;
        const int dlStatus = updateDownload->statusCode();
        const bool dlOk = !updateDownload->hadError() && dlStatus == 200;
        updateDownload.reset();
        if (!dlOk) {
            // v0.19.2: an auth-refused download (401/403/404 — e.g.
            // the asset sits behind the private repo) gets the same
            // private-repo state + browser fallback as the check.
            if (meterbar::httpStatusBlocksAuth(dlStatus)) {
                showPrivateRepoFallback("The installer download itself was refused (HTTP "
                    + juce::String(dlStatus) + ") — the same private-repository limit as the check.");
                return;
            }
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Update",
                "The update download did not complete.\n MeterBar has NOT been changed — you are still on " + kVersion + ".");
            return;
        }
        if (pendingRelease.assetSize > 0 && updateInstallerFile.getSize() != pendingRelease.assetSize) {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Update",
                "The downloaded installer is the wrong size (expected " + juce::String(pendingRelease.assetSize)
                + " bytes, got " + juce::String(updateInstallerFile.getSize()) + ").\n"
                "It will NOT be run. MeterBar has not been changed.");
            return;
        }
        if (pendingRelease.assetDigest.isNotEmpty()) {
            juce::String want = pendingRelease.assetDigest;
            if (want.startsWithIgnoreCase("sha256:")) want = want.substring(7);
            const juce::String got = mbupdate::sha256HexOfFile(updateInstallerFile);
            if (!got.equalsIgnoreCase(want)) {
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Update",
                    "The downloaded installer's SHA-256 does not match the one GitHub published for this release.\n"
                    "It will NOT be run. MeterBar has not been changed.");
                return;
            }
        }
        juce::ChildProcess installer;
        const bool launched = installer.start({ updateInstallerFile.getFullPathName(),
                                                "/SILENT", "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS", "/NORESTART" });
        if (!launched) {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Update",
                "The installer was downloaded to\n" + updateInstallerFile.getFullPathName()
                + "\nbut could not be started. You can run it by hand;\nMeterBar is still on " + kVersion + ".");
            return;
        }
        // The installer replaces {app} while we exit; its silent-gated
        // [Run] entry (MeterBar.iss) relaunches the new version.
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
}
void MainComponent::presentUpdateResult(const mbupdate::ReleaseInfo& r) {
    pendingRelease = r; // the Update action (if offered) refers to this release
    if (r.authBlocked) { // the private-repo state, before generic failure
        showPrivateRepoFallback("MeterBar asked GitHub for the latest release, but GitHub "
            "shows a private repository's releases only to signed-in users — and MeterBar "
            "deliberately carries no sign-in token.");
        return;
    }
    if (!r.ok) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Updates",
            "Update check failed.\n" + r.error + "\nYou are still on MeterBar " + kVersion + ".");
        return;
    }
    if (!r.newer) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Updates",
            "You are up to date.\nMeterBar " + kVersion + " is the latest release (" + r.tag + ").");
        return;
    }
    updateAvailable = true; // the indicator stays lit behind the panel too
    resized();
    showUpdatePanel(r);
}
// The update panel (v0.19.2): version line on top, the release's
// FULL notes in a scrollable read-only box, and the one action that
// matters. Used by the manual flow and by the toolbar indicator the
// launch check lights — same panel either way.
void MainComponent::showUpdatePanel(const mbupdate::ReleaseInfo& r) {
    auto* w = new juce::AlertWindow("Update available",
        "MeterBar " + r.tag + " is available (you are on " + kVersion + ").\n"
        "Release notes:",
        juce::AlertWindow::NoIcon);
    auto* notesBox = new juce::TextEditor();
    notesBox->setMultiLine(true, true);
    notesBox->setReadOnly(true);
    notesBox->setScrollbarsShown(true);
    notesBox->setCaretVisible(false);
    notesBox->setText(r.notes.trim().isNotEmpty() ? r.notes
                                                  : "(This release was published without notes.)", false);
    notesBox->setSize(520, 190);
    w->addCustomComponent(notesBox);
    w->addTextBlock("\nDownload & install will close MeterBar, run the installer,\n"
                    "and relaunch at the new version. Your presets and settings\n"
                    "are stored outside the app folder and are kept.");
    w->addButton("Download & install", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Later", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
        if (result == 1) startUpdateDownload();
    }), true);
}
// The private-repo state (v0.19.2): said plainly, never dressed up
// as a check that "found nothing". One button out: the releases
// page, in the browser where the user is signed in to GitHub.
void MainComponent::showPrivateRepoFallback(const juce::String& detail) {
    auto* w = new juce::AlertWindow("Updates",
        "Automatic update check isn't available while the repository is private.\n\n"
        + detail + "\n\n"
        "You are still on MeterBar " + kVersion + ". Releases can be downloaded "
        "from the releases page in your browser instead:",
        juce::AlertWindow::InfoIcon);
    w->addButton("Open releases page", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Close", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([](int result) {
        if (result == 1) juce::URL(mbupdate::releasesPageUrl()).launchInDefaultBrowser();
    }), true);
}
void MainComponent::startUpdateDownload() {
    if (updateDownloadPending) return;
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("MeterBar-Update");
    dir.createDirectory();
    updateInstallerFile = dir.getChildFile("MeterBar-Setup.exe");
    updateInstallerFile.deleteFile();
    updateDownload = juce::URL(pendingRelease.assetUrl).downloadToFile(updateInstallerFile, juce::URL::DownloadTaskOptions());
    if (updateDownload == nullptr) {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Update",
            "The update download could not be started.\nMeterBar has NOT been changed — you are still on " + kVersion + ".");
        return;
    }
    updateDownloadPending = true; // pollUpdater() finishes the sequence
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Update",
        "Downloading MeterBar " + pendingRelease.tag + "...\n"
        "When the download finishes, MeterBar will close, install,\n"
        "and relaunch itself at the new version.");
}
