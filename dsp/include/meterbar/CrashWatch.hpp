#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
namespace meterbar {
// CrashWatch (v0.28.3) — the pure naming/notice logic behind
// MeterBar's hard-crash record (family crash-log round, Jackson
// 2026-11-03: every bar leaves a sendable crash record + an "Open
// Log Folder" button). The app side (Windows: unhandled-exception
// filter + MiniDumpWriteDump) lives in app/Source/CrashHandler.h;
// everything DECIDABLE lives here so the suite can pin it:
//
//  - Crash files are named crash-YYYY-MM-DD_HHMMSS.txt (+ a sibling
//    .dmp written by the handler). The text file is the marker the
//    next-launch notice keys on.
//  - A crash counts as SEEN once a sibling "<name>.seen" file
//    exists (written when Jackson dismisses the notice or opens
//    the folder from it). A crash file with no .seen sibling is
//    UNSEEN and triggers exactly one notice, however many launches
//    it survives.
//  - The record text is composed here from plain fields, so what a
//    crash file contains is suite-pinned, not handler-improvised.

// Broken-down local time, supplied by the caller (the core never
// reads a clock — the same discipline as the rest of the suite).
struct CrashTimeParts { int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0; };

inline std::string crashTimeText(const CrashTimeParts& t) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d_%02d%02d%02d",
                  t.year, t.month, t.day, t.hour, t.minute, t.second);
    return buf;
}
inline std::string crashFileStem(const CrashTimeParts& t) {
    return "crash-" + crashTimeText(t);
}
inline std::string crashTextName(const CrashTimeParts& t) { return crashFileStem(t) + ".txt"; }
inline std::string crashDumpName(const CrashTimeParts& t) { return crashFileStem(t) + ".dmp"; }

inline bool isCrashTextName(const std::string& name) {
    // crash-YYYY-MM-DD_HHMMSS.txt — lengths: stem 23 ("crash-" 6 +
    // date 10 + "_" 1 + time 6), + ".txt" = 27.
    return name.size() == 27 && name.rfind("crash-", 0) == 0
        && name.compare(name.size() - 4, 4, ".txt") == 0;
}
inline bool isSeenMarkerName(const std::string& name) {
    // crash text name (27) + ".seen" (5) = 32.
    return name.size() == 32 && name.compare(name.size() - 5, 5, ".seen") == 0
        && isCrashTextName(name.substr(0, name.size() - 5));
}
inline std::string seenMarkerFor(const std::string& crashTextName) { return crashTextName + ".seen"; }

// Given a folder's file names, the crash records not yet seen —
// sorted oldest first (name order IS time order for this scheme).
inline std::vector<std::string> unseenCrashes(const std::vector<std::string>& fileNames) {
    std::vector<std::string> out;
    for (auto& n : fileNames) {
        if (!isCrashTextName(n)) continue;
        const std::string seen = seenMarkerFor(n);
        if (std::find(fileNames.begin(), fileNames.end(), seen) == fileNames.end())
            out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --- v0.28.4: WER LocalDumps harvest --------------------------------
// The in-process handler never fires for heap-corruption deaths:
// ntdll fast-fail goes straight to WER (field finding, 2026-11-03),
// so for exactly that crash class the ONLY record is the dump WER
// writes to %LOCALAPPDATA%\CrashDumps. On launch the app copies each
// NEW MeterBar dump from there into the log folder under a
// crash-record name (timestamp = the dump's last-write time), where
// the next-launch notice + Open Log Folder flow above surfaces it
// like any handler-written record. The decidable parts live here:
// which source names are ours, how a source dump is keyed (so it is
// copied exactly once), and which destination stem it gets when
// its timestamp collides with an existing record.

// WER LocalDumps names a per-app dump "MeterBar.exe.<pid>.dmp"
// (or "MeterBar.exe.dmp" with no pid suffix). Anything else in
// that folder — other apps' dumps, our handler's crash-* names —
// is not a harvest source.
inline bool isWerDumpName(const std::string& name) {
    static const std::string bare = "MeterBar.exe.dmp";
    static const std::string prefix = "MeterBar.exe.";
    if (name == bare) return true;
    if (name.rfind(prefix, 0) != 0) return false;
    if (name.size() <= prefix.size() + 4) return false;
    if (name.compare(name.size() - 4, 4, ".dmp") != 0) return false;
    const std::string mid = name.substr(prefix.size(), name.size() - prefix.size() - 4);
    if (mid.empty()) return false;
    for (char c : mid) if (c < '0' || c > '9') return false;
    return true;
}

// Identity of one source dump: name + size + last-write time. A
// dump that reappears with the same identity was already
// harvested; a later crash writes a new file (new pid in the name
// and/or a new write time), so it keys differently and harvests.
inline std::string werHarvestKey(const std::string& name, long long sizeBytes,
                                 const CrashTimeParts& mtime) {
    return name + "|" + std::to_string(sizeBytes) + "|" + crashTimeText(mtime);
}

// The harvest log is one key per line. Membership is whole-line.
inline bool werHarvestKeySeen(const std::string& logText, const std::string& key) {
    std::string::size_type pos = 0;
    while (pos <= logText.size()) {
        const auto nl = logText.find('\n', pos);
        const auto line = logText.substr(pos, nl == std::string::npos ? nl : nl - pos);
        if (line == key) return true;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}

// +1 second with full carry (minute/hour/day/month/year, leap
// years included) — a harvested dump's stem is its write time,
// and a same-second sibling record must be bumped, not clobbered.
inline CrashTimeParts addOneSecond(CrashTimeParts t) {
    static const int daysInMonth[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    auto leap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); };
    if (++t.second < 60) return t;
    t.second = 0;
    if (++t.minute < 60) return t;
    t.minute = 0;
    if (++t.hour < 24) return t;
    t.hour = 0;
    int dim = daysInMonth[(t.month >= 1 && t.month <= 12) ? t.month - 1 : 0];
    if (t.month == 2 && leap(t.year)) dim = 29;
    if (++t.day <= dim) return t;
    t.day = 1;
    if (++t.month <= 12) return t;
    t.month = 1;
    ++t.year;
    return t;
}

// The destination stem for a harvested dump written at `t`: the
// plain timestamp stem, bumped one second at a time until neither
// the .txt nor the .dmp of that stem exists in the log folder.
inline std::string harvestStem(const std::vector<std::string>& existingNames, CrashTimeParts t) {
    for (;;) {
        const std::string stem = crashFileStem(t);
        const bool clash =
            std::find(existingNames.begin(), existingNames.end(), stem + ".txt") != existingNames.end() ||
            std::find(existingNames.begin(), existingNames.end(), stem + ".dmp") != existingNames.end();
        if (!clash) return stem;
        t = addOneSecond(t);
    }
}

// The crash record's text. `stateSnapshot` is the app's last honest
// state (pre-formatted "key: value" lines maintained by the app at
// runtime — the handler itself formats nothing beyond this plain
// composition, which also runs at install time in tests).
inline std::string crashRecordText(const std::string& appVersion,
                                   const std::string& windowsVersion,
                                   const std::string& whenText,
                                   unsigned long exceptionCode,
                                   const std::string& exceptionAddressHex,
                                   const std::string& stateSnapshot) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lx", exceptionCode);
    std::string out = "MeterBar crash record\n";
    out += "app version: " + appVersion + "\n";
    out += "windows: " + windowsVersion + "\n";
    out += "when: " + whenText + "\n";
    out += "exception: " + std::string(code) + " at " + exceptionAddressHex + "\n";
    out += "state at crash (last known):\n";
    out += stateSnapshot.empty() ? "(none recorded)\n" : stateSnapshot;
    if (out.empty() || out.back() != '\n') out += "\n";
    out += "Send the .txt and the matching .dmp from this folder to Milo.\n";
    return out;
}
} // namespace meterbar
