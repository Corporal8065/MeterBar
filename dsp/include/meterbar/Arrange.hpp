#pragma once
#include <algorithm>
namespace meterbar {
// v0.18 ARRANGE MATH — the fitting rule behind the "Arrange windows"
// sweep (app/Source/ScreenSpace.cpp, Windows-only: EnumWindows +
// SetWindowPos). The rule itself is a PURE function with no OS calls, so
// the Linux test suite can grade it exactly, even though the sweep that
// consumes it can only run on Jackson's PC. Same division of labour as
// the rest of the DSP core: math here, platform plumbing in the app.

// Integer rectangle in pixels: x/y = top-left corner, w/h = size.
// On Windows these are PHYSICAL pixels (what GetWindowRect /
// GetMonitorInfo report); the tests use plain numbers.
struct IntRect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool operator==(const IntRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const IntRect& o) const { return !(*this == o); }
};

// A monitor edge — the bar can dock to any of the four (v0.19.1 made
// the reserve/arrange machinery edge-aware; v0.18 knew only Top). The
// reserve state machine (ReserveState.hpp) and the AppBar service
// (ScreenSpace) share this vocabulary.
enum class ReserveEdge { Top, Bottom, Left, Right };

// The free area of `monitor` once a strip of stripPx pixels is reserved
// along `edge`: the strip comes off that edge, the rest is untouched.
// stripPx is clamped to the monitor's extent along that axis.
inline IntRect arrangeFreeArea(IntRect monitor, ReserveEdge edge, int stripPx) {
    switch (edge) {
        case ReserveEdge::Top: {
            const int strip = std::clamp(stripPx, 0, monitor.h);
            return IntRect{ monitor.x, monitor.y + strip, monitor.w, monitor.h - strip };
        }
        case ReserveEdge::Bottom: {
            const int strip = std::clamp(stripPx, 0, monitor.h);
            return IntRect{ monitor.x, monitor.y, monitor.w, monitor.h - strip };
        }
        case ReserveEdge::Left: {
            const int strip = std::clamp(stripPx, 0, monitor.w);
            return IntRect{ monitor.x + strip, monitor.y, monitor.w - strip, monitor.h };
        }
        case ReserveEdge::Right: {
            const int strip = std::clamp(stripPx, 0, monitor.w);
            return IntRect{ monitor.x, monitor.y, monitor.w - strip, monitor.h };
        }
    }
    return monitor;
}

// Fit `window` inside `monitor` OUTSIDE a reserved strip of stripPx
// pixels along `edge` (for Top: the docked MeterBar's height, measured
// from the monitor's top to the bar's bottom edge; the other edges are
// the same measurement against their own edge). Rules, in order:
//  1. stripPx is clamped to the monitor's extent along the strip axis.
//     A monitor with no free area left (strip >= its extent, or a
//     degenerate monitor/window) gets the window back UNCHANGED — the
//     sweep must never make things worse.
//  2. A window already fully inside the free area is returned UNCHANGED
//     (falls out of the clamp math below): arranging must never shuffle
//     windows that are not in the way.
//  3. Otherwise the window keeps as much of its size as fits (shrunk
//     only if it is bigger than the free area in that dimension) and
//     slides the minimum distance to sit fully inside the free area:
//     an edge poking under the strip slides to just clear of it,
//     anything hanging off any edge is pulled back in.
inline IntRect arrangeFit(IntRect window, IntRect monitor, ReserveEdge edge, int stripPx) {
    if (window.w <= 0 || window.h <= 0 || monitor.w <= 0 || monitor.h <= 0) return window;
    const IntRect freeArea = arrangeFreeArea(monitor, edge, stripPx);
    if (freeArea.w <= 0 || freeArea.h <= 0) return window;
    IntRect out;
    out.w = std::min(window.w, freeArea.w);
    out.h = std::min(window.h, freeArea.h);
    out.x = std::clamp(window.x, freeArea.x, freeArea.right() - out.w);
    out.y = std::clamp(window.y, freeArea.y, freeArea.bottom() - out.h);
    return out;
}

// The original v0.18 form: a strip along the TOP edge. Kept as a thin
// wrapper — its outcomes are the edge-aware function's Top outcomes by
// construction, and the v0.18 test block grades them unchanged.
inline IntRect arrangeFit(IntRect window, IntRect monitor, int stripPx) {
    return arrangeFit(window, monitor, ReserveEdge::Top, stripPx);
}
} // namespace meterbar
