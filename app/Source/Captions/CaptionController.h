#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <meterbar/Captions.hpp>
#include "../AudioEngine.h"
#include "CaptionClient.h"
#include "CaptionOverlay.h"
#include "CaptionSettings.h"

// LIVE CAPTIONS — CONTROLLER (v0.23.0, spec sections 1.2, 2.3, 6).
//
// The glue between MeterBar and the caption feature: owns the
// client, the overlay and the settings; tracks mode (user's
// selection) separately from runtime state (what the helper reports
// — a game hold must not un-select Translate); feeds pause / source
// / epoch changes to the helper; applies the one-automatic-restart
// rule; and renders helper captions through the pure display model.
//
// Everything here runs on the message thread: MainComponent calls
// poll() from its UI timer.
class CaptionController {
public:
    CaptionController() = default;
    void initialise(AudioEngine& engine,
                    std::function<juce::Rectangle<int>()> barBoundsProvider);
    void shutdown();

    meterbar::CaptionMode mode() const { return settings_.mode; }
    void setMode(meterbar::CaptionMode m);

    // v0.24.0: presentation flip — the separate overlay window, or a
    // strip docked inside MeterBar (MainComponent owns the strip and
    // the bar's growth; this controller owns the choice + content).
    bool presentationDocked() const { return settings_.presentation == 1; }
    void setPresentation(bool docked);
    bool dockedActive() const {
        return settings_.mode != meterbar::CaptionMode::Off && presentationDocked();
    }
    // The strip's renderable content, same model + badge + pill the
    // overlay would show (MainComponent polls this every UI tick).
    struct DockedContent {
        juce::String previous, current, badge, pill;
        bool currentFinal = false;
    };
    DockedContent dockedContent() const;

    juce::String statusText() const; // the menu's disabled status item
    bool setupNeeded() const { return setupNeeded_; }
    void launchSetup();

    bool overlayAlwaysOnTop() const { return settings_.alwaysOnTop; }
    void setOverlayAlwaysOnTop(bool on);
    void moveCaptionWindow();
    void resetCaptionPosition();

    void poll(); // drain events, track edges, drive the overlay

private:
    void startHelper();
    void stopHelper(bool intentional);
    void applyEvent(const CaptionEvent& e);
    void persistSettings();
    void persistBounds();
    juce::String sourceClass() const;  // "system" | "microphone"
    juce::String sourceLabel() const;  // "SYSTEM AUDIO" | "MICROPHONE"
    juce::String statusBadge() const;  // overlay status strip
    juce::String statusPill() const;   // no-caption status line (may be empty)
    void presentOverlay();             // v0.24.0: home the overlay to the bar's display + push
    void pushOverlay(bool force);

    AudioEngine* engine_ = nullptr;
    CaptionClient client_;
    CaptionOverlay overlay_;
    CaptionSettingsData settings_;
    meterbar::CaptionDisplay display_;

    // runtime state (mirrors the helper's STATUS reports)
    juce::String helperState_ { "stopped" };
    juce::String helperModel_, helperDevice_, helperCompute_;
    juce::String detectedLanguage_;
    int asrLagMs_ = 0;
    bool setupNeeded_ = false;
    juce::String lastError_;
    int64_t readyMs_ = 0;

    // edge tracking for poll()
    bool lastPaused_ = false;
    uint32_t lastEpoch_ = 0;
    juce::String lastSource_;
    bool helperWanted_ = false;   // mode on + source usable
    bool stoppingIntentionally_ = false;
    int restartCount_ = 0;
    int64_t firstRestartMs_ = 0;

    // overlay push throttling / change detection
    int64_t lastOverlayPushMs_ = 0;
    juce::String lastPushedKey_;
    JUCE_DECLARE_NON_COPYABLE(CaptionController)
};
