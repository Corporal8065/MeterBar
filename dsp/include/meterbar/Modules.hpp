#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "RingBuffer.hpp"
namespace meterbar {
// ---------- Waveform history (scrolling min/max display data) ----------
enum class HistoryMode { Off, Fast, Slow };
enum class ChanSel { Left, Right, Mid, Side, Off };
inline float selectChan(float l, float r, ChanSel c) {
    switch (c) { case ChanSel::Left: return l; case ChanSel::Right: return r;
    case ChanSel::Mid: return 0.5f*(l+r); case ChanSel::Side: return 0.5f*(l-r); default: return 0.f; }
}
class WaveformHistory {
public:
    void prepare(int columns = 512) { cols_ = columns; ch1_.assign(cols_,0.f); ch2_.assign(cols_,0.f); }
    // speed: linked speed 0..10 -> columns advanced per block push (scaled).
    void pushBlock(const float* L, const float* R, int n, ChanSel c1, ChanSel c2, HistoryMode hist, float linkedSpeed) {
        if (n <= 0) return;
        float peak1 = 0, peak2 = 0;
        for (int i = 0; i < n; ++i) { peak1 = std::max(peak1, std::abs(selectChan(L[i],R[i],c1)));
                                     if (c2 != ChanSel::Off) peak2 = std::max(peak2, std::abs(selectChan(L[i],R[i],c2))); }
        int steps = std::max(1, (int)std::lround(1 + linkedSpeed));
        for (int s = 0; s < steps; ++s) {
            std::rotate(ch1_.begin(), ch1_.begin()+1, ch1_.end()); std::rotate(ch2_.begin(), ch2_.begin()+1, ch2_.end());
            ch1_.back() = peak1; ch2_.back() = peak2;
        }
        // History decay: Fast decays older columns toward 0 quickly, Slow gently.
        if (hist != HistoryMode::Off) {
            float decay = (hist == HistoryMode::Fast) ? 0.94f : 0.995f;
            for (int i = 0; i < cols_-steps; ++i) { ch1_[i] *= decay; ch2_[i] *= decay; }
        }
    }
    const std::vector<float>& ch1() const { return ch1_; } const std::vector<float>& ch2() const { return ch2_; }
private: int cols_ = 512; std::vector<float> ch1_, ch2_;
};
// ---------- Spectrogram history ----------
enum class Orientation { Horizontal, Vertical };
class SpectrogramHistory {
public:
    void prepare(int bins, int rows = 256) { bins_ = bins; rows_ = rows; data_.assign((size_t)bins_*rows_, -120.f); head_ = 0; }
    void pushRow(const std::vector<float>& dbBins, float linkedSpeed) {
        acc_ += std::max(0.1f, linkedSpeed) * 0.2f;
        while (acc_ >= 1.0f) { acc_ -= 1.0f;
            int n = std::min(bins_, (int)dbBins.size());
            for (int b = 0; b < bins_; ++b) {
                float prev = data_[(size_t)head_*bins_+b];
                float target = (b < n) ? dbBins[b] : -120.f;
                data_[(size_t)head_*bins_+b] = target; (void)prev;
            }
            head_ = (head_ + 1) % rows_;
        }
    }
    // value for display cell: age 0 = newest row, bin = freq bin; fade applied by renderer.
    float at(int age, int bin) const { int row = (head_ - 1 - age) % rows_; if (row < 0) row += rows_; return data_[(size_t)row*bins_+bin]; }
    static float fadeGain(int age, int rows, float fade01) { float a = rows ? (float)age/(float)rows : 0.f; return 1.0f - fade01 * a; }
    int bins() const { return bins_; } int rows() const { return rows_; }
    // v0.20.2: the write cursor, exposed so the renderer can tell how
    // many rows were pushed since its last render (head delta) and skip
    // re-rendering an unchanged spectrogram cache image.
    int head() const { return head_; }
private: int bins_=0, rows_=256, head_=0; float acc_=0; std::vector<float> data_;
};
// ---------- Stereometer ----------
enum class StereoMode { ScaledUnipolar, ScaledBipolar, LinearUnipolar, LinearBipolar, Lissajous };
enum class StereoColor { Single, Rgb, MultiBand };
inline const char* stereoModeName(StereoMode m){ switch(m){case StereoMode::ScaledUnipolar:return "Scaled - Unipolar";
 case StereoMode::ScaledBipolar:return "Scaled - Bipolar";case StereoMode::LinearUnipolar:return "Linear - Unipolar";
 case StereoMode::LinearBipolar:return "Linear - Bipolar";default:return "Lissajous";} }
struct StereoFrame { float correlation = 0; float balance = 0; float widthPct = 0; // v0.16: 100 * rms(Side)/rms(Mid), capped 200 (mono = 0%)
                     std::vector<float> xs, ys; /* Lissajous: x=S, y=M */ };
class StereometerEngine {
public:
    StereoFrame analyse(const float* L, const float* R, int n, int maxPoints = 1024) {
        StereoFrame f; if (n <= 0) return f;
        double sLL=0,sRR=0,sLR=0,sL=0,sR=0,sMM=0,sSS=0;
        int step = std::max(1, n / maxPoints);
        for (int i = 0; i < n; i += step) { double l=L[i], r=R[i]; sLL+=l*l; sRR+=r*r; sLR+=l*r; sL+=std::abs(l); sR+=std::abs(r);
            double m = 0.5*(l+r), s = 0.5*(l-r); sMM+=m*m; sSS+=s*s;
            f.xs.push_back(0.5f*(float)(l-r)); f.ys.push_back(0.5f*(float)(l+r)); }
        double denom = std::sqrt(std::max(sLL*sRR, 1e-18));
        f.correlation = (float)std::clamp(sLR/denom, -1.0, 1.0);
        f.widthPct = sMM > 1e-12 ? (float)std::min(200.0, 100.0 * std::sqrt(sSS / sMM)) : (sSS > 1e-12 ? 200.0f : 0.0f);
        f.balance = (float)((sR - sL) / std::max(sR + sL, 1e-12));
        return f;
    }
    // Lissajous point for the stereo vectorscope (v0.15): recover the
    // per-sample (L, R) pair from the stored Mid/Side (L = M + S,
    // R = M - S) and return {x = R, y = L} — the conventional L-vs-R
    // vectorscope plot (Ozone Imager style): a mono signal draws the
    // main diagonal, stereo width spreads points off it, anti-phase
    // draws the opposite diagonal. Stereo only — no surround geometry.
    static std::pair<float,float> lissajousPoint(float m, float s) { return { m - s, m + s }; }
    // Display transform for a point under a mode (renderer uses same math).
    static std::pair<float,float> project(float m, float s, StereoMode mode) {
        switch (mode) {
        case StereoMode::ScaledUnipolar: { float g = 1.0f/(std::abs(m)+std::abs(s)+1e-6f); return { s*g*std::min(1.f,std::abs(m)+std::abs(s)), std::abs(m)*g*std::min(1.f,std::abs(m)+std::abs(s)) }; }
        case StereoMode::ScaledBipolar: { float g = 1.0f/(std::abs(m)+std::abs(s)+1e-6f); float k = std::min(1.f, std::abs(m)+std::abs(s)); return { s*g*k, m*g*k }; }
        case StereoMode::LinearUnipolar: return { s, std::abs(m) };
        case StereoMode::LinearBipolar: return { s, m };
        default: return { s, m }; // Lissajous
        }
    }
};
// ---------- Oscilloscope ----------
enum class ScopeTrigger { Pitch, Free };
enum class ScopeDisplay { Multi, Single };
class OscilloscopeEngine {
public:
    void prepare(int captureFrames = 2048) { cap_ = captureFrames; buf_.assign(cap_, 0.f); }
    // Fill display buffer from latest samples of one channel.
    // Pitch mode: estimate period via autocorrelation, align to a rising zero crossing at period phase.
    int capture(const float* src, int n, ScopeTrigger trig, float& outFreqHz, double sampleRate) {
        outFreqHz = 0; if (n <= 0) return 0;
        int want = std::min(cap_, n);
        int start = n - want;
        if (trig == ScopeTrigger::Pitch) {
            int period = estimatePeriod(src + start, want, sampleRate);
            if (period > 1) { outFreqHz = (float)(sampleRate / period);
                // find last rising zero-cross that leaves a full period-ish view
                for (int i = n - period - 1; i > 1; --i) if (src[i-1] < 0 && src[i] >= 0) { start = i; break; }
                want = std::min(cap_, n - start);
            }
        }
        // Fill the WHOLE display buffer by tiling the aligned segment.
        // Bug fix (Jackson field test, 2026-10-13): pitch mode left `want` at
        // ~1 period (~10% of 2048) and zero-padded the rest, so the trace only
        // occupied the first ~10% of the scope width.
        for (int i = 0; i < cap_; ++i) buf_[i] = (want > 0) ? src[start + (i % want)] : 0.f;
        return want;
    }
    static int estimatePeriod(const float* x, int n, double sr) {
        int minP = (int)(sr / 1000.0), maxP = std::min((int)(sr / 40.0), n / 2);
        if (maxP <= minP) return 0; double best = 0; int bestP = 0;
        for (int p = minP; p <= maxP; ++p) { double c = 0; for (int i = 0; i < n - p; i += 2) c += (double)x[i]*x[i+p];
            if (c > best) { best = c; bestP = p; } }
        return bestP;
    }
    const std::vector<float>& buffer() const { return buf_; }
private: int cap_ = 2048; std::vector<float> buf_;
};

// ---------- Scope band split (v0.20.0, item i) ----------
// A TRUE 3-band split for the oscilloscope's Multi display — which
// previously drew the scope buffer and then the same left channel
// again faintly (the "multi" was one signal twice). Two cascaded
// one-pole lowpasses at the stated sample rate:
//   low  = LP(lowHz)(x)
//   mid  = LP(highHz)(x - low)
//   high = x - low - mid
// The three bands sum back to the input EXACTLY (by construction —
// the tests pin it), so nothing is invented or lost. Stateless per
// call: filter state restarts per display buffer (a few samples of
// settle at the start; the scope tiles a periodic segment anyway).
struct ScopeBands { std::vector<float> low, mid, high; };
inline ScopeBands splitScopeBands(const std::vector<float>& x, double sampleRate,
                                  double lowHz = 250.0, double highHz = 4000.0) {
    ScopeBands out;
    const int n = (int)x.size();
    out.low.assign((size_t)n, 0.f); out.mid.assign((size_t)n, 0.f); out.high.assign((size_t)n, 0.f);
    if (n == 0 || sampleRate <= 0) return out;
    auto coef = [&](double hz) { return (float)(1.0 - std::exp(-2.0 * 3.141592653589793 * hz / sampleRate)); };
    const float aL = coef(lowHz), aH = coef(highHz);
    float lpL = 0.f, lpM = 0.f;
    for (int i = 0; i < n; ++i) {
        lpL += aL * (x[(size_t)i] - lpL);
        const float rest = x[(size_t)i] - lpL;
        lpM += aH * (rest - lpM);
        out.low[(size_t)i] = lpL;
        out.mid[(size_t)i] = lpM;
        out.high[(size_t)i] = rest - lpM;
    }
    return out;
}

// ---------- Beat / onset detection (v0.16) ----------
// A deliberately simple energy-flux onset detector for the Beat Pulse
// glow and the Anime mascot's bob: each pushed block's mean-square is
// compared against a slow trailing baseline (~0.7 s). A block whose
// energy exceeds the baseline by the fire ratio, after a 180 ms
// refractory, fires an onset whose strength drives a glow envelope that
// decays with a ~120 ms time constant (subtle by design — the view caps
// the visible intensity). Steady material settles into the baseline and
// stops firing; silence never fires (absolute floor gate).
class BeatPulse {
public:
    void prepare(double sr) { sr_ = sr; baseline_ = 0; pulse_ = 0; sinceOnset_ = 1e9; onsetCount_ = 0; elapsed_ = 0; }
    // Push one block; returns true on the block where an onset fires.
    bool pushBlock(const float* L, const float* R, int n) {
        if (n <= 0 || sr_ <= 0) return false;
        double ms = 0; for (int i = 0; i < n; ++i) { double l = L ? L[i] : 0, r = R ? R[i] : 0; ms += l * l + r * r; }
        ms /= (2.0 * n);
        double blockSec = n / sr_;
        // Slow baseline (tau ~0.7 s), but it must not chase a sustained
        // loud passage instantly or every bar would re-fire.
        double aBase = 1.0 - std::exp(-blockSec / 0.7);
        sinceOnset_ += blockSec; elapsed_ += blockSec;
        bool fired = false;
        // Warm-up: no firing in the first 0.6 s — the baseline is still
        // converging toward steady material and would false-fire on it.
        if (elapsed_ > 0.6 && ms > 1e-6 && baseline_ > 1e-9 && ms > baseline_ * 2.4 && sinceOnset_ > 0.180) {
            double strength = std::clamp((ms / baseline_ - 2.4) / 6.0 + 0.45, 0.0, 1.0);
            pulse_ = std::max(pulse_, (float)strength);
            sinceOnset_ = 0; fired = true; ++onsetCount_;
        }
        baseline_ += aBase * (ms - baseline_);
        if (baseline_ < 1e-9 && ms > 0) baseline_ = ms; // cold start: adopt first energy
        pulse_ *= (float)std::exp(-blockSec / 0.120);   // ~120 ms decay
        return fired;
    }
    float level() const { return pulse_; }              // glow envelope 0..1
    uint64_t onsetCount() const { return onsetCount_; } // monotonic — the UI watches for changes
private:
    double sr_ = 48000, baseline_ = 0, sinceOnset_ = 1e9, elapsed_ = 0;
    float pulse_ = 0; uint64_t onsetCount_ = 0;
};
} // namespace meterbar
