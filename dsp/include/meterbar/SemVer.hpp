#pragma once
// SemVer — minimal semantic-version parse + compare for the v0.19
// updater (pure DSP-core header: std-only, unit-tested in test_dsp).
//
// Why this exists: the updater compares the compiled-in version
// against the latest GitHub release tag. That comparison MUST be
// numeric — as strings, "0.9.0" sorts AFTER "0.19.0" ('9' > '1'),
// which would tell a user on 0.9.0 that no update exists when 0.19.0
// is out (and the reverse would offer a downgrade as an upgrade).
//
// Accepted forms: "0.19.0", "v0.19.0", "0.19" (= 0.19.0), "1" (= 1.0.0).
// A leading 'v'/'V' is stripped (GitHub tags are "vX.Y.Z"). Anything
// after a '-' or '+' (pre-release / build metadata) is ignored for
// ordering — release tags in this project never carry them, and
// treating "0.19.0-rc1" as 0.19.0 is the conservative reading here.
// Empty or non-numeric input fails to parse (callers must handle it:
// an unparseable tag is NOT treated as an update).
#include <string>

namespace meterbar {

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    bool operator==(const SemVer& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
    bool operator!=(const SemVer& o) const { return !(*this == o); }
};

// Parses into out; returns false (and leaves out untouched) on bad input.
inline bool parseSemVer(const std::string& text, SemVer& out) {
    std::string s = text;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    // Ignore pre-release / build suffixes for ordering.
    const auto cut = s.find_first_of("-+");
    if (cut != std::string::npos) s.erase(cut);
    if (s.empty()) return false;
    int parts[3] = {0, 0, 0};
    int idx = 0;
    std::string::size_type i = 0;
    while (true) {
        if (idx > 2) return false; // more than three components
        const auto dot = s.find('.', i);
        const std::string chunk = s.substr(i, dot == std::string::npos ? dot : dot - i);
        if (chunk.empty()) return false;
        long value = 0;
        for (char c : chunk) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
            if (value > 1000000) return false; // sanity bound, not a real version
        }
        parts[idx++] = (int)value;
        if (dot == std::string::npos) break;
        i = dot + 1;
        if (i >= s.size()) return false; // trailing dot
    }
    out.major = parts[0]; out.minor = parts[1]; out.patch = parts[2];
    return true;
}

// -1 if a < b, 0 if equal, +1 if a > b. Numeric, component by component.
inline int compareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// Convenience: is `candidate` a strictly newer version than `current`?
// Unparseable input is never "newer" (a garbled tag must not trigger
// an update offer).
inline bool semVerIsNewer(const std::string& candidate, const std::string& current) {
    SemVer c, u;
    if (!parseSemVer(candidate, c) || !parseSemVer(current, u)) return false;
    return compareSemVer(c, u) > 0;
}

} // namespace meterbar
