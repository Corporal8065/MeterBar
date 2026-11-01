#include "CaptionTap.h"
#include <algorithm>
#include <cmath>

void CaptionTap::setEnabled(bool e) {
    enabled_.store(e);
    if (!e) {
        const std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        acc_.clear();
        accPeak_ = 0.f;
        accSq_ = 0.0;
    }
}

void CaptionTap::setEpoch(uint32_t epoch) {
    epoch_.store(epoch);
    const std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    acc_.clear();
    accPeak_ = 0.f;
    accSq_ = 0.0;
    nextNative_ = 0;
    rate_ = 0;
}

void CaptionTap::push(const float* L, const float* R, int n, double sampleRate) {
    if (!enabled_.load() || !sourceLive_.load() || n <= 0 || sampleRate <= 0)
        return;
    const uint32_t rate = (uint32_t)std::lround(sampleRate);
    const size_t frameSamples = (size_t)std::max(1, (int)std::lround(rate * 0.1));
    const std::lock_guard<std::mutex> lock(mutex_);
    if (rate != rate_) { // rate is part of the timeline identity
        rate_ = rate;
        acc_.clear();
        accPeak_ = 0.f;
        accSq_ = 0.0;
        nextNative_ = 0;
    }
    for (int i = 0; i < n; ++i) {
        const float mono = std::clamp(0.5f * ((L ? L[i] : 0.f) + (R ? R[i] : 0.f)),
                                      -1.0f, 1.0f);
        acc_.push_back((int16_t)std::lround(mono * 32767.0f));
        accPeak_ = std::max(accPeak_, std::abs(mono));
        accSq_ += (double)mono * (double)mono;
        if (acc_.size() == frameSamples) {
            Frame f;
            f.epoch = epoch_.load();
            f.sampleRate = rate_;
            f.firstNativeSample = nextNative_;
            f.samples.assign(acc_.begin(), acc_.end());
            f.peak = accPeak_;
            f.rms = (float)std::sqrt(accSq_ / (double)frameSamples);
            if (queue_.size() >= kQueueCapacity) {
                queue_.pop_front(); // oldest drops; never block the audio path
                dropped_.fetch_add(1);
            }
            queue_.push_back(std::move(f));
            nextNative_ += frameSamples;
            acc_.clear();
            accPeak_ = 0.f;
            accSq_ = 0.0;
        }
    }
}

bool CaptionTap::pop(Frame& out) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}
