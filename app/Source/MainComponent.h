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

// v0.22.1: the icon buttons' private LookAndFeel — stock V4 in
// every respect EXCEPT the font. The v0.22.0 glyph buttons render
// as tofu on real Windows (Jackson's TickerBar v0.6.0 field
// evidence): the default button typeface has no emoji glyphs.
// Segoe UI Emoji carries them (Segoe UI Symbol is the fallback
// face); if neither exists at runtime the default sans is used,
// which is the pre-fix behaviour. Applied per button, so every
// other button (Update! included) keeps the existing font.
class IconButtonLookAndFeel : public juce::LookAndFeel_V4 {
public:
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

class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void notifyWindowGeometryChanged(); // v0.18.1: MainWindow forwards moved()/resized() here — the event feed for the reserve machine
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    AudioEngine engine;
private:
    void timerCallback() override;
    void prepareFrame();
    void layoutModules();
    void applyTheme(); void applyFps(); void rebuildSettings();
    void showHamburgerMenu();
    // v0.22.0: the Preset and Reposition dropdowns are SUBMENUS of
    // the hamburger menu now (their toolbar buttons are gone); the
    // builders read live state at open, exactly as the old
    // standalone menus did, and showHamburgerMenu's callback handles
    // their item ids.
    juce::PopupMenu buildPresetSubMenu(std::vector<juce::File>& filesOut);
    juce::PopupMenu buildRepositionSubMenu();
    void savePresetInteractive(bool saveAs);
    void loadPresetFile(const juce::File&);
    void doTopBottom(bool top); void doStick(); void doMoveToDisplay(int idx);
    void toggleRecord(); void exportClip(double seconds);
    void setDeviceByName(bool input, const juce::String& name);
    void adjustFlex(int leftFlexIdx, int rightFlexIdx, int dxPx); // v0.15 splitter drag
    void adjustMascotFlex(int dxPx);                              // v0.16: mascot cell splitter drag
    void uiClick(ClickSounds::Which w);                           // v0.16: Anime-gated cute click
    void syncScreenSpace();                                       // v0.18.1: feed one geometry/toggle observation to the reserve machine; act only on its transitions
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
    juce::TextButton updateBtn { "Update!" };                     // v0.19.2: non-modal launch-check surface (opens showUpdatePanel)
    mbupdate::ReleaseInfo pendingRelease;                         // the release a download/Update action refers to
    std::unique_ptr<juce::URL::DownloadTask> updateDownload;
    bool updateDownloadPending = false;
    juce::File updateInstallerFile;
    IconButtonLookAndFeel iconLnf_; // v0.22.1: declared before the buttons so it outlives them
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
    ScreenSpace screenSpace;                                      // v0.18: AppBar + arrange service (Windows-real, stub elsewhere)
    meterbar::ReserveMachine reserveMachine;                      // v0.18.1: event-driven dock/reserve decisions (hysteresis) — replaces the v0.18 ~4x/sec timer poll
    juce::Label logoLabel, versionLabel, hintLabel;
    std::vector<std::unique_ptr<ModuleView>> modules;
    std::vector<std::unique_ptr<ModuleSplitter>> splitters; // v0.15: one per gap (modules + mascot cell, max 6)
    MascotView mascot;                                      // v0.16: Anime-only, docked after the last module
    ClickSounds clicks;                                     // v0.16: Anime-only UI click playback
    uint64_t lastOnsetCount = 0;                            // v0.16: mascot bob/excited triggers
    juce::int64 bobStartMs = 0, excitedUntilMs = 0;
    AnalysisData frame;
    meterbar::VisualizerEngine visualizer;   // v0.20.0: background-mode particle model (pure core)
    meterbar::VisualizerLines vizLines;      // v0.21.5: the Lines style's ribbon model (pure core)
    std::array<float, 3> latestBands_{};     // v0.20.1: newest spectrum band energies (BarLink audio broadcast, item 17)
    float latestLevel_ = 0.f;                // v0.20.1: newest overall level 0..1 for the same broadcast
    juce::Image vizCache, vizBlur;           // v0.20.0: half-res particle layer + its Gaussian-blurred copy
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
    std::unique_ptr<juce::DocumentWindow> debugWin; juce::Label debugLabel;
    int debugEpsCount = -1; juce::int64 lastEpsCheckMs = 0; // v0.15 item 8: render-endpoint count shown in Debug (refreshed ~2 s)
    std::unique_ptr<juce::FileChooser> chooser;
    double uiFpsMeasured = 0; juce::int64 lastFrameMs = 0; int clipCounter = 1;
    juce::File recordingFile_; // v0.21.4: the take currently being written (the Recordings menu omits it)
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
