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

// WHITE RULE (v0.27.1 — Jackson, 2026-11-02 21:19 PT): "for all
// the text if the front text is white make the back white." When
// a label's own colour is white or near-white, the offset BACK
// copy is drawn PURE WHITE — white text never gets a tinted or
// contrasting backing. (The themes' text colours are tinted
// whites, e.g. 0xfff2e8ee, not pure white — without this rule the
// back copy showed that tint behind the white face.) Coloured
// labels keep their own colour as the back copy; the corrected
// geometry above is otherwise unchanged. Alpha always follows
// the caller's colour.
inline bool isWhiteText(juce::Colour c) {
    const int r = (int)c.getRed(), g = (int)c.getGreen(), b = (int)c.getBlue();
    const int lo = juce::jmin(r, juce::jmin(g, b)), hi = juce::jmax(r, juce::jmax(g, b));
    return lo >= 0xE0 && (hi - lo) <= 0x30;
}
inline juce::Colour backColourFor(juce::Colour colour) {
    return isWhiteText(colour) ? juce::Colours::white.withAlpha(colour.getFloatAlpha())
                               : colour;
}

// drawText with the pop. The caller sets the font first, exactly
// as with Graphics::drawText — the offset derives from it. The
// back copy keeps the caller's colour and alpha (pure white for
// white text, per the rule above); the white front copy keeps
// the caller's alpha too.
inline void draw(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r,
                 juce::Justification j, juce::Colour colour) {
    const float off = offsetFor(g.getCurrentFont().getHeight());
    g.setColour(backColourFor(colour));
    g.drawText(text, r.translated(off, off), j);   // back copy, bottom-right, BEHIND
    g.setColour(juce::Colours::white.withAlpha(colour.getFloatAlpha()));
    g.drawText(text, r, j);                        // white copy, top-left, IN FRONT
}
inline void draw(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                 juce::Justification j, juce::Colour colour) {
    draw(g, text, r.toFloat(), j, colour);
}

// drawFittedText with the pop (toolbar word buttons / labels,
// whose stock LookAndFeel draws fitted text). Same order: back
// copy bottom-right behind, white copy in front.
inline void drawFitted(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                       juce::Justification j, int maxLines, float minScale, juce::Colour colour) {
    const int off = juce::roundToInt(offsetFor(g.getCurrentFont().getHeight()));
    g.setColour(backColourFor(colour));
    g.drawFittedText(text, r.translated(off, off), j, maxLines, minScale);
    g.setColour(juce::Colours::white.withAlpha(colour.getFloatAlpha()));
    g.drawFittedText(text, r, j, maxLines, minScale);
}

// BLACK-BACK variants (v0.28.0 — Jackson, 2026-11-02 21:54 PT,
// correcting the reach of the v0.27.1 white rule): for the METER
// READOUT labels — loudness L/R, the waveform chip (BPM + time
// signature), the stereometer bottom line, the oscilloscope and
// spectrum bottom text — the offset back copy is PURE BLACK.
// The white-back build "looks pretty bad" on those readouts (his
// words); his Loudness remark was about these meter labels, not
// a family-wide rule. The white FRONT copy and the geometry are
// unchanged; every other label keeps draw/drawFitted above.
inline juce::Colour blackBackFor(juce::Colour colour) {
    return juce::Colours::black.withAlpha(colour.getFloatAlpha());
}
inline void drawBlackBack(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r,
                          juce::Justification j, juce::Colour colour) {
    const float off = offsetFor(g.getCurrentFont().getHeight());
    g.setColour(blackBackFor(colour));
    g.drawText(text, r.translated(off, off), j);   // black copy, bottom-right, BEHIND
    g.setColour(juce::Colours::white.withAlpha(colour.getFloatAlpha()));
    g.drawText(text, r, j);                        // white copy, top-left, IN FRONT
}
inline void drawBlackBack(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                          juce::Justification j, juce::Colour colour) {
    drawBlackBack(g, text, r.toFloat(), j, colour);
}
inline void drawFittedBlackBack(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                                juce::Justification j, int maxLines, float minScale, juce::Colour colour) {
    const int off = juce::roundToInt(offsetFor(g.getCurrentFont().getHeight()));
    g.setColour(blackBackFor(colour));
    g.drawFittedText(text, r.translated(off, off), j, maxLines, minScale);
    g.setColour(juce::Colours::white.withAlpha(colour.getFloatAlpha()));
    g.drawFittedText(text, r, j, maxLines, minScale);
}

// SINGLE draw (v0.28.6) — the family's drawSingle convention (named
// identically to TickerBar's): the text once, in its own colour, no
// offset copy at all. For the labels Jackson has ruled plain: the
// Loudness readout column (his 2026-11-03 feel-fixes ask — "remove
// the double text for the number of lufs and the text below that").
inline void drawSingle(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r,
                       juce::Justification j, juce::Colour colour) {
    g.setColour(colour);
    g.drawText(text, r, j);
}
inline void drawSingle(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                       juce::Justification j, juce::Colour colour) {
    drawSingle(g, text, r.toFloat(), j, colour);
}

} // namespace textpop
