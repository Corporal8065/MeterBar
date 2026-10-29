#pragma once
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
namespace meterbar {
enum class FreqScale { Log, Mel, Linear };
inline bool isValidFftSize(int n) { return n==1024||n==2048||n==4096||n==8192||n==16384; }
// Iterative radix-2 FFT + Hann-windowed magnitude spectrum in dBFS, with
// per-bin temporal smoothing and a dB/octave-ish tilt compensation.
class SpectrumAnalyser {
public:
    void prepare(int fftSize) {
        size_ = isValidFftSize(fftSize) ? fftSize : 1024;
        window_.resize(size_); for (int i = 0; i < size_; ++i)
            window_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / (size_ - 1));
        smoothed_.assign(size_ / 2 + 1, -120.0f); rawDb_.assign(size_ / 2 + 1, -120.0f);
        buf_.resize(size_); fftBuf_.resize(size_);
    }
    int size() const { return size_; }
    int numBins() const { return size_ / 2 + 1; }
    void setSmoothing(float s01) { smoothing_ = std::clamp(s01, 0.0f, 1.0f); }
    void setTiltDb(float t) { tiltDb_ = std::clamp(t, -9.0f, 9.0f); }
    float smoothing() const { return smoothing_; } float tiltDb() const { return tiltDb_; }
    // samples: mono mix (caller decides L/R/M/S). Returns dB per bin (smoothed, tilted).
    const std::vector<float>& process(const float* samples, int numSamples) {
        int n = std::min(numSamples, size_);
        for (int i = 0; i < size_; ++i) {
            float v = (i < n) ? samples[i] : 0.0f;
            fftBuf_[i] = { v * window_[i], 0.0f };
        }
        fft(fftBuf_);
        // Hann-windowed sine of amplitude A peaks at |X| = A*N/4, so scale by 4/N.
        const float norm = 4.0f / (float)size_;
        for (int b = 0; b < numBins(); ++b) {
            float mag = std::abs(fftBuf_[b]) * norm;
            float db = 20.0f * std::log10(std::max(mag, 1e-7f));
            float oct = (b > 0) ? std::log2((float)b) : 0.0f;
            float tilted = db + tiltDb_ * oct / 4.0f; // gentle: full tilt spread over ~4 octaves of reference slope
            rawDb_[b] = tilted;
            smoothed_[b] = smoothing_ * smoothed_[b] + (1.0f - smoothing_) * tilted;
            if (smoothed_[b] < tilted && smoothing_ > 0.999f) smoothed_[b] = tilted;
        }
        return smoothed_;
    }
    const std::vector<float>& rawDb() const { return rawDb_; }
    static void fft(std::vector<std::complex<float>>& a) {
        int n = (int)a.size();
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            float ang = -2.0f * 3.14159265358979f / len;
            std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (int i = 0; i < n; i += len) {
                std::complex<float> w(1, 0);
                for (int k = 0; k < len / 2; ++k) {
                    auto u = a[i + k]; auto v = a[i + k + len / 2] * w;
                    a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl;
                }
            }
        }
    }
    // Map a frequency to 0..1 display position under a scale, and back.
    static float freqToX(double freq, double minF, double maxF, FreqScale s) {
        auto warp = [](double f, FreqScale sc) {
            if (sc == FreqScale::Mel) return 2595.0 * std::log10(1.0 + f / 700.0);
            if (sc == FreqScale::Linear) return f;
            return std::log(std::max(f, 1.0));
        };
        double a = warp(minF, s), b = warp(maxF, s), v = warp(freq, s);
        return (float)std::clamp((v - a) / std::max(b - a, 1e-9), 0.0, 1.0);
    }
    static float binFreq(int bin, int fftSize, double sr) { return (float)(bin * sr / fftSize); }
private:
    int size_ = 1024; float smoothing_ = 0.90f, tiltDb_ = 4.5f;
    std::vector<float> window_, smoothed_, rawDb_, buf_;
    std::vector<std::complex<float>> fftBuf_;
};
// ---------- 1/6-octave smoothing (v0.16, display-only) ----------
// Each bin is replaced by the mean of the bins within +/-1/12 octave of
// it (a total span of 1/6 octave) — analyser-style perceptual smoothing
// that makes a dense FFT curve read like an RTA. Applied by the view to
// a copy at draw time; analyser data is never modified.
inline std::vector<float> smoothSixthOctave(const std::vector<float>& dbPerBin, int fftSize, double sampleRate) {
    const int n = (int)dbPerBin.size();
    std::vector<float> out(dbPerBin);
    if (n < 4 || fftSize <= 0 || sampleRate <= 0) return out;
    const double ratio = std::pow(2.0, 1.0 / 12.0); // half-window edge
    for (int i = 1; i < n; ++i) {
        double f = i * sampleRate / fftSize;
        if (f <= 0) continue;
        int lo = std::max(1, (int)std::floor(i / ratio)), hi = std::min(n - 1, (int)std::ceil(i * ratio));
        double sum = 0; int cnt = 0;
        for (int j = lo; j <= hi; ++j) { sum += dbPerBin[(size_t)j]; ++cnt; }
        if (cnt > 0) out[(size_t)i] = (float)(sum / cnt);
    }
    return out;
}

// ---------- Display tilt (v0.15, Pro-Q style) ----------
// Jackson: the spectrum slopes from top-left to bottom-right on music —
// low frequencies dominate the raw FFT magnitudes. Pro Q and similar
// analysers flatten the DRAWN curve with a slope compensation in
// dB/octave (Pro Q's default is +4.5 dB/oct). This compensation is
// DISPLAY-ONLY: it is applied by the spectrum view to a copy of the bin
// values at draw time. The analyser's process() output — which the
// spectrogram and every other consumer read — is never tilted by it.
// Compensation pivots at 1 kHz (0 dB there) and is clamped to +/-30 dB
// so the extremes of the axis stay inside the -90..0 dB draw window.
inline float spectrumDisplayTiltCompDb(double freqHz, float tiltDbPerOct) {
    if (freqHz <= 0.0) return 0.0f;
    float comp = tiltDbPerOct * (float)std::log2(freqHz / 1000.0);
    return std::clamp(comp, -30.0f, 30.0f);
}
// Apply display tilt to a copy of a dB-per-bin vector (bins of an
// fftSize FFT at sample rate sr). Returns the drawn values.
inline std::vector<float> applySpectrumDisplayTilt(const std::vector<float>& dbPerBin,
                                                   int fftSize, double sr, float tiltDbPerOct) {
    std::vector<float> out = dbPerBin;
    for (size_t b = 0; b < out.size(); ++b)
        out[b] += spectrumDisplayTiltCompDb(SpectrumAnalyser::binFreq((int)b, fftSize, sr), tiltDbPerOct);
    return out;
}
} // namespace meterbar
