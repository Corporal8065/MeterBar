#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// TEXT POP (v0.26.1 — Jackson's picture, 2026-11-02). Every piece
// of text the bar draws over its transparent / visual background
// gets a HARD offset duplicate behind it in the contrasting
// colour — white behind dark text, black behind light text —
// exactly the effect in the reference picture Jackson pasted
// (bold "TEST" letters with a chunky white copy shifted
// down-right). It is also Jackson's standing transparent-text
// rule: offset backing text so the words pop and stay readable.
//
// Why v0.26.0's version read as "not implemented" in his field
// test (diagnosed from the shipped code): the duplicate sat 1 px
// away — inside the glyph strokes of the bar's bold text, where
// it reads as slight bolding, not a shadow; it was drawn at 85%
// alpha, softening it further; its colour was always black, the
// wrong contrast direction for dark text; and it only covered
// ModuleViews text. So this version: the offset scales with the
// font (about a fifth of the glyph height, floored at 2 px so it
// always clears the stroke), the backing is FULL strength, the
// colour contrasts the text by luminance, and the draw is
// unconditional — the bar's backgrounds are translucent or
// visual in every configuration Jackson runs, and over an opaque
// background the duplicate simply reads as the pictured hard
// shadow. One helper, used by the module views, the caption
// strip, the toolbar buttons and the toolbar labels alike.
namespace textpop {

// Light text gets the black copy; dark text gets the white copy
// (the picture's case: black letters, white backing).
inline juce::Colour backingFor(juce::Colour text) {
    return text.getPerceivedBrightness() > 0.55f ? juce::Colours::black
                                                 : juce::Colours::white;
}

// Down-right offset for a given font height: ~20% of the glyph,
// clamped so small readouts still clear their strokes and large
// captions don't fly away from their letters.
inline float offsetFor(float fontHeight) {
    return juce::jlimit(2.f, 7.f, fontHeight * 0.20f);
}

// drawText with the pop. The caller sets the font first, exactly
// as with Graphics::drawText — the offset derives from it. The
// backing keeps the text colour's own alpha, so a dim readout
// gets a proportionally dim backing.
inline void draw(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r,
                 juce::Justification j, juce::Colour colour) {
    const float off = offsetFor(g.getCurrentFont().getHeight());
    g.setColour(backingFor(colour).withAlpha(colour.getFloatAlpha()));
    g.drawText(text, r.translated(off, off), j);
    g.setColour(colour);
    g.drawText(text, r, j);
}
inline void draw(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                 juce::Justification j, juce::Colour colour) {
    draw(g, text, r.toFloat(), j, colour);
}

// drawFittedText with the pop (toolbar buttons / labels, whose
// stock LookAndFeel draws fitted text).
inline void drawFitted(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                       juce::Justification j, int maxLines, float minScale, juce::Colour colour) {
    const int off = juce::roundToInt(offsetFor(g.getCurrentFont().getHeight()));
    g.setColour(backingFor(colour).withAlpha(colour.getFloatAlpha()));
    g.drawFittedText(text, r.translated(off, off), j, maxLines, minScale);
    g.setColour(colour);
    g.drawFittedText(text, r, j, maxLines, minScale);
}

} // namespace textpop
