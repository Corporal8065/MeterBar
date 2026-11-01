#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <meterbar/Captions.hpp>

// LIVE CAPTIONS — DOCKED STRIP (v0.24.0, MeterBar ONLY).
//
// The second presentation from Jackson's 2026-11-01 build word: the
// same caption model the overlay renders (CaptionController's
// DockedContent — previous final dimmed, current bright, status
// badge/pill), drawn as a strip INSIDE MeterBar along the bottom of
// the bar instead of in the separate overlay window. MainComponent
// owns this component's bounds: while docked captions are on, the
// bar grows by the strip height (core captionGrownBarRect, anchored
// at the bar's docked edge — the reservation and the BLK4 stack
// follow the real bounds, see WindowGeom.hpp) and this strip fills
// the grown band; when captions end, the bar shrinks back.
//
// The strip never intercepts mouse clicks: the bar's own drag and
// module interactions pass straight through it. Text planning is
// the pure model's two-line rule (meterbar::planCaptionLines), the
// same as the overlay's.
class CaptionStrip : public juce::Component {
public:
    CaptionStrip() { setInterceptsMouseClicks(false, false); }

    // Change-detected content feed (called every UI tick).
    void setContent(const juce::String& previous, const juce::String& current,
                    bool currentFinal, const juce::String& badge,
                    const juce::String& pill) {
        if (previous == prev_ && current == cur_ && currentFinal == curFinal_
            && badge == badge_ && pill == pill_)
            return;
        prev_ = previous; cur_ = current; curFinal_ = currentFinal;
        badge_ = badge; pill_ = pill;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;
        // A dark band over the bar's background — readable on any
        // theme, like the overlay's black backing (spec 1.5 spirit).
        g.setColour(juce::Colours::black.withAlpha(0.66f));
        g.fillRect(0, 0, w, h);
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillRect(0, 0, w, 1); // hairline against the module area

        const int usable = w - 28;
        if (cur_.isEmpty() && prev_.isEmpty()) {
            // Status only: the pill text when there is one, else the
            // badge (e.g. "LIVE — SYSTEM AUDIO") so the strip never
            // sits blank while captions are on.
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.setFont(juce::Font(juce::FontOptions(14.5f)));
            g.drawText(pill_.isNotEmpty() ? pill_ : badge_,
                       getLocalBounds().reduced(14, 0),
                       juce::Justification::centred, false);
            return;
        }
        const auto font = juce::Font(
            juce::FontOptions("Segoe UI Semibold", 19.0f, juce::Font::plain));
        const float avg = font.getStringWidth("abcdefghijklmnopqrstuvwxyz") / 26.0f;
        const int cpl = juce::jlimit(10, 240, avg > 0.f ? (int)(usable / avg) : 60);
        int y = 4;
        if (prev_.isNotEmpty()) {
            auto plan = meterbar::planCaptionLines(prev_.toStdString(), cpl);
            g.setFont(juce::Font(
                juce::FontOptions("Segoe UI Semibold", 15.0f, juce::Font::plain)));
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            if (!plan.lines.empty())
                g.drawText(juce::String(plan.lines.back()), 14, y, usable, 20,
                           juce::Justification::centredLeft, false);
            y += 22;
        }
        // status badge: small, top-right, muted (the overlay's strip)
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.drawText(badge_, 14, 4, usable, 14, juce::Justification::centredRight, false);
        auto plan = meterbar::planCaptionLines(cur_.toStdString(), cpl);
        g.setFont(font);
        g.setColour(juce::Colours::white.withAlpha(curFinal_ ? 1.0f : 0.92f));
        const int linesH = (int)plan.lines.size() * 24;
        int ty = y + juce::jmax(0, (h - y - linesH) / 2);
        for (auto& line : plan.lines) {
            const auto ls = juce::String(line);
            g.setColour(juce::Colours::black.withAlpha(0.7f));
            g.drawText(ls, 15, ty + 1, usable, 24, juce::Justification::centredLeft, false);
            g.setColour(juce::Colours::white.withAlpha(curFinal_ ? 1.0f : 0.92f));
            g.drawText(ls, 14, ty, usable, 24, juce::Justification::centredLeft, false);
            ty += 24;
        }
    }

private:
    juce::String prev_, cur_, badge_, pill_;
    bool curFinal_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CaptionStrip)
};
