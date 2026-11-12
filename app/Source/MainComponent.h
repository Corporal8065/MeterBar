#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "Theme.h"
#include "ModuleViews.h"
#include "Settings.h"
#include "PresetIO.h"
#include "ClickSounds.h"
#include "ScreenSpace.h"
#include "Updater.h"
#include "MeterBarVersion.h" // v0.19: generated — single-source version
#include <meterbar/ReserveState.hpp>
#include <meterbar/Visualizer.hpp> // v0.20.0
#include <meterbar/Glyphs.hpp> // v0.22.2: icon glyphs as explicit UTF-8 bytes
#include "BarLink.h" // v0.20.0 (k): family link client
#include "Captions/CaptionController.h" // v0.23.0: Live Captions (MeterBar only)
#include "Captions/CaptionStrip.h"      // v0.24.0: docked caption strip
#include <meterbar/WindowGeom.hpp>      // v0.24.0: caption homing / dock growth (pure core)
#include <meterbar/Performance.hpp>     // v0.28.2: Performance Mode effective limits + Auto latch (pure core)
#include <meterbar/PinSource.hpp>       // v0.28.7: pinned-app source watch machine (pure core)
#include <meterbar/CloseSave.hpp>       // v0.28.8: close-save state comparison (pure core)

// v0.15: draggable divider between two adjacent modules. Dragging adjusts
// the pair's PresetState::moduleFlex weights (their sum is preserved, so
// only the pair trades width). MainComponent owns one per visible gap and
// (re)binds onDrag targets in layoutModules().
class ModuleSplitter : public juce::Component {
public:
    std::function<void(int)> onDrag; // pixel delta since the last drag event
    ModuleSplitter() { setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); setInterceptsMouseClicks(true, false); }
    void mouseDown(const juce::MouseEvent& e) override { lastX = e.x; }
    void mouseDrag(const juce::MouseEvent& e) override {
        int dx = e.x - lastX; lastX = e.x;
        if (dx != 0 && onDrag) onDrag(dx);
    }
    void paint(juce::Graphics& g) override { // faint centre grip dots so the divider is discoverable
        g.setColour(juce::Colour(0x66ffffff));
        float cx = getWidth() * 0.5f, cy = getHeight() * 0.5f;
        for (int i = -1; i <= 1; ++i) g.fillEllipse(cx - 1.f, cy + i * 6.f - 1.f, 2.f, 2.f);
    }
private:
    int lastX = 0;
};

// v0.26.1: button text with the TEXT POP (TextPop.h). Stock V4
// in every respect except drawButtonText, which mirrors
// LookAndFeel_V2's geometry exactly and adds the hard offset
// backing pass — the stock bare glyph/text washed out over the
// bar's translucent toolbar, the same failure as the module text.
class PoppedButtonLookAndFeel : public juce::LookAndFeel_V4 {
public:
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool /*shouldDrawButtonAsHighlighted*/, bool /*shouldDrawButtonAsDown*/) override {
        juce::Font font(getTextButtonFont(button, button.getHeight()));
        g.setFont(font);
        const auto colour = button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId
                                                                      : juce::TextButton::textColourOffId)
                              .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f);
        const int yIndent = juce::jmin(4, button.proportionOfHeight(0.3f));
        const int cornerSize = juce::jmin(button.getHeight(), button.getWidth()) / 2;
        const int fontHeight = juce::roundToInt(font.getHeight() * 0.6f);
        const int leftIndent = juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
        const int rightIndent = juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
        const int textWidth = button.getWidth() - leftIndent - rightIndent;
        if (textWidth > 0)
            textpop::drawFitted(g, button.getButtonText(),
                                juce::Rectangle<int>(leftIndent, yIndent, textWidth, button.getHeight() - yIndent * 2),
                                juce::Justification::centred, 2, 1.0f, colour);
    }
};

// v0.26.1: the toolbar labels (logo / version / hint) with the
// TEXT POP — mirrors LookAndFeel_V2::drawLabel, adding the
// backing pass before the fitted text. The labels' background
// and outline colours are transparent, so only the text changes.
class PoppedLabelLookAndFeel : public juce::LookAndFeel_V4 {
public:
    void drawLabel(juce::Graphics& g, juce::Label& label) override {
        g.fillAll(label.findColour(juce::Label::backgroundColourId));
        if (!label.isBeingEdited()) {
            const auto alpha = label.isEnabled() ? 1.0f : 0.5f;
            const juce::Font font(getLabelFont(label));
            g.setFont(font);
            auto textArea = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
            textpop::drawFitted(g, label.getText(), textArea, label.getJustificationType(),
                                juce::jmax(1, (int)((float)textArea.getHeight() / font.getHeight())),
                                label.getMinimumHorizontalScale(),
                                label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
            g.setColour(label.findColour(juce::Label::outlineColourId).withMultipliedAlpha(alpha));
        } else if (label.isEnabled()) {
            g.setColour(label.findColour(juce::Label::outlineColourId));
        }
        g.drawRect(label.getLocalBounds());
    }
};

// v0.22.1: the icon buttons' private LookAndFeel — stock V4 in
// every respect EXCEPT the font. The v0.22.0 glyph buttons render
// as tofu on real Windows (Jackson's TickerBar v0.6.0 field
// evidence): the default button typeface has no emoji glyphs.
// Segoe UI Emoji carries them (Segoe UI Symbol is the fallback
// face); if neither exists at runtime the default sans is used,
// which is the pre-fix behaviour. Applied per button, so every
// other button (Update! included) keeps the existing font.
// v0.27.0 (item 6, Jackson's correction): the icon GLYPHS are
// emoji — they must NOT be doubled ("the emoji text did not need
// doubling — it was good as it was"). This class therefore derives
// from LookAndFeel_V4 DIRECTLY again and draws its button text
// plainly, once, exactly as the stock V4 does; only the FONT (the
// emoji typeface below) is overridden. Word buttons keep the pop
// via PoppedButtonLookAndFeel.
class IconButtonLookAndFeel : public juce::LookAndFeel_V4 {
public:
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool /*shouldDrawButtonAsHighlighted*/, bool /*shouldDrawButtonAsDown*/) override {
        juce::Font font(getTextButtonFont(button, button.getHeight()));
        g.setFont(font);
        const auto colour = button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId
                                                                      : juce::TextButton::textColourOffId)
                              .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f);
        const int yIndent = juce::jmin(4, button.proportionOfHeight(0.3f));
        const int cornerSize = juce::jmin(button.getHeight(), button.getWidth()) / 2;
        const int fontHeight = juce::roundToInt(font.getHeight() * 0.6f);
        const int leftIndent = juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
        const int rightIndent = juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
        const int textWidth = button.getWidth() - leftIndent - rightIndent;
        if (textWidth > 0) {
            g.setColour(colour);
            // v0.28.0: minScale 0.55 (was 1.0) — the Rec button's
            // text is swapped to "Rec"/"Stop" by the frame timer,
            // and at minScale 1.0 the stock fitted draw ellipsised
            // it to "R..." in the 44 px cluster button (Jackson,
            // 2026-11-02 21:54 PT: the Recording label must fit in
            // full). Glyphs are single wide characters and are
            // unaffected in practice.
            g.drawFittedText(button.getButtonText(),
                             juce::Rectangle<int>(leftIndent, yIndent, textWidth, button.getHeight() - yIndent * 2),
                             juce::Justification::centred, 2, 0.55f);
        }
    }
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override {
        const float h = juce::jmin(16.0f, (float)buttonHeight * 0.62f);
        // Drag-lag fix (2026-10-31): findAllTypefaceNames() enumerates the
        // whole system font collection, and this callback runs per glyph
        // button per paint — the bar repaints full-window at frame rate,
        // so the enumeration ran hundreds of times a second on the
        // message thread. Resolve the typeface choice ONCE.
        static const juce::String emojiTypeface = [] {
            for (auto* name : { "Segoe UI Emoji", "Segoe UI Symbol" })
                if (juce::Font::findAllTypefaceNames().contains(name))
                    return juce::String(name);
            return juce::String{};
        }();
        if (emojiTypeface.isNotEmpty())
            return juce::Font(juce::FontOptions(emojiTypeface, h, juce::Font::plain));
        return juce::Font(juce::FontOptions(h));
    }
};

// v0.28.2 (addendum, Jackson 2026-11-03): a DocumentWindow whose X
// HIDES it. The Debug window used a stock juce::DocumentWindow, whose
// default closeButtonPressed() does nothing — the X was dead and the
// only way out was re-opening the hamburger menu and toggling "Show
// Debug Window" back off. The menu item reads isVisible() when the
// menu is built, so its tick follows the X with no extra wiring.
// (SettingsWindow in Settings.h already overrides it the same way.)
class HideOnCloseWindow : public juce::DocumentWindow {
public:
    using juce::DocumentWindow::DocumentWindow;
    void closeButtonPressed() override { setVisible(false); }
};

class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override; // v0.28.10: post-show hook — the first syncScreenSpace with a live peer (the constructor's sync runs pre-parent and is a no-op)
    void notifyWindowGeometryChanged(); // v0.18.1: MainWindow forwards moved()/resized() here — the event feed for the reserve machine
    void requestClose(); // v0.28.8: the close gate — every quit path routes here; prompts ONLY when the state changed since open (SessionBar parity)
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    AudioEngine engine;
private:
    void timerCallback() override;
    void prepareFrame();
    void layoutModules();
    void applyTheme(); void applyFps(); void rebuildSettings();
    void toggleSettings(); // v0.28.6: Settings open-or-close (clicking Settings while it is open closes it)
    meterbar::EffectiveLimits perfLimits(); // v0.28.2: mode + Auto latch + user settings -> effective per-frame limits (Performance.hpp)
    void loadPerfPersist();                 // v0.28.2: re-arm the Auto latch from Documents/MeterBar/perf-auto.json (a stepped-down session starts stepped down)
    void savePerfPersist(bool steppedDown); // v0.28.2: write/clear that file (called on latch trip and on any explicit mode change)
    void loadPinPersist();                  // v0.28.7: restore the pinned-app source from Documents/MeterBar/pin-source.json (BEFORE the first applySource)
    void savePinPersist();                  // v0.28.7: write that file from live state (called on every source change)
    void pollPinnedSource(juce::int64 nowMs); // v0.28.7: ~1 s watcher step from timerCallback — attach the pinned exe on appear, detach on disappear, NEVER fall back
    void showHamburgerMenu();
    // v0.22.0: the Preset and Reposition dropdowns are SUBMENUS of
    // the hamburger menu now (their toolbar buttons are gone); the
    // builders read live state at open, exactly as the old
    // standalone menus did, and showHamburgerMenu's callback handles
    // their item ids.
    juce::PopupMenu buildPresetSubMenu(std::vector<juce::File>& filesOut);
    juce::PopupMenu buildRepositionSubMenu();
    void savePresetInteractive(bool saveAs);
    void performSave(bool saveAs, std::function<void(bool)> done); // v0.28.8: THE save — the Presets menu and the close prompt share this one path
    void promptForPresetNameAsync(std::function<void(bool)> done); // v0.28.8: name prompt for performSave (cancel/empty name -> done(false), nothing written)
    void syncWindowRectToState(); // v0.28.8: stamp the top level's live rect into state.windowX/Y/W/H, the docked-caption strip height normalised back out
    void loadPresetFile(const juce::File&);
    void doTopBottom(bool top); void doStick(); void setStick(bool on); void doMoveToDisplay(int idx); // v0.28.9: setStick = the merged Stick=Reserve action (menu + Settings both land here)
    void toggleRecord(); void exportClip(double seconds);
    void setDeviceByName(bool input, const juce::String& name);
    void adjustFlex(int leftFlexIdx, int rightFlexIdx, int dxPx); // v0.15 splitter drag
    void adjustMascotFlex(int dxPx);                              // v0.16: mascot cell splitter drag
    void uiClick(ClickSounds::Which w);                           // v0.16: Anime-gated cute click
    void syncScreenSpace();                                       // v0.18.1: feed one geometry/toggle observation to the reserve machine; act only on its transitions
    void scheduleDeferredSync();                                  // v0.28.11: arm the one-shot settled re-sync (deadline honoured in timerCallback) — after dock actions, first show, and mid-drag geometry events
    void syncCaptionDock();                                       // v0.24.0: reconcile the bar's height with the docked-caption state (grow/shrink on transitions only) + feed the strip
    meterbar::ReserveEdge captionAnchorEdge();                    // v0.24.0: the edge dock growth anchors at (reserved edge, else nearest)
    void doArrangeWindows();                                      // v0.18: sweep other windows clear of the bar (v0.19.1: on its docked edge)
    void doRestoreWindows();                                      // v0.18: undo the sweep
    // v0.19 updater; v0.19.2: wired to the real repo + launch check:
    void checkForUpdates();                                       // menu action: honest-unconfigured, or start the background check
    void startUpdateCheck(bool fromLaunch);                       // the one check starter (menu or launch); worker touches only updateState
    void pollUpdater();                                           // called from timerCallback: present a finished check / finish a download
    void presentUpdateResult(const mbupdate::ReleaseInfo& r);     // manual-check outcomes: up-to-date / failed / private-repo / update panel
    void showUpdatePanel(const mbupdate::ReleaseInfo& r);         // version + full release notes + "Download & install" / "Later"
    void showPrivateRepoFallback(const juce::String& detail);     // the private-repo state + "Open releases page" browser button
    void startUpdateDownload();                                   // download the release installer to %TEMP% (polled to completion)
    std::shared_ptr<mbupdate::SharedState> updateState;           // background-check state (worker touches only this)
    bool updateCheckPending = false;
    bool updateCheckFromLaunch = false;                           // launch checks stay silent unless a newer release is found
    bool updateAvailable = false;                                 // a newer release is known: the toolbar indicator is lit
    // v0.28.0: updateBtn no longer wears wordBtnLnf_ (the text-pop
    // look) — Jackson, 2026-11-02 21:54 PT: the Update button's
    // label "was doubled" by the pop's back copy over the toolbar;
    // it draws plainly, once, with the stock look now. The
    // PoppedButtonLookAndFeel class above stays for the record;
    // nothing else used it.
    juce::TextButton updateBtn { "Update!" };                     // v0.19.2: non-modal launch-check surface (opens showUpdatePanel)
    mbupdate::ReleaseInfo pendingRelease;                         // the release a download/Update action refers to
    std::unique_ptr<juce::URL::DownloadTask> updateDownload;
    bool updateDownloadPending = false;
    juce::File updateInstallerFile;
    IconButtonLookAndFeel iconLnf_; // v0.22.1: declared before the buttons so it outlives them
    PoppedLabelLookAndFeel labelLnf_; // v0.26.1: toolbar labels' text-pop look; declared before the labels so it outlives them
    // toolbar — v0.22.0: ICON-ONLY (Jackson's ask: compress the top
    // row into the Menu). Link / Rec / Pause / Settings / Menu stay
    // as glyph buttons with tooltips; Presets, Arrange, Restore,
    // Reposition, Minimize and Quit live in the hamburger menu now.
    // (v0.21.2 had already moved Reserve into the Reposition menu.)
    // v0.22.2: glyph text is built from the core's explicit UTF-8
    // byte escapes via fromUTF8 — the raw literals used up to
    // v0.22.1 were decoded as Windows-1252 by MSVC and reached the
    // buttons as mojibake (see meterbar/Glyphs.hpp).
    juce::TextButton linkBtn{juce::String::fromUTF8(meterbar::glyphLinkUtf8())}; // v0.20.0 (k)
    juce::TextButton recBtn{juce::String::fromUTF8(meterbar::glyphRecordUtf8())},
                     pauseBtn{juce::String::fromUTF8(meterbar::glyphPauseUtf8())},
                     settingsBtn{juce::String::fromUTF8(meterbar::glyphSettingsUtf8())},
                     burgerBtn{juce::String::fromUTF8(meterbar::glyphMenuUtf8())};
    BarLinkClient linkClient_{ meterbar::kLinkAppMeter };        // v0.20.0 (k)
    int linkBgEffectShadow_ = -1;   // drag-lag fix (2026-10-31): last adopted link effect ordinal MeterBar cannot natively emit (peer glow = 2); getBg passes it through until a local settings change clears it
    // BLK7 (DESIGN §6d/§7): link snapshot/restore + pulse emit state.
    bool linkAppearanceSnapshot_ = false;
    BarLinkClient::Bg linkSnapBg_{};
    int linkSnapColorMap_ = 0; bool linkSnapVizEnabled_ = false; int linkSnapVizStyle_ = 0;
    float linkSnapPal_[4] = { 0.f, 0.f, 0.f, 0.f };
    bool linkAllGeomSnapshot_ = false; juce::Rectangle<int> linkAllGeom_{};
    uint64_t pulseOnsetCount_ = 0; unsigned pulseSeq_ = 0; int pulseColour_ = 0; float pulseStrength_ = 0.f;
    ScreenSpace screenSpace;                                      // v0.18: AppBar + arrange service (Windows-real, stub elsewhere)
    meterbar::ReserveMachine reserveMachine;                      // v0.18.1: event-driven dock/reserve decisions (hysteresis) — replaces the v0.18 ~4x/sec timer poll
    juce::int64 deferredSyncAtMs_ = 0;                            // v0.28.11: when the deferred re-sync is due (0 = disarmed); set by scheduleDeferredSync, honoured once in timerCallback
    juce::Label logoLabel, versionLabel, hintLabel;
    // v0.28.6: the TRIM slider on the bar itself (Jackson's ask) — a
    // second surface for the SAME PresetState::trimDb the Settings
    // slider writes (Settings > Audio Source > Trim dB). One
    // setting, two surfaces: the bar slider writes the state, the
    // frame timer mirrors state back into the slider (so a Settings
    // or preset change shows here), never a second variable.
    juce::Label trimLabel_;
    juce::Slider trimSlider_;
    std::vector<std::unique_ptr<ModuleView>> modules;
    std::vector<std::unique_ptr<ModuleSplitter>> splitters; // v0.15: one per gap (modules + mascot cell, max 6)
    MascotView mascot;                                      // v0.16: Anime-only, docked after the last module
    ClickSounds clicks;                                     // v0.16: Anime-only UI click playback
    uint64_t lastOnsetCount = 0;                            // v0.16: mascot bob/excited triggers
    juce::int64 bobStartMs = 0, excitedUntilMs = 0;
    AnalysisData frame;
    meterbar::VisualizerEngine visualizer;   // v0.20.0: background-mode particle model (pure core)
    meterbar::VisualizerLines vizLines;      // v0.21.5: the Lines style's ribbon model (pure core)
    meterbar::VisualizerField vizField;      // v0.25.0: the Field style's vortex model (pure core)
    meterbar::UndertowModel undertow_;       // v0.28.0: the Undertow style's five-band model (pure core, visualizer council DESIGN.md)
    meterbar::SpectrumAnalyser utAnalyser_;  // v0.28.0: dedicated 2048-pt Path B analyser — unsmoothed, untilted, never the display spectrum
    bool utAnalyserReady_ = false;
    double lastUtMs_ = 0;                    // v0.28.0: the Undertow model's real-dt clock (the fixed frame step is banned for this section)
    std::vector<float> utMono_;              // v0.28.0: Path B's mono feed buffer
    juce::Image vizBloomQ, vizBloomE;        // v0.28.0: thresholded bright-pass bloom results (quarter / eighth res of the layer)
    int utBloomFrame_ = 0;                   // v0.28.0: bloom recompute cadence — every 2nd frame (~30 Hz per DESIGN)
    std::vector<juce::Rectangle<int>> utSafeLocal_; // v0.28.0: label safe zones in local px — the renderer dims the layer inside them
    void drawUndertowLayer(juce::Graphics&, int iw, int ih);   // v0.28.0: render the Undertow model into the half-res layer
    void rebuildUndertowBloom(int iw, int ih);                 // v0.28.0: thresholded bright-pass + dual-scale blur
    std::array<float, 3> latestBands_{};     // v0.20.1: newest spectrum band energies (BarLink audio broadcast, item 17)
    float latestLevel_ = 0.f;                // v0.20.1: newest overall level 0..1 for the same broadcast
    float latestBalance_ = 0.f;              // v0.25.0: newest stereo balance -1..1 for the same broadcast + local emitters
    juce::Image vizCache, vizBlur;           // v0.20.0: half-res particle layer + its Gaussian-blurred glow
    juce::Image vizBlurSrc;                  // v0.23.1: quarter-res downsample the blur convolves (lag fix, audit Finding 3)
    int vizBlurFrame_ = 0;                   // v0.23.1: blur recompute cadence — every 2nd frame, held between
    void drawVisualizerBackground(juce::Graphics&); // v0.20.0: black base + blurred glow + crisp dots
    int lastScopeSamples = 0;                                // v0.15: scope capture size (from scopeWindowMs)
    float dispDbL = -120.f, dispDbR = -120.f;                // v0.15: smoothed per-channel display levels
    float dispPeakDbL = -120.f, dispPeakDbR = -120.f;        // v0.15: per-channel peak holds
    meterbar::SpectrumAnalyser analyser;
    meterbar::SpectrogramHistory spectro{ };
    meterbar::WaveformHistory waveHist;
    meterbar::OscilloscopeEngine scope;
    std::vector<float> bufL, bufR, mono;
    bool analyserReady = false; int lastFftSize = 0;
    double cachedLoudIntegrated_ = -120.0; long long lastLoudIntegratedMs_ = 0; // drag-lag fix: 4 Hz throttle cache (prepareFrame)
    Theme theme = themeByName("Midnight Kids");
    std::unique_ptr<SettingsWindow> settingsWin;
    std::unique_ptr<HideOnCloseWindow> debugWin; juce::Label debugLabel; // v0.28.2: HideOnCloseWindow — the X hides (see class note)
    int debugEpsCount = -1; juce::int64 lastEpsCheckMs = 0; // v0.15 item 8: render-endpoint count shown in Debug (refreshed ~2 s)
    std::unique_ptr<juce::FileChooser> chooser;
    double uiFpsMeasured = 0; juce::int64 lastFrameMs = 0; int clipCounter = 1;
    // v0.28.2 PERFORMANCE MODE (Auto): the latch + the last mode the
    // latch was fed under (a mode change resets it — the latch is a
    // per-session, per-mode-choice decision), the frame counter the
    // scope-pitch throttle divides, and the cached scope frequency
    // served between throttled pitch runs.
    meterbar::PerfAutoLatch perfAuto_;
    meterbar::PinWatch pinWatch_;                       // v0.28.7: pinned-source attach/detach belief (policy in meterbar/PinSource.hpp)
    juce::int64 pinLastTickMs_ = 0, pinLastActionMs_ = 0; // v0.28.7: watcher cadence + settle stamp (stamped continuously while pinned mode is off, so the settle window runs from mode entry)
    int perfModeShadow_ = 0;
    long long perfFrame_ = 0;
    float cachedScopeFreq_ = 0;
    juce::File recordingFile_; // v0.21.4: the take currently being written (the Recordings menu omits it)
    // v0.24.0: the docked caption strip (child component; content is
    // plain strings pulled from the controller each tick, so member
    // order vs captions_ carries no lifetime constraint).
    CaptionStrip captionStrip_;
    bool captionStripInHeight_ = false; // our strip growth is currently in the window height
    // v0.28.8: close-save state — the state as opened (captured on
    // the first UI tick, after MainWindow has placed the window)
    // and the prompt's re-entry guard.
    meterbar::PresetState openBaseline_;
    bool openBaselineCaptured_ = false;
    bool closePromptOpen_ = false;
    static constexpr int kCaptionStripHeight = 76;
    // v0.23.0 Live Captions: declared late so it destructs BEFORE
    // engine (reverse declaration order) — the controller stops the
    // tap/streamer first, then engine teardown cannot call into a
    // dead client (spec step 6).
    CaptionController captions_;
    // Whole-toolbar drag state (v0.8, Jackson's ask: the entire top bar
    // moves the window, grip dots removed). v0.20.2: ComponentDragger is
    // back (SessionBar's path) — the v0.11 OS caption move loop that
    // replaced it is freeze-audit suspect 2; see mouseDown in the .cpp.
    bool winDragging = false; juce::ComponentDragger windowDragger;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
