#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include "Arrange.hpp"    // IntRect
#include "WindowGeom.hpp" // FrameInsets
namespace meterbar {
// DOCK DIAGNOSTIC RECORDS (v0.28.15) — the DIAGNOSTIC BUILD's log
// format. Round 5 (2026-11-12) could not reproduce the field's
// ~196 px Reposition -> Top offset in any model of the app: every
// in-app mover was exonerated, and the two terms of that scale (a
// live sibling's bottom edge; the display's totalArea origin) are
// each contradicted by a field fact. Jackson authorised a log-only
// build ("log build, i think you need more data", 2026-11-12
// 22:11 PT): the app OBSERVES its own dock clicks and writes one
// JSONL record per observation to Documents\MeterBar\dock-diag.jsonl
// (the app side lives in app/Source/DockDiag.h; it only appends).
//
// This header is the pure half, per the family division of labour:
// record composition, JSON escaping, the post-click sample schedule
// and the rect-drift rule — no OS calls, no JUCE, so the suite can
// grade the format exactly. The app feeds it values it read live.
//
// PURE OBSERVATION CONTRACT: nothing in this header or its app-side
// consumer moves a window, arms a reservation, or alters any dock
// input. The sampler READS the window rect; that is all.

// One display as the app enumerated it. `name` is the OS device name
// (Windows: MONITORINFOEX szDevice for the same monitor); JUCE's own
// Display struct carries NO name field (verified in the vendored
// JUCE 8.0.4 juce_Displays.h), so on platforms without an OS name
// this stays empty — the index is JUCE's array order, which is the
// identity JUCE itself reports.
struct DiagDisplay {
    int index = -1;
    std::string name;
    bool isMain = false;
    IntRect totalArea{};
    IntRect userArea{};
    double scale = 1.0;
    double dpi = 0.0;
    int topLeftPhysicalX = 0, topLeftPhysicalY = 0;
};

// The foreground window at an observation instant (the fullscreen-
// game condition is the standing suspect for the un-modelled layer).
struct DiagFg {
    bool available = false; // false off Windows / no foreground window
    std::string title;
    std::string className;
    std::string exe;
};

// JSON string escaping — window titles are attacker-grade input
// (arbitrary app text: quotes, backslashes, control characters), so
// every free-text field goes through this before it is quoted.
inline std::string diagJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

inline std::string diagQuoted(const std::string& s) { return "\"" + diagJsonEscape(s) + "\""; }

inline std::string diagRectJson(const IntRect& r) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", r.x, r.y, r.w, r.h);
    return std::string(buf);
}

// Scale/dpi formatting: up to 3 decimals, trailing zeros trimmed
// (1 -> "1", 1.25 -> "1.25", 1.5 -> "1.5") — stable for the suite.
inline std::string diagDoubleJson(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

inline std::string diagDisplayJson(const DiagDisplay& d) {
    std::string s = "{";
    s += "\"index\":" + std::to_string(d.index);
    s += ",\"name\":" + diagQuoted(d.name);
    s += ",\"isMain\":" + std::string(d.isMain ? "true" : "false");
    s += ",\"totalArea\":" + diagRectJson(d.totalArea);
    s += ",\"userArea\":" + diagRectJson(d.userArea);
    s += ",\"scale\":" + diagDoubleJson(d.scale);
    s += ",\"dpi\":" + diagDoubleJson(d.dpi);
    char buf[64];
    std::snprintf(buf, sizeof buf, ",\"topLeftPhysical\":{\"x\":%d,\"y\":%d}", d.topLeftPhysicalX, d.topLeftPhysicalY);
    s += buf;
    s += "}";
    return s;
}

inline std::string diagFgJson(const DiagFg& f) {
    std::string s = "{";
    s += "\"available\":" + std::string(f.available ? "true" : "false");
    s += ",\"title\":" + diagQuoted(f.title);
    s += ",\"class\":" + diagQuoted(f.className);
    s += ",\"exe\":" + diagQuoted(f.exe);
    s += "}";
    return s;
}

inline std::string diagHead(const char* type, const std::string& isoTs, long long epochMs) {
    std::string s = "{\"type\":";
    s += diagQuoted(type);
    s += ",\"ts\":" + diagQuoted(isoTs);
    s += ",\"epochMs\":" + std::to_string(epochMs);
    return s;
}

// Startup record: the full display enumeration JUCE sees, once per
// launch. `appVersion` is the compiled-in version string.
inline std::string diagStartupRecord(const std::string& isoTs, long long epochMs,
                                     const std::string& appVersion,
                                     const std::vector<DiagDisplay>& displays) {
    std::string s = diagHead("startup", isoTs, epochMs);
    s += ",\"appVersion\":" + diagQuoted(appVersion);
    s += ",\"displays\":[";
    for (size_t i = 0; i < displays.size(); ++i) {
        if (i) s += ",";
        s += diagDisplayJson(displays[i]);
    }
    s += "]}";
    return s;
}

// Everything doTopBottom read and decided for one click. The record
// is composed BEFORE the dock's own sync runs, from values captured
// live inside the function; composing/appending it is the only
// thing the diagnostic adds there.
struct DiagClick {
    std::string edge;            // "top" | "bottom"
    IntRect windowRectBefore{};
    DiagDisplay display{};       // the resolved display (index per JUCE)
    bool insetsMeasured = false; // false: no peer / no DWM answer (v0.28.10 fallback)
    FrameInsets insetsPhys{};    // as measured, physical px (valid when insetsMeasured)
    FrameInsets insetsLogical{}; // as converted and fed to dockedBarWindowRect
    IntRect targetRect{};        // dockedBarWindowRect's output
    IntRect requestedRect{};     // the rect actually passed to setBounds
    IntRect rectAfterSetBounds{};// read back immediately after setBounds returned
    bool linkEnabled = false;
    bool stick = false;
    DiagFg foreground{};
};

inline std::string diagInsetsJson(const FrameInsets& i) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "{\"top\":%d,\"bottom\":%d,\"left\":%d,\"right\":%d}",
                  i.top, i.bottom, i.left, i.right);
    return std::string(buf);
}

inline std::string diagClickRecord(const std::string& isoTs, long long epochMs, const DiagClick& c) {
    std::string s = diagHead("dock-click", isoTs, epochMs);
    s += ",\"edge\":" + diagQuoted(c.edge);
    s += ",\"windowRectBefore\":" + diagRectJson(c.windowRectBefore);
    s += ",\"display\":" + diagDisplayJson(c.display);
    s += ",\"insetsMeasured\":" + std::string(c.insetsMeasured ? "true" : "false");
    s += ",\"insetsPhys\":" + diagInsetsJson(c.insetsPhys);
    s += ",\"insetsLogical\":" + diagInsetsJson(c.insetsLogical);
    s += ",\"targetRect\":" + diagRectJson(c.targetRect);
    s += ",\"requestedRect\":" + diagRectJson(c.requestedRect);
    s += ",\"rectAfterSetBounds\":" + diagRectJson(c.rectAfterSetBounds);
    s += ",\"linkEnabled\":" + std::string(c.linkEnabled ? "true" : "false");
    s += ",\"stick\":" + std::string(c.stick ? "true" : "false");
    s += ",\"foreground\":" + diagFgJson(c.foreground);
    s += "}";
    return s;
}

// The post-click sample schedule: nominal offsets after a click at
// which the sampler reads the actual window rect. +300 ms is the
// reference (the click's own landing, past the deferred sync); a
// later sample that differs from it is a post-click actor's work.
inline const long long kDiagSampleOffsetsMs[] = { 300, 1000, 2000, 3000 };
inline const int kDiagSampleCount = 4;

// How many samples are due by `nowMs` for a click at `clickMs`,
// given `alreadyDone` were taken. Monotonic, capped at the schedule
// size; a stalled UI thread takes the missed samples back-to-back
// on its next tick (each record carries its actual offset).
inline int diagSamplesDue(long long clickMs, long long nowMs, int alreadyDone) {
    int due = 0;
    for (int i = 0; i < kDiagSampleCount; ++i)
        if (nowMs - clickMs >= kDiagSampleOffsetsMs[i]) due = i + 1;
    if (due < alreadyDone) due = alreadyDone;
    if (due > kDiagSampleCount) due = kDiagSampleCount;
    return due;
}

// Any difference at all counts: the field offset is ~196 px, but a
// small late shift is also evidence (a fight would show both).
inline bool diagRectMoved(const IntRect& a, const IntRect& b) { return a != b; }

inline std::string diagSampleRecord(const std::string& isoTs, long long epochMs,
                                    long long clickEpochMs, int sampleIndex,
                                    long long nominalOffsetMs, long long actualOffsetMs,
                                    const IntRect& rect, bool hasReference, bool movedSinceFirst,
                                    const DiagFg& fg) {
    std::string s = diagHead("dock-sample", isoTs, epochMs);
    s += ",\"clickEpochMs\":" + std::to_string(clickEpochMs);
    s += ",\"sampleIndex\":" + std::to_string(sampleIndex);
    s += ",\"nominalOffsetMs\":" + std::to_string(nominalOffsetMs);
    s += ",\"actualOffsetMs\":" + std::to_string(actualOffsetMs);
    s += ",\"rect\":" + diagRectJson(rect);
    s += ",\"movedSinceFirst\":";
    s += hasReference ? (movedSinceFirst ? "true" : "false") : "null";
    s += ",\"foreground\":" + diagFgJson(fg);
    s += "}";
    return s;
}

// Emitted once per click, the first time a later sample's rect
// differs from the +300 ms reference — the greppable form of
// "something pushed the bar after it landed".
inline std::string diagDriftRecord(const std::string& isoTs, long long epochMs,
                                   long long clickEpochMs, long long actualOffsetMs,
                                   const IntRect& fromRect, const IntRect& toRect,
                                   const DiagFg& fg) {
    std::string s = diagHead("rect-drift", isoTs, epochMs);
    s += ",\"clickEpochMs\":" + std::to_string(clickEpochMs);
    s += ",\"actualOffsetMs\":" + std::to_string(actualOffsetMs);
    s += ",\"fromRect\":" + diagRectJson(fromRect);
    s += ",\"toRect\":" + diagRectJson(toRect);
    s += ",\"foreground\":" + diagFgJson(fg);
    s += "}";
    return s;
}
} // namespace meterbar
