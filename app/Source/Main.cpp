#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "CrashHandler.h" // v0.28.3: hard-crash record (family crash-log contract)
#include "MeterBarVersion.h" // v0.19: generated — single-source version (root CMakeLists project VERSION)
#include <meterbar/WindowGeom.hpp> // v0.20.1: startup position clamp (family-wide off-screen guard)
// v0.28.16: DOCK-SAFE BOUNDS CONSTRAINER — the round-7 fix (see
// meterbar/WindowGeom.hpp's DOCK-SAFE CONSTRAINER section for the
// full diagnosis). JUCE's default constrainer clamps every position
// change against the union of the displays' USER areas; the bar's
// own AppBar reservation shrinks that area, so a fresh dock was
// rewritten one bar-height inward, flush against its own strip
// (the first-reposition ping-pong). This constrainer keeps the same
// minimum-on-screen amounts as JUCE's default (0x10000/16/24/16 —
// ResizableWindow::initialise's values) but runs the on-screen
// rules against the displays' TOTAL areas, which a reservation can
// never shrink. Size limits and aspect ratio still run through
// JUCE's own checkBounds first, with the limits widened past any
// real desktop so the base pass's user-area rules cannot fire
// there; the live clamp is the pure core's (suite-pinned).
class DockSafeConstrainer : public juce::ComponentBoundsConstrainer {
public:
    DockSafeConstrainer() { setMinimumOnscreenAmounts(kAmounts.top, kAmounts.left, kAmounts.bottom, kAmounts.right); }
    void checkBounds(juce::Rectangle<int>& bounds, const juce::Rectangle<int>& oldBounds,
                     const juce::Rectangle<int>& limits, bool isStretchingTop, bool isStretchingLeft,
                     bool isStretchingBottom, bool isStretchingRight) override {
        // Pass 1: JUCE's size limits + aspect ratio (inert for this
        // window today — no setResizeLimits call exists in the app —
        // but preserved so a future one keeps working).
        juce::ComponentBoundsConstrainer::checkBounds(bounds, oldBounds, limits.expanded(1 << 20),
                                                      isStretchingTop, isStretchingLeft,
                                                      isStretchingBottom, isStretchingRight);
        // Pass 2: the on-screen rules, against TOTAL-area limits.
        std::vector<meterbar::IntRect> totals;
        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
            totals.push_back({ d.totalArea.getX(), d.totalArea.getY(), d.totalArea.getWidth(), d.totalArea.getHeight() });
        const auto totalLimits = meterbar::totalAreaUnionRect(totals);
        if (totalLimits.w <= 0 || totalLimits.h <= 0) return; // no displays enumerated: leave JUCE's result
        const auto clamped = meterbar::onScreenClampedRect(
            { bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() },
            totalLimits,
            { getMinimumWhenOffTheTop(), getMinimumWhenOffTheLeft(),
              getMinimumWhenOffTheBottom(), getMinimumWhenOffTheRight() },
            isStretchingTop, isStretchingLeft, isStretchingBottom, isStretchingRight);
        bounds = juce::Rectangle<int>(clamped.x, clamped.y, clamped.w, clamped.h);
    }
private:
    // JUCE's argument order is (top, left, bottom, right); these are
    // the amounts ResizableWindow::initialise installs on the
    // default constrainer this one replaces.
    static constexpr meterbar::OnScreenAmounts kAmounts{ 0x10000, 16, 24, 16 };
};
class MainWindow : public juce::DocumentWindow {
public:
    explicit MainWindow(const juce::String& name) : juce::DocumentWindow(name, juce::Colour(0x00000000), 0) {
        setUsingNativeTitleBar(false);
        setTitleBarHeight(0);
        // v0.13: per-pixel transparent top level. The window background is
        // fully clear and neither the window nor its content is opaque, so
        // wherever the content paints with alpha (the Background Opacity
        // setting) the desktop shows through. This is translucency only —
        // no DWM backdrop blur is attempted (that needs platform-specific
        // acrylic hacks outside JUCE's window model).
        setOpaque(false);
        auto* mc = new MainComponent();
        content = mc;
        setContentOwned(mc, true);
        setResizable(true, false); // border-only resize: the corner grabber overlay sat on top of the Spectrum module and was a suspect in the stuck-cursor field report
        setConstrainer(&dockConstrainer_); // v0.28.16: total-area limits — a docked position can no longer be rewritten into the bar's own reserved strip (see DockSafeConstrainer above)
        {
            // v0.20.1: never restore off-screen — clamp the saved
            // rect into a visible work area (core WindowGeom).
            // v0.28.11: the clamp is now the FLOOR case of the core
            // restore rule — a saved rect that is meaningfully
            // visible (an ordinary floating position, and in
            // particular a DOCKED rect, whose window sits one frame
            // inset outside the monitor edge by design since
            // v0.28.10) is restored verbatim; the full clamp still
            // rescues genuinely lost rects. The bare clamp dragged
            // every restored dock fully inside the user area, one
            // inset off the edge, and the reserve machine judged
            // that gap a no-dock forever (Jackson, 2026-11-12).
            std::vector<meterbar::IntRect> areas;
            for (auto& d : juce::Desktop::getInstance().getDisplays().displays) {
                auto ua = d.userArea;
                areas.push_back({ ua.getX(), ua.getY(), ua.getWidth(), ua.getHeight() });
            }
            auto placed = meterbar::restoreRectToWorkAreas(
                { mc->engine.state.windowX, mc->engine.state.windowY, mc->engine.state.windowW, mc->engine.state.windowH }, areas);
            setBounds(placed.x, placed.y, placed.w, placed.h);
        }
        setAlwaysOnTop(mc->engine.state.pinned);
        setVisible(true);
    }
    // v0.28.16: unhook the member constrainer before it dies — the
    // peer holds a raw pointer to it, and the member is destroyed
    // before the base window (and its peer) is.
    ~MainWindow() override { setConstrainer(nullptr); }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    // v0.28.8: the close gate lives in MainComponent (it owns the
    // baseline + the prompt); systemRequestedQuit routes here.
    void requestClose() { if (content != nullptr) content->requestClose(); }
    // v0.18.1: the reserve machine is EVENT-driven — the window's own
    // moved/resized notifications are its geometry feed (these fire for
    // native drags and border resizes too, via the peer). v0.18 instead
    // polled from the UI timer ~4x/sec; see MainComponent::syncScreenSpace.
    void moved() override { if (content != nullptr) content->notifyWindowGeometryChanged(); }
    void resized() override {
        juce::DocumentWindow::resized();
        if (content != nullptr) content->notifyWindowGeometryChanged();
    }
private:
    DockSafeConstrainer dockConstrainer_; // v0.28.16: owned by the window; setConstrainer only borrows it
    MainComponent* content = nullptr; // owned by the window (setContentOwned) — never delete here
};
class MeterBarApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "MeterBar"; }
    const juce::String getApplicationVersion() override { return meterbar::kVersionString; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
        // v0.28.3: install the hard-crash handler FIRST — before the
        // splash, the window, and any audio source starts — so a
        // crash anywhere later in startup still leaves its record
        // (crash-*.txt + .dmp in Documents\MeterBar). Windows-only;
        // a no-op elsewhere (CrashHandler.h).
        crashhandler::install(meterbar::kVersionString);
        // JUCE free-license attribution splash (v0.15 distribution): small,
        // brief, non-intrusive — shows "Made with JUCE" for ~1.5 s at
        // startup, click dismisses it sooner. The free JUCE license asks
        // for this on closed-source free distribution; the About box
        // carries the same line permanently.
        juce::Image splashImg(juce::Image::RGB, 440, 160, true);
        {
            juce::Graphics g(splashImg);
            g.fillAll(juce::Colour(0xff17141f));
            g.setColour(juce::Colour(0xffe04c89)); g.setFont(juce::Font(30.f, juce::Font::bold));
            g.drawText("MeterBar", splashImg.getBounds().withTrimmedBottom(52), juce::Justification::centred);
            g.setColour(juce::Colour(0xfff6eff3)); g.setFont(15.f);
            g.drawText("Made with JUCE", juce::Rectangle<int>(0, 92, 440, 22), juce::Justification::centred);
            g.setFont(12.f); g.setColour(juce::Colour(0xffa79fc0));
            g.drawText(juce::String("v") + meterbar::kVersionString, juce::Rectangle<int>(0, 120, 440, 18), juce::Justification::centred);
        }
        // SplashScreen deletes ITSELF after the delay (and is
        // DeletedAtShutdown if still up at quit) — never own it in a
        // smart pointer; the raw pointer follows JUCE's own example.
        splash = new juce::SplashScreen("MeterBar", splashImg, true);
        splash->deleteAfterDelay(juce::RelativeTime::milliseconds(1500), true);
        mainWindow = std::make_unique<MainWindow>("MeterBar");
    }
    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override {
        // v0.28.8: through the close gate, never a bare quit — the
        // gate quits itself once any save is settled (SessionBar parity).
        if (mainWindow != nullptr) mainWindow->requestClose();
        else quit();
    }
private:
    std::unique_ptr<MainWindow> mainWindow;
    juce::SplashScreen* splash = nullptr; // self-deleting (see initialise) — never delete via this pointer
};
START_JUCE_APPLICATION(MeterBarApp)
