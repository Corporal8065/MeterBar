#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
namespace meterbar {
// RECORDINGS MENU MODEL (v0.21.4) — the pure half of the hamburger
// menu's Recordings submenu. The app scans Documents\MeterBar on
// every menu open (no cached list) and hands the raw facts here;
// the plan filters to recording names, drops the file currently
// being written, sorts newest first and caps the list. Pure: the
// DSP suite grades it exactly, the app only renders the answer.
//
// Names, from the two writers: toggleRecord writes
// record-YYYYMMDD-HHMMSS.wav; exportClip writes clip<secs>s-<n>.wav.
// Anything else in the folder is not the menu's business.
struct RecordingFileInfo {
    std::string name;
    long long mtimeMs = 0;
    long long sizeBytes = 0;
};
struct RecordingMenuEntry {
    std::string name;
    long long mtimeMs = 0;
    long long sizeBytes = 0;
    bool clip = false; // clip*.wav — an Audio Capture clip, not a take
};
inline bool isRecordingFileName(const std::string& n) {
    if (n.size() < 5) return false;
    std::string ext = n.substr(n.size() - 4);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext != ".wav") return false;
    return n.rfind("record-", 0) == 0 || n.rfind("clip", 0) == 0;
}
inline std::vector<RecordingMenuEntry> planRecordingsMenu(std::vector<RecordingFileInfo> files,
                                                          const std::string& activeName,
                                                          int cap = 25) {
    std::vector<RecordingMenuEntry> out;
    for (auto& f : files) {
        if (!isRecordingFileName(f.name)) continue;
        if (!activeName.empty() && f.name == activeName) continue; // never offer the half-written take
        out.push_back({ f.name, f.mtimeMs, f.sizeBytes, f.name.rfind("clip", 0) == 0 });
    }
    std::sort(out.begin(), out.end(), [](const RecordingMenuEntry& a, const RecordingMenuEntry& b) {
        if (a.mtimeMs != b.mtimeMs) return a.mtimeMs > b.mtimeMs; // newest first
        return a.name > b.name;                                    // deterministic on ties
    });
    if (cap >= 0 && (int)out.size() > cap) out.resize((size_t)cap); // the folder item covers the rest
    return out;
}
} // namespace meterbar
