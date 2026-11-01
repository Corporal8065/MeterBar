#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace meterbar {
// LIVE CAPTIONS — PURE MODEL + WIRE FRAMING (v0.23.0, MeterBar ONLY).
//
// This header is the testable core of the Live Captions feature
// (see ~/workspace/bar-apps/captions-spec/SPEC.md): the MBC1 packet
// framing shared with the Python helper, and the caption-block
// display model the overlay renders (provisional replacement, final
// immutability, block expiry, two-line planning). The app and the
// helper both implement the framing from THIS description; the DSP
// suite grades the C++ side exactly.
//
// FAMILY NOTE: although this lives under dsp/include/meterbar/, it
// is NOT part of the shared family core. Do not copy it to
// TickerBar or SessionBar — Live Captions is a MeterBar-only
// feature and the other bars must not grow a dead copy of it.

// ------------------------------------------------------------------
// Framing (spec section 2.3). Every packet on the loopback TCP seam:
//
//   magic "MBC1"        4 bytes
//   protocol version    1 byte   (kCaptionProtocolVersion)
//   packet type         1 byte   (CaptionPacketType)
//   flags               2 bytes, network byte order
//   payload length      4 bytes, network byte order
//   stream epoch        4 bytes, network byte order
//   sequence            8 bytes, network byte order
//                       = 24-byte header, then the payload.
//
// JSON payloads are UTF-8, capped at 64 KiB. AUDIO payloads are a
// 14-byte binary prefix (first_native_sample u64 BE, sample_rate
// u32 BE, frame_samples u16 BE) followed by mono PCM16 LE samples.
// Receivers must tolerate partial reads and several packets per
// read; a bad magic/version closes the connection (decoder error).
// ------------------------------------------------------------------
inline constexpr uint8_t kCaptionProtocolVersion = 1;
inline constexpr size_t kCaptionHeaderBytes = 24;
inline constexpr size_t kCaptionMaxJsonPayload = 64 * 1024;

enum class CaptionPacketType : uint8_t {
    Hello = 1,     // C++ -> helper, JSON
    Config = 2,    // C++ -> helper, JSON
    Audio = 3,     // C++ -> helper, binary prefix + PCM16
    Control = 4,   // C++ -> helper, JSON
    Heartbeat = 5, // either direction, empty
    Ready = 6,     // helper -> C++, JSON
    Status = 7,    // helper -> C++, JSON
    Caption = 8,   // helper -> C++, JSON
    Error = 9      // helper -> C++, JSON
};

struct CaptionPacket {
    CaptionPacketType type = CaptionPacketType::Heartbeat;
    uint16_t flags = 0;
    uint32_t epoch = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> payload;
};

namespace caption_detail {
inline void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)(x & 0xff));
}
inline void putU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}
inline void putU64(std::vector<uint8_t>& v, uint64_t x) {
    for (int s = 56; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}
inline uint16_t getU16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
inline uint32_t getU32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint64_t getU64(const uint8_t* p) {
    uint64_t x = 0; for (int i = 0; i < 8; ++i) x = (x << 8) | p[i]; return x;
}
} // namespace caption_detail

inline std::vector<uint8_t> encodeCaptionPacket(const CaptionPacket& pkt) {
    std::vector<uint8_t> out;
    out.reserve(kCaptionHeaderBytes + pkt.payload.size());
    out.push_back('M'); out.push_back('B'); out.push_back('C'); out.push_back('1');
    out.push_back(kCaptionProtocolVersion);
    out.push_back((uint8_t)pkt.type);
    caption_detail::putU16(out, pkt.flags);
    caption_detail::putU32(out, (uint32_t)pkt.payload.size());
    caption_detail::putU32(out, pkt.epoch);
    caption_detail::putU64(out, pkt.sequence);
    out.insert(out.end(), pkt.payload.begin(), pkt.payload.end());
    return out;
}

// Incremental decoder: feed bytes as they arrive, pull complete
// packets with next(). error() latches on a bad magic/version or an
// implausible payload length — the caller closes the connection.
class CaptionPacketDecoder {
public:
    void feed(const uint8_t* data, size_t n) {
        if (error_) return;
        buffer_.insert(buffer_.end(), data, data + n);
    }
    bool next(CaptionPacket& out) {
        if (error_ || buffer_.size() < kCaptionHeaderBytes) return false;
        const uint8_t* h = buffer_.data();
        if (h[0] != 'M' || h[1] != 'B' || h[2] != 'C' || h[3] != '1'
            || h[4] != kCaptionProtocolVersion) { error_ = true; return false; }
        const uint32_t len = caption_detail::getU32(h + 8);
        // Sanity ceiling: 4 MiB. Real payloads are <= ~400 KB (a 100 ms
        // frame at 192 kHz is 38.4 KB); anything larger is a desync.
        if (len > 4u * 1024u * 1024u) { error_ = true; return false; }
        if (buffer_.size() < kCaptionHeaderBytes + len) return false; // partial — wait for more
        out.type = (CaptionPacketType)h[5];
        out.flags = caption_detail::getU16(h + 6);
        out.epoch = caption_detail::getU32(h + 12);
        out.sequence = caption_detail::getU64(h + 16);
        out.payload.assign(buffer_.begin() + (ptrdiff_t)kCaptionHeaderBytes,
                           buffer_.begin() + (ptrdiff_t)(kCaptionHeaderBytes + len));
        buffer_.erase(buffer_.begin(), buffer_.begin() + (ptrdiff_t)(kCaptionHeaderBytes + len));
        return true;
    }
    bool error() const { return error_; }
    void reset() { buffer_.clear(); error_ = false; }
private:
    std::vector<uint8_t> buffer_;
    bool error_ = false;
};

// AUDIO payload prefix (14 bytes) + PCM16 LE samples.
struct CaptionAudioPrefix {
    uint64_t firstNativeSample = 0;
    uint32_t sampleRate = 0;
    uint16_t frameSamples = 0;
};
inline constexpr size_t kCaptionAudioPrefixBytes = 14;
inline std::vector<uint8_t> buildCaptionAudioPayload(const CaptionAudioPrefix& p,
                                                     const int16_t* samples) {
    std::vector<uint8_t> out;
    out.reserve(kCaptionAudioPrefixBytes + (size_t)p.frameSamples * 2);
    caption_detail::putU64(out, p.firstNativeSample);
    caption_detail::putU32(out, p.sampleRate);
    caption_detail::putU16(out, p.frameSamples);
    for (uint32_t i = 0; i < p.frameSamples; ++i) { // PCM16 little-endian
        const uint16_t s = (uint16_t)samples[i];
        out.push_back((uint8_t)(s & 0xff)); out.push_back((uint8_t)(s >> 8));
    }
    return out;
}
inline bool parseCaptionAudioPayload(const uint8_t* data, size_t n,
                                     CaptionAudioPrefix& p, size_t& samplesOffset) {
    if (n < kCaptionAudioPrefixBytes) return false;
    p.firstNativeSample = caption_detail::getU64(data);
    p.sampleRate = caption_detail::getU32(data + 8);
    p.frameSamples = caption_detail::getU16(data + 12);
    if (n < kCaptionAudioPrefixBytes + (size_t)p.frameSamples * 2) return false;
    samplesOffset = kCaptionAudioPrefixBytes;
    return true;
}

// ------------------------------------------------------------------
// Caption blocks + display model (spec sections 1.4 and 2.5).
//
// A block is one utterance segment on the epoch timeline. Its id is
// derived from epoch + segment start, so rolling ASR passes for the
// same segment reconcile by id: provisionals replace provisionals,
// a final locks the text forever, and nothing may rewrite a final.
//
// The display clock is supplied by the caller (any monotonic ms).
// A final applied at display time T stands in for its audio end —
// finals are produced within ~a second of the audio end, and every
// expiry rule below is expressed against display times so the model
// is deterministic and testable without a media clock.
// ------------------------------------------------------------------
enum class CaptionMode : int { Off = 0, Transcribe = 1, Translate = 2 };

inline const char* captionModeName(CaptionMode m) {
    switch (m) {
        case CaptionMode::Transcribe: return "transcribe";
        case CaptionMode::Translate: return "translate";
        default: return "off";
    }
}

struct CaptionBlock {
    std::string id;              // captionBlockId(epoch, startMs)
    uint32_t epoch = 0;
    int64_t startMs = 0;         // epoch timeline
    int64_t endMs = 0;           // epoch timeline
    std::string text;
    bool final = false;
    CaptionMode mode = CaptionMode::Transcribe;
    std::string language;        // detected source language, may be empty
    float languageProb = 0.f;
};

inline std::string captionBlockId(uint32_t epoch, int64_t startMs) {
    return "cap-" + std::to_string(epoch) + "-" + std::to_string(startMs);
}

// Expiry constants (spec 1.4), milliseconds on the display clock.
inline constexpr int64_t kCaptionFinalHoldMs = 6000;      // final stays current
inline constexpr int64_t kCaptionPreviousHoldMs = 4000;   // previous final lingers
inline constexpr int64_t kCaptionProvisionalStaleMs = 3000; // unfinalised provisional dies

struct CaptionView {
    bool hasPrevious = false;
    CaptionBlock previous;
    bool hasCurrent = false;
    CaptionBlock current;
};

class CaptionDisplay {
public:
    // Feed one caption event. Provisional events for the current id
    // revise it in place; a final for the current id locks it; a
    // block with a NEW id takes over as current (a final current is
    // demoted to previous; an unfinalised current is discarded —
    // uncommitted text never becomes history).
    void apply(const CaptionBlock& b, int64_t nowMs) {
        if (hasCurrent_ && current_.id == b.id) {
            if (current_.final) return; // final text never changes
            current_ = b;
            currentUpdatedMs_ = nowMs;
            if (b.final) currentFinalMs_ = nowMs;
            return;
        }
        if (hasPrevious_ && previous_.id == b.id && previous_.final) return; // already history
        if (hasCurrent_ && current_.final) {
            previous_ = current_;
            hasPrevious_ = true;
            previousSinceMs_ = nowMs;
        }
        current_ = b;
        hasCurrent_ = true;
        currentUpdatedMs_ = nowMs;
        currentFinalMs_ = b.final ? nowMs : 0;
    }
    // The renderable state at nowMs, with expiry applied.
    CaptionView view(int64_t nowMs) const {
        CaptionView v;
        if (hasCurrent_) {
            const bool staleProvisional = !current_.final
                && (nowMs - currentUpdatedMs_ > kCaptionProvisionalStaleMs);
            const bool finalExpired = current_.final
                && (nowMs - currentFinalMs_ > kCaptionFinalHoldMs);
            if (!staleProvisional && !finalExpired) {
                v.hasCurrent = true; v.current = current_;
            }
        }
        if (hasPrevious_ && nowMs - previousSinceMs_ <= kCaptionPreviousHoldMs) {
            v.hasPrevious = true; v.previous = previous_;
        }
        return v;
    }
    bool empty(int64_t nowMs) const {
        const CaptionView v = view(nowMs);
        return !v.hasCurrent && !v.hasPrevious;
    }
    void clear() { hasCurrent_ = false; hasPrevious_ = false; }
private:
    bool hasCurrent_ = false, hasPrevious_ = false;
    CaptionBlock current_, previous_;
    int64_t currentUpdatedMs_ = 0, currentFinalMs_ = 0, previousSinceMs_ = 0;
};

// Two-line planning (spec 1.4): greedy word wrap at maxCharsPerLine;
// if the text needs more than two lines, keep the LAST two lines so
// the newest words are what shows (the block "scrolls" internally)
// and flag the overflow. Words longer than a line are hard-split.
struct CaptionLinePlan {
    std::vector<std::string> lines;
    bool overflowed = false;
};
inline CaptionLinePlan planCaptionLines(const std::string& text, int maxCharsPerLine) {
    CaptionLinePlan plan;
    if (maxCharsPerLine < 4) maxCharsPerLine = 4;
    std::vector<std::string> words;
    std::string cur;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) words.push_back(cur);
    std::vector<std::string> lines;
    std::string line;
    auto flush = [&] { if (!line.empty()) { lines.push_back(line); line.clear(); } };
    for (auto w : words) {
        while ((int)w.size() > maxCharsPerLine) { // hard-split an over-long word
            flush();
            lines.push_back(w.substr(0, (size_t)maxCharsPerLine));
            w = w.substr((size_t)maxCharsPerLine);
        }
        if (line.empty()) line = w;
        else if ((int)line.size() + 1 + (int)w.size() <= maxCharsPerLine) line += " " + w;
        else { flush(); line = w; }
    }
    flush();
    if (lines.size() > 2) {
        plan.overflowed = true;
        plan.lines.assign(lines.end() - 2, lines.end());
    } else plan.lines = lines;
    return plan;
}
} // namespace meterbar
