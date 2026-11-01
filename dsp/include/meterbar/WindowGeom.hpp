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

// ------------------------------------------------------------------
// CAPTION OVERLAY HOMING (v0.24.0) — Jackson's field report on
// v0.23.2: he enabled captions and the overlay appeared on his
// OTHER monitor, behind his game. Cause: when captions are shown,
// the overlay's saved position (or the primary-display default) is
// trusted blindly, wherever MeterBar itself currently lives. The
// rule now: whenever captions are (re)shown, the overlay belongs
// on the display the BAR is on. A saved rect that already sits
// mainly (>= half its area) on the bar's display is kept verbatim
// — the user's placement wins; anything else relocates to
// bottom-centre of the bar's display work area, using the v1
// default placement's sizing and margins (width clamped
// 560..1200 and to at most 92% of the work area, bottom edge
// 24 px above the work-area bottom, raised to sit 12 px above the
// bar when the home rect would intersect it). Pure: the app feeds
// the displays' user areas and the overlay's current height.
struct CaptionHomePlan {
    bool relocate = false;  // false: keep the saved rect, just raise
    IntRect rect{};         // the rect to apply when relocate is true
};

namespace windowgeom_detail {
// The work area a rect belongs to: largest overlap wins; when it
// overlaps none, the nearest area centre (the clamp's pick rule).
inline int dominantAreaIndex(const IntRect& r, const std::vector<IntRect>& areas) {
    int best = 0;
    long long bestOverlap = 0;
    for (size_t i = 0; i < areas.size(); ++i) {
        const long long ov = rectOverlapArea(r, areas[i]);
        if (ov > bestOverlap) { bestOverlap = ov; best = (int)i; }
    }
    if (bestOverlap > 0) return best;
    long long bestD2 = -1;
    const long long cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (size_t i = 0; i < areas.size(); ++i) {
        const long long ax = areas[i].x + areas[i].w / 2, ay = areas[i].y + areas[i].h / 2;
        const long long d2 = (ax - cx) * (ax - cx) + (ay - cy) * (ay - cy);
        if (bestD2 < 0 || d2 < bestD2) { bestD2 = d2; best = (int)i; }
    }
    return best;
}
} // namespace windowgeom_detail

inline CaptionHomePlan captionHomeRect(IntRect saved, bool hasSaved, IntRect bar,
                                       const std::vector<IntRect>& workAreas,
                                       int overlayHeight) {
    CaptionHomePlan plan;
    if (workAreas.empty() || bar.w <= 0 || bar.h <= 0) return plan;
    const int barArea = windowgeom_detail::dominantAreaIndex(bar, workAreas);
    if (hasSaved && saved.w > 0 && saved.h > 0) {
        const int savedArea = windowgeom_detail::dominantAreaIndex(saved, workAreas);
        if (savedArea == barArea
            && rectOverlapArea(saved, workAreas[(size_t)savedArea]) * 2
                   >= (long long)saved.w * (long long)saved.h)
            return plan; // already mainly on the bar's display: keep
    }
    const IntRect& a = workAreas[(size_t)barArea];
    int w = (hasSaved && saved.w > 0) ? saved.w : 900;
    w = w < 560 ? 560 : (w > 1200 ? 1200 : w);
    const int maxW = (int)(a.w * 0.92);
    if (w > maxW) w = maxW < 280 ? a.w : maxW;
    if (w > a.w) w = a.w;
    int h = overlayHeight > 0 ? overlayHeight : 132;
    if (h < 40) h = 40;
    if (h > a.h) h = a.h;
    IntRect out{ a.x + (a.w - w) / 2, a.bottom() - 24 - h, w, h };
    if (rectOverlapArea(out, bar) > 0) out.y = bar.y - 12 - h; // above the bar
    if (out.y < a.y) out.y = a.y;
    if (out.bottom() > a.bottom()) out.y = a.bottom() - h;
    if (out.x < a.x) out.x = a.x;
    if (out.right() > a.right()) out.x = a.right() - w;
    plan.relocate = true;
    plan.rect = out;
    return plan;
}

// ------------------------------------------------------------------
// CAPTION DOCK GROWTH (v0.24.0) — the docked caption presentation
// renders a strip inside MeterBar along the bottom of the bar, so
// the window grows by the strip height while docked captions are
// on and shrinks back when they end. Growth is anchored at the
// bar's docked (or nearest) edge: the docked edge's own coordinate
// never moves, so the edge gap the reserve machine observes is
// unchanged and a reserved bar stays reserved on the same edge —
// the machine reads the grown rect on the next geometry event and
// emits exactly one Update whose strip is derived from the NEW
// rect (ReserveMachine::stripRect takes the bar's far edge as the
// thickness). The BLK4 stack plans likewise read the anchor's
// current rect, so followers re-stack flush against the grown
// bar. Nothing in either system caches a height this could desync.
// The shrink restores the pre-growth size exactly (height floor
// 48 px: a bar manually shrunk below strip+floor while docked
// keeps the floor, and only the applied delta is un-grown).
inline IntRect captionGrownBarRect(IntRect bar, int stripH, ReserveEdge anchor) {
    if (stripH <= 0 || bar.w <= 0 || bar.h <= 0) return bar;
    if (anchor == ReserveEdge::Top) return IntRect{ bar.x, bar.y, bar.w, bar.h + stripH };
    // Bottom (and Left/Right — the family bars are horizontal, so
    // their dock axis is vertical): the bottom edge stays put.
    return IntRect{ bar.x, bar.y - stripH, bar.w, bar.h + stripH };
}

inline IntRect captionShrunkBarRect(IntRect bar, int stripH, ReserveEdge anchor) {
    if (stripH <= 0 || bar.w <= 0 || bar.h <= 0) return bar;
    const int minH = 48;
    int h = bar.h - stripH;
    if (h < minH) h = minH;
    const int delta = bar.h - h; // what was actually applied
    if (anchor == ReserveEdge::Top) return IntRect{ bar.x, bar.y, bar.w, h };
    return IntRect{ bar.x, bar.y + delta, bar.w, h };
}
} // namespace meterbar
