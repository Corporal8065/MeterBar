#pragma once
#include "Arrange.hpp" // IntRect + ReserveEdge
namespace meterbar {
// RESERVE STATE MACHINE — the decision layer behind the Windows AppBar
// reservation (app/Source/ScreenSpace.cpp does the OS calls; the UI
// feeds this machine EVENTS — window moved/resized, toggle flipped,
// preset loaded — and performs ONLY the action the machine returns).
//
// v0.18.1: this machine exists because of the v0.18 flicker (Jackson's
// field report 2026-10-28): v0.18 re-ran a sync from the UI timer
// ~4x/sec, tested "flush with the top" against a single 2 px threshold,
// and let ScreenSpace::reserveTop compare the requested rect against
// the rect Windows had GRANTED (post-QUERYPOS) — a comparison that
// could never come back equal once the system trimmed the granted
// rect, so every poll tore the AppBar down (ABM_REMOVE: work area
// expands, maximised windows grow) and rebuilt it (ABM_SETPOS: work
// area shrinks, they shrink again), several times a second. Other
// windows bounced between the two heights and their title bars / tab
// strips / close buttons kept moving out from under the user's cursor.
// The rules below make that class of flap impossible by construction:
//  - ACTIONS ARE TRANSITIONS, NOT STATES. update() returns Register /
//    Update / Release / Switch only when the desired registration
//    actually changes; identical repeat observations return None
//    forever.
//  - HYSTERESIS on the dock test. The bar counts as docked at
//    gap <= kDockGapPx (2 px). Once reserved it stays reserved until
//    the gap to the REGISTERED edge exceeds kUndockGapPx (8 px). The
//    3..8 px band is a dead zone: 1 px rect wobble inside it changes
//    nothing, in either direction.
//  - GEOMETRY IS A TRANSITION TOO. While reserved, a changed bar rect,
//    monitor rect, or monitor identity emits exactly one Update; the
//    same geometry repeated emits None.
//  - TOGGLE OFF always wins: one Release, then None.
//
// v0.19.1: EDGE-AWARE (MiniMeters semantics — Jackson's correction of
// v0.18/18.1 in the field). The v0.18 line only ever reserved the TOP
// edge, but Jackson's bar is docked at the BOTTOM of his screen, so
// Reserve did nothing in his layout. The machine now takes the gap to
// ALL FOUR edges of the bar's monitor: the docked edge is the nearest
// one, the hysteresis band applies per-edge (judged on the gap to the
// REGISTERED edge), and dragging the bar from one docked edge to
// another is a Switch — the caller releases the old edge and registers
// the new one, exactly once each. The requested rect is the FULL edge
// strip of the monitor (see stripRect): full width for top/bottom with
// the bar's height as thickness, full height for left/right with the
// bar's width — anchored at the monitor edge and reaching the bar's
// far edge, so a bar floating inside its dead zone still has maximised
// windows stopping AT the bar, not at a strip floating off it.
enum class ReserveAction { None, Register, Update, Release, Switch };

// Absolute distance (physical px) from each bar edge to the matching
// monitor edge: top = bar.y - mon.y, bottom = mon.bottom - bar.bottom,
// left = bar.x - mon.x, right = mon.right - bar.right (caller passes
// absolute values).
struct EdgeGaps { int top = 0, bottom = 0, left = 0, right = 0; };

class ReserveMachine {
public:
    static constexpr int kDockGapPx = 2;   // gap at/below which a non-reserved bar docks
    static constexpr int kUndockGapPx = 8; // gap beyond which a reserved bar releases

    static int gapTo(EdgeGaps g, ReserveEdge e) {
        switch (e) {
            case ReserveEdge::Top:    return g.top;
            case ReserveEdge::Bottom: return g.bottom;
            case ReserveEdge::Left:   return g.left;
            case ReserveEdge::Right:  return g.right;
        }
        return g.top;
    }

    // The edge the bar is docked to. A bar docks ALONG an edge, so the
    // candidates are the two edges of the bar's own orientation: a
    // wide bar can only dock Top/Bottom, a tall bar only Left/Right.
    // The perpendicular gaps are NOT candidates — a full-width bar is
    // incidentally flush with the left and right edges wherever it
    // sits, and calling that a side dock would request a strip the
    // size of the whole screen. Within the pair, the smaller gap wins
    // and a tie goes to Top (wide) / Left (tall).
    static ReserveEdge nearestEdge(EdgeGaps g, IntRect bar) {
        if (bar.w >= bar.h)
            return gapTo(g, ReserveEdge::Top) <= gapTo(g, ReserveEdge::Bottom)
                       ? ReserveEdge::Top : ReserveEdge::Bottom;
        return gapTo(g, ReserveEdge::Left) <= gapTo(g, ReserveEdge::Right)
                   ? ReserveEdge::Left : ReserveEdge::Right;
    }
    static int nearestGap(EdgeGaps g, IntRect bar) { return gapTo(g, nearestEdge(g, bar)); }

    // The AppBar rect to request for a bar docked at `edge`: the full
    // edge strip of the monitor — full width (top/bottom) or full
    // height (left/right), anchored at the monitor edge and extending
    // to the bar's far edge. Flush with the edge, the thickness is
    // exactly the bar's height (top/bottom) or width (left/right).
    static IntRect stripRect(ReserveEdge edge, IntRect bar, IntRect mon) {
        if (mon.w <= 0 || mon.h <= 0) return IntRect{};
        auto clampThickness = [](int t, int extent) { return t < 0 ? 0 : (t > extent ? extent : t); };
        switch (edge) {
            case ReserveEdge::Top: {
                const int th = clampThickness(bar.bottom() - mon.y, mon.h);
                return IntRect{ mon.x, mon.y, mon.w, th };
            }
            case ReserveEdge::Bottom: {
                const int th = clampThickness(mon.bottom() - bar.y, mon.h);
                return IntRect{ mon.x, mon.bottom() - th, mon.w, th };
            }
            case ReserveEdge::Left: {
                const int th = clampThickness(bar.right() - mon.x, mon.w);
                return IntRect{ mon.x, mon.y, th, mon.h };
            }
            case ReserveEdge::Right: {
                const int th = clampThickness(mon.right() - bar.x, mon.w);
                return IntRect{ mon.right() - th, mon.y, th, mon.h };
            }
        }
        return IntRect{};
    }

    // Feed one observation. gaps = distance to each monitor edge;
    // barRect / monitorRect are physical-pixel screen rects; monitorId
    // is any value that changes when the bar changes monitor (the
    // caller passes the HMONITOR as an integer).
    ReserveAction update(bool toggleOn, EdgeGaps gaps, IntRect barRect,
                         IntRect monitorRect, long long monitorId) {
        if (!toggleOn) {
            if (reserved_) { reserved_ = false; return ReserveAction::Release; }
            return ReserveAction::None;
        }
        const bool barOk = barRect.w > 0 && barRect.h > 0 && monitorRect.w > 0 && monitorRect.h > 0;
        if (!reserved_) {
            if (barOk && nearestGap(gaps, barRect) <= kDockGapPx) {
                reserved_ = true;
                edge_ = nearestEdge(gaps, barRect);
                bar_ = barRect; mon_ = monitorRect; monitor_ = monitorId;
                strip_ = stripRect(edge_, barRect, monitorRect);
                return ReserveAction::Register;
            }
            return ReserveAction::None; // includes the 3..8 px dead zone: no docking from it
        }
        if (!barOk) return ReserveAction::None; // transient degenerate rect: hold, judge nothing
        if (gapTo(gaps, edge_) > kUndockGapPx) {
            // Clear of the registered edge. If another edge is already
            // docked, the bar was dragged (or repositioned) straight
            // from one edge to the other: Switch — the caller releases
            // the old edge and registers the new one, once each.
            const ReserveEdge nearEdge = nearestEdge(gaps, barRect);
            if (nearEdge != edge_ && gapTo(gaps, nearEdge) <= kDockGapPx) {
                edge_ = nearEdge;
                bar_ = barRect; mon_ = monitorRect; monitor_ = monitorId;
                strip_ = stripRect(edge_, barRect, monitorRect);
                return ReserveAction::Switch;
            }
            reserved_ = false;
            return ReserveAction::Release;
        }
        // Still docked at the registered edge (dead zone included):
        // geometry drift is a transition too — exactly one Update.
        const IntRect want = stripRect(edge_, barRect, monitorRect);
        if (barRect != bar_ || monitorRect != mon_ || monitorId != monitor_ || want != strip_) {
            bar_ = barRect; mon_ = monitorRect; monitor_ = monitorId; strip_ = want;
            return ReserveAction::Update;
        }
        return ReserveAction::None;
    }

    bool reserved() const { return reserved_; }
    // The registered edge and the strip rect to request for it; both
    // are meaningful while reserved() (and right after Register /
    // Update / Switch returns).
    ReserveEdge edge() const { return edge_; }
    IntRect requestedRect() const { return strip_; }

private:
    bool reserved_ = false;
    ReserveEdge edge_ = ReserveEdge::Top;
    IntRect bar_{};
    IntRect mon_{};
    IntRect strip_{};
    long long monitor_ = 0;
};
} // namespace meterbar
