#pragma once
#include <string>
#include <vector>
#include <meterbar/Arrange.hpp>
// Screen-space management for the docked bar (v0.18, Jackson's ask):
//  1) RESERVE — register MeterBar's window as a Windows AppBar at the top
//     edge of its monitor, so Windows shrinks that monitor's work area
//     and maximised windows stop below the bar. This is the mechanism
//     the taskbar itself uses; no other window is touched by hand.
//     v0.18.1: the CALLER drives this only on state transitions (see
//     meterbar::ReserveMachine — dock/undock hysteresis, event-fed);
//     nothing here is re-asserted from a timer.
//  2) ARRANGE — a one-shot sweep that slides every other top-level
//     window on the bar's monitor into the area below the bar (fitting
//     rule = meterbar::arrangeFit, unit-tested in the DSP suite), with
//     Restore putting them back where they were. Runs ONLY when the
//     user presses Arrange; never from a timer or a sync path.
// Windows: the real implementation lives in ScreenSpace.cpp.
// Other platforms: every call is a no-op and statusInfo() says so — the
// same stub idiom as WindowsLoopback, so the UI never branches on
// platform and the feature reads honestly as unavailable there.
class ScreenSpace {
public:
    ScreenSpace();
    ~ScreenSpace();
    // Register hwnd as a top-edge AppBar occupying the given rect in
    // PHYSICAL pixels (the window's current screen rect, e.g. from
    // GetWindowRect — AppBar rects are physical pixels, and per-monitor
    // DPI means a logical/JUCE rect would land at the wrong size).
    // Idempotent: a repeat call with the same REQUESTED rect does
    // nothing. (v0.18 compared against the rect Windows GRANTED after
    // ABM_QUERYPOS, which the system may trim — that comparison could
    // never come back equal and every UI-timer poll tore the AppBar
    // down and rebuilt it: the flicker. See BUILD_LOG v0.18.1.)
    // A different requested rect (moved / resized / other monitor)
    // re-registers cleanly. False = refused.
    bool reserveTop(void* hwnd, int physX, int physY, int physW, int physH);
    void releaseTop(); // ABM_REMOVE; safe to call at any time
    bool isReserved() const;
    struct SavedWindow { void* hwnd; meterbar::IntRect rect; };
    // Fit every other top-level window on hwnd's monitor into the area
    // below a strip of stripPhysPx pixels at that monitor's top (the
    // caller passes the bar's bottom edge measured from the monitor's
    // top). Skips: the bar itself, the shell, minimised windows,
    // maximised windows (the AppBar above owns those), cloaked windows
    // (other virtual desktops / UWP ghosts), tool windows, and any
    // window currently in an interactive move/size loop (never fight
    // the user's own drag). Original rects are remembered for
    // restore(); once a window's original is recorded it is NEVER
    // overwritten by a later sweep (write-once) — only restore()
    // clears the snapshots, and it stays available after any sweep.
    // Returns how many windows were actually moved.
    int arrange(void* hwnd, int stripPhysPx);
    int restore(); // put the remembered windows back; returns the count
    std::string statusInfo() const; // one status line for the Debug window
private:
    struct Impl; Impl* impl_ = nullptr;
};
