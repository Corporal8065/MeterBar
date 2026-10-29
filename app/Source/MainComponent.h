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
#include <meterbar/ReserveState.hpp>

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
    void showPresetMenu(); void showRepositionMenu(); void showHamburgerMenu();
    void savePresetInteractive(bool saveAs);
    void loadPresetFile(const juce::File&);
    void doTopBottom(bool top); void doStick(); void doMoveToDisplay(int idx);
    void toggleRecord(); void exportClip(double seconds);
    void setDeviceByName(bool input, const juce::String& name);
    void adjustFlex(int leftFlexIdx, int rightFlexIdx, int dxPx); // v0.15 splitter drag
    void adjustMascotFlex(int dxPx);                              // v0.16: mascot cell splitter drag
    void uiClick(ClickSounds::Which w);                           // v0.16: Anime-gated cute click
    void syncScreenSpace();                                       // v0.18.1: feed one geometry/toggle observation to the reserve machine; act only on its transitions
    void doArrangeWindows();                                      // v0.18: sweep other windows below the bar
    void doRestoreWindows();                                      // v0.18: undo the sweep
    // toolbar
    juce::TextButton presetBtn{"Preset"}, reposBtn{"Reposition"}, recBtn{"Rec"}, pauseBtn{"Pause"},
                     settingsBtn{"Settings"}, burgerBtn{"Menu"}, minBtn{"_"}, quitBtn{"Quit"};
    // v0.18 screen-space controls, sitting with the Reposition/pin family:
    // Reserve = the AppBar toggle; Arrange/Restore = the window sweep.
    juce::TextButton reserveBtn{"Reserve"}, arrangeBtn{"Arrange"}, restoreBtn{"Restore"};
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
    int lastScopeSamples = 0;                                // v0.15: scope capture size (from scopeWindowMs)
    float dispDbL = -120.f, dispDbR = -120.f;                // v0.15: smoothed per-channel display levels
    float dispPeakDbL = -120.f, dispPeakDbR = -120.f;        // v0.15: per-channel peak holds
    meterbar::SpectrumAnalyser analyser;
    meterbar::SpectrogramHistory spectro{ };
    meterbar::WaveformHistory waveHist;
    meterbar::OscilloscopeEngine scope;
    std::vector<float> bufL, bufR, mono;
    bool analyserReady = false; int lastFftSize = 0;
    Theme theme = themeByName("Midnight Kids");
    std::unique_ptr<SettingsWindow> settingsWin;
    std::unique_ptr<juce::DocumentWindow> debugWin; juce::Label debugLabel;
    int debugEpsCount = -1; juce::int64 lastEpsCheckMs = 0; // v0.15 item 8: render-endpoint count shown in Debug (refreshed ~2 s)
    std::unique_ptr<juce::FileChooser> chooser;
    double uiFpsMeasured = 0; juce::int64 lastFrameMs = 0; int clipCounter = 1;
    // Manual whole-toolbar drag state (v0.8, Jackson's ask: the entire top bar
    // moves the window, grip dots removed). ComponentDragger was replaced after
    // the field report of the cursor staying grabbed (2026-10-13).
    bool winDragging = false; juce::Point<int> dragOffset;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
