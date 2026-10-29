#include "ScreenSpace.h"
#include <cstdio>
// v0.18 — READ THIS BEFORE TOUCHING THE GUARD (the v0.14 lesson, applied
// on purpose this time): this translation unit deliberately includes NO
// JUCE header — it is driven entirely through void* window handles the
// caller passes in. JUCE_WINDOWS exists only inside JUCE's own headers,
// so a `#if JUCE_WINDOWS` guard here would read 0 in EVERY build and the
// stub branch below would ship on Windows — that exact bug kept WASAPI
// loopback out of every binary from v0.10 to v0.13. The guard therefore
// tests the compiler's own predefined target macro: MSVC and MinGW both
// predefine _WIN32 for Windows targets, no header needed. The build's
// verification greps THIS file's preprocessed Windows-target output for
// the "appbar active" status string, which exists only in the real
// branch — proof the branch is compiled in, not just present in source.
#if defined(_WIN32)
 #define METERBAR_SCREENSPACE_WINDOWS 1
#else
 #define METERBAR_SCREENSPACE_WINDOWS 0
#endif

#if METERBAR_SCREENSPACE_WINDOWS
#ifndef NOMINMAX
 #define NOMINMAX // keep windows.h's min/max macros out of the std:: calls below (the target also defines it; harmless twice)
#endif
 #include <windows.h>
 #include <shellapi.h>  // SHAppBarMessage / APPBARDATA
 #include <commctrl.h>  // SetWindowSubclass (AppBar callback messages)
 #include <dwmapi.h>    // DWMWA_CLOAKED (arrange sweep skips cloaked windows)

// ---- AppBar runtime state ----------------------------------------------
// MeterBar is a single-instance app (moreThanOneInstanceAllowed() ==
// false) with exactly one dockable window, so the one live registration
// is kept at file scope: the subclass callback below is a plain function
// pointer and needs somewhere to stand. The ScreenSpace methods are the
// only writers.
namespace {
struct AppBarState {
    HWND hwnd = nullptr;
    UINT callbackMsg = WM_APP + 0x4D; // private app message for ABN_* notices
    bool registered = false;
    UINT edge = ABE_TOP; // v0.19.1: the ABE_* edge of the live registration
    RECT requested{}; // the rect the CALLER last asked for — idempotence is judged on THIS
    RECT applied{};   // the rect Windows last accepted via ABM_SETPOS (may be trimmed)
};
AppBarState g_appBar;

// v0.19.1: the registration edge is data, not a constant — the bar may
// be docked to any edge of its monitor (Jackson's is at the BOTTOM).
UINT abeFromEdge(meterbar::ReserveEdge e) {
    switch (e) {
        case meterbar::ReserveEdge::Bottom: return ABE_BOTTOM;
        case meterbar::ReserveEdge::Left:   return ABE_LEFT;
        case meterbar::ReserveEdge::Right:  return ABE_RIGHT;
        case meterbar::ReserveEdge::Top:    break;
    }
    return ABE_TOP;
}
const char* edgeName(meterbar::ReserveEdge e) {
    switch (e) {
        case meterbar::ReserveEdge::Bottom: return "BOTTOM";
        case meterbar::ReserveEdge::Left:   return "LEFT";
        case meterbar::ReserveEdge::Right:  return "RIGHT";
        case meterbar::ReserveEdge::Top:    break;
    }
    return "TOP";
}
// The strip's thickness on its strip axis (height for top/bottom,
// width for left/right) — for the status line.
long stripThickness(const RECT& r, UINT abe) {
    return (abe == ABE_LEFT || abe == ABE_RIGHT) ? (r.right - r.left) : (r.bottom - r.top);
}

// Ask for the position and set it ONLY if the system-granted rect
// differs from what is already applied. v0.18's ABN_POSCHANGED handler
// called ABM_SETPOS unconditionally — and an ABM_SETPOS generates fresh
// ABN_POSCHANGED notifications, so the handler could re-trigger itself
// and keep the work area (and every maximised window sized against it)
// recalculating in a loop: a second, self-sustaining flicker path on
// top of the timer poll. With the equality gate, a notification that
// changes nothing costs one ABM_QUERYPOS and zero window movement.
void setAppBarPosIfChanged() {
    APPBARDATA abd{}; abd.cbSize = sizeof abd; abd.hWnd = g_appBar.hwnd;
    abd.uEdge = g_appBar.edge; abd.rc = g_appBar.requested;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    // Re-anchor the coordinates that ARE the ask — the edge-adjacent
    // side and the strip's start along the monitor — and let the
    // system trim only the far side. (v0.18.1 restored left/top, which
    // was the whole story for ABE_TOP; each edge has its own pair.)
    switch (g_appBar.edge) {
        case ABE_BOTTOM: abd.rc.left = g_appBar.requested.left; abd.rc.bottom = g_appBar.requested.bottom; break;
        case ABE_LEFT:   abd.rc.left = g_appBar.requested.left; abd.rc.top = g_appBar.requested.top; break;
        case ABE_RIGHT:  abd.rc.right = g_appBar.requested.right; abd.rc.top = g_appBar.requested.top; break;
        default:         abd.rc.left = g_appBar.requested.left; abd.rc.top = g_appBar.requested.top; break; // ABE_TOP
    }
    if (abd.rc.left == g_appBar.applied.left && abd.rc.top == g_appBar.applied.top &&
        abd.rc.right == g_appBar.applied.right && abd.rc.bottom == g_appBar.applied.bottom)
        return; // already exactly this bar — touching it again only broadcasts noise
    SHAppBarMessage(ABM_SETPOS, &abd);
    g_appBar.applied = abd.rc;
}

// ABN_POSCHANGED: another bar (or the taskbar) changed the available
// space — the documented response is to re-ask; we re-SET only when the
// answer actually moved us (see setAppBarPosIfChanged above).
LRESULT CALLBACK appBarSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId; (void)refData;
    if (msg == g_appBar.callbackMsg && wp == ABN_POSCHANGED && g_appBar.registered)
        setAppBarPosIfChanged();
    return DefSubclassProc(hwnd, msg, wp, lp);
}
} // namespace

struct ScreenSpace::Impl {
    std::string status = "not registered";
    std::vector<SavedWindow> saved;
};

bool ScreenSpace::reserveEdge(void* hwndV, meterbar::ReserveEdge edge,
                              int physX, int physY, int physW, int physH) {
    HWND hwnd = static_cast<HWND>(hwndV);
    if (!hwnd || physW <= 0 || physH <= 0) return g_appBar.registered;
    const RECT want{ physX, physY, physX + physW, physY + physH };
    const UINT abe = abeFromEdge(edge);
    // THE v0.18 FLICKER, fixed: idempotence is judged on the REQUESTED
    // rect (and, v0.19.1, the requested edge). v0.18 compared `want`
    // against g_appBar.applied — the rect Windows GRANTED, which
    // ABM_QUERYPOS is allowed to trim (far side adjusted to fit the
    // remaining space). Once trimmed, the two could never compare
    // equal, so every sync fell through to the release() + full
    // re-register below: ABM_REMOVE (work area expands, maximised
    // windows jump to full size) then ABM_SETPOS (work area shrinks,
    // they jump back) — at the UI timer's ~4 Hz.
    if (g_appBar.registered && g_appBar.hwnd == hwnd && g_appBar.edge == abe &&
        want.left == g_appBar.requested.left && want.top == g_appBar.requested.top &&
        want.right == g_appBar.requested.right && want.bottom == g_appBar.requested.bottom)
        return true; // already exactly this bar — nothing to do
    if (g_appBar.registered) release(); // moved / resized / other monitor / edge switch: clean re-register
    APPBARDATA abd{}; abd.cbSize = sizeof abd; abd.hWnd = hwnd;
    abd.uCallbackMessage = g_appBar.callbackMsg;
    if (!SHAppBarMessage(ABM_NEW, &abd)) {
        impl_->status = "appbar registration refused by Windows";
        return false;
    }
    g_appBar.hwnd = hwnd;
    g_appBar.edge = abe;
    g_appBar.requested = want;
    g_appBar.applied = RECT{}; // nothing applied yet — setAppBarPosIfChanged always places the first position
    SetWindowSubclass(hwnd, appBarSubclassProc, 1, 0);
    g_appBar.registered = true; // live BEFORE the first SETPOS, so a POSCHANGED landing mid-call sees a consistent bar
    setAppBarPosIfChanged();
    char buf[96];
    std::snprintf(buf, sizeof buf, "appbar active - %s strip %ld px reserved",
                  edgeName(edge), (long)stripThickness(g_appBar.applied, abe));
    impl_->status = buf;
    return true;
}

void ScreenSpace::release() {
    if (!g_appBar.registered || !g_appBar.hwnd) { g_appBar.registered = false; return; }
    APPBARDATA abd{}; abd.cbSize = sizeof abd; abd.hWnd = g_appBar.hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
    RemoveWindowSubclass(g_appBar.hwnd, appBarSubclassProc, 1);
    g_appBar.hwnd = nullptr;
    g_appBar.registered = false;
    g_appBar.edge = ABE_TOP;
    g_appBar.requested = RECT{};
    g_appBar.applied = RECT{};
    impl_->status = "not registered";
}

bool ScreenSpace::isReserved() const { return g_appBar.registered; }

// ---- Arrange sweep ------------------------------------------------------
namespace {
struct ArrangeMove { HWND hwnd; meterbar::IntRect from, to; };
struct ArrangeCtx {
    HMONITOR mon; HWND exclude; int strip;
    meterbar::ReserveEdge edge;
    meterbar::IntRect monRect;
    std::vector<ArrangeMove> moves;
};
BOOL CALLBACK arrangeEnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<ArrangeCtx*>(lp);
    if (hwnd == ctx->exclude || hwnd == GetShellWindow()) return TRUE;
    // Minimised windows have no meaningful on-screen rect; MAXIMISED
    // windows are deliberately left alone — the AppBar reservation owns
    // them (Windows sizes them against the shrunken work area).
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return TRUE;
    // Never fight the user: a window in an interactive move/size loop
    // (being dragged or edge-resized at this very moment) is skipped
    // outright. v0.18 had no such guard, so a sweep could yank the very
    // window the user was holding — part of the field report's
    // "stopping the user from interacting with the UI".
    {
        GUITHREADINFO gti{}; gti.cbSize = sizeof gti;
        const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
        if (tid && GetGUIThreadInfo(tid, &gti) && (gti.flags & GUI_INMOVESIZE) && gti.hwndActive == hwnd)
            return TRUE;
    }
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != ctx->mon) return TRUE;
    DWORD cloaked = 0; // other virtual desktops / suspended UWP ghosts
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return TRUE;
    if (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    const meterbar::IntRect wr{ r.left, r.top, r.right - r.left, r.bottom - r.top };
    if (wr.w <= 0 || wr.h <= 0) return TRUE;
    // Fit FIRST, move later: mutating windows mid-enumeration can disturb
    // the enumeration itself, so this pass only collects the plan.
    const meterbar::IntRect fit = meterbar::arrangeFit(wr, ctx->monRect, ctx->edge, ctx->strip);
    if (fit != wr) ctx->moves.push_back({ hwnd, wr, fit });
    return TRUE;
}
} // namespace

int ScreenSpace::arrange(void* hwndV, meterbar::ReserveEdge edge, int stripPhysPx) {
    HWND hwnd = static_cast<HWND>(hwndV);
    if (!hwnd || stripPhysPx <= 0) {
        impl_->status = "arrange: no strip to clear (dock the bar at a monitor edge first)";
        return 0;
    }
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) { impl_->status = "arrange: monitor lookup failed"; return 0; }
    ArrangeCtx ctx{ mon, hwnd, stripPhysPx, edge,
                    { mi.rcMonitor.left, mi.rcMonitor.top,
                      mi.rcMonitor.right - mi.rcMonitor.left,
                      mi.rcMonitor.bottom - mi.rcMonitor.top }, {} };
    EnumWindows(arrangeEnumProc, reinterpret_cast<LPARAM>(&ctx));
    int moved = 0;
    for (auto& mv : ctx.moves) {
        // Snapshots are WRITE-ONCE: a window whose original rect is
        // already recorded keeps that original. v0.18 cleared the whole
        // record at the top of every sweep, so a second sweep
        // "remembered" post-first-sweep positions as home (and windows
        // absent from the latest sweep were forgotten entirely) —
        // Restore could then never take them back: the field report's
        // "permanently moving the tabs". Only restore() clears this.
        bool known = false;
        for (auto& s : impl_->saved) if (s.hwnd == mv.hwnd) { known = true; break; }
        if (!known) impl_->saved.push_back({ mv.hwnd, mv.from });
        if (SetWindowPos(mv.hwnd, nullptr, mv.to.x, mv.to.y, mv.to.w, mv.to.h,
                         SWP_NOZORDER | SWP_NOACTIVATE))
            ++moved;
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "arranged %d window(s) clear of the bar (%s)", moved, edgeName(edge));
    impl_->status = buf;
    return moved;
}

int ScreenSpace::restore() {
    int n = 0;
    for (auto it = impl_->saved.rbegin(); it != impl_->saved.rend(); ++it) {
        HWND hwnd = static_cast<HWND>(it->hwnd);
        if (IsWindow(hwnd) && SetWindowPos(hwnd, nullptr, it->rect.x, it->rect.y,
                                            it->rect.w, it->rect.h,
                                            SWP_NOZORDER | SWP_NOACTIVATE))
            ++n;
    }
    impl_->saved.clear();
    char buf[96];
    std::snprintf(buf, sizeof buf, "restored %d window(s)", n);
    impl_->status = buf;
    return n;
}

std::string ScreenSpace::statusInfo() const { return impl_->status; }

#else // ---- non-Windows stub: same idiom as WindowsLoopback's stub ------
struct ScreenSpace::Impl {
    std::string status = "unavailable (non-Windows build)";
    std::vector<SavedWindow> saved;
};
bool ScreenSpace::reserveEdge(void*, meterbar::ReserveEdge, int, int, int, int) { return false; }
void ScreenSpace::release() {}
bool ScreenSpace::isReserved() const { return false; }
int ScreenSpace::arrange(void*, meterbar::ReserveEdge, int) { return 0; }
int ScreenSpace::restore() { return 0; }
std::string ScreenSpace::statusInfo() const { return impl_->status; }
#endif

ScreenSpace::ScreenSpace() : impl_(new Impl) {}
ScreenSpace::~ScreenSpace() { release(); delete impl_; }
