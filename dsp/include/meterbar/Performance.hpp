#pragma once
// v0.28.2: PERFORMANCE MODE (Jackson's low-end laptop round, scope B:
// toggle + Auto). The app paints in software on the message thread, so
// on a low-end CPU the per-frame budget is the whole game. This header
// is the single, pure-core place that turns the user's Performance
// Mode + their own settings into the EFFECTIVE per-frame limits, so
// the DSP suite can pin every number and the app side carries no
// policy of its own.
//
// Modes (persisted as PresetState::performanceMode, APPENDED in
// v0.28.2 — 0/1/2 are the only values and none existed before):
//   0 = Full    — every limit is the user's own setting, unchanged.
//   1 = Low-End — the capped limits below, immediately.
//   2 = Auto    — THE DEFAULT (see State.hpp for the field evidence
//                 that forced it). Starts at Full; the PerfAutoLatch
//                 steps it down to the Low-End limits after ~3 s of
//                 sustained fps sag, then STAYS down — for the rest
//                 of the session AND (via the app-side persisted
//                 latch file, Documents/MeterBar/perf-auto.json)
//                 across launches: the next launch starts stepped
//                 down. Step-up is deliberate only: the user picking
//                 Full or Low-End in Settings clears the persisted
//                 latch. No automatic step-up exists, so Auto cannot
//                 flap by construction.
namespace meterbar {

// The Full-quality Undertow caps — must equal UndertowModel::kSwarmCap
// / kDustCap (the suite static-pins the equality; Undertow.hpp cannot
// include this header without a cycle through State.hpp's users, so
// the numbers live here and are checked there in the tests).
inline constexpr int kPerfFullSwarmCap = 240;
inline constexpr int kPerfFullDustCap = 60;

struct EffectiveLimits {
    int fpsCap;            // effective UI timer rate (Hz)
    bool bloomQuarter;     // Undertow fine bloom scale allowed
    bool bloomEighth;      // Undertow halo bloom scale allowed
    int vizMaxW, vizMaxH;  // visualizer layer size caps (px)
    int scopePitchEvery;   // run the scope pitch estimate at most every Nth frame
    int swarmCap;          // Undertow swarm live-dot cap
    int dustCap;           // Undertow presence-dust cap
    float sgScale;         // v0.28.6: spectrogram cache resolution scale (the bilinear render makes a reduced cache read smooth, not blocky)
    bool lowActive;        // true when the Low-End limits are in force
};

// lowActive: for mode Full always false, for Low-End always true, for
// Auto it is the PerfAutoLatch's output. userFps <= 0 means "unset"
// and reads as the historical 60.
inline EffectiveLimits effectiveLimits(bool lowActive, int userFps,
                                       bool userBloomQuarter, bool userBloomEighth) {
    EffectiveLimits e;
    e.lowActive = lowActive;
    const int uf = userFps > 0 ? userFps : 60;
    if (!lowActive) {
        e.fpsCap = uf;
        e.bloomQuarter = userBloomQuarter;
        e.bloomEighth = userBloomEighth;
        e.vizMaxW = 480; e.vizMaxH = 270;   // the v0.20.0 half-res caps, unchanged
        e.scopePitchEvery = 1;
        e.swarmCap = kPerfFullSwarmCap;
        e.dustCap = kPerfFullDustCap;
        e.sgScale = 1.0f;                  // full-res spectrogram cache
    } else {
        e.fpsCap = uf < 30 ? uf : 30;       // cap at 30, never raise a lower user limit
        e.bloomQuarter = false;
        e.bloomEighth = false;
        e.vizMaxW = 320; e.vizMaxH = 180;
        e.scopePitchEvery = 3;
        e.swarmCap = kPerfFullSwarmCap / 2; // 120
        e.dustCap = kPerfFullDustCap / 2;   // 30
        e.sgScale = 0.5f;                  // half-res spectrogram cache (quarter the pixels); the v0.28.6 bilinear sample + blit keep it smooth
    }
    return e;
}

// Resolve the mode + latch state into the single lowActive boolean the
// limits function consumes. Unknown persisted values read as Full.
inline bool perfLowActive(int performanceMode, bool autoLatchedLow) {
    if (performanceMode == 1) return true;
    if (performanceMode == 2) return autoLatchedLow;
    return false;
}

// Visualizer layer size under a cap pair: the historical rule is
// half the window size, clamped to [1, cap] per axis.
inline void perfVizLayerSize(int boundsW, int boundsH, const EffectiveLimits& e, int& outW, int& outH) {
    outW = boundsW / 2; if (outW > e.vizMaxW) outW = e.vizMaxW; if (outW < 1) outW = 1;
    outH = boundsH / 2; if (outH > e.vizMaxH) outH = e.vizMaxH; if (outH < 1) outH = 1;
}

// Scope pitch cadence: in Full the estimate runs every frame; under
// Low-End limits it runs on frame indices divisible by scopePitchEvery
// and the caller serves the cached capture/frequency between runs.
inline bool perfScopeCaptureDue(long long frameIndex, const EffectiveLimits& e) {
    return e.scopePitchEvery <= 1 || (frameIndex % e.scopePitchEvery) == 0;
}

// AUTO step-down latch. Rule (v0.28.2, stated in the round receipt):
//   - Feed it the measured UI fps (the app's uiFpsMeasured EMA), the
//     user's target fps, and real elapsed dt once per UI frame.
//   - While not latched: if measured < 0.85 x target, sag time
//     accumulates; any frame at/above the threshold resets it to 0.
//   - At 3.0 s of CONTINUOUS sag the latch trips to Low-End — fast
//     enough to rescue a session inside its first ~10 s (the laptop
//     evidence), slow enough that a one-off hitch never trips it.
//   - The latch NEVER releases on its own: not in-session, and not
//     across launches (the app persists it and re-arms it with
//     forceLatched() at startup). reset() happens only on an
//     explicit Performance Mode change in Settings. Hysteresis by
//     construction: a stepped-down bar sits at its 30 fps cap, where
//     "comfortable" would otherwise read true and flap it back up.
//   - Samples with measured <= 0 (EMA not yet warm) are ignored.
class PerfAutoLatch {
public:
    bool update(double measuredFps, double targetFps, double dtSeconds) {
        if (latched_) return true;
        if (measuredFps <= 0 || targetFps <= 0 || dtSeconds <= 0) return false;
        if (measuredFps < 0.85 * targetFps) {
            sag_ += dtSeconds;
            if (sag_ >= 3.0) latched_ = true;
        } else {
            sag_ = 0;
        }
        return latched_;
    }
    bool latched() const { return latched_; }
    double sagSeconds() const { return sag_; }
    void reset() { latched_ = false; sag_ = 0; }
    // v0.28.2: re-arm from the persisted latch at startup — a session
    // that ended stepped down starts stepped down (see class rule).
    void forceLatched() { latched_ = true; sag_ = 0; }
private:
    bool latched_ = false;
    double sag_ = 0;
};
} // namespace meterbar
