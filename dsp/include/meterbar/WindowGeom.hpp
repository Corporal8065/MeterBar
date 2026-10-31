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

// VISIBILITY FLOOR FOR REMOTE APPLIES (v0.21.1) — the hole the
// startup clamp never covered. clampRectToWorkAreas guards the two
// places a SAVED rect is applied (startup restore, preset apply),
// but the family link moves a bar AFTER startup: stack plans, drag
// follows, snap settles and group docks all land through the link
// client's setBounds callback with NO guard — so with Link on, a
// stale stack request or a peer docked against space no monitor
// owns any more could move a just-restored bar off-screen within a
// tick of launch, and the position-save feed then kept it there.
// Jackson, 2026-10-31 (of SessionBar, same defect here by
// construction): "Session bar is also for me still stuck somewhere
// i cannot see upon load up and have no idea how to fix plz help".
//
// The rule for remote applies is a floor, not a clamp: a rect that
// is MEANINGFULLY visible — at least 100x40 px of it inside ONE
// display's work area (the bar itself when it is smaller than that)
// — is returned untouched, straddles included, so link choreography
// is never fought. Below that floor the rect keeps its size and is
// placed on the PRIMARY work area (areas[0], in the order the app
// feeds the displays), centred horizontally, near the top — a spot
// the user can always see and grab.
inline bool rectMeaningfullyVisible(const IntRect& r, const std::vector<IntRect>& areas) {
    const int needW = r.w < 100 ? r.w : 100;
    const int needH = r.h < 40 ? r.h : 40;
    for (auto& a : areas) {
        const int iw = (r.right() < a.right() ? r.right() : a.right()) - (r.x > a.x ? r.x : a.x);
        const int ih = (r.bottom() < a.bottom() ? r.bottom() : a.bottom()) - (r.y > a.y ? r.y : a.y);
        if (iw >= needW && ih >= needH) return true;
    }
    return false;
}

inline IntRect ensureRectVisible(IntRect r, const std::vector<IntRect>& areas) {
    if (areas.empty() || r.w <= 0 || r.h <= 0) return r;
    if (rectMeaningfullyVisible(r, areas)) return r;
    const IntRect& a = areas.front(); // primary display
    r.x = r.w >= a.w ? a.x : a.x + (a.w - r.w) / 2;
    r.y = r.h >= a.h ? a.y : a.y + 24;
    if (r.h < a.h && r.y > a.bottom() - r.h) r.y = a.bottom() - r.h;
    return r;
}
} // namespace meterbar
