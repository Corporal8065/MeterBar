#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// LIVE CAPTIONS — AUDIO TAP (v0.23.0, spec section 2.2).
//
// AudioEngine::pushStereo() calls push() with the PRE-TRIM samples.
// The tap downmixes to mono PCM16 at the native rate and assembles
// fixed 100 ms frames into a bounded queue (2 s). A streamer thread
// (CaptionClient) pops frames and forwards them to the helper.
//
// The audio path only ever does: convert, append to a deque, maybe
// complete a frame. No I/O, no allocation beyond pre-sized buffers,
// never blocking on a full queue — when the queue is full the OLDEST
// frame drops (captions are expendable; MeterBar's own audio work
// is not). JUCE-free on purpose: plain std types only.
class CaptionTap {
public:
    struct Frame {
        uint32_t epoch = 0;
        uint32_t sampleRate = 0;
        uint64_t firstNativeSample = 0; // in the epoch's native timeline
        std::vector<int16_t> samples;   // exactly one 100 ms frame
        float peak = 0.f;
        float rms = 0.f;
    };

    static constexpr size_t kQueueCapacity = 20; // 20 x 100 ms = 2 s

    // Feature on/off (CaptionController). Turning off clears pending
    // audio so a later enable never leaks stale speech.
    void setEnabled(bool e);
    bool enabled() const { return enabled_.load(); }

    // Source gate: AudioEngine clears this for Test Tone.
    void setSourceLive(bool live) { sourceLive_.store(live); }

    // New stream epoch (AudioEngine::applySource): the accumulator
    // and native timeline restart; queued frames are dropped (they
    // belong to the old source and must never splice into the new).
    void setEpoch(uint32_t epoch);
    uint32_t epoch() const { return epoch_.load(); }

    // Audio thread. L/R may be null (treated as silence).
    void push(const float* L, const float* R, int n, double sampleRate);

    // Streamer thread. Returns false when the queue is empty.
    bool pop(Frame& out);

    uint64_t droppedFrames() const { return dropped_.load(); }

private:
    std::atomic<bool> enabled_{false};
    std::atomic<bool> sourceLive_{false};
    std::atomic<uint32_t> epoch_{0};
    std::atomic<uint64_t> dropped_{0};
    std::mutex mutex_;
    std::deque<Frame> queue_;
    std::deque<int16_t> acc_;       // partial-frame accumulator
    uint32_t rate_ = 0;             // native rate of the current timeline
    uint64_t nextNative_ = 0;       // first native sample of the next frame
    float accPeak_ = 0.f;
    double accSq_ = 0.0;
};
