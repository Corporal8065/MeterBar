#pragma once
// DockDiag (v0.28.15) — the DIAGNOSTIC BUILD's app-side plumbing:
// WHERE the records go and how the OS facts are read. All record
// FORMAT lives in the pure core (meterbar/DockDiag.hpp, suite-
// tested); this header only reads live values and appends lines.
//
// PURE OBSERVATION CONTRACT (round-6 contract): every function here
// READS — display enumeration, the foreground window, the window
// rect — and APPENDS to the log file. Nothing here moves a window,
// feeds the reserve machine, or touches a dock input. The sampler
// in MainComponent rides the existing UI timer; no new timer, no
// hooks, no interception.
#include <juce_gui_basics/juce_gui_basics.h>
#include <meterbar/DockDiag.hpp>
#include <string>
#include <vector>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace dockdiag {

// The one log file: Documents\MeterBar\dock-diag.jsonl — the app's
// normal per-user folder (crash records, capture log, presets live
// here too), append-only, survives restarts, and copyable by hand
// in a single drag. Exact Windows path on Jackson's PC:
// C:\Users\rpgja\Documents\MeterBar\dock-diag.jsonl
inline juce::File logFile() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("MeterBar").getChildFile("dock-diag.jsonl");
}

inline void appendRecord(const std::string& jsonLine) {
    auto f = logFile();
    f.getParentDirectory().createDirectory();
    f.appendText(juce::String(jsonLine) + "\n");
}

inline long long nowMs() { return (long long) juce::Time::getCurrentTime().toMilliseconds(); }
inline std::string isoNow() { return juce::Time::getCurrentTime().toISO8601(true).toStdString(); }

inline meterbar::IntRect toDiagRect(const juce::Rectangle<int>& r) {
    return { r.getX(), r.getY(), r.getWidth(), r.getHeight() };
}

#if JUCE_WINDOWS
// The OS device name (e.g. "\\.\DISPLAY2") for the monitor JUCE's
// rect describes. JUCE reports no name; the physical rect is built
// the way JUCE itself builds it (topLeftPhysical + totalArea size
// scaled), so MonitorFromRect names the same glass.
inline std::string deviceNameFor(const juce::Displays::Display& d) {
    RECT rc{};
    rc.left = d.topLeftPhysical.getX();
    rc.top = d.topLeftPhysical.getY();
    rc.right = rc.left + (int)(d.totalArea.getWidth() * d.scale + 0.5);
    rc.bottom = rc.top + (int)(d.totalArea.getHeight() * d.scale + 0.5);
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    if (!mon) return {};
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, reinterpret_cast<LPMONITORINFO>(&mi))) return {};
    return juce::String(mi.szDevice).toStdString();
}
#else
inline std::string deviceNameFor(const juce::Displays::Display&) { return {}; }
#endif

inline meterbar::DiagDisplay toDiagDisplay(const juce::Displays::Display& d, int index) {
    meterbar::DiagDisplay out;
    out.index = index;
    out.name = deviceNameFor(d);
    out.isMain = d.isMain;
    out.totalArea = toDiagRect(d.totalArea);
    out.userArea = toDiagRect(d.userArea);
    out.scale = d.scale;
    out.dpi = d.dpi;
    out.topLeftPhysicalX = d.topLeftPhysical.getX();
    out.topLeftPhysicalY = d.topLeftPhysical.getY();
    return out;
}

inline std::vector<meterbar::DiagDisplay> enumerateDisplays() {
    std::vector<meterbar::DiagDisplay> out;
    auto& all = juce::Desktop::getInstance().getDisplays().displays;
    for (int i = 0; i < all.size(); ++i) out.push_back(toDiagDisplay(all.getReference(i), i));
    return out;
}

// Which slot of JUCE's display array `disp` points at (-1 when it
// matches none). Pointer identity first (getDisplayForRect returns
// an element of this very array); geometry equality as fallback.
inline int displayIndex(const juce::Displays::Display* disp) {
    if (!disp) return -1;
    auto& all = juce::Desktop::getInstance().getDisplays().displays;
    for (int i = 0; i < all.size(); ++i) if (&all.getReference(i) == disp) return i;
    for (int i = 0; i < all.size(); ++i) {
        auto& d = all.getReference(i);
        if (d.totalArea == disp->totalArea && d.userArea == disp->userArea && d.isMain == disp->isMain) return i;
    }
    return -1;
}

// The foreground window at this instant — title / class / exe base
// name. Read-only Win32 queries; empty + unavailable off Windows.
inline meterbar::DiagFg foregroundInfo() {
    meterbar::DiagFg fg;
#if JUCE_WINDOWS
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return fg;
    fg.available = true;
    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, 512) > 0) fg.title = juce::String(title).toStdString();
    wchar_t cls[256] = {};
    if (GetClassNameW(hwnd, cls, 256) > 0) fg.className = juce::String(cls).toStdString();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t path[1024] = {}; DWORD len = 1024;
            if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
                juce::String full(path);
                fg.exe = full.fromLastOccurrenceOf("\\", false, false).toStdString();
            }
            CloseHandle(proc);
        }
    }
#endif
    return fg;
}

// Once per launch: version + every display JUCE sees.
inline void logStartup(const char* appVersion) {
    appendRecord(meterbar::diagStartupRecord(isoNow(), nowMs(),
                                             appVersion ? appVersion : "?",
                                             enumerateDisplays()));
}
} // namespace dockdiag
