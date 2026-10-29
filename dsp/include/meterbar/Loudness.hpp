#pragma once
#include <cmath>
#include <algorithm>
#include <vector>
namespace meterbar {
// Simple one-pole / biquad helpers.
struct Biquad {
    double b0=1,b1=0,b2=0,a1=0,a2=0, x1=0,x2=0,y1=0,y2=0;
    void set(double nb0,double nb1,double nb2,double na1,double na2){b0=nb0;b1=nb1;b2=nb2;a1=na1;a2=na2;}
    void reset(){x1=x2=y1=y2=0;}
    inline float process(float x){ double y=b0*x+b1*x1+b2*x2-a1*y1-a2*y2; x2=x1;x1=x; y2=y1;y1=y; return (float)y; }
};
// K-weighting per ITU-R BS.1770: a high shelf (~+4 dB) followed by a high-pass.
// Coefficients are the published 48 kHz values; for other rates we re-derive the
// shelf with the standard pre-filter formula and design the HP via bilinear transform.
struct KWeight {
    Biquad shelf, hp;
    void prepare(double sr) {
        if (std::abs(sr - 48000.0) < 1.0) {
            shelf.set(1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585);
            hp.set(0.99505079984832, -1.9901015996964, 0.99505079984832, -1.989975179354, 0.99014983038322);
        } else {
            // Shelf: DeMan pre-filter approximation re-evaluated for arbitrary sr.
            double f0 = 1681.974450955, G = 3.999843853973, Q = 0.70717523695542;
            double K = std::tan(3.14159265358979 * f0 / sr), Vh = std::pow(10.0, G / 20.0);
            double a0 = 1 + std::sqrt(Vh) / Q * K + K * K;
            shelf.set((Vh + std::sqrt(Vh) / Q * K + K * K) / a0,
                      2 * (K * K - Vh) / a0, (Vh - std::sqrt(Vh) / Q * K + K * K) / a0,
                      2 * (K * K - 1) / a0, (1 - std::sqrt(Vh) / Q * K + K * K) / a0);
            double fc = 38.135470876; double Kh = std::tan(3.14159265358979 * fc / sr);
            double norm = 1 / (1 + std::sqrt(2.0) * Kh + Kh * Kh);
            hp.set(norm, -2 * norm, norm, 2 * (Kh * Kh - 1) * norm, (1 - std::sqrt(2.0) * Kh + Kh * Kh) * norm);
        }
        shelf.reset(); hp.reset();
    }
    inline float process(float x) { return hp.process(shelf.process(x)); }
};
enum class LoudnessMode { LufsMomentary, LufsShortTerm, RmsFast, RmsSlow, RmsPlus3Fast, RmsPlus3Slow };
inline const char* loudnessModeName(LoudnessMode m){
    switch(m){case LoudnessMode::LufsMomentary:return "LUFS: Momentary";case LoudnessMode::LufsShortTerm:return "LUFS: Short Term";
    case LoudnessMode::RmsFast:return "RMS: Fast";case LoudnessMode::RmsSlow:return "RMS: Slow";
    case LoudnessMode::RmsPlus3Fast:return "RMS +3: Fast";default:return "RMS +3: Slow";}
}
// Loudness engine: feed every sample (stereo). K-weighted mean-square over
// 400 ms (momentary) and 3 s (short-term) windows; RMS with fast (125 ms) /
// slow (1 s) exponential ballistics; channel sum per BS.1770 (L+R, no surround).
class LoudnessEngine {
public:
    void prepare(double sr) { sr_ = sr; kL_.prepare(sr); kR_.prepare(sr); reset(); }
    void reset() {
        momBuf_.assign((size_t)(0.400 * sr_) + 1, 0.0); stBuf_.assign((size_t)(3.0 * sr_) + 1, 0.0);
        momHead_ = stHead_ = 0; momSum_ = stSum_ = 0; momCount_ = stCount_ = 0;
        rmsFast_ = rmsSlow_ = 0; peak_ = 0;
        // v0.16 loudness pack
        intBlocks_.clear(); blockAcc_ = 0; blockN_ = 0; truePeak_ = 0; histN_ = 0;
        for (int i = 0; i < 16; ++i) { histL_[i] = 0; histR_[i] = 0; }
    }
    void push(float l, float r) {
        float kl = kL_.process(l), kr = kR_.process(r);
        double ms = (double)kl * kl + (double)kr * kr;
        pushWin(momBuf_, momHead_, momSum_, momCount_, ms);
        pushWin(stBuf_, stHead_, stSum_, stCount_, ms);
        double raw = (double)l * l + (double)r * r;
        double aF = 1.0 - std::exp(-1.0 / (0.125 * sr_)), aS = 1.0 - std::exp(-1.0 / (1.0 * sr_));
        rmsFast_ += aF * (raw - rmsFast_); rmsSlow_ += aS * (raw - rmsSlow_);
        peak_ = std::max({peak_, std::abs(l), std::abs(r)});
        // --- v0.16: integrated-LUFS blocks (100 ms K-weighted mean-square)
        blockAcc_ += ms; ++blockN_;
        const int blockLen = std::max(1, (int)(sr_ / 10.0));
        if (blockN_ >= blockLen) {
            if (intBlocks_.size() < 36000) intBlocks_.push_back(blockAcc_ / blockN_); // cap 1 hour
            blockAcc_ = 0; blockN_ = 0;
        }
        // --- v0.16: true peak via 16-tap windowed-sinc interpolation at
        // 4x (inter-sample peaks live between samples). A 4-point
        // Lagrange kernel under-read a 12 kHz sine's crest by ~1 dB in
        // testing (0.884 for a true 1.0), so the interpolator is a
        // Blackman-windowed sinc — proper bandlimited reconstruction.
        for (int i = 0; i < 15; ++i) { histL_[i] = histL_[i + 1]; histR_[i] = histR_[i + 1]; }
        histL_[15] = l; histR_[15] = r;
        if (++histN_ >= 16) {
            static const double fracs[3] = { 0.25, 0.5, 0.75 };
            for (double mu : fracs) {
                truePeak_ = std::max(truePeak_, std::abs(interpSinc(histL_, 7.0 + mu)));
                truePeak_ = std::max(truePeak_, std::abs(interpSinc(histR_, 7.0 + mu)));
            }
        }
    }
    void pushBlock(const float* L, const float* R, int n){ for(int i=0;i<n;++i) push(L?L[i]:0.f, R?R[i]:0.f); }
    double momentaryLufs() const { return msToLufs(momSum_, momCount_); }
    double shortTermLufs() const { return msToLufs(stSum_, stCount_); }
    double rmsDb(bool slow, bool plus3) const {
        double v = slow ? rmsSlow_ : rmsFast_; if (v <= 1e-12) return -120.0;
        double db = 10.0 * std::log10(v / 2.0); // per-channel mean square
        return plus3 ? db + 3.0 : db;
    }
    double valueFor(LoudnessMode m) const {
        switch (m) { case LoudnessMode::LufsMomentary: return momentaryLufs();
        case LoudnessMode::LufsShortTerm: return shortTermLufs();
        case LoudnessMode::RmsFast: return rmsDb(false,false); case LoudnessMode::RmsSlow: return rmsDb(true,false);
        case LoudnessMode::RmsPlus3Fast: return rmsDb(false,true); default: return rmsDb(true,true); }
    }
    float peak() const { return peak_; } void resetPeak(){ peak_ = 0; truePeak_ = 0; }
    // --- v0.16 loudness pack
    // Integrated LUFS, BS.1770 gating: absolute gate -70 LUFS, then a
    // relative gate 10 LU below the ungated value; two passes over the
    // stored 100 ms block energies. -120 until at least ~1 s has arrived.
    double integratedLufs() const {
        if (intBlocks_.size() < 10) return -120.0;
        double sum = 0; for (double z : intBlocks_) sum += z;
        double ungated = sum / (double)intBlocks_.size();
        if (ungated <= 0) return -120.0;
        double ungatedDb = -0.691 + 10.0 * std::log10(ungated);
        double relGateDb = ungatedDb - 10.0;
        double gSum = 0; int64_t gN = 0;
        for (double z : intBlocks_) {
            if (z <= 0) continue;
            double bDb = -0.691 + 10.0 * std::log10(z);
            if (bDb > -70.0 && bDb > relGateDb) { gSum += z; ++gN; }
        }
        if (gN == 0) return -120.0;
        return -0.691 + 10.0 * std::log10(gSum / (double)gN);
    }
    double truePeak() const { return truePeak_; }                        // linear
    double truePeakDb() const { return truePeak_ > 0 ? 20.0 * std::log10(truePeak_) : -120.0; }
    void resetIntegrated() { intBlocks_.clear(); blockAcc_ = 0; blockN_ = 0; }
private:
    // Windowed-sinc reconstruction of the 16-sample history h at fractional
    // position p (sample j sits at coordinate j). Blackman window, 8-sample
    // support each side.
    static double interpSinc(const float h[16], double p) {
        const double pi = 3.14159265358979;
        double y = 0;
        for (int j = 0; j < 16; ++j) {
            double d = p - j;
            if (std::abs(d) >= 8.0) continue;
            double s = std::abs(d) < 1e-9 ? 1.0 : std::sin(pi * d) / (pi * d);
            double w = 0.42 + 0.5 * std::cos(pi * d / 8.0) + 0.08 * std::cos(2.0 * pi * d / 8.0);
            y += h[j] * s * w;
        }
        return y;
    }
    static double msToLufs(double sum, int64_t count){ if(count<=0||sum<=0) return -120.0; return -0.691 + 10.0*std::log10(sum/(double)count); }
    void pushWin(std::vector<double>& buf, size_t& head, double& sum, int64_t& count, double v){
        sum -= buf[head]; buf[head] = v; sum += v; head = (head + 1) % buf.size(); if (count < (int64_t)buf.size()) ++count;
    }
    double sr_ = 48000; KWeight kL_, kR_;
    std::vector<double> momBuf_, stBuf_; size_t momHead_=0, stHead_=0; double momSum_=0, stSum_=0; int64_t momCount_=0, stCount_=0;
    double rmsFast_=0, rmsSlow_=0; float peak_=0;
    std::vector<double> intBlocks_; double blockAcc_=0; int blockN_=0;   // v0.16 integrated
    double truePeak_=0; float histL_[16]={}, histR_[16]={}; int histN_=0;  // v0.16 true peak
};
} // namespace meterbar
