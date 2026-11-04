#pragma once
#include <string>
// v0.28.7: the pinned-app audio source (appSource 4) — pure policy
// for MeterBar's per-exe pin. The user pins ONE executable (Jackson's
// case: Ableton Live); while pinned, MeterBar's EXISTING per-process
// capture attaches to that exe's process tree whenever it appears
// and hears silence while it is absent — never a fallback to another
// app or the system mix (isolation is the point of the feature).
// This header holds only the testable policy: exe-name handling
// (Windows process matching is by exe NAME, case-insensitively —
// the same rule the Spotify/Tidal resolver in WindowsLoopback uses)
// and the attach/detach watch machine the UI timer drives.
// Activation itself stays in WindowsLoopback, reused unchanged.
namespace meterbar {

// The exe NAME part of a path or name: both separator kinds are
// accepted (a FileChooser hands back a full path), surrounding
// whitespace is trimmed. A bare name passes through unchanged.
inline std::string exeBaseName(const std::string& pathOrName) {
    const auto begin = pathOrName.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = pathOrName.find_last_not_of(" \t\r\n");
    std::string s = pathOrName.substr(begin, end - begin + 1);
    const auto sep = s.find_last_of("\\/");
    if (sep != std::string::npos) s = s.substr(sep + 1);
    return s;
}

inline std::string lowerAscii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Display label for an exe name: the base name without its ".exe"
// suffix (matched case-insensitively), e.g. "Ableton Live 12 Suite".
// Names without the suffix pass through whole.
inline std::string pinDisplayLabel(const std::string& exeName) {
    std::string base = exeBaseName(exeName);
    if (base.size() > 4 && lowerAscii(base.substr(base.size() - 4)) == ".exe")
        base = base.substr(0, base.size() - 4);
    return base;
}

// The resolver's match rule, stated once: two exe references match
// when their base names are equal case-insensitively.
inline bool exeNamesMatch(const std::string& a, const std::string& b) {
    const std::string x = exeBaseName(a), y = exeBaseName(b);
    return !x.empty() && lowerAscii(x) == lowerAscii(y);
}

// The watch machine the UI timer steps about once a second while
// pinned mode is on. Inputs per step: whether the pinned process is
// running, whether the capture currently reports itself running
// (already hearing that process — e.g. the startup applySource
// attached before the first step), and whether the settle window
// since the last source action has elapsed (activation takes time;
// never restart an in-flight activation). At most ONE action per
// appearance/disappearance edge. A failed activation leaves
// `attached` set, so the watcher does not churn the crash-sensitive
// activation path — it reads exactly like the existing picker
// (status line says why; re-picking is the user's retry). A
// quit+relaunch inside a single step is indistinguishable from a
// quiet app — the same property the Spotify/Tidal sources have.
enum class PinAction { None, Attach, Detach };
struct PinWatch {
    bool attached = false; // our belief: capture is bound to the pin
    PinAction step(bool processRunning, bool captureRunning, bool settleElapsed) {
        if (processRunning && captureRunning) { attached = true; return PinAction::None; }
        if (!processRunning) {
            if (attached) { attached = false; return PinAction::Detach; }
            return PinAction::None; // absent and unattached: the waiting state
        }
        if (!attached && settleElapsed) { attached = true; return PinAction::Attach; }
        return PinAction::None;
    }
};

} // namespace meterbar
