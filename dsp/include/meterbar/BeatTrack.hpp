#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>
namespace meterbar {
// ---------- Auto-BPM beat grid (v0.26.0) ----------
// Jackson's ask (2026-11-02): use the waveform's significant impacts
// to auto-detect BPM and lay downbeats 1 2 3 4 over the displays.
// Design, in his terms: the KICK is the impact to follow — it is
// generally the most transient element in a song, it has space
// between hits, and it carries a very noticeable low end. So onset
// detection runs on a ~140 Hz low-passed copy of the mono mix (the
// kick's band), NOT on broadband energy: a snare or a vocal stab
// cannot masquerade as a kick here the way it can in BeatPulse.
//
// Tempo: the median of the recent kick-to-kick intervals, folded
// into 50..214 BPM. The grid LOCKS once >= 4 intervals agree with
// that median (within 9%, whole multiples allowed) at >= 60%
// consistency. The locking kick is taken as count 1 of a bar.
//
// v0.26.1 onset gate: a low-band crossing only counts as a kick if
// it is also a substantial fraction (>= 45%) of the band's recent
// PEAK level (an 8 s decaying maximum). A real mix's kick band
// carries more than the kick — bass lines, sub tails — and those
// weaker crossings are not kicks. At slow song tempos (< ~107 BPM)
// their spacings (eighth-note bass at 0.375 s under an 80 BPM song)
// sit inside the fold window whole, so v0.26.0 locked those trains
// at DOUBLE tempo — 160 read on Jackson's ~80 BPM field song, the
// grid apparently unable to find the beat. The kick itself sets
// the peak as it fires, so genuine kicks pass the gate at any
// tempo; the bass under them does not.
//
// Through a breakdown (kick drops out) the grid keeps counting on
// its own clock — Jackson: "keep the count even when it's not
// there". Confidence sags slowly while nothing confirms it, and
// the grid only gives up after 12 s without a single kick. A
// re-lock needs a SIGNIFICANT margin (also his words): five
// mutually consistent intervals (>= within 7% of their median)
// whose tempo sits more than 5% away from the grid's. On-grid
// kicks nudge the grid's phase/tempo gently (25% / 20% pulls) so
// human drift is followed without jumps.
//
// Meter: setMeter(beatsPerBar, subdivision). 4/4 = (4,1), 3/4 =
// (3,1), 6/8 = (6,3) — in 6/8 the kick lands on pulses 1 and 4, so
// the grid subdivides the detected kick period into 3 eighth
// pulses and counts 1..6 across the bar. bpm() always reports the
// detected KICK tempo, never the subdivided pulse rate.
class BeatTracker {
public:
    void prepare(double sr) {
        sr_ = sr > 0 ? sr : 48000;
        lpCoef_ = 1.0 - std::exp(-2.0 * 3.141592653589793 * 140.0 / sr_);
        lp_ = 0; lp2_ = 0; baseLow_ = 0; peakLow_ = 0;
        winPow_.assign(kWinPow, 0.0); winHead_ = 0; winSum_ = 0; winCount_ = 0;
        samples_ = 0; elapsed_ = 0; sinceOnset_ = 1e9; lastOnsetT_ = -1;
        intervals_.clear();
        locked_ = false; period_ = 0; anchorT_ = 0; nextBeatT_ = 0;
        pendingCount_ = 1; countInBar_ = 0; conf_ = 0;
        beatEnv_ = 0; downbeatEnv_ = 0; onsets_ = 0; gridBeats_ = 0;
        resetPhase(); countBaseIdx_ = 0; countBasePending_ = false; // v0.28.1
        // The meter (bpb_/sub_) deliberately survives prepare():
        // it is a user setting, not stream state.
    }
    void setMeter(int beatsPerBar, int subdivision) {
        bpb_ = beatsPerBar < 1 ? 1 : beatsPerBar > 12 ? 12 : beatsPerBar;
        sub_ = subdivision < 1 ? 1 : subdivision > 4 ? 4 : subdivision;
        pendingCount_ = 1; // re-phase: the next emitted beat is a downbeat
        resetPhase(); countBasePending_ = true; // v0.28.1: counts re-base at the next count-1 emission; old evidence is meaningless under a new meter
    }
    int beatsPerBar() const { return bpb_; }
    int subdivision() const { return sub_; }

    // v0.28.1: MEASURE PHASE (evidence accumulates in onOnset).
    // The grid's count phase is anchored at whichever kick happened
    // to lock the tempo — musically arbitrary — so until now the
    // app drew every beat identically rather than assert a downbeat
    // it could not support (the v0.26.x 1-2-3-4 numbering was
    // removed for exactly that reason). downbeatCount() returns the
    // count (1..beatsPerBar) the ACCENT EVIDENCE supports as the
    // downbeat, or 0 = unproven. It turns non-zero only when one
    // count's share of on-grid onset power clears the proof gates
    // in recomputePhase(); uniform kicks — most four-on-the-floor —
    // carry no phase information and stay 0, and the UI must then
    // draw beats without measure emphasis.
    int downbeatCount() const { return downbeatCount_; }
    bool isDownbeatCount(int count) const { return downbeatCount_ > 0 && count == downbeatCount_; }
    // Best-minus-second share of onset power across counts (the
    // margin the proof gates judge). 0 with no evidence.
    float phaseMargin() const {
        double tot = 0; for (int i = 0; i < bpb_; ++i) tot += slotE_[i];
        if (tot <= 0) return 0.f;
        double best = 0, second = 0;
        for (int i = 0; i < bpb_; ++i) {
            const double sh = slotE_[i] / tot;
            if (sh > best) { second = best; best = sh; }
            else if (sh > second) second = sh;
        }
        return (float)(best - second);
    }

    // Push one block (the app feeds the same 256-sample sub-blocks
    // as BeatPulse). Returns true on the block where a kick onset
    // fires. The block statistic is the loudest 128-sample
    // (~2.7 ms) moving average of the 140 Hz low band's power
    // anywhere in the block (v0.26.1) — a plain block mean reads
    // the SAME kick up to ~3x weaker when its peak straddles a
    // block boundary, and the peak gate below needs a statistic
    // that reads a kick the same however it lands.
    bool pushBlock(const float* L, const float* R, int n) {
        if (n <= 0 || sr_ <= 0) return false;
        double blockMax = 0;
        for (int i = 0; i < n; ++i) {
            const double m = 0.5 * ((L ? (double)L[i] : 0.0) + (R ? (double)R[i] : 0.0));
            lp_ += lpCoef_ * (m - lp_);
            // v0.27.0 (item 13, busy tracks): a SECOND one-pole stage
            // at the same 140 Hz corner cascades the kick band's
            // rolloff to ~12 dB/oct, so a dense mix's snare body
            // (~190 Hz) and bass harmonics leak far less energy into
            // the statistic the peak gate judges. One extra multiply
            // per sample — the compute stays trivial. Measured on
            // synthetic busy mixes (kick+snare+hats+bassline): the
            // 80 BPM read tightened 79.9 -> 80.0 with the whole
            // v0.26.1 slow-tempo suite unchanged.
            lp2_ += lpCoef_ * (lp_ - lp2_);
            const double p = lp2_ * lp2_;
            winSum_ += p - winPow_[(size_t)winHead_];
            winPow_[(size_t)winHead_] = p;
            winHead_ = (winHead_ + 1) % kWinPow;
            if (winCount_ < kWinPow) ++winCount_;
            const double w = winSum_ / (double)winCount_;
            if (w > blockMax) blockMax = w;
        }
        const double lowMs = blockMax;
        const double blockSec = n / sr_;
        samples_ += n; elapsed_ += blockSec; sinceOnset_ += blockSec;
        const double now = (double)samples_ / sr_;
        const double aBase = 1.0 - std::exp(-blockSec / 0.7);
        bool fired = false;
        // The recent peak updates BEFORE the fire test, with the
        // current block included: a genuine kick's own loudest block
        // trivially clears 45% of a peak it just set, while a bass
        // pluck at a fraction of the kick's power — measured against
        // a peak the kicks keep refreshing — never does.
        peakLow_ = std::max(lowMs, peakLow_ * std::exp(-blockSec / 8.0));
        // Warm-up gate as in BeatPulse (baseline converging), an
        // absolute floor so silence never fires, the v0.26.1 peak
        // gate (see header), and a 220 ms refractory (kicks closer
        // than that are one hit's ring, not two hits).
        // v0.28.1: MISSED-BEAT GATE RELAXATION. Measured failure of
        // the flat 0.45 gate: a loud chorus sets peakLow_, and the
        // quieter verse after it (kick power ~0.2x the chorus's)
        // can NEVER clear 45% of that peak — whole verses went
        // onset-less (probe K3: 11 of 47 kicks missed). Cutting the
        // peak decay to 3 s fixed the verse but let a kick-less
        // breakdown's bassline fire within seconds (probe K6: 11
        // false onsets), because the gate then sits at the bass's
        // own level. The discriminator is the GRID: while locked,
        // count grid beats whose confirmation window has closed
        // with no confirming onset; the gate for an ON-GRID onset
        // (inside the same +-18% pulse window onOnset confirms
        // with) relaxes stepwise per missed beat, floor ~0.15 of
        // peak. Off-grid onsets always face the full gate, so the
        // eighth-note bass trap stays shut, and pre-lock there is
        // no relaxation at all (lock acquisition keeps v0.26.1's
        // strict gate).
        double gateScale = 1.0;
        if (locked_ && period_ > 0) {
            const double pulse = period_ / sub_;
            if (pulse > 0) {
                const double k = std::round((now - anchorT_) / pulse);
                const double err = now - (anchorT_ + k * pulse);
                if (std::abs(err) <= 0.18 * pulse) {
                    const int missed = missedBeats(now);
                    gateScale = missed >= 3 ? 0.33 : missed == 2 ? 0.48 : missed == 1 ? 0.70 : 1.0;
                }
            }
        }
        // v0.28.1 (V4): the effective gate peak adapts to the level
        // of CONFIRMED KICK-LIKE onsets (see classifyPending below).
        const double gatePeak = adaptValid_ ? std::min(peakLow_, peakAdapt_) : peakLow_;
        if (elapsed_ > 0.6 && lowMs > 1e-8 && baseLow_ > 1e-10 &&
            lowMs > baseLow_ * 2.0 && lowMs > gatePeak * 0.45 * gateScale &&
            sinceOnset_ > 0.22) {
            fired = true; ++onsets_;
            pendClassT_ = now; pendClassMs_ = lowMs; pendClass_ = true;
            onOnset(now, lowMs);
        }
        // Classification of the pending fire, judged 250 ms later:
        // a kick is a burst — by +250 ms the band statistic must
        // have fallen under 40% of the fire value. A sustained bass
        // note (or the next eighth-note) keeps it up, and such a
        // fire does NOT lower the adaptive peak: it was rescued by
        // the grid, not confirmed as a kick.
        if (pendClass_ && now - pendClassT_ >= 0.25) {
            pendClass_ = false;
            if (lowMs < pendClassMs_ * 0.40) {
                const double cand = pendClassMs_ * 2.55; // level that passes this kick at the 0.45 ratio, +15% headroom
                if (!adaptValid_ || cand < peakAdapt_) peakAdapt_ = cand;
                adaptValid_ = true;
            }
        }
        // The adaptive peak forgets toward the raw peak over ~20 s,
        // so a later loud section re-raises the gate through its own
        // strict fires (each strict fire is also classified above).
        if (adaptValid_)
            peakAdapt_ += (peakLow_ - peakAdapt_) * (1.0 - std::exp(-blockSec / 20.0));
        baseLow_ += aBase * (lowMs - baseLow_);
        if (baseLow_ < 1e-10 && lowMs > 0) baseLow_ = lowMs; // cold start
        // --- grid emission: predicted beats fire on the clock,
        // whether or not a kick confirmed them (breakdown rule) ---
        if (locked_) {
            const double pulse = period_ / sub_;
            if (pulse > 0) {
                while (nextBeatT_ <= now + 1e-9) {
                    lastGridBeatT_ = nextBeatT_; // v0.28.0: plumbing for Undertow's confirmation window
                    beatEnv_ = std::max(beatEnv_, 0.85f);
                    if (pendingCount_ == 1) downbeatEnv_ = 1.0f;
                    countInBar_ = pendingCount_;
                    if (countBasePending_ && pendingCount_ == 1) { // v0.28.1: this count-1 beat re-bases the phase-evidence mapping (setMeter re-phase)
                        countBaseIdx_ = (long long)std::llround((lastGridBeatT_ - anchorT_) / pulse);
                        countBasePending_ = false;
                    }
                    pendingCount_ = pendingCount_ % bpb_ + 1;
                    nextBeatT_ += pulse;
                    ++gridBeats_;
                }
            }
            if (sinceOnset_ > 2.0) conf_ = std::max(0.35f, conf_ - (float)(blockSec * 0.02));
            if (sinceOnset_ > 12.0) { locked_ = false; conf_ = 0; intervals_.clear(); lastGridBeatT_ = -1; }
        }
        beatEnv_ *= (float)std::exp(-blockSec / 0.10);
        downbeatEnv_ *= (float)std::exp(-blockSec / 0.18);
        return fired;
    }

    bool locked() const { return locked_; }
    double bpm() const { return locked_ && period_ > 0 ? 60.0 / period_ : 0.0; }
    // v0.28.0 PLUMBING ONLY (Undertow, DESIGN.md build notes): the
    // tracker already holds nextBeatT_ internally; these accessors
    // export the grid's timing so a consumer can schedule against
    // the predicted beat. No tuning, gate or state changes — the
    // tracker is consumed, never second-guessed.
    double nowSec() const { return sr_ > 0 ? (double)samples_ / sr_ : 0.0; } // the tracker's own sample clock
    double secondsToNextBeat() const { // until the next grid pulse; -1 when no grid is running
        if (!locked_ || period_ <= 0) return -1.0;
        return nextBeatT_ - nowSec();
    }
    bool nextBeatIsDownbeat() const { return locked_ && pendingCount_ == 1; }
    double lastGridBeatSec() const { return lastGridBeatT_; } // sample-clock time of the most recent emitted grid beat (-1 = none)
    double beatPeriodSec() const { return period_; }
    float confidence() const { return locked_ ? conf_ : 0.f; }
    int countInBar() const { return countInBar_; }       // count of the most recent grid beat (1..bpb), 0 before the first
    float beatEnv() const { return beatEnv_; }           // decays ~100 ms after any grid beat
    float downbeatEnv() const { return downbeatEnv_; }   // decays ~180 ms after a count-1 beat
    uint64_t onsetCount() const { return onsets_; }
    uint64_t gridBeatCount() const { return gridBeats_; }
    // Position inside the current grid pulse, 0..1 (0 = on the beat).
    float beatPhase() const {
        if (!locked_ || period_ <= 0) return 0.f;
        const double pulse = period_ / sub_;
        const double now = (double)samples_ / sr_;
        const double ph = 1.0 - (nextBeatT_ - now) / pulse;
        return (float)std::clamp(ph, 0.0, 1.0);
    }
    // Recent grid beats for display overlays: fills up to maxOut
    // (ageSec, countInBar) pairs, most recent first, ages ascending,
    // covering at most maxAgeSec back. Returns the count filled.
    int collectRecentBeats(float maxAgeSec, float* agesOut, int* countsOut, int maxOut) const {
        if (!locked_ || period_ <= 0 || maxOut <= 0) return 0;
        const double pulse = period_ / sub_;
        const double now = (double)samples_ / sr_;
        double age = now - (nextBeatT_ - pulse); // age of the most recent emitted beat
        int filled = 0;
        for (int k = 0; k < maxOut; ++k) {
            const double a = age + k * pulse;
            if (a > maxAgeSec) break;
            if (a < -1e-6) break; // grid has not emitted yet (cannot happen once locked, kept honest)
            agesOut[k] = (float)std::max(0.0, a);
            countsOut[k] = ((countInBar_ - 1 - k) % bpb_ + bpb_) % bpb_ + 1;
            ++filled;
        }
        return filled;
    }

private:
    static double medianOf(std::vector<double> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        const size_t m = v.size() / 2;
        return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
    }
    static double foldPeriod(double p) { // into 0.28..1.2 s (50..214 BPM)
        if (p <= 0) return 0;
        while (p > 1.2) p *= 0.5;
        while (p < 0.28) p *= 2.0;
        return p;
    }
    // v0.28.1: grid beats whose +-18% confirmation window has fully
    // closed since the last confirmed onset (or since the lock).
    int missedBeats(double now) const {
        if (!locked_ || period_ <= 0) return 0;
        const double pulse = period_ / sub_;
        if (pulse <= 0) return 0;
        const double from = lastConfirmT_ > 0 ? lastConfirmT_ : anchorT_;
        const double kFirst = std::floor((from - anchorT_) / pulse) + 1.0;
        const double kLast = std::floor((now - 0.18 * pulse - anchorT_) / pulse);
        const double n = kLast - kFirst + 1.0;
        return n > 0 ? (int)n : 0;
    }
    // v0.28.1 measure-phase helpers.
    void resetPhase() {
        for (int i = 0; i < 12; ++i) slotE_[i] = 0;
        slotN_ = 0; lastEvidenceT_ = -1; downbeatCount_ = 0;
    }
    void recomputePhase() {
        if (slotN_ < 10) return; // too little evidence to judge; verdict stands (0 after any reset)
        double tot = 0; for (int i = 0; i < bpb_; ++i) tot += slotE_[i];
        if (tot <= 0) { downbeatCount_ = 0; return; }
        int best = 0; double bestSh = 0, secondSh = 0;
        for (int i = 0; i < bpb_; ++i) {
            const double sh = slotE_[i] / tot;
            if (sh > bestSh) { secondSh = bestSh; bestSh = sh; best = i; }
            else if (sh > secondSh) secondSh = sh;
        }
        if (downbeatCount_ == 0) {
            // Proof gates: one count must hold at least 40% of the
            // onset power AND lead the runner-up by 15 points.
            if (bestSh >= 0.40 && bestSh - secondSh >= 0.15) downbeatCount_ = best + 1;
        } else {
            const double provenSh = slotE_[downbeatCount_ - 1] / tot;
            if (best != downbeatCount_ - 1 && bestSh >= 0.50 && bestSh - provenSh >= 0.20)
                downbeatCount_ = best + 1; // the accent pattern moved; follow the evidence
            else if (provenSh < 0.33)
                downbeatCount_ = 0; // evidence dissolved; un-prove rather than keep asserting
        }
    }
    void relock(double t, double period) {
        locked_ = true; period_ = period; anchorT_ = t; nextBeatT_ = t;
        resetPhase(); countBaseIdx_ = 0; countBasePending_ = false; // v0.28.1: a new grid restarts phase evidence; its first emitted beat (index 0) carries count 1
        lastConfirmT_ = -1; // v0.28.1: no beat confirmed on the new grid yet
        lastGridBeatT_ = -1; // v0.28.0 plumbing: a fresh grid has emitted nothing yet
        pendingCount_ = 1;
    }
    void tryLock(double t) {
        if (intervals_.size() < 4) return;
        const double folded = foldPeriod(medianOf(std::vector<double>(intervals_.begin(), intervals_.end())));
        if (folded <= 0) return;
        int good = 0;
        for (double v : intervals_) {
            double cand = v;
            while (cand > folded * 1.45) cand *= 0.5;
            while (cand < folded / 1.45) cand *= 2.0;
            if (std::abs(cand - folded) <= 0.09 * folded) ++good;
        }
        const float cons = (float)good / (float)intervals_.size();
        if (cons >= 0.6f) { relock(t, folded); conf_ = cons; }
    }
    void onOnset(double t, double lowMs) {
        if (lastOnsetT_ > 0) {
            const double dt = t - lastOnsetT_;
            if (dt >= 0.24 && dt <= 2.4) {
                intervals_.push_back(dt);
                if (intervals_.size() > 16) intervals_.pop_front();
            }
        }
        lastOnsetT_ = t; sinceOnset_ = 0;
        if (!locked_) { tryLock(t); return; }
        // Significant-margin re-lock (see header): five consistent
        // intervals at a tempo >5% off the grid's replace it.
        if (intervals_.size() >= 5) {
            std::vector<double> last5(intervals_.end() - 5, intervals_.end());
            const double med = medianOf(last5);
            bool consistent = med > 0;
            for (double v : last5) if (std::abs(v - med) > 0.07 * med) consistent = false;
            const double folded = foldPeriod(med);
            if (consistent && folded > 0 && std::abs(folded - period_) > 0.05 * period_) {
                relock(t, folded);
                conf_ = std::max(conf_, 0.6f);
                return;
            }
        }
        // On-grid confirmation: gentle phase + tempo pulls.
        const double pulse = period_ / sub_;
        if (pulse <= 0) return;
        const double k = std::round((t - anchorT_) / pulse);
        const double err = t - (anchorT_ + k * pulse);
        if (std::abs(err) <= 0.18 * pulse) {
            lastConfirmT_ = t; // v0.28.1: a grid beat was confirmed here
            // v0.28.1 MEASURE-PHASE EVIDENCE: this onset confirmed
            // the grid beat with index k; its count follows from the
            // count base (the beat index carrying count 1).
            // Accumulate the onset's band power per count under a
            // ~24 s forgetting time, so an arrangement change can
            // move the answer. Accumulation asserts nothing by
            // itself — proof is gated in recomputePhase().
            {
                const long long ki = (long long)k;
                const int cnt = (int)(((ki - countBaseIdx_) % bpb_ + bpb_) % bpb_) + 1;
                if (lastEvidenceT_ > 0)
                    for (int i = 0; i < 12; ++i) slotE_[i] *= std::exp(-(t - lastEvidenceT_) / 24.0);
                lastEvidenceT_ = t;
                // Weight = the onset's band power SQUARED. The
                // 140 Hz statistic compresses accent contrast (a
                // 0.95-amp kick over the probe's bass bed reads
                // 0.498 vs a 0.55-amp kick's 0.294 — a 1.7x power
                // ratio, not the 3x the amplitudes suggest), and
                // raw-power shares topped out at 0.36, just under
                // the 0.40 proof gate. Squaring restores the
                // contrast (shares ~0.49 vs 0.17) while uniform
                // kicks stay uniform under it, so the no-evidence
                // case still proves nothing. (A baseline-relative
                // weight was tried first and measured WORSE: the
                // baseline itself is phase-locked to the bar, so
                // the ratio cancelled the accent out.)
                slotE_[cnt - 1] += lowMs * lowMs;
                ++slotN_;
                recomputePhase();
            }
            anchorT_ += err * 0.25; nextBeatT_ += err * 0.25;
            if (!intervals_.empty()) {
                double cand = intervals_.back();
                while (cand > period_ * 1.4) cand *= 0.5;
                while (cand < period_ / 1.4) cand *= 2.0;
                if (std::abs(cand - period_) <= 0.12 * period_)
                    period_ += (cand - period_) * 0.2;
            }
        }
    }

    double sr_ = 48000, lpCoef_ = 0, lp_ = 0, lp2_ = 0, baseLow_ = 0, peakLow_ = 0;
    // Sliding power window for the block statistic (see pushBlock).
    static constexpr int kWinPow = 128;
    std::vector<double> winPow_; int winHead_ = 0, winCount_ = 0; double winSum_ = 0;
    uint64_t samples_ = 0;
    double lastConfirmT_ = -1; // v0.28.1: last on-grid confirmed onset (sample-clock seconds)
    // v0.28.1 measure phase: per-count onset-power evidence + verdict
    double slotE_[12] = {}; int slotN_ = 0; double lastEvidenceT_ = -1;
    int downbeatCount_ = 0; long long countBaseIdx_ = 0; bool countBasePending_ = false;
    double peakAdapt_ = 0; bool adaptValid_ = false; // v0.28.1: gate peak adapted to confirmed kick levels
    double pendClassT_ = 0, pendClassMs_ = 0; bool pendClass_ = false; // pending fire awaiting burst classification
    double elapsed_ = 0, sinceOnset_ = 1e9, lastOnsetT_ = -1;
    std::deque<double> intervals_;
    bool locked_ = false;
    double period_ = 0, anchorT_ = 0, nextBeatT_ = 0, lastGridBeatT_ = -1;
    int bpb_ = 4, sub_ = 1;
    int pendingCount_ = 1, countInBar_ = 0;
    float conf_ = 0, beatEnv_ = 0, downbeatEnv_ = 0;
    uint64_t onsets_ = 0, gridBeats_ = 0;
};
} // namespace meterbar
