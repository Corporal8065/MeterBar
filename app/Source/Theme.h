#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <meterbar/ColorMap.hpp>
#include <meterbar/State.hpp>
// One palette everywhere (v0.12 UI pass, Jackson's ask): deep dark chrome,
// saturated accents from the icon/logo family (teal/green/yellow/orange/pink),
// level language = paletteGradient (quiet teal -> hot pink) in every module.
// moduleAccents is indexed by ModuleView::Kind:
//   0 Spectrum (pink)  1 Spectrogram (orange)  2 Waveform (teal)
//   3 Loudness (green) 4 Stereometer (violet)  5 Oscilloscope (yellow)
struct Theme {
    juce::Colour bg, panel, deepest, accent, accent2, text, dim, good, warn, bad;
    juce::Colour border, grid;
    juce::Colour moduleAccents[6];
    juce::String name;
    juce::Colour moduleAccent(int kindIndex) const {
        return moduleAccents[juce::jlimit(0, 5, kindIndex)];
    }
};
inline Theme themeByName(const juce::String& n) {
    if (n == "Midnight Pink") return { juce::Colour(0xff1e1b24), juce::Colour(0xff26232e), juce::Colour(0xff161519),
        juce::Colour(0xffe04c89), juce::Colour(0xff40aac0), juce::Colour(0xfff2e8ee), juce::Colour(0xff9a8fa0),
        juce::Colour(0xff40aac0), juce::Colour(0xfffad927), juce::Colour(0xfffe8a0a),
        juce::Colour(0xff3a3547), juce::Colour(0xff45405a),
        { juce::Colour(0xffe04c89), juce::Colour(0xfffe8a0a), juce::Colour(0xff40aac0),
          juce::Colour(0xff3ddc84), juce::Colour(0xff9a6bff), juce::Colour(0xfffad927) }, "Midnight Pink" };
    if (n == "Deep Teal") return { juce::Colour(0xff0d1f22), juce::Colour(0xff14333a), juce::Colour(0xff081417),
        juce::Colour(0xff40aac0), juce::Colour(0xffe04c89), juce::Colour(0xffe8f4f6), juce::Colour(0xff7fa3ab),
        juce::Colour(0xff40c060), juce::Colour(0xfffad927), juce::Colour(0xfffe5a3c),
        juce::Colour(0xff1e4a52), juce::Colour(0xff2a5a64),
        { juce::Colour(0xff40aac0), juce::Colour(0xfffe8a0a), juce::Colour(0xff35d0ba),
          juce::Colour(0xff3ddc84), juce::Colour(0xff7fb2ff), juce::Colour(0xfffad927) }, "Deep Teal" };
    if (n == "Anime") return { juce::Colour(0xfff6e3ee), juce::Colour(0xfffdf3f8), juce::Colour(0xfff3d9e8),
        juce::Colour(0xffe77fb1), juce::Colour(0xff8fb8e8), juce::Colour(0xff5a4356), juce::Colour(0xffa58ba0),
        juce::Colour(0xff3fae6a), juce::Colour(0xffd9a92c), juce::Colour(0xffe5533c),
        juce::Colour(0xffe3c8dd), juce::Colour(0xffedd8ea),
        { juce::Colour(0xffe77fb1), juce::Colour(0xfff0a35e), juce::Colour(0xff7fc8bc),
          juce::Colour(0xff7fce8f), juce::Colour(0xffb49ae0), juce::Colour(0xfff2cf5e) }, "Anime" };
    if (n == "Daylight") return { juce::Colour(0xffe9e9ee), juce::Colour(0xffffffff), juce::Colour(0xffd8d8e0),
        juce::Colour(0xff3050c0), juce::Colour(0xffc22a6d), juce::Colour(0xff181824), juce::Colour(0xff606078),
        juce::Colour(0xff1a9e4b), juce::Colour(0xffc9a227), juce::Colour(0xffd23c28),
        juce::Colour(0xffb9b9c6), juce::Colour(0xffc6c6d2),
        { juce::Colour(0xffc22a6d), juce::Colour(0xffd96a06), juce::Colour(0xff1d7f96),
          juce::Colour(0xff1a9e4b), juce::Colour(0xff6a4fd0), juce::Colour(0xffb98a04) }, "Daylight" };
    // "Midnight Kids" — the v0.12 default: deep violet-black, warm text,
    // pink brand accent + teal secondary, fully saturated module accents.
    return { juce::Colour(0xff17141f), juce::Colour(0xff231f31), juce::Colour(0xff0e0c15),
        juce::Colour(0xffe04c89), juce::Colour(0xff40aac0), juce::Colour(0xfff6eff3), juce::Colour(0xffa79fc0),
        juce::Colour(0xff2fd68a), juce::Colour(0xfffad927), juce::Colour(0xfffe5a3c),
        juce::Colour(0xff3d3654), juce::Colour(0xff494164),
        { juce::Colour(0xffe04c89), juce::Colour(0xfffe8a0a), juce::Colour(0xff40aac0),
          juce::Colour(0xff3ddc84), juce::Colour(0xff9a6bff), juce::Colour(0xfffad927) }, "Midnight Kids" };
}
// Background styles (v0.16): the window background and module panels are
// painted through this one helper, honouring the Background Opacity
// setting exactly as the old flat fill did (style paints at alpha=op).
// CleanDark: near-black blue-grey vertical gradient (Default recipe).
// MidnightKids: colourful gradient in the theme family's violet/pink/teal.
// Flat: the pre-v0.16 flat theme.bg fill, unchanged. Pastel: the Anime
// theme's soft pale-pink gradient (family of the mascot art background).
inline void fillStyledBackground(juce::Graphics& g, juce::Rectangle<float> r,
                                 meterbar::BackgroundStyle style, const Theme& th, float alpha) {
    alpha = juce::jlimit(0.15f, 1.0f, alpha);
    juce::Colour top, bottom;
    switch (style) {
    case meterbar::BackgroundStyle::CleanDark:
        top = juce::Colour(0xff272c36); bottom = juce::Colour(0xff101216); break;
    case meterbar::BackgroundStyle::MidnightKids:
        top = juce::Colour(0xff2b1a4d); bottom = juce::Colour(0xff150f24); break;
    case meterbar::BackgroundStyle::Pastel:
        top = juce::Colour(0xfffdeef5); bottom = juce::Colour(0xfff6d3e4); break;
    default: // Flat
        g.fillAll(th.bg.withAlpha(alpha)); return;
    }
    juce::ColourGradient grad(top.withAlpha(alpha), r.getX(), r.getY(),
                              bottom.withAlpha(alpha), r.getX(), r.getBottom(), false);
    if (style == meterbar::BackgroundStyle::MidnightKids) {
        grad.addColour(0.55, juce::Colour(0xff3d1d43).withAlpha(alpha)); // magenta-plum mid-stop
    }
    g.setGradientFill(grad);
    g.fillRect(r);
}
inline juce::Colour mapColour(const meterbar::ColorMap& cm, float t) {
    auto c = cm.at(t); return juce::Colour(c.r, c.g, c.b);
}
inline juce::Colour paletteGrad(float t) {
    auto c = meterbar::paletteGradient(t); return juce::Colour(c.r, c.g, c.b);
}
