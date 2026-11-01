#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

// LIVE CAPTIONS — OVERLAY (v0.23.0, spec sections 1.3–1.5).
//
// A separate borderless window owned by MeterBar: always-on-top,
// non-activating, click-through outside move mode. It is NOT an
// AppBar, never registers screen space, and does not join the
// family link stack. Content: an optional previous (dimmed) and
// current (bright) caption block at 22 px Segoe UI Semibold on
// black ~78% alpha, plus an 11 px status strip. The text planning
// (two-line rule) is the pure model's job — see
// meterbar::planCaptionLines; this class only measures and paints.
//
// Known limitation (spec 1.3): exclusive-fullscreen applications
// can cover the overlay; borderless-fullscreen video is fine.
class CaptionOverlay {
public:
    CaptionOverlay();
    ~CaptionOverlay();

    // provider for MeterBar's own bounds (default placement rule)
    std::function<juce::Rectangle<int>()> barBoundsProvider;
    // fired when a move ends (controller persists the bounds)
    std::function<void()> onBoundsChanged;

    // Content modes: captions (blocks + status), a status-only pill
    // (loading / holds / errors), or hidden.
    void showCaptions(const juce::String& previousText,
                      const juce::String& currentText, bool currentFinal,
                      const juce::String& statusLine);
    void showPill(const juce::String& text);
    void hide();

    void setAlwaysOnTopFlag(bool on);
    void restoreBounds(int x, int y, int w, bool hasPosition);
    void resetPosition();
    juce::Rectangle<int> bounds() const;
    bool hasPosition() const { return hasPosition_; }
    bool isVisible() const;

    // v0.24.0: bring the overlay onto the display MeterBar is on.
    // A saved placement already mainly on that display is kept (the
    // window is only raised); anything else relocates to the home
    // rect the pure core computes (meterbar::captionHomeRect).
    // force = recompute the home rect ignoring the saved placement
    // (Reset Caption Position). Never yanks a window in move mode.
    void homeToBarDisplay(bool force);

    void enterMoveMode();
    void exitMoveMode();
    bool isMoveMode() const;

private:
    class Content;
    class Window;
    std::unique_ptr<Window> window_;
    Content* content_ = nullptr;
    bool hasPosition_ = false;
    bool alwaysOnTop_ = true;
    juce::Rectangle<int> defaultBounds(int height) const;
    void applyNativeStyles();
    JUCE_DECLARE_NON_COPYABLE(CaptionOverlay)
};
