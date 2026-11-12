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
// RESTORE RECT (v0.28.11) — the startup / preset-apply placement of
// a SAVED rect (MainWindow's constructor, MainComponent's preset
// load). Until now both fed the saved rect straight through
// clampRectToWorkAreas against the displays' USER areas — correct
// for the v0.3.0 rescue it was built for (a rect saved against a
// monitor that is gone, or dragged mostly off-screen, must come
// back reachable), but wrong for a rect that is exactly where the
// user left it. The case that broke in the field (Jackson,
// 2026-11-12, on v0.28.10): a bar docked by the app's own dock
// action saves its WINDOW rect, whose edge-adjacent side sits one
// frame inset OUTSIDE the monitor (that offset is the whole
// v0.28.10 placement fix — the visible edge is what lands flush).
// The user area starts at the monitor edge, so the clamp dragged
// the restored rect fully inside it: the visible edge landed one
// inset BELOW the edge, the reserve machine judged its ~8-12 px
// dead-zone gap, and a Stick-on relaunch never registered — every
// update install walked the bar into this. The rule here is the
// link path's floor applied to restores (restoreRect is to saved
// rects what ensureRectVisible is to remote applies): a saved
// rect that is MEANINGFULLY VISIBLE in some work area is the
// user's placement and is restored VERBATIM, dock offsets and
// straddles included; only a rect below that floor — genuinely
// lost — is clamped, exactly as before.
inline IntRect restoreRectToWorkAreas(IntRect r, const std::vector<IntRect>& areas) {
    if (areas.empty() || r.w <= 0 || r.h <= 0) return r;
    if (rectMeaningfullyVisible(r, areas)) return r;
    return clampRectToWorkAreas(r, areas);
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

// ------------------------------------------------------------------
// REPOSITION DOCK RECT (v0.28.9) — Jackson's rule, restated in his
// 2026-11-11 dock-merge ruling (and the 2026-11-03 family rule):
// Reposition -> At the Top / At the Bottom moves the bar FLUSH to
// that edge of the monitor it is on, sets x = the monitor's left
// edge and width = the monitor's FULL width, and keeps the bar's
// current height. The monitor rect here is the display's TOTAL
// area, never its work/user area: the user area is the monitor
// minus the taskbar and any AppBar reservations — including this
// bar's own — so user-area math landed a stuck bar BELOW its own
// reserved strip (Jackson's "reserve drops it lower" report) and
// could leave the width short of the screen's edges. Pure: the app
// resolves the display for the bar's current rect and feeds its
// total area. A degenerate monitor or bar height returns the bar
// unchanged (the caller keeps its own fallback chain).
inline IntRect dockedBarRect(IntRect bar, IntRect mon, bool top) {
    if (mon.w <= 0 || mon.h <= 0 || bar.h <= 0) return bar;
    return IntRect{ mon.x, top ? mon.y : mon.bottom() - bar.h, mon.w, bar.h };
}

// ------------------------------------------------------------------
// FRAME INSETS + THE WINDOW-SPACE DOCK RECT (v0.28.10) — the fix for
// the v0.28.9 placed-vs-judged mismatch (DIAGNOSIS candidate 1, a
// definite defect in the shipped 0.28.9). The dock ACTION places a
// WINDOW rect (what setBounds / SetWindowPos consume); the reserve
// machine JUDGES the bar's VISIBLE rect (DWM extended frame bounds).
// The two differ by the DWM invisible resize border — ~7-8 px a side
// at 100% scaling, ~10-12 px at 125-150% — while the machine only
// registers at a judged gap <= 2 px and never docks from the 3-8 px
// band. So a dock performed by the app's own Reposition / Stick
// buttons in 0.28.9 placed the window flush and left the judged gap
// at ~7-8 px: the reservation never engaged ("still fucked" —
// Jackson's field report on 0.28.9), the strip came out ~one inset
// thin when a drag-overshoot did engage it, and at >=125% scaling
// the inset alone exceeded the 8 px undock band so a reserved bar
// released itself.
//
// The fix makes placement speak the machine's language: measure the
// live frame inset (window rect minus visible rect, per side), dock
// the VISIBLE rect flush via dockedBarRect, then expand the target
// back out by the insets to the WINDOW rect to hand the OS. The
// machine's thresholds are NOT widened — the inset is DPI-dependent,
// and a fixed wider dock band would re-open the flap class the dead
// zone exists to prevent (the rejected alternative; see DIAGNOSIS).
// Placement, judgment and the strip derivation (already visible-rect
// based) then all describe the same geometry, and the machine's
// Switch branch works because the judged gap after an edge-to-edge
// reposition is genuinely ~0. Pure: the app measures the insets from
// the live window (GetWindowRect + DWM extended frame bounds) and
// feeds them here, in the same pixel domain as the rects it passes.
struct FrameInsets { int top = 0, bottom = 0, left = 0, right = 0; };

// The insets of a window whose window rect and visible rect are both
// known (same pixel domain). A visible rect that matches or exceeds
// the window rect on a side clamps that side to 0 — a DWM refusal
// handled by the caller's fallback never reaches here, and a
// borderless window honestly reports 0.
inline FrameInsets frameInsetsOf(IntRect windowRect, IntRect visibleRect) {
    auto side = [](int d) { return d > 0 ? d : 0; };
    return FrameInsets{ side(visibleRect.y - windowRect.y),
                        side(windowRect.bottom() - visibleRect.bottom()),
                        side(visibleRect.x - windowRect.x),
                        side(windowRect.right() - visibleRect.right()) };
}

// The visible rect of a window placed at `windowRect` with the given
// insets — the exact rect the reserve machine will judge after the
// placement lands.
inline IntRect visibleRectFromWindowRect(IntRect windowRect, FrameInsets in) {
    return IntRect{ windowRect.x + in.left, windowRect.y + in.top,
                    windowRect.w - in.left - in.right,
                    windowRect.h - in.top - in.bottom };
}

// The WINDOW rect to place so the bar's VISIBLE rect lands flush at
// the chosen edge, full monitor width, visible height unchanged:
// dockedBarRect run on the visible rect, expanded back by the
// insets. The resulting window height equals the input's (visible
// height + both insets), and the edge-adjacent side of the window
// rect sits one inset OUTSIDE the monitor edge — exactly where a
// borderless-looking bar's window must sit for its visible edge to
// be flush. Degenerate inputs (bad monitor/bar, or insets that meet
// or exceed the bar) fall back to the plain v0.28.9 window-rect dock.
inline IntRect dockedBarWindowRect(IntRect bar, IntRect mon, bool top, FrameInsets in) {
    if (mon.w <= 0 || mon.h <= 0 || bar.h <= 0) return bar;
    const IntRect vis = visibleRectFromWindowRect(bar, in);
    if (vis.w <= 0 || vis.h <= 0) return dockedBarRect(bar, mon, top);
    const IntRect vt = dockedBarRect(vis, mon, top);
    return IntRect{ vt.x - in.left, vt.y - in.top,
                    vt.w + in.left + in.right, vt.h + in.top + in.bottom };
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
