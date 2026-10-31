#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <meterbar/State.hpp>
#include <functional>
#include "Theme.h" // v0.12: panel headers wear their module's accent colour
#include "WindowsLoopback.h" // v0.13: the output picker is built from real WASAPI endpoints (name + ID)
// v0.19: the dark-title-bar call below is Windows-only. The guard tests
// the COMPILER's own predefined _WIN32 (MSVC and MinGW both predefine
// it) — never JUCE_WINDOWS: the v0.14 lesson is that a macro a header
// may or may not have defined is how Windows code silently compiles
// out. dwmapi is already linked by the app target (ScreenSpace, v0.18).
#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <dwmapi.h>
 #ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
  #define DWMWA_USE_IMMERSIVE_DARK_MODE 20
 #endif
#endif
// v0.19 SETTINGS PALETTE — Settings is DECOUPLED from the bar theme.
// Before this, SettingsContent painted nothing and coloured nothing:
// the window inherited theme.panel via MainComponent::applyTheme, which
// is near-white (0xfffdf3f8) in the Anime theme and pure white
// (0xffffffff) in Daylight — Jackson: "blinding when I hit Settings" —
// and its controls fell back to stock JUCE LookAndFeel_V4 whites/slates
// in every theme. Settings now always uses this one fixed soft
// pink-dark palette, identical in all five bar themes. (Plan:
// docs/V019_PLAN.md section 1.2.)
namespace settingsPalette {
    inline juce::Colour base()       { return juce::Colour(0xff2a1e2e); } // window/content background
    inline juce::Colour surface()    { return juce::Colour(0xff3a2438); } // combos, buttons, slider text boxes
    inline juce::Colour well()       { return juce::Colour(0xff241722); } // slider tracks, tick boxes, scrollbar track
    inline juce::Colour border()     { return juce::Colour(0xff57304d); } // outlines, dividers
    inline juce::Colour text()       { return juce::Colour(0xfff6e3ee); } // body text (~13:1 on base)
    inline juce::Colour dimText()    { return juce::Colour(0xffc9a3bb); } // notes (~7:1 on base)
    inline juce::Colour accent()     { return juce::Colour(0xffe04c89); } // brand pink: thumbs, ticks, highlights
    inline juce::Colour softPink()   { return juce::Colour(0xffe77fb1); } // combo arrows, secondary accents
    inline void applyDarkTitleBar(juce::Component* c) {
#if defined(_WIN32)
        if (c == nullptr) return;
        if (auto* peer = c->getPeer()) {
            if (auto* hwnd = (HWND) peer->getNativeHandle()) {
                BOOL dark = TRUE;
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            }
        }
#else
        (void) c;
#endif
    }
} // namespace settingsPalette
// Scoped LookAndFeel for the Settings window ONLY (the main bar keeps
// stock JUCE behaviour — this is set on the Settings content + its
// viewport, never on any bar component). Seeds every colour the stock
// V4 look would otherwise take from its own white/slate defaults;
// the builder helpers in SettingsContent ALSO set explicit colours, so
// the palette holds even for a control that misses the L&F chain.
class SettingsLookAndFeel : public juce::LookAndFeel_V4 {
public:
    SettingsLookAndFeel() {
        using namespace settingsPalette;
        setColour(juce::Label::textColourId, text());
        setColour(juce::ComboBox::backgroundColourId, surface());
        setColour(juce::ComboBox::textColourId, text());
        setColour(juce::ComboBox::arrowColourId, softPink());
        setColour(juce::ComboBox::outlineColourId, border());
        setColour(juce::ComboBox::buttonColourId, surface());
        setColour(juce::PopupMenu::backgroundColourId, base());
        setColour(juce::PopupMenu::textColourId, text());
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent());
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(0xffffffff));
        setColour(juce::PopupMenu::headerTextColourId, dimText());
        setColour(juce::Slider::backgroundColourId, well());
        setColour(juce::Slider::trackColourId, well());
        setColour(juce::Slider::thumbColourId, accent());
        setColour(juce::Slider::textBoxTextColourId, text());
        setColour(juce::Slider::textBoxBackgroundColourId, surface());
        setColour(juce::Slider::textBoxOutlineColourId, border());
        setColour(juce::ToggleButton::textColourId, text());
        setColour(juce::ToggleButton::tickColourId, accent());
        setColour(juce::ToggleButton::tickDisabledColourId, dimText());
        setColour(juce::TextButton::buttonColourId, surface());
        setColour(juce::TextButton::buttonOnColourId, accent());
        setColour(juce::TextButton::textColourOffId, text());
        setColour(juce::TextButton::textColourOnId, juce::Colour(0xffffffff));
        setColour(juce::ScrollBar::backgroundColourId, well());
        setColour(juce::ScrollBar::thumbColourId, border());
        setColour(juce::ScrollBar::trackColourId, well());
    }
};
// Big Settings window content: one panel per module + Audio + Performance.
// Controls write straight into the shared PresetState and fire onChanged.
class SettingsContent : public juce::Component {
public:
    meterbar::PresetState* st = nullptr;
    juce::AudioDeviceManager* deviceManager = nullptr; // v0.8 device pickers
    std::function<void()> onChanged, onSourceChanged, onFpsChanged;
    std::function<void()> onArrange, onRestore; // v0.18: Window panel buttons (wired by MainComponent)
    SettingsContent() { setSize(760, 640); }
    // v0.19: Settings paints its OWN background in the fixed palette —
    // this is the line that ends the theme.panel inheritance (near-white
    // in Anime, pure white in Daylight) for the content area.
    void paint(juce::Graphics& g) override { g.fillAll(settingsPalette::base()); }
    void build() {
        removeAllChildren(); owned.clear(); y_ = 8;
        if (!st) return;
        th_ = themeByName(juce::String(st->themeName));
        nextKind_ = 1; panel("Spectrogram", &st->spectrogramEnabled, [&]{
            combo("Orientation", {"Horizontal","Vertical"}, (int)st->spectrogramOrientation, [&](int v){ st->spectrogramOrientation = (meterbar::Orientation)v; });
            slider("Fade", 0.0, 1.0, st->fade, [&](double v){ st->fade = (float)v; });
            toggle("Loop (disabled — future)", st->loopEnabled, [](bool){}, true);
            slider("Linked Speed (0-10)", 0.0, 10.0, st->linkedSpeed, [&](double v){ st->linkedSpeed = (float)v; });
            note("Sections: Main / FFT (size+scale under Spectrum, linked) / Overlay (style under Spectrum).");
            note("The spectrogram colours each cell by its level through the selected Color Map (Menu -> Color Map): quiet = the map's cool end, loudest = red. It has its own sensitivity, independent of the other meters.");
        });
        nextKind_ = 2; panel("Waveform", &st->waveformEnabled, [&]{
            combo("Channel 1", {"Left","Right","Mid","Side"}, (int)st->waveCh1, [&](int v){ st->waveCh1 = (meterbar::ChanSel)v; });
            combo("Channel 2", {"Off","Left","Right","Mid","Side"},
                  st->waveCh2 == meterbar::ChanSel::Off ? 0 : (int)st->waveCh2 + 1, [&](int v){
                      // items: 0=Off,1=L,2=R,3=M,4=S mapped onto ChanSel with Off sentinel handled below
                      st->waveCh2 = v == 0 ? meterbar::ChanSel::Off : (meterbar::ChanSel)(v - 1); });
            combo("History", {"Off","Fast","Slow"}, (int)st->waveHistory, [&](int v){ st->waveHistory = (meterbar::HistoryMode)v; });
            slider("Linked Speed (0-10)", 0.0, 10.0, st->linkedSpeed, [&](double v){ st->linkedSpeed = (float)v; });
        });
        nextKind_ = 3; panel("Loudness", &st->loudnessEnabled, [&]{
            combo("Mode", {"LUFS: Momentary","LUFS: Short Term","RMS: Fast","RMS: Slow","RMS +3: Fast","RMS +3: Slow"},
                  (int)st->loudnessMode, [&](int v){ st->loudnessMode = (meterbar::LoudnessMode)v; });
            toggle("Streaming target lines (Spotify / Apple)", st->loudnessShowTargets, [&](bool b){ st->loudnessShowTargets = b; });
            note("Target lines compare Integrated LUFS against Spotify/YouTube -14 and Apple Music -16, beside the bars. Off by default in the Default preset.");
        });
        nextKind_ = 4; panel("Stereometer", &st->stereometerEnabled, [&]{
            note("v0.15: display is a stereo VECTORSCOPE (L vs R, Ozone-style) — mono draws the diagonal, width spreads off it.");
            combo("Color", {"Single (S)","RGB","Multi-band (M band)"}, (int)st->stereoColor, [&](int v){ st->stereoColor = (meterbar::StereoColor)v; });
            toggle("Point Size Auto", st->stereoPointAuto, [&](bool b){ st->stereoPointAuto = b; });
            slider("Point Size (1-20)", 1.0, 20.0, st->stereoPointSize, [&](double v){ st->stereoPointSize = (float)v; });
            note("Reference: load/overlay a reference track is a later phase (panel reserved).");
        });
        nextKind_ = 5; panel("Oscilloscope", &st->oscilloscopeEnabled, [&]{
            combo("Trigger Mode", {"Pitch","Free"}, (int)st->scopeTrigger, [&](int v){ st->scopeTrigger = (meterbar::ScopeTrigger)v; });
            combo("Display", {"Multi","Single"}, st->scopeDisplay == meterbar::ScopeDisplay::Multi ? 0 : 1,
                  [&](int v){ st->scopeDisplay = v == 0 ? meterbar::ScopeDisplay::Multi : meterbar::ScopeDisplay::Single; });
            slider("Window ms (10-200) - wider = more waveform", 10.0, 200.0, st->scopeWindowMs, [&](double v){ st->scopeWindowMs = (float)v; });
        });
        nextKind_ = 0; panel("Spectrum", &st->spectrumEnabled, [&]{
            combo("Style", {"FFT","Color","Bars","Both"}, (int)st->spectrumStyle, [&](int v){ st->spectrumStyle = (meterbar::PresetState::SpectrumStyle)v; });
            slider("Smoothing % (0-100)", 0.0, 100.0, st->spectrumSmoothingPct, [&](double v){ st->spectrumSmoothingPct = (float)v; });
            slider("Tilt - Display Slope dB/oct (0-6)", 0.0, 6.0, st->spectrumDisplayTiltDb, [&](double v){ st->spectrumDisplayTiltDb = (float)v; });
            slider("Tilt dB (analyser, -9..+9)", -9.0, 9.0, st->spectrumTiltDb, [&](double v){ st->spectrumTiltDb = (float)v; });
            note("Tilt (Display Slope) is the Pro-Q style one: it flattens how the spectrum DRAWS (Default preset: 1.15) and changes nothing else — the analyser tilt below is the older shared one.");
            combo("FFT Size", {"1024","2048","4096","8192","16384"},
                  st->fftSize == 1024 ? 0 : st->fftSize == 2048 ? 1 : st->fftSize == 4096 ? 2 : st->fftSize == 8192 ? 3 : 4,
                  [&](int v){ st->fftSize = 1024 << v; });
            combo("FFT Scale (Linked)", {"Mel","Log","Linear"},
                  st->fftScale == meterbar::FreqScale::Mel ? 0 : st->fftScale == meterbar::FreqScale::Log ? 1 : 2,
                  [&](int v){ st->fftScale = v == 0 ? meterbar::FreqScale::Mel : v == 1 ? meterbar::FreqScale::Log : meterbar::FreqScale::Linear; });
            combo("Colour By", {"Level (quiet cool -> loud RED)","Frequency (low -> high gradient)","Blend (level + frequency)","Color Map (classic)","Chroma (colour = pitch class)"},
                  (int)st->spectrumColorMode, [&](int v){ st->spectrumColorMode = (meterbar::SpectrumColorMode)v; });
            toggle("1/6-octave smoothing (display only)", st->spectrumSixthOctave, [&](bool b){ st->spectrumSixthOctave = b; });
            toggle("Show Reference section", st->spectrumShowReference, [&](bool b){ st->spectrumShowReference = b; });
            toggle("Show Target section", st->spectrumShowTarget, [&](bool b){ st->spectrumShowTarget = b; });
            note("Sections: Main / Audio / Reference / Target — Reference & Target toggles above.");
        });
        panel("Appearance", nullptr, [&]{
            combo("Background Style", {"Clean Dark","Midnight Kids","Flat","Pastel"},
                  (int)st->backgroundStyle, [&](int v){ st->backgroundStyle = (meterbar::BackgroundStyle)v; });
            slider("Background Opacity % (15-100)", 15.0, 100.0, st->backgroundOpacity * 100.0,
                   [&](double v){ st->backgroundOpacity = (float)(v / 100.0); });
            note("Below 100 the desktop shows through the module backgrounds (traces and text stay solid). Translucency only — no background blur in this build.");
            toggle("Beat Pulse (subtle glow on beats)", st->beatPulseEnabled, [&](bool b){ st->beatPulseEnabled = b; });
            toggle("Visualizer background (particles + rainbow bursts, v0.20.0)", st->visualizerEnabled, [&](bool b){ st->visualizerEnabled = b; });
            slider("Visualizer base hue (deg)", 0.0, 360.0, st->visualizerBaseHue * 360.0,
                   [&](double v){ st->visualizerBaseHue = (float)(v / 360.0); });
            slider("Visualizer saturation %", 0.0, 100.0, st->visualizerSaturation * 100.0,
                   [&](double v){ st->visualizerSaturation = (float)(v / 100.0); });
            slider("Visualizer palette spread %", 0.0, 100.0, st->visualizerSpread * 100.0,
                   [&](double v){ st->visualizerSpread = (float)(v / 100.0); });
            slider("Visualizer palette cycle (s, 0 = static)", 0.0, 60.0, st->visualizerCycleSec,
                   [&](double v){ st->visualizerCycleSec = (float)v; });
            note("Visualizer: black background, white particles converging to the centre, frequency bands bursting rainbow strokes that dissolve into dots, over a blurred glow layer. Particle count is hard-capped (600) for gaming PCs. Needs a visual field test for tuning — see BUILD_LOG.");
            toggle("Cute click sounds (Anime theme only)", st->animeClickSounds, [&](bool b){ st->animeClickSounds = b; });
            note("Click sounds only ever play in the Anime theme; switching themes silences them completely.");
        });
        panel("Window", nullptr, [&]{
            toggle("Reserve screen space when docked at a screen edge (Windows)", st->reserveScreenSpace, [&](bool b){ st->reserveScreenSpace = b; });
            note("Windows only: while ON and the bar is docked flush at any edge of a monitor (top, bottom, or a side), the bar registers as an App Bar on that edge — Windows shrinks that monitor's work area, so maximised windows stop at the bar instead of sliding under it. Same control as the toolbar's Reserve button. Moving the bar off its edge releases the space again; dragging it to another edge moves the reservation with it.");
            buttonRow("Arrange windows clear of the bar", "Restore arranged windows");
            note("Arrange slides every other (non-maximised) window on the bar's monitor into the space clear of the bar's docked edge; Restore puts them back where they were. The Debug window's 'Screen space' line shows the current state, including which edge is reserved. On non-Windows builds these do nothing.");
        });
        panel("Audio Source", nullptr, [&]{
            soundSourceCombo();
            slider("Trim dB (-48..+48)", -48.0, 48.0, st->trimDb, [&](double v){ st->trimDb = (float)v; }); // v0.20.3: range doubled (Jackson's ask — a larger input trim)
            combo("Channel Pair", {"Pair 1 (ch 1-2)","Pair 2 (ch 3-4)","Pair 3 (ch 5-6)","Pair 4 (ch 7-8)"}, st->routingPair,
                  [&](int v){ st->routingPair = v; });
            combo("Routing Ch 1", {"1","2","3","4","5","6","7","8"}, st->routingCh1, [&](int v){ st->routingCh1 = v; });
            combo("Routing Ch 2", {"1","2","3","4","5","6","7","8"}, st->routingCh2, [&](int v){ st->routingCh2 = v; });
            note("ONE picker = the program's ears. Input device: meters hear that mic. Output device: meters hear whatever is PLAYING through those speakers/headphones (loopback). Menu > Audio Source has the same list. Loopback locks onto the exact endpoint picked — the Debug window shows 'Loopback wanted' vs 'Loopback device' if a source ever looks wrong.");
        });
        panel("Performance", nullptr, [&]{
            toggle("VSync (default on)", st->vsync, [&](bool b){ st->vsync = b; });
            combo("FPS Limit", {"15","30","60","120"}, st->fpsLimit == 15 ? 0 : st->fpsLimit == 30 ? 1 : st->fpsLimit == 60 ? 2 : 3,
                  [&](int v){ st->fpsLimit = v == 0 ? 15 : v == 1 ? 30 : v == 2 ? 60 : 120; if (onFpsChanged) onFpsChanged(); });
        });
        setSize(760, y_ + 8);
    }
    void resized() override {}
private:
    int y_ = 8;
    Theme th_; int nextKind_ = -1; // v0.12: header accent for the panel being built
    std::vector<std::unique_ptr<juce::Component>> owned;
    template <typename T> T* add(T* c) { owned.emplace_back(c); addAndMakeVisible(c); return c; }
    void changed() { if (onChanged) onChanged(); }
    // v0.19: explicit palette colours for every control the helpers
    // build (the SettingsLookAndFeel seeds the same palette; setting
    // both means no control can fall back to stock JUCE whites).
    void applyComboColours(juce::ComboBox* c) {
        c->setColour(juce::ComboBox::backgroundColourId, settingsPalette::surface());
        c->setColour(juce::ComboBox::textColourId, settingsPalette::text());
        c->setColour(juce::ComboBox::arrowColourId, settingsPalette::softPink());
        c->setColour(juce::ComboBox::outlineColourId, settingsPalette::border());
        c->setColour(juce::ComboBox::buttonColourId, settingsPalette::surface());
    }
    void applyLabelColour(juce::Label* l) { l->setColour(juce::Label::textColourId, settingsPalette::text()); }
    void applyToggleColours(juce::ToggleButton* t) {
        t->setColour(juce::ToggleButton::textColourId, settingsPalette::text());
        t->setColour(juce::ToggleButton::tickColourId, settingsPalette::accent());
        t->setColour(juce::ToggleButton::tickDisabledColourId, settingsPalette::dimText());
    }
    void panel(const juce::String& title, bool* enabledFlag, std::function<void()> body) {
        auto* hdr = add(new juce::Label({}, title)); hdr->setFont(juce::Font(15.f, juce::Font::bold));
        hdr->setColour(juce::Label::textColourId, nextKind_ >= 0 ? th_.moduleAccent(nextKind_) : th_.accent);
        nextKind_ = -1; // consumed (set by build() before each module panel)
        hdr->setBounds(12, y_, 300, 22); y_ += 24;
        if (enabledFlag) {
            bool* flag = enabledFlag;
            auto* t = add(new juce::ToggleButton("Enabled"));
            // v0.12 bug fix: this passed `enabledFlag` (a bool*) where a bool
            // was expected — a non-null pointer converts to true, so EVERY
            // module's Enabled box showed ON even when the module was off.
            t->setToggleState(*enabledFlag, juce::dontSendNotification); t->setBounds(560, y_ - 24, 120, 22);
            applyToggleColours(t);
            t->onClick = [this, t, flag]{ *flag = t->getToggleState(); changed(); };
        }
        body(); y_ += 10;
    }
    // v0.9 UNIFIED sound-source picker (Jackson's model): ONE dropdown, grouped
    // Test Tone / INPUT DEVICES / OUTPUT DEVICES. Picking any entry makes it the
    // program's sound source — an input = that mic; an output = loopback of
    // whatever plays through those speakers/headphones.
    void soundSourceCombo() {
        auto* l = add(new juce::Label({}, "Sound Source")); l->setBounds(24, y_, 200, 22); applyLabelColour(l);
        auto* c = add(new juce::ComboBox()); c->setBounds(230, y_, 500, 22); applyComboColours(c);
        struct Entry { int id; int kind; juce::String name; juce::String devId; }; // kind: 0 tone, 1 input, 2 output
        auto entries = std::make_shared<std::vector<Entry>>();
        juce::StringArray inNames, outNames; juce::String curIn;
        if (deviceManager) {
            if (auto* type = deviceManager->getCurrentDeviceTypeObject()) {
                type->scanForDevices();
                inNames = type->getDeviceNames(true); outNames = type->getDeviceNames(false);
            }
            curIn = deviceManager->getAudioDeviceSetup().inputDeviceName;
        }
        // v0.13: on Windows the OUTPUT list is the real WASAPI endpoint list
        // (friendly name + endpoint ID straight from MMDevice). JUCE's
        // device-type names are only the fallback if enumeration fails —
        // a picked name that matches no endpoint was the silent-headphones
        // bug: loopback fell back to the default speakers.
        std::vector<WindowsLoopback::EndpointInfo> eps;
#if JUCE_WINDOWS
        eps = WindowsLoopback::enumerateRenderEndpoints();
#endif
        int nextId = 1, selId = 0;
        auto addEntry = [&](int kind, const juce::String& name, const juce::String& devId, bool selected) {
            int id = nextId++; c->addItem(name, id); entries->push_back({id, kind, name, devId});
            if (selected) selId = id;
        };
        addEntry(0, "Test Tone", {}, st && st->audioSource == "Test Tone");
        c->addSectionHeading("INPUT DEVICES (mic / line-in)");
        if (inNames.isEmpty()) c->addSectionHeading("(no input devices found)");
        for (auto& n : inNames)
            addEntry(1, n, {}, st && st->audioSource == "Input" &&
                     (juce::String(st->audioDeviceName) == n || (st->audioDeviceName.empty() && curIn == n)));
        c->addSectionHeading("OUTPUT DEVICES (speakers - hear what's playing)");
        if (!eps.empty()) {
            for (auto& ep : eps) {
                juce::String nm(ep.name), did(ep.id);
                addEntry(2, nm, did, st && st->audioSource == "System Loopback" &&
                         juce::String(st->outputDeviceName) == nm &&
                         (st->outputDeviceId.empty() || juce::String(st->outputDeviceId) == did));
            }
        } else {
            if (outNames.isEmpty()) c->addSectionHeading("(no output devices found)");
            for (auto& n : outNames)
                addEntry(2, n, {}, st && st->audioSource == "System Loopback" && juce::String(st->outputDeviceName) == n);
        }
        if (selId) c->setSelectedId(selId, juce::dontSendNotification);
        c->onChange = [this, c, entries]{
            if (!st) return;
            int id = c->getSelectedId();
            for (auto& e : *entries) {
                if (e.id != id) continue;
                if (e.kind == 0) { st->audioSource = "Test Tone"; }
                else if (e.kind == 1) {
                    st->audioSource = "Input"; st->audioDeviceName = e.name.toStdString();
                    if (deviceManager) {
                        auto setup = deviceManager->getAudioDeviceSetup();
                        setup.inputDeviceName = e.name; setup.useDefaultInputChannels = true;
                        deviceManager->setAudioDeviceSetup(setup, true);
                    }
                } else { st->audioSource = "System Loopback"; st->outputDeviceName = e.name.toStdString(); st->outputDeviceId = e.devId.toStdString(); }
                if (onSourceChanged) onSourceChanged();
                changed();
                return;
            }
        };
        y_ += 26;
        // v0.15 item 8: the count this picker ACTUALLY saw, on screen — if
        // outputs ever "don't show up", this line + a screenshot settle
        // whether Windows reported the endpoints at all.
#if JUCE_WINDOWS
        note("Loopback check: Windows reports " + juce::String((int)eps.size()) + " active output device(s) in the list above. "
             "If your headphones/speakers are missing, Windows is not reporting them as active (check Sound settings / plug them in) — the app can only offer what Windows reports.");
#else
        note("Loopback check: on this platform the output list comes from the audio driver (WASAPI endpoint enumeration is Windows-only).");
#endif
    }
    void combo(const juce::String& label, std::vector<juce::String> items, int sel, std::function<void(int)> fn) {
        auto* l = add(new juce::Label({}, label)); l->setBounds(24, y_, 200, 22); applyLabelColour(l);
        auto* c = add(new juce::ComboBox()); applyComboColours(c); int id = 1; for (auto& s : items) c->addItem(s, id++);
        c->setSelectedItemIndex(juce::jlimit(0, (int)items.size() - 1, sel), juce::dontSendNotification);
        c->setBounds(230, y_, 260, 22);
        c->onChange = [this, c, fn]{ fn(c->getSelectedItemIndex()); changed(); };
        y_ += 26;
    }
    void slider(const juce::String& label, double mn, double mx, double val, std::function<void(double)> fn) {
        auto* l = add(new juce::Label({}, label)); l->setBounds(24, y_, 200, 22); applyLabelColour(l);
        auto* s = add(new juce::Slider()); s->setRange(mn, mx, 0.01); s->setValue(val, juce::dontSendNotification);
        s->setColour(juce::Slider::backgroundColourId, settingsPalette::well());
        s->setColour(juce::Slider::trackColourId, settingsPalette::well());
        s->setColour(juce::Slider::thumbColourId, settingsPalette::accent());
        s->setColour(juce::Slider::textBoxTextColourId, settingsPalette::text());
        s->setColour(juce::Slider::textBoxBackgroundColourId, settingsPalette::surface());
        s->setColour(juce::Slider::textBoxOutlineColourId, settingsPalette::border());
        s->setBounds(230, y_, 300, 22);
        s->onValueChange = [this, s, fn]{ fn(s->getValue()); changed(); };
        y_ += 26;
    }
    void toggle(const juce::String& label, bool val, std::function<void(bool)> fn, bool disabled = false) {
        auto* t = add(new juce::ToggleButton(label)); t->setToggleState(val, juce::dontSendNotification);
        t->setBounds(24, y_, 400, 22); t->setEnabled(!disabled); applyToggleColours(t);
        t->onClick = [this, t, fn]{ fn(t->getToggleState()); changed(); };
        y_ += 26;
    }
    void buttonRow(const juce::String& labelA, const juce::String& labelB) { // v0.18: two action buttons on one row
        auto* a = add(new juce::TextButton(labelA)); a->setBounds(24, y_, 260, 22);
        a->setColour(juce::TextButton::buttonColourId, settingsPalette::surface());
        a->setColour(juce::TextButton::textColourOffId, settingsPalette::text());
        a->onClick = [this]{ if (onArrange) onArrange(); };
        auto* b = add(new juce::TextButton(labelB)); b->setBounds(296, y_, 260, 22);
        b->setColour(juce::TextButton::buttonColourId, settingsPalette::surface());
        b->setColour(juce::TextButton::textColourOffId, settingsPalette::text());
        b->onClick = [this]{ if (onRestore) onRestore(); };
        y_ += 26;
    }
    void note(const juce::String& text) {
        auto* l = add(new juce::Label({}, text)); l->setFont(11.f); l->setBounds(24, y_, 700, 16);
        l->setColour(juce::Label::textColourId, settingsPalette::dimText()); y_ += 18;
    }
};
class SettingsWindow : public juce::DocumentWindow {
public:
    SettingsContent content;
    SettingsWindow() : juce::DocumentWindow("MeterBar Settings", settingsPalette::base(), juce::DocumentWindow::closeButton) {
        // v0.19: the scoped pink L&F lives as long as the window and is
        // attached to the content + viewport ONLY — never to the bar.
        content.setLookAndFeel(&settingsLnf);
        viewport.setLookAndFeel(&settingsLnf);
        // v0.20.2: no content.build() here — at this point content.st is
        // always null (MainComponent wires it AFTER constructing the
        // window), so the call was a guaranteed no-op that made every
        // startup read as a double build. MainComponent's post-wiring
        // build() is the single first build; rebuildSettings() covers
        // later opens.
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false); // v0.8: full settings scroll into view
        setContentNonOwned(&viewport, true);
        setSize(800, 680); setResizable(true, false); setUsingNativeTitleBar(true);
    }
    ~SettingsWindow() override {
        content.setLookAndFeel(nullptr);
        viewport.setLookAndFeel(nullptr);
    }
    // v0.19: once the native peer exists (window shown), ask Windows
    // for a dark title bar — the native frame otherwise renders white
    // under the default Windows light app mode, in every theme.
    void visibilityChanged() override {
        juce::DocumentWindow::visibilityChanged();
        if (isVisible()) settingsPalette::applyDarkTitleBar(this);
    }
    void closeButtonPressed() override { setVisible(false); }
private:
    SettingsLookAndFeel settingsLnf;
    juce::Viewport viewport;
};
