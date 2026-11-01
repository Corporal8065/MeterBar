#include "CaptionOverlay.h"
#include <meterbar/Captions.hpp>
#include <meterbar/WindowGeom.hpp>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace {
constexpr int kPadX = 14;
constexpr int kPadTop = 6;
constexpr int kStatusH = 16;
constexpr int kLineH = 28;
constexpr int kPadBottom = 8;

juce::Font captionFont() {
    // Segoe UI Semibold when the face exists; JUCE falls back to the
    // default sans otherwise (spec 1.5).
    return juce::Font(juce::FontOptions("Segoe UI Semibold", 22.0f, juce::Font::plain));
}
juce::Font statusFont() {
    return juce::Font(juce::FontOptions(11.0f));
}
int charsPerLine(const juce::Font& f, int usablePx) {
    const float avg = f.getStringWidth("abcdefghijklmnopqrstuvwxyz") / 26.0f;
    if (avg <= 0.f) return 40;
    return juce::jlimit(10, 200, (int)(usablePx / avg));
}
} // namespace

class CaptionOverlay::Content : public juce::Component, private juce::Timer {
public:
    CaptionOverlay& owner;
    juce::TextButton doneBtn { "Done" };
    juce::String prevText, curText, statusLine, pillText;
    bool curFinal = true;
    bool pillMode = false;
    bool moveMode = false;
    float alpha = 0.f, alphaTarget = 0.f;
    int64_t moveDeadlineMs = 0;
    juce::ComponentDragger dragger;

    explicit Content(CaptionOverlay& o) : owner(o) {
        setInterceptsMouseClicks(true, false);
        addAndMakeVisible(doneBtn);
        doneBtn.setVisible(false);
        doneBtn.onClick = [this] { owner.exitMoveMode(); };
        startTimer(40);
    }
    void setContentHeight(int h) {
        if (auto* w = getTopLevelComponent())
            if (w->getHeight() != h) {
                auto b = w->getBounds();
                w->setBounds(b.getX(), b.getBottom() - h, b.getWidth(), h); // bottom-anchored
            }
        setSize(getWidth(), h);
        doneBtn.setBounds(getWidth() - 70, 4, 62, 20);
    }
    int contentHeight() const {
        if (pillMode) return 36;
        const int usable = getWidth() - 2 * kPadX;
        const int cpl = charsPerLine(captionFont(), usable);
        int lines = 0;
        if (prevText.isNotEmpty())
            lines += (int)meterbar::planCaptionLines(prevText.toStdString(), cpl).lines.size();
        if (curText.isNotEmpty())
            lines += (int)meterbar::planCaptionLines(curText.toStdString(), cpl).lines.size();
        if (lines == 0) lines = 1;
        return kPadTop + kStatusH + lines * kLineH + kPadBottom;
    }
    void paint(juce::Graphics& g) override {
        auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.78f));
        g.fillRoundedRectangle(bounds, 10.f);
        if (moveMode) {
            g.setColour(juce::Colour(0xffe04c89));
            g.drawRoundedRectangle(bounds.reduced(1.f), 10.f, 2.f);
        }
        if (pillMode) {
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.setFont(statusFont().withHeight(12.5f));
            g.drawText(pillText, getLocalBounds().reduced(kPadX, 0),
                       juce::Justification::centredLeft, false);
            return;
        }
        // status strip
        g.setFont(statusFont());
        g.setColour(juce::Colours::white.withAlpha(0.65f));
        g.drawText(statusLine, kPadX, kPadTop, getWidth() - 2 * kPadX, kStatusH,
                   juce::Justification::centredRight, false);
        int y = kPadTop + kStatusH;
        const int usable = getWidth() - 2 * kPadX;
        const int cpl = charsPerLine(captionFont(), usable);
        auto drawBlock = [&](const juce::String& text, float opacity) {
            if (text.isEmpty()) return;
            auto plan = meterbar::planCaptionLines(text.toStdString(), cpl);
            g.setFont(captionFont());
            for (auto& line : plan.lines) {
                const auto ls = juce::String(line);
                // dark outline for readability over any picture
                g.setColour(juce::Colours::black.withAlpha(0.85f * opacity));
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        if (dx || dy)
                            g.drawText(ls, kPadX + dx, y + dy, usable, kLineH,
                                       juce::Justification::centredLeft, false);
                g.setColour(juce::Colours::white.withAlpha(opacity));
                g.drawText(ls, kPadX, y, usable, kLineH,
                           juce::Justification::centredLeft, false);
                y += kLineH;
            }
        };
        drawBlock(prevText, 0.72f);            // previous final, dimmed
        drawBlock(curText, curFinal ? 1.0f : 0.92f);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (moveMode) dragger.startDraggingComponent(getTopLevelComponent(), e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (moveMode) dragger.dragComponent(getTopLevelComponent(), e, nullptr);
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (moveMode && owner.onBoundsChanged) owner.onBoundsChanged();
    }
    void timerCallback() override {
        if (moveMode && juce::Time::currentTimeMillis() > moveDeadlineMs) {
            owner.exitMoveMode();
            return;
        }
        if (std::abs(alpha - alphaTarget) > 0.01f) {
            alpha += (alphaTarget - alpha) * 0.35f;
            if (auto* w = getTopLevelComponent()) w->setAlpha(alpha);
            if (alphaTarget == 0.f && alpha < 0.05f) {
                if (auto* w = getTopLevelComponent()) w->setVisible(false);
            }
        }
    }
};

class CaptionOverlay::Window : public juce::DocumentWindow {
public:
    Window() : juce::DocumentWindow("MeterBar Captions", juce::Colours::transparentBlack, 0) {
        setUsingNativeTitleBar(false);
        setTitleBarHeight(0);
        setResizable(false, false);
        setDropShadowEnabled(false);
    }
    void closeButtonPressed() override {} // no chrome; never user-closed
};

CaptionOverlay::CaptionOverlay() {
    window_ = std::make_unique<Window>();
    auto content = std::make_unique<Content>(*this);
    content_ = content.get();
    window_->setContentOwned(content.release(), false);
    window_->setSize(900, 120);
    window_->setAlwaysOnTop(true);
    window_->setVisible(false);
}
CaptionOverlay::~CaptionOverlay() = default;

juce::Rectangle<int> CaptionOverlay::defaultBounds(int height) const {
    std::vector<meterbar::IntRect> areas;
    auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    juce::Rectangle<int> area = primary ? primary->userArea
                                        : juce::Rectangle<int>(0, 0, 1920, 1080);
    for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
        areas.push_back({ d.userArea.getX(), d.userArea.getY(),
                          d.userArea.getWidth(), d.userArea.getHeight() });
    int w = juce::jlimit(560, 1200, 900);
    w = juce::jmin(w, (int)(area.getWidth() * 0.92f));
    int x = area.getCentreX() - w / 2;
    int y = area.getBottom() - 24 - height;
    if (barBoundsProvider) {
        const auto bar = barBoundsProvider();
        if (!bar.isEmpty() && juce::Rectangle<int>(x, y, w, height).intersects(bar))
            y = bar.getY() - 12 - height;
    }
    auto safe = meterbar::ensureRectVisible({ x, y, w, height }, areas);
    return { safe.x, safe.y, safe.w, safe.h };
}

void CaptionOverlay::applyNativeStyles() {
#if JUCE_WINDOWS
    if (!window_ || !window_->isVisible()) return;
    if (auto* peer = window_->getPeer()) {
        HWND hwnd = static_cast<HWND>(peer->getNativeHandle());
        if (hwnd) {
            LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            if (content_ && content_->moveMode) ex &= ~WS_EX_TRANSPARENT;
            else ex |= WS_EX_TRANSPARENT; // click-through in normal mode
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
        }
    }
#endif
}

void CaptionOverlay::showCaptions(const juce::String& previousText,
                                  const juce::String& currentText, bool currentFinal,
                                  const juce::String& status) {
    if (!window_) return;
    content_->pillMode = false;
    content_->prevText = previousText;
    content_->curText = currentText;
    content_->curFinal = currentFinal;
    content_->statusLine = status;
    if (!window_->isVisible()) {
        auto b = hasPosition_ ? window_->getBounds() : defaultBounds(content_->contentHeight());
        window_->setBounds(b.getX(), b.getY(), b.getWidth(), content_->contentHeight());
        content_->alpha = 0.f;
        window_->setVisible(true);
        applyNativeStyles();
    }
    content_->setContentHeight(content_->contentHeight());
    content_->alphaTarget = 1.f;
    content_->repaint();
}

void CaptionOverlay::showPill(const juce::String& text) {
    if (!window_) return;
    content_->pillMode = true;
    content_->pillText = text;
    if (!window_->isVisible()) {
        auto b = hasPosition_ ? window_->getBounds() : defaultBounds(36);
        window_->setBounds(b.getX(), b.getY(), b.getWidth(), 36);
        content_->alpha = 0.f;
        window_->setVisible(true);
        applyNativeStyles();
    }
    content_->setContentHeight(36);
    content_->alphaTarget = 1.f;
    content_->repaint();
}

void CaptionOverlay::hide() {
    if (!window_) return;
    content_->alphaTarget = 0.f; // the content timer hides at ~0
    if (!window_->isVisible()) return;
    if (content_->alpha < 0.05f) window_->setVisible(false);
}

void CaptionOverlay::setAlwaysOnTopFlag(bool on) {
    alwaysOnTop_ = on;
    if (window_) window_->setAlwaysOnTop(on);
}

void CaptionOverlay::restoreBounds(int x, int y, int w, bool hasPosition) {
    hasPosition_ = hasPosition;
    if (!window_) return;
    if (hasPosition) {
        std::vector<meterbar::IntRect> areas;
        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
            areas.push_back({ d.userArea.getX(), d.userArea.getY(),
                              d.userArea.getWidth(), d.userArea.getHeight() });
        auto safe = meterbar::ensureRectVisible(
            { x, y, juce::jlimit(320, 3840, w), window_->getHeight() }, areas);
        window_->setBounds(safe.x, safe.y, safe.w, window_->getHeight());
    }
}

void CaptionOverlay::resetPosition() {
    hasPosition_ = false;
    if (!window_) return;
    const auto b = defaultBounds(window_->getHeight());
    window_->setBounds(b);
    if (onBoundsChanged) onBoundsChanged();
}

juce::Rectangle<int> CaptionOverlay::bounds() const {
    return window_ ? window_->getBounds() : juce::Rectangle<int>();
}

void CaptionOverlay::enterMoveMode() {
    if (!window_ || !content_) return;
    if (!window_->isVisible()) showPill("Move the caption window, then click Done");
    content_->moveMode = true;
    content_->doneBtn.setVisible(true);
    content_->moveDeadlineMs = juce::Time::currentTimeMillis() + 30000;
    applyNativeStyles();
    content_->repaint();
}

void CaptionOverlay::exitMoveMode() {
    if (!content_) return;
    content_->moveMode = false;
    content_->doneBtn.setVisible(false);
    hasPosition_ = true;
    applyNativeStyles();
    content_->repaint();
    if (onBoundsChanged) onBoundsChanged();
}

bool CaptionOverlay::isMoveMode() const {
    return content_ && content_->moveMode;
}
