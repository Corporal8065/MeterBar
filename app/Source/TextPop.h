#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// TEXT POP (v0.27.0 — CORRECTED GEOMETRY, Jackson 2026-11-02
// 19:40 PT, superseding v0.26.1). His words: the doubled text is a
// WHITE copy offset to the TOP-LEFT and drawn IN FRONT, with the
// text's own COLOUR copy at the BOTTOM-RIGHT behind it. So the
// draw order is: (1) the colour copy at the rect translated
// (+off, +off) — bottom-right, behind; (2) a WHITE copy at the
// untranslated rect — top-left relative to the colour copy, in
// front. The visible face of every popped label is therefore
// white, with its own colour peeking out at the bottom-right —
// the chunky hard-offset double from his reference picture, in
// the direction he corrected it to. The offset magnitude is
// v0.26.1's (offsetFor: ~20% of font height, clamped 2–7 px),
// full strength, unconditional.
//
// EMOJI / ICON GLYPHS ARE NEVER POPPED (Jackson, same evening:
// the emoji text "did not need doubling — it was good as it
// was"). This helper is for WORD text only; the icon-glyph
// buttons' LookAndFeel (IconButtonLookAndFeel) deliberately does
// NOT route through here any more — it draws its glyphs plainly,
// once. Audited v0.27.0: the call sites are ModuleViews (module
// labels + readouts), CaptionStrip (caption words) and
// MainComponent's PoppedButtonLookAndFeel / PoppedLabelLookAndFeel
// (word buttons + the "MeterBar" title/logo and toolbar labels) —
// all word text.
namespace textpop {

// Down-right offset for a given font height: ~20% of the glyph,
// clamped so small readouts still clear their strokes and large
// captions don't fly away from their letters. (Unchanged from
// v0.26.1 — Jackson corrected the DIRECTION and the colours, not
// the size of the offset.)
inline float offsetFor(float fontHeight) {
    return juce::jlimit(2.f, 7.f, fontHeight * 0.20f);
}

// drawText with the pop. The caller sets the font first, exactly
// as with Graphics::drawText — the offset derives from it. The
// colour copy keeps the caller's colour and alpha; the white
// front copy keeps the caller's alpha too.
inline void draw(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r,
                 juce::Justification j, juce::Colour colour) {
    const float off = offsetFor(g.getCurrentFont().getHeight());
    g.setColour(colour);
    g.drawText(text, r.translated(off, off), j);   // colour copy, bottom-right, BEHIND
    g.setColour(juce::Colours::white.withAlpha(colour.getFloatAlpha()));
    g.drawText(text, r, j);                        // white copy, top-left, IN FRONT
}
inline void draw(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                 juce::Justification j, juce::Colour colour) {
    draw(g, text, r.toFloat(), j, colour);
}

// drawFittedText with the pop (toolbar word buttons / labels,
// whose stock LookAndFeel draws fitted text). Same order: colour
// copy bottom-right behind, white copy in front.
inline void drawFitted(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                       juce::Justification j, int maxLines, float minScale, juce::Colour colour) {
    const int off = juce::roundToInt(offsetFor(g.getCurrentFont().getHeight()));
    g.setColour(colour);
    g.drawFittedText(text, r.translated(off, off), j, maxLines, minScale);
    g.setColour(juce::Colours::white.withAlpha(colour.getFloatAlpha()));
    g.drawFittedText(text, r, j, maxLines, minScale);
}

} // namespace textpop
