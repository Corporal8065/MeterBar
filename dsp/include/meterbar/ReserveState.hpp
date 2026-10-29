#pragma once
#include "Arrange.hpp" // IntRect
namespace meterbar {
// v0.18.1 RESERVE STATE MACHINE — the decision layer behind the Windows
// AppBar reservation (app/Source/ScreenSpace.cpp does the OS calls; the
// UI feeds this machine EVENTS — window moved/resized, toggle flipped,
// preset loaded — and performs ONLY the action the machine returns).
//
// Why this exists (the v0.18 flicker, Jackson's field report 2026-10-28):
// v0.18 re-ran a sync from the UI timer ~4x/sec, tested "flush with the
// top" against a single 2 px threshold, and let ScreenSpace::reserveTop
// compare the requested rect against the rect Windows had GRANTED
// (post-QUERYPOS) — a comparison that could never come back equal once
// the system trimmed the granted rect, so every poll tore the AppBar
// down (ABM_REMOVE: work area expands, maximised windows grow) and
// rebuilt it (ABM_SETPOS: work area shrinks, they shrink again), several
// times a second. Other windows bounced between the two heights and
// their title bars / tab strips / close buttons kept moving out from
// under the user's cursor.
//
// The rules here make that class of flap impossible by construction:
//  - ACTIONS ARE TRANSITIONS, NOT STATES. update() returns Register /
//    Update / Release only when the desired registration actually
//    changes; identical repeat observations return None forever.
//  - HYSTERESIS on the dock test. The bar counts as docked at
//    gap <= kDockGapPx (2 px). Once reserved it stays reserved until
//    gap > kUndockGapPx (8 px). The 3..8 px band is a dead zone: 1 px
//    rect wobble inside it changes nothing, in either direction (a bar
//    floating 5 px off the top does NOT reserve, and a reserved bar
//    wobbling to 5 px does NOT release).
//  - GEOMETRY IS A TRANSITION TOO. While reserved, a changed bar rect
//    or monitor emits exactly one Update; the same geometry repeated
//    emits None.
//  - TOGGLE OFF always wins: one Release, then None.
enum class ReserveAction { None, Register, Update, Release };

class ReserveMachine {
public:
    static constexpr int kDockGapPx = 2;   // gap at/below which a non-reserved bar docks
    static constexpr int kUndockGapPx = 8; // gap beyond which a reserved bar releases

    // Feed one observation. gapPx is the (absolute) distance in physical
    // pixels between the bar's top edge and its monitor's top edge;
    // barRect is the bar's full screen rect (physical pixels);
    // monitorId is any value that changes when the bar changes monitor
    // (the caller passes the HMONITOR as an integer).
    ReserveAction update(bool toggleOn, int gapPx, IntRect barRect, long long monitorId) {
        if (!toggleOn) {
            if (reserved_) { reserved_ = false; return ReserveAction::Release; }
            return ReserveAction::None;
        }
        if (!reserved_) {
            if (gapPx <= kDockGapPx && barRect.w > 0 && barRect.h > 0) {
                reserved_ = true; rect_ = barRect; monitor_ = monitorId;
                return ReserveAction::Register;
            }
            return ReserveAction::None; // includes the 3..8 px dead zone: no docking from it
        }
        if (gapPx > kUndockGapPx) { reserved_ = false; return ReserveAction::Release; }
        if (barRect != rect_ || monitorId != monitor_) {
            rect_ = barRect; monitor_ = monitorId;
            return ReserveAction::Update;
        }
        return ReserveAction::None;
    }

    bool reserved() const { return reserved_; }

private:
    bool reserved_ = false;
    IntRect rect_{};
    long long monitor_ = 0;
};
} // namespace meterbar
