#pragma once
#include <string>
#include <vector>
#include <meterbar/ReserveState.hpp> // ReserveEdge (+ Arrange.hpp's IntRect)
// Screen-space management for the docked bar (v0.18, Jackson's ask):
//  1) RESERVE — register MeterBar's window as a Windows AppBar at the
//     edge of its monitor the bar is docked to, so Windows shrinks that
//     monitor's work area and maximised windows stop at the bar instead
//     of sliding under it. This is the mechanism the taskbar itself
//     uses; no other window is touched by hand.
//     v0.18.1: the CALLER drives this only on state transitions (see
//     meterbar::ReserveMachine — dock/undock hysteresis, event-fed);
//     nothing here is re-asserted from a timer.
//     v0.19.1: the edge is a parameter — v0.18/18.1 registered ABE_TOP
//     only, but Jackson's bar is docked at the BOTTOM of his screen,
//     so the reservation did nothing in his layout (MiniMeters
//     semantics: the reservation follows the bar's docked edge).
//  2) ARRANGE — a one-shot sweep that slides every other top-level
//     window on the bar's monitor into the area clear of the bar's
//     strip on its docked edge (fitting rule = meterbar::arrangeFit,
//     unit-tested in the DSP suite), with Restore putting them back
//     where they were. Runs ONLY when the user presses Arrange; never
//     from a timer or a sync path.
// Windows: the real implementation lives in ScreenSpace.cpp.
// Other platforms: every call is a no-op and statusInfo() says so — the
// same stub idiom as WindowsLoopback, so the UI never branches on
// platform and the feature reads honestly as unavailable there.
class ScreenSpace {
public:
    ScreenSpace();
    ~ScreenSpace();
    // Register hwnd as an AppBar on `edge` of its monitor, occupying
    // the given rect in PHYSICAL pixels (the full edge strip the
    // caller asks for — ReserveMachine::requestedRect; AppBar rects
    // are physical pixels, and per-monitor DPI means a logical/JUCE
    // rect would land at the wrong size).
    // Idempotent: a repeat call with the same edge and REQUESTED rect
    // does nothing. (v0.18 compared against the rect Windows GRANTED
    // after ABM_QUERYPOS, which the system may trim — that comparison
    // could never come back equal and every UI-timer poll tore the
    // AppBar down and rebuilt it: the flicker. See BUILD_LOG v0.18.1.)
    // A different edge or requested rect (moved / resized / other
    // monitor / edge switch) re-registers cleanly. False = refused.
    bool reserveEdge(void* hwnd, meterbar::ReserveEdge edge,
                     int physX, int physY, int physW, int physH);
    void release(); // ABM_REMOVE; safe to call at any time
    bool isReserved() const;
    struct SavedWindow { void* hwnd; meterbar::IntRect rect; };
    // Fit every other top-level window on hwnd's monitor into the area
    // clear of a strip of stripPhysPx pixels along `edge` of that
    // monitor (the caller passes the bar's far edge measured from that
    // monitor edge). Skips: the bar itself, the shell, minimised
    // windows, maximised windows (the AppBar above owns those),
    // cloaked windows (other virtual desktops / UWP ghosts), tool
    // windows, and any window currently in an interactive move/size
    // loop (never fight the user's own drag). Original rects are
    // remembered for restore(); once a window's original is recorded
    // it is NEVER overwritten by a later sweep (write-once) — only
    // restore() clears the snapshots, and it stays available after
    // any sweep. Returns how many windows were actually moved.
    int arrange(void* hwnd, meterbar::ReserveEdge edge, int stripPhysPx);
    int restore(); // put the remembered windows back; returns the count
    std::string statusInfo() const; // one status line for the Debug window
private:
    struct Impl; Impl* impl_ = nullptr;
};
