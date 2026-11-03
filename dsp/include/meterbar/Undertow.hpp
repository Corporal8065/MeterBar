#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Visualizer.hpp" // VizRgb / vizHsvToRgb
namespace meterbar {
// ==================================================================
// UNDERTOW (v0.28.0) — the five-band visualizer section designed by
// the 2026-11-02 visualizer council (DESIGN.md in
// ~/workspace/bar-apps/visualizer-council-2026-11-02/). Jackson's
// word: council first, then build to its output. This header is the
// pure core: the app layer feeds it and renders what it produces.
//
// ARCHITECTURE (DESIGN principles 3 + 4, build notes — invariants):
//  - Path A (timing): FiveBandOnsets — a time-domain 5-band
//    crossover + per-band flux onset detection, fed on the audio
//    ingest path in 256-sample sub-blocks. Timing NEVER passes
//    through an FFT or through any smoothing slower than τ 10 ms.
//  - Path B (level): fiveBandLevelsFromRawDb — the dedicated
//    2048-point SpectrumAnalyser's rawDb() (NEVER the
//    display-smoothed spectrum), regrouped into five bands as RMS
//    over bins in linear power, mapped −60…−6 dB -> 0…1.
//  - The two paths are never derived from each other. Exactly one
//    smoothing stage per signal path; the renderer applies no
//    further filtering of audio envelopes (hold-don't-interpolate),
//    and the model takes REAL elapsed dt, never a fixed frame step.
//  - No second-order spring exists anywhere in this section. The
//    Field style's sub spring is not used here; bounce character
//    comes only from a render-mapping easeOutBack on the kick spike.
//  - Kick detection is BeatTracker, consumed unmodified (the model
//    reads its exported grid timing; it never retunes it).
// ==================================================================

// ---------- follower maths (one-pole, dt-corrected) ----------
inline float tauCoef(float dt, float tau) {
    if (tau <= 0.f) return 1.f;
    const float a = 1.f - std::exp(-dt / tau);
    return a < 0.f ? 0.f : a > 1.f ? 1.f : a;
}
// Attack/release follower step toward `target`.
inline float followAR(float v, float target, float dt, float tauAttack, float tauRelease) {
    const float tau = target > v ? tauAttack : tauRelease;
    return v + (target - v) * tauCoef(dt, tau);
}
// Exponential decay toward zero with time constant tau.
inline float decayTau(float v, float dt, float tau) {
    return v * std::exp(-dt / std::max(tau, 1e-6f));
}
// DESIGN §(c) tail tapers (age in seconds; nothing lives past 220 ms).
inline float tailWidthFactor(float ageSec) {
    const float t = 1.f - ageSec / 0.220f;
    return t <= 0.f ? 0.f : std::pow(t, 1.5f);
}
inline float tailAlphaFactor(float ageSec) {
    const float t = 1.f - ageSec / 0.220f;
    return t <= 0.f ? 0.f : t * t; // quadratic: below ~2% by prune time, so removal is invisible
}
// DESIGN §(f): the drop regime's element-size soft clip.
inline float softClipUnit(float x) {
    return std::tanh(1.3f * x) / std::tanh(1.3f);
}
// DESIGN §(b): the kick pulse's committed numbers, named once.
inline constexpr float kUndertowKickReleaseTau = 0.065f;     // exponential release τ 65 ms
inline constexpr float kUndertowPipelineCompSec = 0.034f;    // fixed pipeline compensation for LOCKED prediction
inline constexpr float kUndertowPredictedCap = 0.45f;        // unconfirmed grid prediction amplitude cap
inline constexpr float kUndertowConfirmWindowSec = 0.060f;   // onset within ±60 ms of the grid beat confirms it

// ==================================================================
// PATH A — five-band time-domain onset detection (DESIGN §(a)).
// Corners 60 / 250 / 2000 / 8000 Hz, cascaded one-pole splits in
// the splitScopeBands pattern extended to four corners (the five
// bands sum back to the input exactly). Per band: flux onset
// detection against the band's own slow baseline (τ 0.7 s,
// BeatPulse's logic per band), a peak gate (~45% of the band's 8 s
// decaying peak) on sub and low — the v0.26.1 bass-train trap is
// why the gate exists — per-band refractories, and a per-band
// decaying-peak AGC that lives INSIDE detection only (it sets
// thresholds; it never scales how loud the picture is — DESIGN
// §(f) keeps detection normalisation and display gain apart).
// Onsets latch (strength, detector-clock time, count) for the UI
// thread to drain — the same unsynchronised-scalar pattern the
// engine already uses for BeatPulse/BeatTracker reads.
// ==================================================================
class FiveBandOnsets {
public:
    static constexpr int kBands = 5; // 0 sub, 1 low, 2 mid, 3 high, 4 presence
    static constexpr int kSubBlock = 256; // 5.33 ms at 48 kHz

    void prepare(double sr) {
        sr_ = sr > 0 ? sr : 48000;
        auto coef = [&](double hz) { return 1.0 - std::exp(-2.0 * 3.141592653589793 * hz / sr_); };
        a1_ = coef(60.0); a2_ = coef(250.0); a3_ = coef(2000.0); a4_ = coef(8000.0);
        lp1_ = lp2_ = lp3_ = lp4_ = 0;
        for (int b = 0; b < kBands; ++b) {
            sumSq_[b] = 0; baseline_[b] = 0; peak_[b] = 0; prevMs_[b] = 0; msAvg_[b] = 0;
            sinceOnset_[b] = 1e9; lastStrength_[b] = 0; lastOnsetSec_[b] = -1; count_[b] = 0;
            thresholdScale_[b] = 1.f;
        }
        fill_ = 0; elapsed_ = 0; samples_ = 0;
    }

    // Feed any block; the class sub-blocks at 256 samples. L/R are
    // mixed to mono exactly as BeatTracker mixes (0.5*(L+R)).
    void pushBlock(const float* L, const float* R, int n) {
        if (n <= 0 || sr_ <= 0) return;
        for (int i = 0; i < n; ++i) {
            const double m = 0.5 * ((L ? (double)L[i] : 0.0) + (R ? (double)R[i] : 0.0));
            lp1_ += a1_ * (m - lp1_);
            const double r1 = m - lp1_;
            lp2_ += a2_ * (r1 - lp2_);
            const double r2 = r1 - lp2_;
            lp3_ += a3_ * (r2 - lp3_);
            const double r3 = r2 - lp3_;
            lp4_ += a4_ * (r3 - lp4_);
            const double band[kBands] = { lp1_, lp2_, lp3_, lp4_, r3 - lp4_ };
            for (int b = 0; b < kBands; ++b) sumSq_[b] += band[b] * band[b];
            ++samples_;
            if (++fill_ >= kSubBlock) { fill_ = 0; finaliseSubBlock(); }
        }
    }

    float lastStrength(int band) const { return lastStrength_[band]; }
    double lastOnsetSec(int band) const { return lastOnsetSec_[band]; }
    uint64_t onsetCount(int band) const { return count_[band]; }
    double nowSec() const { return sr_ > 0 ? (double)samples_ / sr_ : 0.0; }
    // Pre-arm ceiling (DESIGN T2-5): the model may lower a band's
    // ratio threshold by at most ~12% for one beat position. Written
    // by the UI thread, read here — a benign float race, the house
    // pattern for cross-thread scalars.
    void setThresholdScale(int band, float s) {
        thresholdScale_[band] = s < 0.5f ? 0.5f : s > 1.f ? 1.f : s;
    }

private:
    void finaliseSubBlock() {
        const double blockSec = kSubBlock / sr_;
        elapsed_ += blockSec;
        const double aBase = 1.0 - std::exp(-blockSec / 0.7); // τ 0.7 s baseline (BeatPulse)
        // Committed per-band detection table (DESIGN §(a) + element
        // table refractories; presence has the strictest floor).
        static constexpr double kRatio[kBands] = { 2.2, 2.0, 1.9, 1.8, 2.4 };
        static constexpr double kFluxPrev = 1.58, kFluxBase = 1.35; // BeatPulse v0.27.0 flux bars
        static constexpr double kRefractory[kBands] = { 0.150, 0.150, 0.120, 0.090, 0.080 };
        static constexpr double kFloor[kBands] = { 1e-8, 1e-8, 1e-8, 1e-8, 1e-6 };
        const double now = (double)samples_ / sr_;
        for (int b = 0; b < kBands; ++b) {
            const double msRaw = sumSq_[b] / kSubBlock;
            sumSq_[b] = 0;
            // Build correction (test-caught, v0.28.0): a 256-sample
            // block is a FRACTION of one cycle at low frequencies
            // (5.3 ms vs the 16.7 ms period at 60 Hz), so raw
            // per-block MS oscillates with phase and the flux bar
            // false-fires on STEADY tones — a held bass note would
            // pulse like a kick. The oscillation reaches EVERY
            // band, because the linear crossover leaks a small
            // steady low-frequency residual upward. All bands
            // therefore test a 4-block (~21 ms, just over one
            // 60 Hz cycle) trailing integral of block MS.
            // Detection-internal only: no display path sees this
            // window, onset times shift by ~2 blocks (inside the
            // model's ±60 ms confirmation window), genuine band
            // content (periods at or under the block length) is
            // numerically unaffected, and 90 ms kick bursts still
            // clear the ratio bar easily.
            double ms;
            if (msAvg_[b] <= 0 && msRaw > 0) msAvg_[b] = msRaw; // cold start, like the baseline seed
            else msAvg_[b] += (msRaw - msAvg_[b]) * 0.45;
            ms = msAvg_[b];
            // v0.28.1: the trailing integral above shortens from
            // 0.25 (~21 ms) to 0.45 (~12 ms). Scored probes (known
            // kick placements) found TP/FP counts identical at
            // 0.25 / 0.45 / 1.0 on the synthetic set, so the lag
            // halves while some phase smoothing — the reason the
            // integral exists — is kept.
            sinceOnset_[b] += blockSec;
            // The decaying peak updates BEFORE the fire test, with
            // the current block included (BeatTracker's rule: a
            // genuine hit trivially clears a gate set by its own
            // peak; the bass under it never does).
            peak_[b] = std::max(ms, peak_[b] * std::exp(-blockSec / 8.0));
            const double ratioThresh = kRatio[b] * (double)thresholdScale_[b];
            const bool ratioHit = baseline_[b] > 1e-12 && ms > baseline_[b] * ratioThresh;
            const bool fluxHit = baseline_[b] > 1e-12 && ms > baseline_[b] * kFluxBase
                                 && prevMs_[b] > 0 && ms > prevMs_[b] * kFluxPrev;
            // Peak gate on sub and low only (45% of the 8 s decaying
            // peak, power vs power — BeatTracker's form).
            // v0.28.1: the SUB band's gate peak adapts (see the
            // candidate classification below) — scored probe F4
            // showed the flat gate losing 24 of 47 kicks on
            // verse/chorus material, because a quiet verse can never
            // reach 45% of the chorus's peak.
            if (b == 0 && adaptValid0_)
                peakAdapt0_ += (peak_[0] - peakAdapt0_) * (1.0 - std::exp(-blockSec / 20.0));
            const double gatePeak = (b == 0 && adaptValid0_) ? std::min(peak_[0], peakAdapt0_) : peak_[b];
            const bool gated = (b <= 1) && ms < gatePeak * 0.45;
            // Shadow candidate (sub only): a transient clearing a
            // STRONGER flux bar (x3.2 vs the x2.2 fire bar) but
            // stopped by the peak gate is remembered, not fired.
            // 250 ms later it is classified: a kick is a burst, so
            // the band statistic must have fallen under 40% of the
            // candidate value; a kick-like candidate lowers the
            // adaptive peak to its own level (+ headroom), and the
            // NEXT verse kick passes the real gate. Sustained bass
            // fails the burst test and never lowers anything.
            if (b == 0) {
                if (pendCand0_ && now - pendCandT0_ >= 0.25) {
                    pendCand0_ = false;
                    if (ms < pendCandMs0_ * 0.40) {
                        const double cand = pendCandMs0_ * 2.55;
                        if (!adaptValid0_ || cand < peakAdapt0_) peakAdapt0_ = cand;
                        adaptValid0_ = true;
                    }
                }
                if (!pendCand0_ && gated && elapsed_ > 0.6 && ms > kFloor[0]
                    && baseline_[0] > 1e-12 && ms > baseline_[0] * 3.2
                    && sinceOnset_[0] > kRefractory[0]) {
                    pendCand0_ = true; pendCandT0_ = now; pendCandMs0_ = ms;
                }
            }
            if (elapsed_ > 0.6 && ms > kFloor[b] && (ratioHit || fluxHit) && !gated
                && sinceOnset_[b] > kRefractory[b]) {
                double strength = std::clamp((ms / baseline_[b] - ratioThresh) / 5.0 + 0.45, 0.0, 1.0);
                if (fluxHit && strength < 0.4) strength = 0.4; // a sharp attack reads even inside loud material
                lastStrength_[b] = (float)strength;
                lastOnsetSec_[b] = now;
                ++count_[b];
                sinceOnset_[b] = 0;
            }
            baseline_[b] += aBase * (ms - baseline_[b]);
            if (baseline_[b] < 1e-12 && ms > 0) baseline_[b] = ms; // cold start
            prevMs_[b] = ms;
        }
    }

    double sr_ = 48000, a1_ = 0, a2_ = 0, a3_ = 0, a4_ = 0;
    double lp1_ = 0, lp2_ = 0, lp3_ = 0, lp4_ = 0;
    double sumSq_[kBands] = {};
    double baseline_[kBands] = {}, peak_[kBands] = {}, prevMs_[kBands] = {}, msAvg_[kBands] = {};
    double sinceOnset_[kBands] = {};
    float lastStrength_[kBands] = {};
    double lastOnsetSec_[kBands] = {};
    uint64_t count_[kBands] = {};
    float thresholdScale_[kBands] = { 1.f, 1.f, 1.f, 1.f, 1.f };
    // v0.28.1: sub-band adaptive gate peak + shadow candidate state
    double peakAdapt0_ = 0; bool adaptValid0_ = false;
    double pendCandT0_ = 0, pendCandMs0_ = 0; bool pendCand0_ = false;
    int fill_ = 0;
    double elapsed_ = 0;
    uint64_t samples_ = 0;
};

// ==================================================================
// PATH B — five-band levels from the UNSMOOTHED spectrum.
// rawDb is SpectrumAnalyser::rawDb() (tilted by the analyser's own
// tilt, unsmoothed in time). Regrouping: 20–60 / 60–250 / 250–2k /
// 2k–8k / 8k–18k Hz, aggregated as RMS over bins in linear power,
// mapped −60…−6 dB -> 0…1 (DESIGN §(a)). The 64-point edge profile
// is the sub/low spectrum (20–250 Hz, log-spaced slices) the ground
// membrane's top edge is drawn from.
// ==================================================================
struct FiveBandLevels {
    float band[5] = { 0, 0, 0, 0, 0 };
    static constexpr int kEdgePoints = 64;
    float edge[kEdgePoints] = {};
};
inline float undertowDbToLevel(float db) {
    const float t = (db + 60.f) / 54.f;
    return t < 0.f ? 0.f : t > 1.f ? 1.f : t;
}
inline FiveBandLevels fiveBandLevelsFromRawDb(const std::vector<float>& rawDb, int fftSize, double sr) {
    FiveBandLevels out;
    if (rawDb.empty() || fftSize <= 0 || sr <= 0) return out;
    static constexpr double kLo[5] = { 20.0, 60.0, 250.0, 2000.0, 8000.0 };
    static constexpr double kHi[5] = { 60.0, 250.0, 2000.0, 8000.0, 18000.0 };
    double sum[5] = { 0, 0, 0, 0, 0 }; int cnt[5] = { 0, 0, 0, 0, 0 };
    for (size_t b = 1; b < rawDb.size(); ++b) { // skip DC
        const double f = (double)b * sr / fftSize;
        if (f < 20.0 || f >= 18000.0) continue;
        const double p = std::pow(10.0, (double)rawDb[b] / 10.0); // amplitude dB -> power
        for (int band = 0; band < 5; ++band)
            if (f >= kLo[band] && f < kHi[band]) { sum[band] += p; ++cnt[band]; break; }
    }
    for (int band = 0; band < 5; ++band)
        if (cnt[band] > 0) out.band[band] = undertowDbToLevel((float)(10.0 * std::log10(sum[band] / cnt[band] + 1e-12)));
    // Edge profile: 64 log-spaced slices across 20–250 Hz; a slice
    // with no bin takes its nearest bin (at 2048/48k the bin width
    // is 23.4 Hz, so the lowest slices share bins — honest shape,
    // no invented resolution).
    for (int i = 0; i < FiveBandLevels::kEdgePoints; ++i) {
        const double f0 = 20.0 * std::pow(250.0 / 20.0, (double)i / FiveBandLevels::kEdgePoints);
        const double f1 = 20.0 * std::pow(250.0 / 20.0, (double)(i + 1) / FiveBandLevels::kEdgePoints);
        double s = 0; int c = 0;
        for (size_t b = 1; b < rawDb.size(); ++b) {
            const double f = (double)b * sr / fftSize;
            if (f >= f0 && f < f1) { s += std::pow(10.0, (double)rawDb[b] / 10.0); ++c; }
        }
        if (c == 0) { // nearest bin to the slice centre
            const double fc = std::sqrt(f0 * f1);
            const int b = std::clamp((int)std::lround(fc * fftSize / sr), 1, (int)rawDb.size() - 1);
            s = std::pow(10.0, (double)rawDb[(size_t)b] / 10.0); c = 1;
        }
        out.edge[i] = undertowDbToLevel((float)(10.0 * std::log10(s / c + 1e-12)));
    }
    return out;
}

// ==================================================================
// The model. Fed once per UI frame with REAL elapsed dt (DESIGN
// build notes: the fixed 1/fpsLimit step other styles use is banned
// here — envelope wall-clock rates must be true). Consumes Path B
// levels, a BeatTracker feed (grid timing exported by the tracker,
// consumed unmodified) and the Path A onset latch. Produces only
// draw state; the renderer applies no further smoothing.
// ==================================================================
struct UndertowBeatFeed {
    bool locked = false;
    float confidence = 0;
    float bpm = 0;
    float secondsToNextBeat = -1;   // tracker clock; -1 = no grid
    bool nextIsDownbeat = false;
    uint64_t gridBeatCount = 0;
    int countInBar = 0;             // count of the most recent grid beat (1..bpb)
    int beatsPerBar = 4;
    double trackerNowSec = 0;
    double lastGridBeatSec = -1;
};
struct UndertowOnsetFeed {
    uint64_t count[5] = { 0, 0, 0, 0, 0 };
    float strength[5] = { 0, 0, 0, 0, 0 };
    double lastSec[5] = { -1, -1, -1, -1, -1 };
    double nowSec = 0;
};
// Label safe zones in unit space (DESIGN §(g)/principle 8): the
// layer is dimmed ×0.35 inside them, bloom is masked out of them,
// and nothing spawns inside them (+6 px pad applied by the model
// in unit terms at the canonical 480×270 layer size).
struct UndertowSafeRect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

enum class UndertowBeatState { Reactive, Locked, Ambient };

struct UndertowTrail {
    static constexpr int kMax = 12; // DESIGN §(c): last 12 positions, pruned at 220 ms
    float x[kMax] = {}, y[kMax] = {}, age[kMax] = {};
    int n = 0;
    void reset(float px, float py) { n = 1; x[0] = px; y[0] = py; age[0] = 0; }
    void push(float px, float py) { // newest at index 0
        const int m = n < kMax ? n : kMax - 1;
        for (int i = m; i >= 1; --i) { x[i] = x[i - 1]; y[i] = y[i - 1]; age[i] = age[i - 1]; }
        x[0] = px; y[0] = py; age[0] = 0;
        n = m + 1;
    }
    void advance(float dt) {
        for (int i = 0; i < n; ++i) age[i] += dt;
        while (n > 0 && age[n - 1] > 0.220f) --n;
    }
};
struct UndertowSwarmDot {
    bool alive = false;
    float x = 0, y = 0, vx = 0, vy = 0, age = 0, life = 1, size = 2, alpha = 0;
    UndertowTrail trail; float trailTimer = 0;
};
struct UndertowOrb {
    float x = 0.5f, y = 0.5f, vx = 0, vy = 0;
    float baseRadius = 20;   // px at the canonical 270 px layer height (diameter 28–70 px per DESIGN §(a))
    float slice = 0;         // this orb's sub-range of the low band, 0..1
    float radius = 0, alpha = 0;
    UndertowTrail trail; float trailTimer = 0;
};
struct UndertowTick {
    bool alive = false;
    float x = 0, y = 0, vx = 0, vy = 0, age = 0, life = 0.17f, len = 14, alpha = 0, strength = 0;
    UndertowTrail trail; float trailTimer = 0;
};
struct UndertowRing {
    bool active = false;
    float x = 0.5f;          // emitter position (unit width); y is fixed at 0.76 height
    float age = 0, amp = 0;
    bool warm = false;       // beat-1 ring: ~10% toward #E8D9B0 (DESIGN §(e))
};

class UndertowModel {
public:
    static constexpr int kSwarmCap = 240;
    static constexpr int kOrbCount = 12;
    static constexpr int kTickCap = 32;
    static constexpr int kRingCap = 6;
    static constexpr int kDustCap = 60;
    static constexpr int kDotTarget = 260;  // DESIGN §(f): live dots ≤260 target
    static constexpr int kDotHardCap = 400; // / 400 hard cap

    UndertowModel() {
        // Deterministic pool seeding (splitmix, like the other engines).
        for (int i = 0; i < kOrbCount; ++i) {
            auto& o = orbs_[(size_t)i];
            const float ang = (float)i / (float)kOrbCount * 6.2831853f;
            o.x = 0.5f + 0.30f * std::cos(ang); o.y = 0.5f + 0.26f * std::sin(ang);
            o.baseRadius = 14.f + next01() * 21.f; // radius 14–35 px -> diameter 28–70 px
            o.slice = ((float)i + 0.5f) / (float)kOrbCount;
            o.trail.reset(o.x, o.y);
        }
    }

    // --- grading hooks (DESIGN build notes): one committed table,
    // live-editable; defaults ARE the council's numbers. ---
    float pulseTau_[5] = { kUndertowKickReleaseTau, 0.055f, 0.045f, 0.035f, 0.028f }; // per-band pulse release τ (element table)
    bool preArmEnabled = true;

    void setSafeRects(const std::vector<UndertowSafeRect>& r) { safe_ = r; }
    void setKickTau(float tauSec) { pulseTau_[0] = tauSec < 0.040f ? 0.040f : tauSec > 0.120f ? 0.120f : tauSec; }

    void update(float dt, const FiveBandLevels& lv, float overallDb,
                const UndertowBeatFeed& beat, const UndertowOnsetFeed& onsets) {
        if (dt <= 0) return;
        if (dt > 0.1f) dt = 0.1f; // a stalled frame must not teleport envelopes
        time_ += dt;
        levels_ = lv;

        // ---- Path B level envelopes: one per band, attack τ 30 ms
        // / release τ 90 ms (DESIGN §(a) level column). ----
        for (int b = 0; b < 5; ++b)
            levelEnv_[b] = followAR(levelEnv_[b], lv.band[b], dt, 0.030f, 0.090f);
        // Membrane channels (DESIGN §(a)): base height rides the
        // level envelope; fill alpha rides ONLY the sustain envelope
        // (attack τ 15 / release τ 220) — the 808-tail channel.
        membSustain_ = followAR(membSustain_, lv.band[0], dt, 0.015f, 0.220f);
        if (state_ == UndertowBeatState::Ambient) // no-kick music: the ground follows the LOW band at the sustain τ
            membLevel_ = followAR(membLevel_, lv.band[1], dt, 0.220f, 0.220f);
        else
            membLevel_ = followAR(membLevel_, lv.band[0], dt, 0.030f, 0.090f);

        // ---- grid beat bookkeeping (positions, bars, lock streak) ----
        const bool condLocked = beat.locked && beat.confidence >= 0.60f;
        if (beat.gridBeatCount != seenGridCount_) {
            const uint64_t fresh = beat.gridBeatCount - seenGridCount_;
            seenGridCount_ = beat.gridBeatCount;
            for (uint64_t k = 0; k < fresh; ++k) {
                if (condLocked) ++lockStreakBeats_; else lockStreakBeats_ = 0;
                if (beat.countInBar == 1) {
                    ++barCounter_;
                    if (drop_) { ++barsAtHigh_; if (barsAtHigh_ >= 16) habitScale_ = 0.85f; } // 16-bar habituation (DESIGN §(f))
                }
                currentBarPos_ = beat.countInBar;
                // Backbeat side-bursts (DESIGN §(a)): grid mode only,
                // non-downbeat even counts answer the kick.
                if (state_ == UndertowBeatState::Locked && beat.countInBar > 1 && beat.countInBar % 2 == 0)
                    spawnBackbeatBurst();
            }
        }

        // ---- beat state machine (hysteresis: enter on 2 beats, exit on 4 s) ----
        if (state_ != UndertowBeatState::Locked && lockStreakBeats_ >= 2) {
            state_ = UndertowBeatState::Locked; lockedLostTimer_ = 0;
        }
        if (state_ == UndertowBeatState::Locked) {
            if (condLocked) lockedLostTimer_ = 0;
            else { lockedLostTimer_ += dt; if (lockedLostTimer_ > 4.f) { state_ = UndertowBeatState::Reactive; lockStreakBeats_ = 0; } }
        }
        if (!beat.locked) noGridTimer_ += dt; else noGridTimer_ = 0;
        if (lv.band[0] < 0.03f) subQuietTimer_ += dt; else subQuietTimer_ = 0;
        if (state_ != UndertowBeatState::Ambient && noGridTimer_ > 10.f && subQuietTimer_ > 2.f)
            state_ = UndertowBeatState::Ambient;
        if (state_ == UndertowBeatState::Ambient && (beat.locked || lv.band[0] > 0.10f))
            state_ = UndertowBeatState::Reactive;

        // ---- drain Path A onsets (latest latch per band) ----
        for (int b = 0; b < 5; ++b) {
            if (onsets.count[b] == seenOnsetCount_[b]) continue;
            seenOnsetCount_[b] = onsets.count[b];
            const float strength = onsets.strength[b];
            const double oTime = onsets.lastSec[b];
            recordOnsetForPreArm(b);
            if (b == 0) onSubOnset(strength, oTime, beat);
            else if (b == 1) bandPulse_[1] = std::max(bandPulse_[1], strength);
            else if (b == 2) bandPulse_[2] = std::max(bandPulse_[2], strength);
            else if (b == 3) pendingTicks_ += 1; // spawned below, rate-capped
            // band 4 (presence): computed, deliberately drives nothing in v1 (DESIGN §(a))
        }

        // ---- LOCKED grid prediction (DESIGN §(b)): fires at the
        // predicted beat minus the 34 ms pipeline compensation,
        // capped at 0.45 unless an onset already confirmed it. ----
        if (state_ == UndertowBeatState::Locked && beat.secondsToNextBeat >= 0) {
            const uint64_t upcoming = beat.gridBeatCount + 1;
            const double beatTimeEst = beat.trackerNowSec + beat.secondsToNextBeat;
            if (predictedFor_ != upcoming && beat.secondsToNextBeat - (double)kUndertowPipelineCompSec <= 0.0) {
                predictedFor_ = upcoming;
                const bool confirmed = std::abs(onsets.lastSec[0] - beatTimeEst) <= (double)kUndertowConfirmWindowSec
                                       && onsets.count[0] > 0;
                float amp = confirmed ? 1.f : kUndertowPredictedCap;
                if (dropFirstBeat_) { amp = 1.f; dropFirstBeat_ = false; } // the drop's first kick fires at full accent (DESIGN §(f))
                fireKick(amp, beat.nextIsDownbeat);
                pendingRingSlot_ = spawnRing(amp, beat.nextIsDownbeat, beat.bpm);
                pendingPredicted_ = true; pendingBeatTime_ = beatTimeEst;
                pendingDownbeat_ = beat.nextIsDownbeat; pendingAmp_ = amp;
            }
        }

        // ---- pulse envelope decays (the committed τ table) ----
        kickAge_ += dt;
        kickValue_ = decayTau(kickValue_, dt, pulseTau_[0]);
        bandPulse_[0] = decayTau(bandPulse_[0], dt, pulseTau_[0]);
        for (int b = 1; b < 5; ++b) bandPulse_[b] = decayTau(bandPulse_[b], dt, pulseTau_[b]);

        // ---- pre-arm ceiling (DESIGN T2-5, shipped form): a band
        // that fired at the upcoming bar position in BOTH of the
        // last 2 bars pre-brightens ≤15% and gets a slightly lower
        // detection threshold for that beat. It may never fire,
        // time, or scale a visual event. ----
        {
            const int bpb = beat.beatsPerBar > 0 ? beat.beatsPerBar : 4;
            const int upcomingPos = currentBarPos_ > 0 ? (currentBarPos_ % bpb) + 1 : 1;
            for (int b = 0; b < 5; ++b) {
                preArm_[b] = preArmEnabled && state_ == UndertowBeatState::Locked
                             && firedAtPos(b, upcomingPos, barCounter_ - 1)
                             && firedAtPos(b, upcomingPos, barCounter_ - 2);
                thresholdScale_[b] = preArm_[b] ? 0.88f : 1.f;
                preArmGlow_[b] = preArm_[b] ? 1.15f : 1.f;
            }
        }

        updateGovernor(dt, lv, overallDb, beat);
        updateElements(dt, lv, beat);
        updateLimiter(dt);
    }

    // ---- read surface for the renderer (and the tests) ----
    UndertowBeatState beatState() const { return state_; }
    float kickValue() const { return kickValue_; }
    // The spike's drawn height: the envelope through the render-only
    // easeOutBack overshoot (~40 ms) — bounce lives in the mapping,
    // never in the signal (DESIGN §(b), dissent: spring).
    float kickSpikeDisplay() const {
        float m = 1.f;
        if (kickAge_ < 0.040f) m = 1.f + 0.22f * std::sin(3.14159265f * kickAge_ / 0.040f);
        return kickValue_ * m;
    }
    float bandPulse(int b) const { return bandPulse_[b]; }
    float bandLevel(int b) const { return levelEnv_[b]; }
    float membraneHeight() const { return 0.18f * membLevel_; }        // fraction of section height, 0..0.18
    float membraneFillAlpha() const { return 0.85f * membSustain_; }   // sustain channel drives fill alpha only
    const FiveBandLevels& levels() const { return levels_; }
    const std::array<UndertowSwarmDot, kSwarmCap>& swarm() const { return swarm_; }
    const std::array<UndertowOrb, kOrbCount>& orbs() const { return orbs_; }
    const std::array<UndertowTick, kTickCap>& ticks() const { return ticks_; }
    const std::array<UndertowRing, kRingCap>& rings() const { return rings_; }
    int dustCount() const { return dustCount_; }
    float dustAlpha() const { return dustAlpha_; }
    void dustPoint(int i, float& x, float& y) const {
        // Near-still shimmer points at signal-driven positions:
        // hashed placements, refreshed on a slow epoch (DESIGN §(a)).
        const uint32_t h1 = splitmix(i * 2 + dustEpoch_ * 1315423911u);
        const uint32_t h2 = splitmix(i * 2 + 1 + dustEpoch_ * 2654435761u);
        x = 0.03f + (h1 / 4294967296.0f) * 0.94f;
        y = 0.03f + (h2 / 4294967296.0f) * 0.94f;
    }
    // Governor read surface.
    float activity() const { return activityA_; }
    bool still() const { return still_; }
    bool drop() const { return drop_; }
    float silenceGate() const { return gate_; }
    float limiterGain() const { return limiterGain_; }
    float displayGain() const { return limiterGain_ * gate_; }
    float estimatedLuminance() const { return lastEst_; }
    float darkFraction() const { return 1.f - lastEst_; } // the ≥60%-near-black soak figure (DESIGN §(e))
    bool preArm(int b) const { return preArm_[b]; }
    float thresholdScale(int b) const { return thresholdScale_[b]; }
    int liveDots() const {
        int n = dustCount_;
        for (auto& p : swarm_) if (p.alive) ++n;
        for (auto& t : ticks_) if (t.alive) ++n;
        return n + kOrbCount;
    }
    double time() const { return time_; }

    // DESIGN §(e): the fixed Midnight Kits band palette — hues are
    // redundant coding only; form, location and motion carry band
    // identity. No hue cycling, ever.
    static VizRgb bandColour(int band) {
        switch (band) {
            case 0: return VizRgb{ 0x4A, 0x5F, 0xD9 };  // sub: indigo core
            case 1: return VizRgb{ 0x33, 0x50, 0x7A };  // low: slate blue
            case 2: return VizRgb{ 0xE8, 0xEE, 0xF5 };  // mid: warm white (the hero)
            case 3: return VizRgb{ 0x9F, 0xD8, 0xE8 };  // high: pale cyan
            default: return VizRgb{ 0xCF, 0xC8, 0xE8 }; // presence: lavender-white
        }
    }
    static VizRgb membraneEdgeColour() { return VizRgb{ 0x7C, 0x8C, 0xF0 }; }
    static VizRgb ringWarmColour() { return VizRgb{ 0xE8, 0xD9, 0xB0 }; } // the bar's sole warm note

private:
    static uint32_t splitmix(uint32_t z) {
        z = (z ^ (z >> 16)) * 0x85ebca6bu;
        z = (z ^ (z >> 13)) * 0xc2b2ae35u;
        return z ^ (z >> 16);
    }
    float next01() { rng_ += 0x9e3779b9u; return (float)(splitmix(rng_) >> 8) / 16777216.f; }

    bool insideSafe(float x, float y, float padX, float padY) const {
        for (auto& r : safe_)
            if (x >= r.x0 - padX && x <= r.x1 + padX && y >= r.y0 - padY && y <= r.y1 + padY) return true;
        return false;
    }

    void fireKick(float amp, bool downbeat) {
        const float v = amp * (downbeat ? 1.25f : 1.f);
        if (v > kickValue_) { kickValue_ = v; kickAge_ = 0; }
        bandPulse_[0] = std::max(bandPulse_[0], amp);
    }

    int spawnRing(float amp, bool warm, float bpm) {
        if (state_ == UndertowBeatState::Ambient || still_) return -1; // accents disabled in AMBIENT; Still is rest
        if (bpm > 160.f && (barCounter_ % 2) != 0) return -1;          // >160 BPM: accents halve (DESIGN §(g))
        for (int i = 0; i < kRingCap; ++i) {
            auto& r = rings_[(size_t)i];
            if (r.active) continue;
            static constexpr float kEmitters[5] = { 0.12f, 0.31f, 0.50f, 0.69f, 0.88f }; // distributed (DESIGN §(a))
            emitter_ = (emitter_ + 1) % 5; // consecutive rings never share an emitter
            r.active = true; r.x = kEmitters[emitter_]; r.age = 0; r.amp = amp; r.warm = warm;
            return i;
        }
        return -1;
    }

    void onSubOnset(float strength, double oTime, const UndertowBeatFeed& beat) {
        // Detector-fault guard (DESIGN §(g)): sustained sub onsets
        // above ~8/s are a fault signature, not music — distrust
        // sub onsets until the rate normalises below ~4/s.
        subOnsetTimes_[subOnsetHead_] = oTime; subOnsetHead_ = (subOnsetHead_ + 1) % 16;
        int recent = 0; for (double t : subOnsetTimes_) if (t > 0 && oTime - t <= 1.0) ++recent;
        if (recent > 8) subDistrust_ = true;
        if (subDistrust_ && recent <= 4) subDistrust_ = false;
        if (state_ == UndertowBeatState::Ambient) state_ = UndertowBeatState::Reactive; // the kick came back
        if (subDistrust_ || state_ == UndertowBeatState::Ambient) return;
        // Confirmation: an onset within ±60 ms of the pending
        // predicted beat upgrades THAT beat to full strength — its
        // ring (already born at the cap) follows the spike up, and
        // no second ring is born for the same hit.
        const bool confirmedPending = pendingPredicted_
            && std::abs(oTime - pendingBeatTime_) <= (double)kUndertowConfirmWindowSec;
        if (confirmedPending) {
            pendingPredicted_ = false;
            if (pendingRingSlot_ >= 0 && rings_[(size_t)pendingRingSlot_].active)
                rings_[(size_t)pendingRingSlot_].amp = 1.f;
            pendingRingSlot_ = -1;
            fireKick(1.f, pendingDownbeat_);
            return;
        }
        const bool nearGridDownbeat = beat.lastGridBeatSec >= 0
            && std::abs(oTime - beat.lastGridBeatSec) <= (double)kUndertowConfirmWindowSec
            && beat.countInBar == 1;
        fireKick(strength, nearGridDownbeat);
        spawnRing(strength, nearGridDownbeat, beat.bpm);
    }

    void recordOnsetForPreArm(int band) {
        auto& h = onsetHistory_[band];
        h[onsetHistHead_[band]] = { barCounter_, currentBarPos_ };
        onsetHistHead_[band] = (onsetHistHead_[band] + 1) % 32;
    }
    bool firedAtPos(int band, int pos, int bar) const {
        if (bar < 0 || pos <= 0) return false;
        for (auto& e : onsetHistory_[band]) if (e.bar == bar && e.pos == pos) return true;
        return false;
    }

    void updateGovernor(float dt, const FiveBandLevels& lv, float overallDb, const UndertowBeatFeed& beat) {
        activityA_ = 0.45f * lv.band[0] + 0.25f * lv.band[1] + 0.20f * lv.band[2] + 0.10f * lv.band[3];
        // Still (DESIGN §(f)): A < 0.15 for >1 s, or RMS < −38 dBFS
        // sustained 600 ms; exits when activity returns.
        if (activityA_ < 0.15f) aLowTimer_ += dt; else aLowTimer_ = 0;
        if (overallDb < -38.f) rmsLowTimer_ += dt; else rmsLowTimer_ = 0;
        if (!still_ && (aLowTimer_ > 1.f || rmsLowTimer_ > 0.6f)) still_ = true;
        if (still_ && activityA_ >= 0.20f) still_ = false;
        // Silence gate: attack τ 20 ms, release τ 300 ms, hard-zero
        // after 800 ms of true silence (tightened from Field's τ≈1.25 s).
        if (overallDb > -58.f) {
            absentTimer_ = 0;
            gate_ = followAR(gate_, 1.f, dt, 0.020f, 0.300f);
        } else {
            absentTimer_ += dt;
            gate_ = followAR(gate_, 0.f, dt, 0.020f, 0.300f);
            if (absentTimer_ > 0.8f) gate_ = 0.f;
        }
        // Drop: A ≥ 0.75 sustained 500 ms, or a ≥14 dB sub/mid rise
        // within 250 ms after ≥2 s of Still/low. (Levels are mapped
        // −60…−6 dB -> 0…1, so 14 dB ≈ 0.259 of the level range.)
        aDelayed_ = followAR(aDelayed_, activityA_, dt, 0.250f, 0.250f);
        if (activityA_ < minA_) minA_ = activityA_;
        else minA_ = followAR(minA_, activityA_, dt, 2.0f, 2.0f);
        if (activityA_ >= 0.75f) aHighTimer_ += dt; else aHighTimer_ = 0;
        const float rise = std::max(lv.band[0], lv.band[2]) - delayedSubMid_;
        delayedSubMid_ = followAR(delayedSubMid_, std::max(lv.band[0], lv.band[2]), dt, 0.250f, 0.250f);
        if (!drop_ && (aHighTimer_ > 0.5f || (rise >= 0.259f && minA_ < 0.30f))) {
            drop_ = true; dropRampT_ = 0; dropFirstBeat_ = true; barsAtHigh_ = 0; habitScale_ = 1.f;
        }
        if (drop_) {
            const float beatDur = beat.locked && beat.bpm > 0 ? 60.f / beat.bpm : 0.5f;
            dropRampT_ = std::min(1.f, dropRampT_ + dt / (2.f * beatDur)); // density ramps over 2 beats, never in one frame
            if (activityA_ < 0.45f) { dropExitTimer_ += dt; if (dropExitTimer_ > 1.f) { drop_ = false; habitScale_ = 1.f; } }
            else dropExitTimer_ = 0;
        }
        // Density (DESIGN §(f)): drop scales ∝ level^1.6; presence
        // halves and caps tighten under load (applied in elements).
        if (still_) densityDrive_ = 0.f;
        else if (drop_) densityDrive_ = std::pow(std::clamp(activityA_ * 1.25f, 0.f, 1.f), 1.6f) * (0.35f + 0.65f * dropRampT_);
        else densityDrive_ = std::clamp(0.20f + 0.80f * activityA_, 0.f, 1.f);
    }

    void updateElements(float dt, const FiveBandLevels& lv, const UndertowBeatFeed& beat) {
        (void)beat;
        // --- echo rings: follow-through only, dead at 280 ms ---
        for (auto& r : rings_) if (r.active) { r.age += dt; if (r.age >= 0.280f) r.active = false; }
        // --- low orbs: drift AGAINST the swarm's swirl so the
        // layers read as separate (DESIGN §(a)); pulse from the low
        // band's own transient envelope, radius from its level. ---
        {
            const float speed = (still_ ? 0.003f : 0.02f + 0.10f * levelEnv_[1]);
            for (auto& o : orbs_) {
                const float dx = 0.5f - o.x, dy = 0.5f - o.y;
                const float dist = std::sqrt(dx * dx + dy * dy) + 1e-5f;
                // tangential (negative = against the swarm) + weak inward pull
                o.vx += ((dy / dist) * speed + (dx / dist) * 0.010f) * dt * 8.f;
                o.vy += ((-dx / dist) * speed + (dy / dist) * 0.010f) * dt * 8.f;
                o.vx *= 0.985f; o.vy *= 0.985f;
                o.x += o.vx * dt; o.y += o.vy * dt;
                if (o.x < 0.03f) { o.x = 0.03f; o.vx = std::abs(o.vx); }
                if (o.x > 0.97f) { o.x = 0.97f; o.vx = -std::abs(o.vx); }
                if (o.y < 0.03f) { o.y = 0.03f; o.vy = std::abs(o.vy); }
                if (o.y > 0.97f) { o.y = 0.97f; o.vy = -std::abs(o.vy); }
                o.radius = o.baseRadius * (0.45f + softClipUnit(levelEnv_[1]) * (0.55f + 0.45f * o.slice));
                float a = 0.10f + 0.18f * std::max(levelEnv_[1], bandPulse_[1] * 0.85f);
                if (still_) a = std::min(a, 0.10f); // Still: orbs fade to ≤0.10
                o.alpha = a * preArmGlow_[1];
                o.trailTimer += dt;
                if (o.trailTimer >= 0.018f) { o.trailTimer = 0; o.trail.push(o.x, o.y); }
                o.trail.advance(dt);
            }
        }
        // --- mid swarm: the hero. Spawn at the edges, swirl inward
        // (radial:tangential ≈ 0.6:1); density/speed from the level
        // channel, spawn/brightness from the transient channel; on a
        // mid transient the inward pull strengthens — the surface
        // inhales, then relaxes in the gap (DESIGN §(a)). ---
        {
            int target = (int)std::lround(220.f * densityDrive_ * habitScale_);
            const int others = kOrbCount + dustCount_ + countTicks();
            if (target > kDotTarget - others) target = std::max(0, kDotTarget - others);
            if (target > kSwarmCap) target = kSwarmCap;
            int alive = 0; for (auto& p : swarm_) if (p.alive) ++alive;
            int spawnBudget = 8; // per frame — fast fill without a pop-in frame
            for (auto& p : swarm_) {
                if (!p.alive && alive < target && spawnBudget > 0) { spawnSwarmDot(p); --spawnBudget; ++alive; }
                if (!p.alive) continue;
                p.age += dt;
                const float dx = 0.5f - p.x, dy = 0.5f - p.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < 0.03f || p.age > p.life) { p.alive = false; --alive; continue; }
                const float inv = 1.f / std::max(dist, 1e-4f);
                const float pull = 0.6f * (1.f + 2.2f * bandPulse_[2]); // the inhale
                const float speed = 0.10f + 0.28f * levelEnv_[2];
                const float desX = ((-dy * inv) * 1.0f + (dx * inv) * pull) * speed;
                const float desY = ((dx * inv) * 1.0f + (dy * inv) * pull) * speed;
                const float steer = tauCoef(dt, 0.12f);
                p.vx += (desX - p.vx) * steer; p.vy += (desY - p.vy) * steer;
                p.x += p.vx * dt; p.y += p.vy * dt;
                p.alpha = std::clamp((0.25f + 0.75f * levelEnv_[2]) * (0.55f + 0.65f * bandPulse_[2]) * preArmGlow_[2], 0.f, 1.f);
                p.trailTimer += dt;
                if (p.trailTimer >= 0.018f) { p.trailTimer = 0; p.trail.push(p.x, p.y); }
                p.trail.advance(dt);
            }
        }
        // --- high ticks: transient ONLY for spawning (DESIGN §(a));
        // sustained level lifts the brightness ceiling slightly but
        // never the spawn rate — ticks must mean "a hit happened". ---
        {
            tickBudget_ = std::min(2.f, tickBudget_ + dt * (drop_ ? 8.f : 12.f)); // birth cap ≈12/s
            while (pendingTicks_ > 0) {
                --pendingTicks_;
                if (tickBudget_ < 1.f) continue;
                UndertowTick* slot = nullptr;
                for (auto& t : ticks_) if (!t.alive) { slot = &t; break; }
                if (!slot) break;
                tickBudget_ -= 1.f;
                spawnTick(*slot);
            }
            for (auto& t : ticks_) {
                if (!t.alive) continue;
                t.age += dt;
                if (t.age >= t.life) { t.alive = false; continue; }
                t.x += t.vx * dt; t.y += t.vy * dt;
                t.alpha = t.strength * decayTau(1.f, t.age, pulseTau_[3]) * (1.f + 0.3f * levelEnv_[3]) * preArmGlow_[3];
                t.trailTimer += dt;
                if (t.trailTimer >= 0.018f) { t.trailTimer = 0; t.trail.push(t.x, t.y); }
                t.trail.advance(dt);
            }
        }
        // --- presence dust: level only, alpha written directly each
        // frame (any smoothing here turns air into fog). In Still it
        // is the only element allowed to move, at minimum density. ---
        dustEpoch_ = (uint32_t)(time_ / 2.5);
        if (still_) dustCount_ = 8;
        else {
            int n = 6 + (int)std::lround(lv.band[4] * 54.f);
            dustCount_ = n < 6 ? 6 : n > kDustCap ? kDustCap : n;
        }
        dustAlpha_ = lv.band[4] * 0.18f * (drop_ ? 0.5f : 1.f);
    }

    int countTicks() const { int n = 0; for (auto& t : ticks_) if (t.alive) ++n; return n; }

    void spawnSwarmDot(UndertowSwarmDot& p) {
        // Edge-weighted spawn: sides 60%, top 25%, bottom 15%
        // (DESIGN §(a)); rejection-sampled against the label safe
        // zones (+6 px pad in unit terms at 480×270).
        float x = 0.5f, y = 0.5f;
        for (int tries = 0; tries < 4; ++tries) {
            const float r = next01();
            if (r < 0.60f) { x = next01() < 0.5f ? 0.012f : 0.988f; y = 0.06f + next01() * 0.88f; }
            else if (r < 0.85f) { x = 0.06f + next01() * 0.88f; y = 0.012f; }
            else { x = 0.06f + next01() * 0.88f; y = 0.988f; }
            if (!insideSafe(x, y, 0.0125f, 0.022f)) break;
        }
        p.alive = true; p.x = x; p.y = y; p.vx = 0; p.vy = 0;
        p.age = 0; p.life = 1.2f + next01() * 1.4f;
        p.size = 1.f + next01() * 2.f; // 1–3 px
        p.alpha = 0; p.trail.reset(x, y); p.trailTimer = 0;
    }
    void spawnBackbeatBurst() {
        burstSide_ = 1 - burstSide_;
        const float x = burstSide_ == 0 ? 0.012f : 0.988f;
        int want = 8 + (int)(next01() * 7.f); // 8–14 particles
        for (auto& p : swarm_) {
            if (want <= 0) break;
            if (p.alive) continue;
            p.alive = true; p.x = x; p.y = 0.15f + next01() * 0.70f;
            p.vx = (burstSide_ == 0 ? 1.f : -1.f) * (0.05f + next01() * 0.08f); p.vy = 0;
            p.age = 0; p.life = 1.0f + next01() * 1.2f;
            p.size = 1.f + next01() * 2.f; p.alpha = 0.6f;
            p.trail.reset(p.x, p.y); p.trailTimer = 0;
            --want;
        }
    }
    void spawnTick(UndertowTick& t) {
        t.alive = true;
        t.x = 0.04f + next01() * 0.92f;
        t.y = 0.02f + next01() * 0.23f;               // top 25% of the frame
        t.life = 0.120f + next01() * 0.100f;          // hard lifespan 120–220 ms
        const float fall = (30.f + next01() * 30.f) / 270.f; // fall 30–60 px over the life
        t.vx = (next01() - 0.5f) * 0.03f; t.vy = fall / t.life;
        t.len = 8.f + next01() * 14.f;                // streak 8–22 px
        t.age = 0; t.strength = bandPulse_[3] > 0.05f ? bandPulse_[3] : 0.6f;
        t.alpha = t.strength;
        t.trail.reset(t.x, t.y); t.trailTimer = 0;
    }

    // The visual limiter (DESIGN §(f)): a rolling 1-second average
    // of the section's estimated luminance, capped at 18% of full
    // scale, with frame-to-frame change clamped to Δ ≤0.18. The
    // gain scales ALPHAS ONLY — it never touches a follower's
    // timing (the envelope-law tests pin that identity).
    void updateLimiter(float dt) {
        const float rawEst = estimateLuminanceRaw();
        float est = rawEst * limiterGain_;
        rollAvg_ += (est - rollAvg_) * tauCoef(dt, 0.34f); // ~1 s rolling window
        if (rollAvg_ > 0.18f && rawEst > 1e-5f) {
            limiterGain_ = std::max(0.05f, limiterGain_ * (0.18f / rollAvg_));
            est = rawEst * limiterGain_;
        } else if (rollAvg_ < 0.16f && limiterGain_ < 1.f) {
            limiterGain_ = std::min(1.f, limiterGain_ + (1.f - limiterGain_) * tauCoef(dt, 2.0f));
            est = rawEst * limiterGain_;
        }
        if (est - prevDisplayed_ > 0.18f && rawEst > 1e-5f) { // Δ clamp (photosensitivity safeguard)
            limiterGain_ = (prevDisplayed_ + 0.18f) / rawEst;
            est = prevDisplayed_ + 0.18f;
        }
        prevDisplayed_ = est;
        lastEst_ = est;
    }
    float estimateLuminanceRaw() const {
        // Model-level proxy for lit fraction × brightness, with a
        // bloom safety factor: bloom spreads light beyond the drawn
        // pixels, so the estimate over-reads on purpose — the real
        // frame stays under the cap the estimate enforces.
        double est = 0;
        est += (double)membraneHeight() * (double)membraneFillAlpha();      // the ground's lit band
        est += (double)kickValue_ * 0.02;                                    // the edge spike
        for (auto& r : rings_) if (r.active) est += 0.18 * (double)r.amp * 0.006;
        for (auto& o : orbs_) est += 3.14159265 * (double)o.radius * (double)o.radius * (double)o.alpha / 129600.0;
        for (auto& p : swarm_) if (p.alive) est += (double)p.alpha * (double)p.size * (double)p.size * 0.785 / 129600.0;
        for (auto& t : ticks_) if (t.alive) est += (double)t.alpha * (double)t.len * 2.0 / 129600.0;
        est += (double)dustCount_ * (double)dustAlpha_ / 129600.0;
        return (float)(est * 1.6);
    }

    // ---- state ----
    std::uint32_t rng_ = 0x28d4eb2fu;
    double time_ = 0;
    FiveBandLevels levels_;
    float levelEnv_[5] = { 0, 0, 0, 0, 0 };
    float membLevel_ = 0, membSustain_ = 0;
    float bandPulse_[5] = { 0, 0, 0, 0, 0 };
    float kickValue_ = 0, kickAge_ = 1;
    UndertowBeatState state_ = UndertowBeatState::Reactive;
    int lockStreakBeats_ = 0;
    float lockedLostTimer_ = 0, noGridTimer_ = 0, subQuietTimer_ = 0;
    uint64_t seenGridCount_ = 0, seenOnsetCount_[5] = { 0, 0, 0, 0, 0 };
    uint64_t predictedFor_ = 0;
    bool pendingPredicted_ = false, pendingDownbeat_ = false;
    int pendingRingSlot_ = -1;
    double pendingBeatTime_ = 0; float pendingAmp_ = 0;
    int currentBarPos_ = 0, barCounter_ = 0;
    struct OnsetHistEntry { int bar = -1, pos = 0; };
    OnsetHistEntry onsetHistory_[5][32];
    int onsetHistHead_[5] = { 0, 0, 0, 0, 0 };
    bool preArm_[5] = { false, false, false, false, false };
    float thresholdScale_[5] = { 1.f, 1.f, 1.f, 1.f, 1.f };
    float preArmGlow_[5] = { 1.f, 1.f, 1.f, 1.f, 1.f };
    double subOnsetTimes_[16] = {};
    int subOnsetHead_ = 0;
    bool subDistrust_ = false;
    // Governor
    float activityA_ = 0, aDelayed_ = 0, minA_ = 0, delayedSubMid_ = 0;
    float aLowTimer_ = 0, rmsLowTimer_ = 0, aHighTimer_ = 0, dropExitTimer_ = 0;
    bool still_ = false, drop_ = false, dropFirstBeat_ = false;
    float gate_ = 0, absentTimer_ = 0;
    float dropRampT_ = 0, densityDrive_ = 0, habitScale_ = 1.f;
    int barsAtHigh_ = 0;
    float limiterGain_ = 1.f, rollAvg_ = 0, prevDisplayed_ = 0, lastEst_ = 0;
    // Elements
    std::array<UndertowSwarmDot, kSwarmCap> swarm_;
    std::array<UndertowOrb, kOrbCount> orbs_;
    std::array<UndertowTick, kTickCap> ticks_;
    std::array<UndertowRing, kRingCap> rings_;
    int emitter_ = 0, burstSide_ = 0;
    int pendingTicks_ = 0;
    float tickBudget_ = 0;
    int dustCount_ = 0;
    float dustAlpha_ = 0;
    uint32_t dustEpoch_ = 0;
    std::vector<UndertowSafeRect> safe_;
};
} // namespace meterbar
