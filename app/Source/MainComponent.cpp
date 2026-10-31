#include "MainComponent.h"
#include <meterbar/WindowGeom.hpp> // v0.20.1: preset/startup position clamp
#if JUCE_WINDOWS
 #include <windows.h> // native window move (v0.11)
#endif
using namespace meterbar;
// v0.19: the version string comes from the GENERATED header — root
// CMakeLists project(VERSION) is the single source of truth. (Before
// this it was a hard-coded "v1.0.0" literal here, one of six places.)
static const juce::String kVersion = juce::String("v") + meterbar::kVersionString;
MainComponent::MainComponent() {
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
    for (auto* b : { &presetBtn, &reposBtn, &recBtn, &pauseBtn, &settingsBtn, &burgerBtn, &minBtn, &quitBtn,
                     &reserveBtn, &arrangeBtn, &restoreBtn, &linkBtn }) addAndMakeVisible(b);
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
        if (auto* tl = getTopLevelComponent()) tl->setBounds(r);
    };
    linkClient_.getBg = [this]{ return BarLinkClient::Bg{ (int)engine.state.backgroundStyle, engine.state.backgroundOpacity }; };
    linkClient_.applyBg = [this](const BarLinkClient::Bg& bg){
        engine.state.backgroundStyle = (meterbar::BackgroundStyle)juce::jlimit(0, 3, bg.style); // v0.20.1: Pastel (3) is a family style too
        engine.state.backgroundOpacity = juce::jlimit(0.15f, 1.0f, bg.opacity);
        repaint();
    };
    linkClient_.getDockedEdge = [this]{ return reserveMachine.reserved() ? (int)reserveMachine.edge() : -1; };
    linkClient_.isLinked = [this]{ return engine.state.linkEnabled; };
    // v0.20.1 (item 17): broadcast the band energies + level the
    // visualizer already computes, so linked siblings can render the
    // reactive glow from the same music.
    linkClient_.getAudio = [this](float out[4]) {
        out[0] = latestBands_[0]; out[1] = latestBands_[1]; out[2] = latestBands_[2]; out[3] = latestLevel_;
    };
    linkClient_.start();
    presetBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); showPresetMenu(); };
    reposBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); showRepositionMenu(); };
    // v0.18 screen-space controls: Reserve toggles the AppBar reservation
    // (state is the source of truth; the button mirrors it), Arrange /
    // Restore run the one-shot window sweep on this monitor.
    reserveBtn.setClickingTogglesState(true);
    reserveBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop);
        engine.state.reserveScreenSpace = reserveBtn.getToggleState(); syncScreenSpace(); };
    arrangeBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); doArrangeWindows(); };
    restoreBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); doRestoreWindows(); };
    burgerBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); showHamburgerMenu(); };
    settingsBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); rebuildSettings(); settingsWin->setVisible(true); settingsWin->toFront(true); };
    minBtn.onClick = [this]{ if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                                 if (auto* peer = w->getPeer()) peer->setMinimised(true); };
    quitBtn.onClick = [this]{ juce::JUCEApplication::getInstance()->systemRequestedQuit(); };
    pauseBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); engine.paused = !engine.paused.load(); pauseBtn.setButtonText(engine.paused ? "Resume" : "Pause"); };
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
    add(ModuleView::Kind::Stereometer); add(ModuleView::Kind::Oscilloscope); add(ModuleView::Kind::Spectrum);
    // v0.15: splitters between modules (drag to resize a module). Created
    // after the modules so they sit on top; layoutModules() positions and
    // binds them. One per gap — at most modules-1 are ever visible.
    // v0.16: one extra splitter for the mascot cell's leading edge.
    addAndMakeVisible(mascot); mascot.setVisible(false); // Anime theme only (applyTheme drives); added before the splitters so they paint on top
    for (int i = 0; i < 6; ++i) { auto s = std::make_unique<ModuleSplitter>(); addAndMakeVisible(s.get()); splitters.push_back(std::move(s)); }
    spectro.prepare(256, 220); waveHist.prepare(512); scope.prepare(2048);
    settingsWin = std::make_unique<SettingsWindow>();
    settingsWin->content.st = &engine.state;
    settingsWin->content.deviceManager = &engine.deviceManager;
    settingsWin->content.onChanged = [this]{ applyTheme(); layoutModules(); repaint(); syncScreenSpace(); linkClient_.notifyLocalSettings(); }; // v0.2.1: bg publish feed (was missing)
    settingsWin->content.onSourceChanged = [this]{ engine.applySource(); };
    settingsWin->content.onFpsChanged = [this]{ applyFps(); };
    settingsWin->content.onArrange = [this]{ doArrangeWindows(); };   // v0.18: Settings > Window buttons
    settingsWin->content.onRestore = [this]{ doRestoreWindows(); };   // v0.18
    settingsWin->content.build();
    debugWin = std::make_unique<juce::DocumentWindow>("MeterBar Debug", juce::Colour(0xff231f31), juce::DocumentWindow::closeButton);
    debugLabel.setFont(12.5f); // v0.13: endpoint IDs are long — smaller font + wider window keeps them on one line
    debugWin->setContentNonOwned(&debugLabel, true); debugWin->setSize(470, 262); debugWin->setUsingNativeTitleBar(true);
    engine.applySource(); applyTheme(); applyFps();
    setSize(engine.state.windowW, engine.state.windowH);
    syncScreenSpace(); // v0.18: a preset loaded at startup may already ask for the reservation
    // v0.19.2: silent background check on launch — non-blocking, no
    // dialog. Up-to-date, offline, private-repo and error outcomes
    // are all silent at launch; only a NEWER release surfaces, as
    // the toolbar "Update!" indicator (see pollUpdater).
    if (mbupdate::configured()) startUpdateCheck(true);
}
MainComponent::~MainComponent() { stopTimer(); screenSpace.release(); screenSpace.restore(); if (engine.isRecording()) engine.stopRecording(); }
void MainComponent::applyTheme() {
    theme = themeByName(engine.state.themeName);
    frame.cmap = meterbar::ColorMap(engine.state.colorMap);
    logoLabel.setColour(juce::Label::textColourId, theme.accent);
    versionLabel.setColour(juce::Label::textColourId, theme.dim);
    hintLabel.setColour(juce::Label::textColourId, theme.dim);
    for (auto* b : { &presetBtn, &reposBtn, &recBtn, &pauseBtn, &settingsBtn, &burgerBtn, &minBtn, &quitBtn,
                     &reserveBtn, &arrangeBtn, &restoreBtn }) {
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
void MainComponent::applyFps() { int fps = engine.state.fpsLimit; startTimerHz(fps > 0 ? fps : 60); }
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
void MainComponent::syncScreenSpace() {
#if JUCE_WINDOWS
    auto* w = findParentComponentOfClass<juce::DocumentWindow>();
    HWND hwnd = (w && w->getPeer()) ? static_cast<HWND>(w->getPeer()->getNativeHandle()) : nullptr;
    if (hwnd) {
        RECT wr{}; GetWindowRect(hwnd, &wr);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof mi; GetMonitorInfoW(mon, &mi);
        const meterbar::IntRect rect{ wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top };
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
    // The toolbar toggle mirrors state whichever surface changed it
    // (toolbar button, Settings > Window, a loaded preset).
    if (reserveBtn.getToggleState() != engine.state.reserveScreenSpace)
        reserveBtn.setToggleState(engine.state.reserveScreenSpace, juce::dontSendNotification);
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
        const meterbar::IntRect rect{ wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top };
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
            case meterbar::ReserveEdge::Top:    strip = wr.bottom - mi.rcMonitor.top; break;
            case meterbar::ReserveEdge::Bottom: strip = mi.rcMonitor.bottom - wr.top; break;
            case meterbar::ReserveEdge::Left:   strip = wr.right - mi.rcMonitor.left; break;
            case meterbar::ReserveEdge::Right:  strip = mi.rcMonitor.right - wr.left; break;
        }
        screenSpace.arrange(hwnd, edge, strip);
        return;
    }
#endif
    screenSpace.arrange(nullptr, meterbar::ReserveEdge::Top, 0); // stub: records the honest "unavailable" status
}
void MainComponent::doRestoreWindows() { screenSpace.restore(); }
void MainComponent::rebuildSettings() { settingsWin->content.build(); }
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
        quitBtn.setBounds(bar.removeFromRight(52)); minBtn.setBounds(bar.removeFromRight(30));
        burgerBtn.setBounds(bar.removeFromRight(56)); settingsBtn.setBounds(bar.removeFromRight(74));
        pauseBtn.setBounds(bar.removeFromRight(58)); recBtn.setBounds(bar.removeFromRight(48));
        reposBtn.setBounds(bar.removeFromRight(96)); presetBtn.setBounds(bar.removeFromRight(66));
        // v0.18: the screen-space trio sits just left of Preset (visual
        // order: Reserve | Arrange | Restore | Preset | Reposition ...).
        restoreBtn.setBounds(bar.removeFromRight(62)); arrangeBtn.setBounds(bar.removeFromRight(66));
        linkBtn.setBounds(bar.removeFromRight(48));
        reserveBtn.setBounds(bar.removeFromRight(64));
        // v0.19.2: the update indicator takes a slice of the leftover
        // middle strip (where the hint label lives) — only while lit.
        if (updateAvailable) { updateBtn.setBounds(bar.removeFromLeft(78)); updateBtn.setVisible(true); }
        else updateBtn.setVisible(false);
        if (st.showRepositionHint) { hintLabel.setBounds(bar); hintLabel.setVisible(true); } else hintLabel.setVisible(false);
    } else {
        for (auto* b : { &presetBtn, &reposBtn, &recBtn, &pauseBtn, &settingsBtn, &burgerBtn, &minBtn, &quitBtn,
                         &reserveBtn, &arrangeBtn, &restoreBtn }) b->setVisible(false);
        logoLabel.setVisible(false); versionLabel.setVisible(false); hintLabel.setVisible(false);
        updateBtn.setVisible(false);
    }
    if (st.showToolbar) for (auto* b : { &presetBtn, &reposBtn, &recBtn, &pauseBtn, &settingsBtn, &burgerBtn, &minBtn, &quitBtn,
                                         &reserveBtn, &arrangeBtn, &restoreBtn }) b->setVisible(true);
    logoLabel.setVisible(st.showToolbar); versionLabel.setVisible(st.showToolbar);
    layoutModules();
    syncScreenSpace(); // v0.18.1: a resize is a reserve-machine event (bar height / position may have changed)
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
void MainComponent::drawVisualizerBackground(juce::Graphics& g) {
    auto bounds = getLocalBounds();
    g.fillAll(juce::Colours::black);
    if (bounds.isEmpty()) return;
    const int iw = juce::jlimit(1, 480, bounds.getWidth() / 2);
    const int ih = juce::jlimit(1, 270, bounds.getHeight() / 2);
    if (!vizCache.isValid() || vizCache.getWidth() != iw || vizCache.getHeight() != ih) {
        vizCache = juce::Image(juce::Image::RGB, iw, ih, true);
        vizBlur = juce::Image(juce::Image::RGB, iw, ih, true);
    }
    meterbar::VisualizerSettings vs;
    vs.baseHue = engine.state.visualizerBaseHue;
    vs.saturation = engine.state.visualizerSaturation;
    vs.paletteSpread = engine.state.visualizerSpread;
    vs.cycleSeconds = engine.state.visualizerCycleSec;
    {
        juce::Graphics ig(vizCache);
        ig.fillAll(juce::Colours::black);
        for (auto& p : visualizer.particles()) {
            auto c = visualizer.colorFor(p, vs);
            ig.setColour(juce::Colour(c.r, c.g, c.b));
            const float px = p.x * (float)iw, py = p.y * (float)ih;
            const float r = p.size * 1.35f; // v0.20.3: plumper dots — softer, rounder blobs (TickerBar v0.4.0 sibling)
            ig.fillEllipse(px - r, py - r, r * 2.f, r * 2.f);
        }
    }
    vizBlur = vizCache;
    vizBlur.duplicateIfShared(); // the convolution reads source while writing dest — they must not share pixels
    juce::ImageConvolutionKernel kernel(7);
    kernel.createGaussianBlur(2.8f); // v0.20.3: stronger blur (was 5/1.8) — the vibey wash
    kernel.applyToImage(vizBlur, vizCache, vizBlur.getBounds());
    g.drawImage(vizBlur, bounds.toFloat());
    g.setOpacity(0.45f); // v0.20.3: crisp pass backed off (was 0.9)
    g.drawImage(vizCache, bounds.toFloat());
    g.setOpacity(1.0f);
    g.setColour(juce::Colours::black.withAlpha(0.18f)); // v0.20.3: dark scrim — the moodier tone
    g.fillRect(bounds);
}
void MainComponent::timerCallback() {
    pollUpdater(); // v0.19: surface a finished background update check / download on the message thread
    auto now = juce::Time::getCurrentTime().toMilliseconds();
    // v0.18.1: NO screen-space work happens here any more. The v0.18
    // ~4x/sec timer poll of syncScreenSpace() was the flicker's engine;
    // the reservation is event-driven now (see syncScreenSpace).
    if (lastFrameMs) { double inst = 1000.0 / std::max<juce::int64>(1, now - lastFrameMs); uiFpsMeasured = 0.9 * uiFpsMeasured + 0.1 * inst; }
    lastFrameMs = now;
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
    // Loudness + stereo + scope
    frame.loudDb = engine.loudnessValue(); frame.peakLin = engine.peakValue();
    // v0.16 loudness pack readouts (integrated is gated BS.1770; true peak
    // is the oversampled inter-sample peak — see Loudness.hpp).
    frame.loudIntegrated = engine.loudnessIntegrated();
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
    float f = 0; scope.capture(bufL.data() + (need - scopeSamples), scopeSamples, st.scopeTrigger, f, frame.sampleRate);
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
    if (st.visualizerEnabled) {
        auto e = latestBands_;
        meterbar::VisualizerSettings vs;
        vs.enabled = true;
        vs.baseHue = st.visualizerBaseHue;
        vs.saturation = st.visualizerSaturation;
        vs.paletteSpread = st.visualizerSpread;
        vs.cycleSeconds = st.visualizerCycleSec;
        const float dt = (float)juce::jlimit(0.004, 0.1, 1.0 / std::max(1, st.fpsLimit));
        visualizer.update(dt, e.data(), vs);
    }
}
// ---------------- Menus ----------------
enum { idNew=1, idSave, idSaveAs, idOpenFolder, idLoadBase=1000,
       idTop=2001, idBottom, idStick, idPin, idDisplayBase=2100,
       idAbout=3001, idOpenSettings, idSrcTone, idSrcInput, idSrcLoop, idThemeBase=3100, idMapBase=3200,
       idCapPause, idCapRecord, idClip10, idClip60, idHint, idToolbar, idDebug, idUpdates, idQuit,
       idInDevBase=3300, idOutDevBase=3400 };
void MainComponent::savePresetInteractive(bool saveAs) {
    auto& st = engine.state;
    if (!saveAs && st.name != "Default" && !st.name.empty()) { presetio::save(st, juce::String(st.name)); return; }
    auto w = std::make_shared<juce::AlertWindow>("Save Preset", "Preset name:", juce::AlertWindow::NoIcon);
    w->addTextEditor("name", juce::String(st.name == "Default" ? "My Preset" : st.name)); w->addButton("Save", 1); w->addButton("Cancel", 0);
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r){
        if (r == 1) { engine.state.name = w->getTextEditorContents("name").toStdString(); presetio::save(engine.state, w->getTextEditorContents("name")); }
    }));
}
void MainComponent::loadPresetFile(const juce::File& f) {
    if (presetio::load(f, engine.state)) {
        engine.applySource(); applyTheme(); applyFps(); layoutModules(); repaint(); syncScreenSpace(); uiClick(ClickSounds::Which::Sparkle);
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
            tl->setBounds(clamped.x, clamped.y, clamped.w, clamped.h);
        }
    }
}
void MainComponent::showPresetMenu() {
    juce::PopupMenu m;
    m.addItem(idNew, "New Preset"); m.addItem(idSave, "Save"); m.addItem(idSaveAs, "Save As...");
    m.addItem(idOpenFolder, "Open Preset Folder"); m.addSeparator();
    auto files = presetio::presetFolder().findChildFiles(juce::File::findFiles, false, "*.json");
    int i = 0; for (auto& f : files) m.addItem(idLoadBase + (i++), f.getFileNameWithoutExtension());
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(presetBtn), [this, files](int r){
        if (r == idNew) { auto keepTheme = engine.state.themeName; engine.state = PresetState(); engine.state.themeName = keepTheme; engine.applySource(); applyTheme(); applyFps(); layoutModules(); }
        else if (r == idSave) savePresetInteractive(false);
        else if (r == idSaveAs) savePresetInteractive(true);
        else if (r == idOpenFolder) presetio::presetFolder().revealToUser();
        else if (r >= idLoadBase && r < idLoadBase + (int)files.size()) loadPresetFile(files[r - idLoadBase]);
    });
}
void MainComponent::doTopBottom(bool top) {
    auto disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (!disp) return; auto ua = disp->userArea;
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) {
        w->setBounds(ua.getX(), top ? ua.getY() : ua.getBottom() - getHeight(), ua.getWidth(), getHeight());
    }
    syncScreenSpace(); // v0.18: docking to the top is what arms the reservation (v0.18.1: the window's moved/resized events feed it too)
}
void MainComponent::doStick() {
    engine.state.stick = !engine.state.stick;
    if (engine.state.stick) doTopBottom(true);
}
void MainComponent::doMoveToDisplay(int idx) {
    auto disp = juce::Desktop::getInstance().getDisplays().displays[idx];
    juce::Rectangle<int> target(disp.userArea.getCentreX() - getWidth() / 2, disp.userArea.getY(), getWidth(), getHeight());
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setBounds(target);
    else setBounds(target);
}
void MainComponent::showRepositionMenu() {
    juce::PopupMenu m; auto& st = engine.state;
    m.addItem(idTop, "At the Top"); m.addItem(idBottom, "At the Bottom");
    m.addItem(idStick, "Stick", true, st.stick); m.addItem(idPin, "Pin Above Other Windows", true, st.pinned);
    juce::PopupMenu disp; auto displays = juce::Desktop::getInstance().getDisplays().displays;
    for (int i = 0; i < displays.size(); ++i) disp.addItem(idDisplayBase + i, "Display " + juce::String(i + 1));
    m.addSubMenu("Move to Display", disp);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(reposBtn), [this](int r){
        // Defer ALL window moves until the popup menu has fully closed.
        // Field report (2026-10-13): moving the window from inside the menu
        // callback left it stuck at the top with dead buttons (stuck mouse
        // capture during menu dismissal on Windows).
        juce::MessageManager::callAsync([this, r]{
            if (r == idTop) doTopBottom(true); else if (r == idBottom) doTopBottom(false);
            else if (r == idStick) doStick();
            else if (r == idPin) { engine.state.pinned = !engine.state.pinned;
                if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setAlwaysOnTop(engine.state.pinned); }
            else if (r >= idDisplayBase) doMoveToDisplay(r - idDisplayBase);
        });
    });
}
void MainComponent::toggleRecord() {
    if (engine.isRecording()) { engine.stopRecording(); recBtn.setButtonText("Rec"); return; }
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MeterBar");
    dir.createDirectory();
    auto f = dir.getChildFile("record-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".wav");
    if (engine.startRecording(f)) recBtn.setButtonText("Stop");
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
    m.addSeparator();
    m.addItem(idHint, "Show Reposition Hint", true, st.showRepositionHint);
    m.addItem(idToolbar, "Show Toolbar", true, st.showToolbar);
    m.addItem(idDebug, "Show Debug Window", true, debugWin->isVisible());
    m.addItem(idUpdates, "Check For Updates");
    m.addSeparator(); m.addItem(idQuit, "Quit");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(burgerBtn), [this, inNames, outNames](int r){
        auto& s = engine.state;
        if (r == idAbout) juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "MeterBar",
            "MeterBar " + kVersion + "\nA compact audio metering bar: Spectrum, Spectrogram, Waveform,\nLoudness, Stereometer, Oscilloscope.\nBuilt with JUCE + a custom DSP core.\nMade with JUCE (juce.com) — free-license attribution.");
        else if (r == idOpenSettings) { rebuildSettings(); settingsWin->setVisible(true); settingsWin->toFront(true); }
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
        else if (r == idHint) { s.showRepositionHint = !s.showRepositionHint; resized(); }
        else if (r == idToolbar) { s.showToolbar = !s.showToolbar; resized(); repaint(); }
        else if (r == idDebug) debugWin->setVisible(!debugWin->isVisible());
        else if (r == idUpdates) checkForUpdates();
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
