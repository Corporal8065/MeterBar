#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "MeterBarVersion.h" // v0.19: generated — single-source version (root CMakeLists project VERSION)
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
        setBounds(mc->engine.state.windowX, mc->engine.state.windowY, mc->engine.state.windowW, mc->engine.state.windowH);
        setAlwaysOnTop(mc->engine.state.pinned);
        setVisible(true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
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
    MainComponent* content = nullptr; // owned by the window (setContentOwned) — never delete here
};
class MeterBarApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "MeterBar"; }
    const juce::String getApplicationVersion() override { return meterbar::kVersionString; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
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
    void systemRequestedQuit() override { quit(); }
private:
    std::unique_ptr<MainWindow> mainWindow;
    juce::SplashScreen* splash = nullptr; // self-deleting (see initialise) — never delete via this pointer
};
START_JUCE_APPLICATION(MeterBarApp)
