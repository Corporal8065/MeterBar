#pragma once
#include <vector>
#include "Arrange.hpp" // IntRect
namespace meterbar {
// WINDOW GEOMETRY GUARDS (v0.3.0) — the off-screen restore fix.
//
// Field report (Jackson, v0.2.1): after dock/arrange/link
// manipulation, SessionBar restored to a position off-screen / under
// the taskbar — running, visible in the taskbar preview, impossible
// to grab. The saved position is trusted blindly at startup in all
// three bars; a rect saved while a monitor was unplugged, a link
// follower mid-stack, or a drag ended oddly can point at space no
// monitor owns any more.
//
// clampRectToWorkAreas is the startup (and preset-apply) guard: the
// rect keeps its SIZE, and its position is pulled so the whole rect
// sits inside ONE visible work area — the one it overlaps most, or
// the nearest one when it overlaps none. Pure arithmetic, unit-tested
// here; the app feeds it the displays' user areas.

inline long long rectOverlapArea(const IntRect& a, const IntRect& b) {
    const int w = (a.right() < b.right() ? a.right() : b.right()) - (a.x > b.x ? a.x : b.x);
    const int h = (a.bottom() < b.bottom() ? a.bottom() : b.bottom()) - (a.y > b.y ? a.y : b.y);
    return (w > 0 && h > 0) ? (long long)w * (long long)h : 0;
}

// True when any part of r is inside any of the areas.
inline bool rectTouchesAnyArea(const IntRect& r, const std::vector<IntRect>& areas) {
    for (auto& a : areas) if (rectOverlapArea(r, a) > 0) return true;
    return false;
}

inline IntRect clampRectToWorkAreas(IntRect r, const std::vector<IntRect>& areas) {
    if (areas.empty() || r.w <= 0 || r.h <= 0) return r;
    // Pick the area: largest overlap wins; if nothing overlaps, the
    // nearest area centre to the rect's centre.
    const IntRect* best = nullptr;
    long long bestOverlap = 0;
    for (auto& a : areas) {
        const long long ov = rectOverlapArea(r, a);
        if (ov > bestOverlap) { bestOverlap = ov; best = &a; }
    }
    if (best == nullptr) {
        long long bestD2 = -1;
        const long long cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        for (auto& a : areas) {
            const long long ax = a.x + a.w / 2, ay = a.y + a.h / 2;
            const long long d2 = (ax - cx) * (ax - cx) + (ay - cy) * (ay - cy);
            if (bestD2 < 0 || d2 < bestD2) { bestD2 = d2; best = &a; }
        }
    }
    const IntRect& a = *best;
    // Clamp position so the rect is fully inside the chosen area; a
    // rect wider/taller than the area pins to the area's origin edge.
    r.x = r.w >= a.w ? a.x : (r.x < a.x ? a.x : (r.x > a.right() - r.w ? a.right() - r.w : r.x));
    r.y = r.h >= a.h ? a.y : (r.y < a.y ? a.y : (r.y > a.bottom() - r.h ? a.bottom() - r.h : r.y));
    return r;
}
} // namespace meterbar
