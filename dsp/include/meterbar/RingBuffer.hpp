#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <algorithm>
namespace meterbar {
// Single-producer / single-consumer stereo ring buffer.
// Writer = audio thread, Reader = UI/analysis thread. No locks on the write path.
class StereoRingBuffer {
public:
    void prepare(int capacityFrames) {
        cap_ = std::max(1024, capacityFrames);
        buf_.assign((size_t)cap_ * 2, 0.0f);
        writePos_.store(0, std::memory_order_relaxed);
        totalWritten_.store(0, std::memory_order_relaxed);
    }
    int capacity() const { return cap_; }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); writePos_.store(0); totalWritten_.store(0); }
    void push(float l, float r) {
        if (cap_ == 0) return;
        int64_t w = writePos_.load(std::memory_order_relaxed);
        buf_[(size_t)(w % cap_) * 2] = l; buf_[(size_t)(w % cap_) * 2 + 1] = r;
        writePos_.store(w + 1, std::memory_order_release);
        totalWritten_.fetch_add(1, std::memory_order_relaxed);
    }
    void pushBlock(const float* L, const float* R, int n) { for (int i = 0; i < n; ++i) push(L ? L[i] : 0.f, R ? R[i] : 0.f); }
    int64_t totalWritten() const { return totalWritten_.load(std::memory_order_acquire); }
    // Copy the most recent n frames (oldest->newest) into outL/outR. Returns frames copied.
    int readLatest(float* outL, float* outR, int n) const {
        int64_t total = totalWritten_.load(std::memory_order_acquire);
        if (cap_ == 0 || total == 0 || n <= 0) return 0;
        int avail = (int)std::min<int64_t>({total, cap_, n});
        int64_t start = total - avail;
        for (int i = 0; i < avail; ++i) {
            size_t idx = (size_t)((start + i) % cap_) * 2;
            if (outL) outL[i] = buf_[idx];
            if (outR) outR[i] = buf_[idx + 1];
        }
        return avail;
    }
    // Extract last seconds of audio for "clip" feature. Returns frames.
    int readClip(float* outL, float* outR, double seconds, double sampleRate) const {
        return readLatest(outL, outR, (int)(seconds * sampleRate));
    }
private:
    std::vector<float> buf_; int cap_ = 0;
    std::atomic<int64_t> writePos_{0}, totalWritten_{0};
};
inline void toMidSide(float l, float r, float& m, float& s) { m = 0.5f * (l + r); s = 0.5f * (l - r); }
inline void fromMidSide(float m, float s, float& l, float& r) { l = m + s; r = m - s; }
} // namespace meterbar
