#pragma once
// ReleaseParse — pull the fields MeterBar's updater needs out of a
// GitHub "latest release" API response (v0.19.2). Pure DSP-core
// header: std-only, unit-tested in test_dsp, following the SemVer.hpp
// pattern.
//
// Why this exists, and why it is NOT a general JSON parser: the
// updater used to read the response with juce::JSON inside UI code,
// which made the notes/asset extraction impossible to unit-test.
// This header extracts exactly four things — tag_name, body (the
// release notes), and the MeterBar-Setup.exe asset's download URL,
// size and digest — with a small, careful string scanner: keys are
// matched as whole quoted keys, string values are fully unescaped
// (\" \\ \/ \b \f \n \r \t and \uXXXX incl. surrogate pairs), and
// asset fields are only read inside the matching asset's own object
// span. Anything malformed fails the parse; the caller then reports
// an unreadable response instead of acting on half-read data.
//
// Also here: httpStatusBlocksAuth — GitHub answers 401/403/404 to
// unauthenticated calls against a PRIVATE repository (404 by design:
// a private repo "does not exist" if you are not signed in). Those
// three statuses are the updater's "private repo" signal.
#include <string>

namespace meterbar {

struct ReleaseFields {
    std::string tag;         // "tag_name", e.g. "v0.20.0"
    std::string notes;       // "body" — the release notes ("" when null/absent)
    bool hasAsset = false;   // an asset named exactly "MeterBar-Setup.exe" exists
    std::string assetUrl;    // its "browser_download_url"
    long long assetSize = 0; // its "size" in bytes (0 when absent)
    std::string assetDigest; // its "digest" ("sha256:<hex>") or "" when absent
};

// True for the HTTP statuses that mean "GitHub won't show this to an
// unauthenticated caller" — for this app: the repository is private.
inline bool httpStatusBlocksAuth(int status) {
    return status == 401 || status == 403 || status == 404;
}

namespace releaseparse_detail {

inline void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += (char) cp;
    } else if (cp < 0x800) {
        out += (char) (0xC0 | (cp >> 6));
        out += (char) (0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char) (0xE0 | (cp >> 12));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    } else {
        out += (char) (0xF0 | (cp >> 18));
        out += (char) (0x80 | ((cp >> 12) & 0x3F));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    }
}

inline bool parseHex4(const std::string& s, std::string::size_type pos, unsigned& value) {
    if (pos + 4 > s.size()) return false;
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = s[pos + (std::string::size_type) i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned) (c - 'A' + 10);
        else return false;
    }
    value = v;
    return true;
}

// Parses the JSON string whose opening quote is at s[pos]. On success
// `out` holds the unescaped value and `end` is one past the closing
// quote.
inline bool parseJsonString(const std::string& s, std::string::size_type pos,
                            std::string& out, std::string::size_type& end) {
    if (pos >= s.size() || s[pos] != '"') return false;
    std::string result;
    std::string::size_type i = pos + 1;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            out = result;
            end = i + 1;
            return true;
        }
        if (c == '\\') {
            if (i + 1 >= s.size()) return false;
            const char e = s[i + 1];
            switch (e) {
                case '"':  result += '"';  i += 2; break;
                case '\\': result += '\\'; i += 2; break;
                case '/':  result += '/';  i += 2; break;
                case 'b':  result += '\b'; i += 2; break;
                case 'f':  result += '\f'; i += 2; break;
                case 'n':  result += '\n'; i += 2; break;
                case 'r':  result += '\r'; i += 2; break;
                case 't':  result += '\t'; i += 2; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parseHex4(s, i + 2, cp)) return false;
                    i += 6;
                    if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate: pair it
                        unsigned low = 0;
                        if (i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u'
                            && parseHex4(s, i + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i += 6;
                        } else {
                            cp = 0xFFFD; // unpaired surrogate: replacement char
                        }
                    }
                    appendUtf8(result, cp);
                    break;
                }
                default: return false;
            }
        } else {
            result += c;
            ++i;
        }
    }
    return false; // unterminated string
}

// Finds `"key"` followed by ':' at or after `from` and before `bound`;
// returns the position of the value (whitespace skipped), or npos.
inline std::string::size_type findKeyValue(const std::string& s, const std::string& key,
                                           std::string::size_type from,
                                           std::string::size_type bound) {
    const std::string needle = "\"" + key + "\"";
    std::string::size_type pos = from;
    while (true) {
        pos = s.find(needle, pos);
        if (pos == std::string::npos || pos >= bound) return std::string::npos;
        std::string::size_type i = pos + needle.size();
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i < s.size() && s[i] == ':') {
            ++i;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
            return i < bound ? i : std::string::npos;
        }
        pos += needle.size(); // a string VALUE that equals the key text — keep looking
    }
}

inline bool findStringField(const std::string& s, const std::string& key,
                            std::string::size_type from, std::string::size_type bound,
                            std::string& out, std::string::size_type* valueEnd = nullptr) {
    const auto vpos = findKeyValue(s, key, from, bound);
    if (vpos == std::string::npos) return false;
    std::string::size_type end = vpos;
    if (!parseJsonString(s, vpos, out, end)) return false; // e.g. value is null/number
    if (valueEnd != nullptr) *valueEnd = end;
    return true;
}

inline bool findNumberField(const std::string& s, const std::string& key,
                            std::string::size_type from, std::string::size_type bound,
                            long long& out) {
    const auto vpos = findKeyValue(s, key, from, bound);
    if (vpos == std::string::npos) return false;
    std::string::size_type i = vpos;
    bool negative = false;
    if (i < s.size() && s[i] == '-') { negative = true; ++i; }
    long long value = 0;
    bool anyDigit = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        value = value * 10 + (s[i] - '0');
        ++i;
        anyDigit = true;
    }
    if (!anyDigit) return false;
    out = negative ? -value : value;
    return true;
}

} // namespace releaseparse_detail

// Extracts the updater's fields from a GitHub release JSON document.
// Returns false (out untouched) when the document has no usable
// tag_name — the one field nothing else makes sense without.
inline bool parseReleaseJson(const std::string& json, ReleaseFields& out) {
    namespace d = releaseparse_detail;
    const auto npos = std::string::npos;
    ReleaseFields f;
    std::string v;
    if (!d::findStringField(json, "tag_name", 0, npos, v) || v.empty()) return false;
    f.tag = v;
    // "body" may be null (a release with no notes) — that is not an error.
    if (d::findStringField(json, "body", 0, npos, v)) f.notes = v;
    // Assets: scan `"name"` fields from the assets array onward; the
    // fields of the MeterBar-Setup.exe asset are read only up to the
    // next asset's `"name"` key, so a neighbouring asset's URL, size or
    // digest can never be picked up by mistake.
    const auto assetsPos = json.find("\"assets\"");
    if (assetsPos != npos) {
        std::string::size_type scan = assetsPos;
        while (true) {
            std::string name;
            std::string::size_type nameEnd = scan;
            if (!d::findStringField(json, "name", scan, npos, name, &nameEnd)) break;
            if (name == "MeterBar-Setup.exe") {
                const auto bound = json.find("\"name\"", nameEnd);
                f.hasAsset = true;
                if (d::findStringField(json, "browser_download_url", nameEnd, bound, v)) f.assetUrl = v;
                long long size = 0;
                if (d::findNumberField(json, "size", nameEnd, bound, size)) f.assetSize = size;
                if (d::findStringField(json, "digest", nameEnd, bound, v)) f.assetDigest = v;
                break;
            }
            scan = nameEnd;
        }
    }
    out = f;
    return true;
}

} // namespace meterbar
