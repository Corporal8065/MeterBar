#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace meterbar {
// VISUALIZER BACKGROUND MODE (v0.20.0, item l) — the pure model behind
// the optional animated background. Deterministic (fixed-seed RNG),
// allocation-bounded, and fully graded by the DSP tests; the app layer
// only renders what this model produces (into a half-res image, blurred
// with a Gaussian convolution, behind the modules).
//
// The picture: WHITE PARTICLES drift inward and converge on the centre
// of the bar. When a frequency band's energy crosses its trigger, a
// RAINBOW BRUSH STROKE bursts at that band's anchor — an elongated
// cluster of dots whose hues sweep the palette across the stroke; the
// stroke expands outward and dissolves into its constituent dots,
// which then fade. Palette controls live in VisualizerSettings:
// base hue (where the rainbow starts), saturation, palette spread
// (how much of the hue wheel one stroke spans) and cycle period
// (how fast the starting hue drifts; 0 = static).
//
// COST CAP (16 GB gaming PC): the total particle count NEVER exceeds
// settings.particleCap (default 600, hard ceiling 1200) — bursts are
// truncated to the remaining budget, ambient particles are recycled
// in place and never allocated per frame.

struct VizRgb { std::uint8_t r = 255, g = 255, b = 255; };

inline VizRgb vizHsvToRgb(float h, float s, float v) {
    h = h - std::floor(h); // wrap
    s = s < 0.f ? 0.f : s > 1.f ? 1.f : s;
    v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    const float c = v * s;
    const float x = c * (1.f - std::abs(std::fmod(h * 6.f, 2.f) - 1.f));
    const float m = v - c;
    float r = 0, g = 0, b = 0;
    const int seg = (int)(h * 6.f) % 6;
    if (seg == 0) { r = c; g = x; }
    else if (seg == 1) { r = x; g = c; }
    else if (seg == 2) { g = c; b = x; }
    else if (seg == 3) { g = x; b = c; }
    else if (seg == 4) { r = x; b = c; }
    else { r = c; b = x; }
    return VizRgb{ (std::uint8_t)std::lround((r + m) * 255.f),
                   (std::uint8_t)std::lround((g + m) * 255.f),
                   (std::uint8_t)std::lround((b + m) * 255.f) };
}

// DISTRIBUTED EMITTERS (v0.25.0, Jackson's critique 2026-11-01):
// the field's centre is a coordinate reference, NOT a spawn point.
// A band's activity lives at a horizontal position across the field
// — low at the left, mid at the centre, high at the right — shifted
// by the stereo balance (positive = right-heavy, the StereoFrame
// convention). Bursts, brush heads and Field dots are all born near
// their band's position instead of at the middle of the bar.
inline float vizBandFieldX(int band, float balance) {
    const float base = band == 0 ? 0.20f : band == 1 ? 0.50f : 0.80f;
    float x = base + balance * 0.15f;
    return x < 0.06f ? 0.06f : x > 0.94f ? 0.94f : x;
}

struct VisualizerSettings {
    bool enabled = false;          // the Settings toggle — default OFF
    float baseHue = 0.83f;         // 0..1, family pink
    float saturation = 0.85f;      // 0..1
    float paletteSpread = 0.75f;   // 0..1 of the hue wheel per stroke
    float cycleSeconds = 24.f;     // palette drift period; <= 0 = static
    int particleCap = 600;         // hard cap on live particles
};

struct VizParticle {
    float x = 0.5f, y = 0.5f;      // unit-square space (bar-local)
    float vx = 0, vy = 0;          // units / second
    float age = 0, life = 1;       // seconds
    float size = 2;                // px at render scale (dot radius-ish)
    float hueT = 0;                // 0..1 position along the stroke's palette sweep (bursts)
    bool burst = false;            // false = ambient white converger
};

class VisualizerEngine {
public:
    static constexpr int kHardCap = 1200;
    static constexpr int kAmbientTarget = 140;
    static constexpr float kTrigger = 0.55f;      // band energy that fires a burst
    static constexpr float kRefractorySec = 0.12f;
    // v0.21.5: peak brightness of the ambient whites, as a fraction
    // of full white. They used to render at a full 255 — the loudest
    // thing in the bar (Jackson's frames, 2026-10-31). 0.55 puts
    // them at the colourful burst dots' energy, where they belong.
    static constexpr float kAmbientPeak = 0.55f;

    // Advance the model. bandEnergy = [low, mid, high], each 0..1
    // (see spectrumBandEnergies). timeSec = a free-running clock for
    // palette cycling (the engine also tracks it from dt if negative).
    void update(float dt, const float bandEnergy[3], const VisualizerSettings& st,
                float stereoBalance = 0.f) {
        if (dt <= 0) return;
        time_ += dt;
        // v0.25.0: smoothed stereo balance steers where new
        // particles are born (see vizBandFieldX).
        {
            float b = stereoBalance < -1.f ? -1.f : stereoBalance > 1.f ? 1.f : stereoBalance;
            float a = 8.f * dt; if (a > 1.f) a = 1.f;
            balance_ += (b - balance_) * a;
        }
        // --- activity gate (v0.20.3, Jackson's ask; TickerBar
        // v0.4.0 sibling): the ambient WHITE convergers are only
        // visible while the bands carry real energy. In silence
        // activity decays to zero and the whites fade out entirely
        // instead of idling white on screen. Fast attack, slow
        // release, so a quiet gap between hits doesn't flicker.
        {
            const float frame = std::max({ bandEnergy[0], bandEnergy[1], bandEnergy[2] });
            const float rate = frame > activity_ ? 6.f : 0.8f;
            float a = rate * dt; if (a > 1.f) a = 1.f;
            activity_ += (frame - activity_) * a;
        }
        const int cap = st.particleCap < 16 ? 16 : (st.particleCap > kHardCap ? kHardCap : st.particleCap);
        // --- ambient: keep the converger population topped up ---
        int ambientWanted = kAmbientTarget;
        if (ambientWanted > cap / 2) ambientWanted = cap / 2;
        int ambient = 0;
        for (auto& p : ps_) if (!p.burst) ++ambient;
        while (ambient < ambientWanted && (int)ps_.size() < cap) { ps_.push_back(spawnAmbient()); ++ambient; }
        // --- band triggers ---
        // v0.26.0 (Bubbles rework, Jackson 2026-11-02: bubbles/lines
        // "need more impact on the song"): a burst now scales with
        // the IMPACT of the hit — the size of the energy jump, not
        // just the level it lands at. impact_ is a fast-decaying
        // envelope of the strongest recent jump; spawnBurst spends
        // it on particle count, expansion speed and dot size, and
        // colorFor spends it on brightness. The low band's trigger
        // sits a touch lower (0.50): the kick is the heartbeat and
        // it should not need to shout to be seen.
        impact_ *= std::exp(-dt * 4.5f);
        float shockDelta = 0; // this frame's strongest jump (shockwave below)
        for (int band = 0; band < 3; ++band) {
            refractory_[band] -= dt;
            const float trig = band == 0 ? 0.50f : kTrigger;
            const float delta = bandEnergy[band] - prevBand_[band];
            if (delta > shockDelta) shockDelta = delta;
            if (bandEnergy[band] >= trig && refractory_[band] <= 0) {
                // v0.25.0: a sharp rise is a TRANSIENT — its burst
                // stays tight and local at the band's field position
                // instead of stroking wide.
                const bool transient = delta >= 0.30f;
                if (transient) {
                    const float imp = delta * 2.4f;
                    if (imp > impact_) impact_ = imp > 1.f ? 1.f : imp;
                }
                refractory_[band] = transient ? 0.09f : kRefractorySec;
                spawnBurst(band, bandEnergy[band], cap, transient);
            }
            prevBand_[band] = bandEnergy[band];
        }
        // --- integrate ---
        // v0.26.0: SHOCKWAVE — a strong hit (any band jumping >=
        // 0.45 in a frame) kicks the AMBIENT convergers radially
        // outward from the centre, so the whole field ripples on
        // the hit instead of only the burst noticing it.
        const bool shock = shockDelta >= 0.45f;
        for (auto& p : ps_) {
            p.age += dt;
            if (!p.burst) {
                if (shock) {
                    const float dx = p.x - 0.5f, dy = p.y - 0.5f;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist > 0.02f) {
                        const float kick = 0.10f * shockDelta / dist;
                        p.vx += dx * kick; p.vy += dy * kick;
                    }
                }
                // converge: velocity steers toward the centre + a
                // gentle tangential swirl; near the centre, respawn.
                const float dx = 0.5f - p.x, dy = 0.5f - p.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < 0.02f || p.age > p.life) { p = spawnAmbient(); continue; }
                const float pull = 0.10f, swirl = 0.05f;
                p.vx += (dx / dist * pull + (-dy / dist) * swirl) * dt;
                p.vy += (dy / dist * pull + (dx / dist) * swirl) * dt;
                // v0.21.5: the burst dots' own ease-out (0.985,
                // below) — the whites kept 99.5% of their velocity
                // per frame, so they streamed hotter than the
                // colourful dots and piled up at the centre.
                p.vx *= 0.985f; p.vy *= 0.985f;
            } else {
                p.vx *= 0.985f; p.vy *= 0.985f; // expansion eases out
                // v0.25.0: as a burst dot's expansion dies it picks
                // up the ambient whites' inward swirl (Jackson
                // likes that motion — the dots used to just stop
                // wherever the stroke left them). Weaker than the
                // ambients' own steering, so the stroke still reads
                // as a stroke first.
                const float dx = 0.5f - p.x, dy = 0.5f - p.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > 0.02f) {
                    const float pull = 0.035f, swirl = 0.018f;
                    p.vx += (dx / dist * pull + (-dy / dist) * swirl) * dt;
                    p.vy += (dy / dist * pull + (dx / dist) * swirl) * dt;
                }
            }
            p.x += p.vx * dt; p.y += p.vy * dt;
        }
        ps_.erase(std::remove_if(ps_.begin(), ps_.end(),
                                 [](const VizParticle& p) { return p.burst && p.age >= p.life; }),
                  ps_.end());
    }

    const std::vector<VizParticle>& particles() const { return ps_; }
    int burstCount() const { return bursts_; } // total bursts fired (test/diagnostic)
    double time() const { return time_; }
    float activity() const { return activity_; } // smoothed band activity 0..1 (v0.20.3)
    float impact() const { return impact_; }     // v0.26.0: strongest recent energy jump, 0..1 (decaying)
    // How visible the ambient whites are: full once activity
    // reaches 0.30, zero in silence (see the gate in update()).
    float ambientVisibility() const {
        const float v = activity_ / 0.30f;
        return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    }

    // Colour of one particle under the palette settings. Ambient
    // particles are WHITE by design (fade by age only). Burst dots
    // sweep baseHue -> baseHue + spread across the stroke (hueT),
    // drifted by the cycle clock.
    VizRgb colorFor(const VizParticle& p, const VisualizerSettings& st) const {
        if (!p.burst) {
            const float fadeIn = p.age < 0.5f ? 0.4f + 0.6f * (p.age / 0.5f) : 1.f;
            // v0.21.5: the burst dots' envelope, mirrored — fade
            // OUT over the last 30% of life (the bursts' own rule,
            // below) instead of sitting at full brightness until
            // the respawn pop, and cap the peak at kAmbientPeak.
            const float fadeOut = p.age > p.life * 0.7f ? 1.f - (p.age - p.life * 0.7f) / (p.life * 0.3f + 1e-6f) : 1.f;
            // v0.20.3 gate unchanged: whites are gated by band
            // activity — in silence this returns (near-)black,
            // i.e. invisible against the black background.
            const auto v = (std::uint8_t)std::lround(255.f * kAmbientPeak * fadeIn * fadeOut * ambientVisibility());
            return VizRgb{ v, v, v };
        }
        float cycle = 0.f;
        if (st.cycleSeconds > 0.01f) cycle = (float)std::fmod(time_ / (double)st.cycleSeconds, 1.0);
        const float h = st.baseHue + cycle + p.hueT * st.paletteSpread;
        const float fadeOut = p.age > p.life * 0.7f ? 1.f - (p.age - p.life * 0.7f) / (p.life * 0.3f + 1e-6f) : 1.f;
        // v0.26.0: burst dots burn brighter while the impact
        // envelope is up (vizHsvToRgb clamps the value at 1).
        return vizHsvToRgb(h, st.saturation, (0.35f + 0.65f * fadeOut) * (0.82f + 0.5f * impact_));
    }

private:
    std::uint32_t rng_ = 0x9e3779b9u;
    float next01() { // splitmix32 — deterministic across runs/platforms
        rng_ += 0x9e3779b9u;
        std::uint32_t z = rng_;
        z = (z ^ (z >> 16)) * 0x85ebca6bu;
        z = (z ^ (z >> 13)) * 0xc2b2ae35u;
        return (float)((z ^ (z >> 16)) >> 8) / 16777216.f;
    }
    VizParticle spawnAmbient() {
        VizParticle p;
        // v0.25.0: born anywhere across the field, not on a ring
        // around the centre — the inward convergence below is the
        // motion Jackson likes; where a particle starts is the
        // part that read as "everything in the middle".
        p.x = 0.03f + next01() * 0.94f;
        p.y = 0.04f + next01() * 0.92f;
        p.vx = 0; p.vy = 0;
        p.age = 0; p.life = 6.f + next01() * 8.f;
        p.size = 1.2f + next01() * 1.6f;
        p.burst = false;
        return p;
    }
    void spawnBurst(int band, float energy, int cap, bool transient = false) {
        // Band anchors: the band's FIELD position (v0.25.0 — low
        // left, mid centre, high right, shifted by the stereo
        // balance) at the band's row; a little jitter keeps repeat
        // bursts from stacking on one exact spot.
        const float ax = vizBandFieldX(band, balance_) + (next01() - 0.5f) * 0.10f;
        const float ay = band == 0 ? 0.72f : band == 1 ? 0.5f : 0.28f;
        const float ang = next01() * 6.2831853f;
        const float dx = std::cos(ang), dy = std::sin(ang); // stroke direction
        float halfLen = 0.10f + 0.08f * energy;
        if (transient) halfLen *= 0.55f; // a hit stays local (v0.25.0)
        // v0.26.0: the impact envelope buys MORE dots, thrown
        // HARDER and drawn BIGGER — a hard hit reads as a bigger
        // event than a soft threshold crossing, which is the whole
        // complaint this rework answers.
        int want = (int)std::lround((26.f + 34.f * energy) * (1.f + 0.6f * impact_));
        const int budget = cap - (int)ps_.size();
        if (want > budget) want = budget;
        if (want <= 0) return;
        ++bursts_;
        for (int i = 0; i < want; ++i) {
            const float t = want > 1 ? (float)i / (float)(want - 1) : 0.5f;
            VizParticle p;
            p.x = ax + dx * (t - 0.5f) * 2.f * halfLen + (next01() - 0.5f) * 0.012f;
            p.y = ay + dy * (t - 0.5f) * 2.f * halfLen + (next01() - 0.5f) * 0.012f;
            // expansion: outward from the anchor, stronger mid-stroke
            const float out = (0.10f + 0.25f * energy * (1.f - std::abs(t - 0.5f))) * (1.f + 0.7f * impact_);
            p.vx = (p.x - ax) * out * 8.f + (next01() - 0.5f) * 0.05f;
            p.vy = (p.y - ay) * out * 8.f + (next01() - 0.5f) * 0.05f;
            p.age = 0; p.life = 0.9f + next01() * 0.8f;
            p.size = 1.6f + next01() * 2.2f + impact_ * 1.1f;
            p.hueT = t;
            p.burst = true;
            ps_.push_back(p);
        }
    }
    std::vector<VizParticle> ps_;
    float refractory_[3] = { 0, 0, 0 };
    float prevBand_[3] = { 0, 0, 0 }; // v0.25.0: transient detection
    float balance_ = 0;               // v0.25.0: smoothed stereo balance
    int bursts_ = 0;
    float activity_ = 0; // v0.20.3: smoothed max band energy (the silence gate)
    float impact_ = 0;   // v0.26.0: transient-impact envelope (see the trigger block)
    double time_ = 0;
};

// ------------------------------------------------------------------
// LINES STYLE (v0.21.5, Jackson's ask 2026-10-31) — the second
// visualizer style, alongside the particle Bubbles (the engine
// above; NOT replaced). Three flowing ribbons, one per frequency
// band (low/mid/high) at the burst anchors, drawn like hand-inked
// lines: each band's level is a damped SPRING toward the band
// energy, so a ribbon swells and settles instead of jittering;
// the centreline is two detuned sines plus a fixed per-point
// wobble (baked at construction from a splitmix stream), so the
// curve never reads as a perfect machine sine. Thickness swells
// with the springed level and breathes along the line. When the
// music is quiet the phase creep and the amplitude collapse —
// near-still — and the SAME activity gate as the particle field
// fades the ribbons out entirely in silence (the family's
// no-visuals-in-silence rule). Pure model, like the engine: the
// app layer turns centreY/halfThick into a filled path.
class VisualizerLines {
public:
    static constexpr int kPoints = 48;

    VisualizerLines() {
        // Fixed hand-drawn wobble: a per-(band, point) phase
        // offset in [-0.35, 0.35] rad, baked once from a splitmix
        // stream — deterministic across runs, like the engine's.
        std::uint32_t rng = 0x51ab3f19u;
        for (int b = 0; b < 3; ++b)
            for (int i = 0; i < kPoints; ++i) {
                rng += 0x9e3779b9u;
                std::uint32_t z = rng;
                z = (z ^ (z >> 16)) * 0x85ebca6bu;
                z = (z ^ (z >> 13)) * 0xc2b2ae35u;
                wob_[b][i] = ((float)((z ^ (z >> 16)) >> 8) / 16777216.f - 0.5f) * 0.7f;
            }
    }

    void update(float dt, const float bandEnergy[3]) {
        if (dt <= 0) return;
        time_ += dt;
        {   // the particle engine's activity gate, same constants
            const float frame = std::max({ bandEnergy[0], bandEnergy[1], bandEnergy[2] });
            const float rate = frame > activity_ ? 6.f : 0.8f;
            float a = rate * dt; if (a > 1.f) a = 1.f;
            activity_ += (frame - activity_) * a;
        }
        static constexpr float kSpeed[3] = { 1.0f, 1.31f, 1.73f }; // bands flow at detuned rates
        for (int b = 0; b < 3; ++b) {
            float target = bandEnergy[b];
            target = target < 0.f ? 0.f : target > 1.f ? 1.f : target;
            // v0.26.0 (Lines rework, same ask as Bubbles): a rising
            // edge FEED-FORWARDS into the spring — the ribbon lunges
            // on a hit instead of only swelling toward it — and an
            // impact envelope (fast decay) fattens the stroke,
            // brightens the colour and speeds the flow while the
            // hit is fresh. The spring itself is unchanged, so the
            // settle Jackson approved stays the settle.
            const float delta = target - prevTarget_[b];
            prevTarget_[b] = target;
            impact_[b] *= std::exp(-dt * 5.f);
            if (delta > 0.04f) {
                const float imp = delta * 1.9f;
                if (imp > impact_[b]) impact_[b] = imp > 1.f ? 1.f : imp;
                vel_[b] += delta * 2.2f;
            }
            // Damped spring: stiff enough to answer a hit, soft
            // enough to settle like ink — never a raw copy of the
            // frame energy (that jitter is what this style avoids).
            vel_[b] += ((target - level_[b]) * 42.f - vel_[b] * 9.f) * dt;
            level_[b] += vel_[b] * dt;
            if (level_[b] < 0.f) { level_[b] = 0.f; if (vel_[b] < 0.f) vel_[b] = 0.f; }
            if (level_[b] > 1.15f) { level_[b] = 1.15f; if (vel_[b] > 0.f) vel_[b] = 0.f; }
            // Near-still when quiet: the creep floor is 0.15 rad/s
            // and the amplitude (centreY) collapses with the level.
            phase_[b] += dt * (0.15f + 1.35f * level_[b]) * kSpeed[b] * (1.f + 0.6f * impact_[b]);
        }
    }

    float level(int band) const { return level_[band]; }
    float impact(int band) const { return impact_[band]; } // v0.26.0: this band's fresh-hit envelope 0..1
    double time() const { return time_; }
    // The particle field's gate curve: full at activity 0.30,
    // zero in silence.
    float visibility() const {
        const float v = activity_ / 0.30f;
        return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    }
    // Centreline height (unit space) of ribbon `band` at control
    // point i: the band anchor (the burst anchors) plus the
    // two-sine flow. Amplitude collapses with the springed level.
    float centreY(int band, int i) const {
        const float anchor = band == 0 ? 0.72f : band == 1 ? 0.5f : 0.28f;
        const float u = kPoints > 1 ? (float)i / (float)(kPoints - 1) : 0.f;
        const float amp = 0.012f + 0.085f * level_[band];
        const float ph = phase_[band] + wob_[band][i];
        return anchor + (std::sin(ph + u * 4.6f) * 0.55f + std::sin(ph * 1.7f - u * 7.9f) * 0.45f) * amp;
    }
    // Ribbon half-thickness (unit space): swells with the level,
    // breathes along the line so the stroke feels hand-inked.
    // v0.26.0: a fresh hit fattens the stroke on top of the level.
    float halfThick(int band, int i) const {
        const float u = kPoints > 1 ? (float)i / (float)(kPoints - 1) : 0.f;
        const float breathe = 0.75f + 0.25f * std::sin(phase_[band] * 0.9f + u * 3.1f + (float)band * 1.7f);
        return (0.004f + 0.030f * level_[band] + 0.012f * impact_[band]) * breathe;
    }
    // Band colour from the same palette as the burst dots: the
    // three bands fan across the palette spread from the base hue.
    // v0.26.0: brightness follows the level and the impact envelope
    // instead of sitting at a constant 0.9 — hits visibly flare.
    VizRgb colorFor(int band, const VisualizerSettings& st) const {
        float cycle = 0.f;
        if (st.cycleSeconds > 0.01f) cycle = (float)std::fmod(time_ / (double)st.cycleSeconds, 1.0);
        const float h = st.baseHue + cycle + (float)band * st.paletteSpread * 0.33f;
        float v = 0.5f + 0.5f * level_[band] + 0.35f * impact_[band];
        v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
        return vizHsvToRgb(h, st.saturation, v);
    }

private:
    float level_[3] = { 0, 0, 0 };
    float vel_[3] = { 0, 0, 0 };
    float phase_[3] = { 0, 0, 0 };
    float impact_[3] = { 0, 0, 0 };    // v0.26.0: per-band fresh-hit envelope
    float prevTarget_[3] = { 0, 0, 0 }; // v0.26.0: last frame's target per band (edge detection)
    float wob_[3][kPoints] = {};
    float activity_ = 0;
    double time_ = 0;
};

// ------------------------------------------------------------------
// FIELD STYLE (v0.25.0, Jackson's rollout word 2026-11-01) — the
// third visualizer style, alongside Bubbles and Lines. Instead of
// one global centre, FOUR vortex centres wander slowly around the
// field on detuned Lissajous paths. A dot is born near its band's
// field position (the distributed-emitter rule above), joins the
// vortex nearest its birthplace, and orbits it; the vortex is FED
// by its band — orbit speed follows the band energy, so the vortex
// nearest the active frequency's position is the one that spins
// up. A dot the vortex pulls into its core is consumed and
// respawned. The SAME activity gate as the particle engine fades
// the whole field out in silence. Pure model, deterministic
// (splitmix stream), population hard-capped — the app layer draws
// the dots through the same half-res + blur path as Bubbles, so
// the render cost envelope is unchanged.
struct VizFieldDot {
    float x = 0.5f, y = 0.5f;
    float vx = 0, vy = 0;
    float age = 0, life = 1;
    float size = 2;
    int band = 1;    // the band that feeds this dot (palette + orbit speed)
    int vortex = 0;  // the vortex it orbits
    // v0.26.0: the TAIL — the dot's positions ~45 ms and ~90 ms
    // ago, rendered behind it as fading ghosts that brighten with
    // the field's pulse envelope (Jackson: "a tail coming from it
    // that also pulses with the transients"). trailSet is false
    // until the first trail shift stamps them.
    float trailX[2] = { 0, 0 }, trailY[2] = { 0, 0 };
    bool trailSet = false;
};

class VisualizerField {
public:
    static constexpr int kVortices = 4;
    static constexpr int kFieldTarget = 150;
    static constexpr int kFieldHardCap = 400;

    // beatEnv (v0.26.0): the beat grid's downbeat envelope 0..1
    // from the app (BeatTracker's downbeatEnv; the caller falls
    // back to BeatPulse's level until the grid locks). Defaulted
    // so existing callers — and the whole pre-v0.26 behaviour —
    // are untouched when no beat is fed.
    void update(float dt, const float bandEnergy[3], const VisualizerSettings& st,
                float stereoBalance = 0.f, float beatEnv = 0.f) {
        if (dt <= 0) return;
        time_ += dt;
        {
            float b = stereoBalance < -1.f ? -1.f : stereoBalance > 1.f ? 1.f : stereoBalance;
            float a = 8.f * dt; if (a > 1.f) a = 1.f;
            balance_ += (b - balance_) * a;
        }
        {   // the particle engine's activity gate, same constants
            const float frame = std::max({ bandEnergy[0], bandEnergy[1], bandEnergy[2] });
            const float rate = frame > activity_ ? 6.f : 0.8f;
            float a = rate * dt; if (a > 1.f) a = 1.f;
            activity_ += (frame - activity_) * a;
        }
        // --- v0.26.0 TRANSIENTS + PULSE (Jackson, 2026-11-02:
        // the field "needs to be more reactive to transient
        // differences and pulse more maybe in transparency, and
        // have a tail that also pulses with the transients") ---
        // Per band, the jump above a slow (~0.7 s) smoothed level
        // is the transient; pulse_ is the field-wide envelope of
        // the strongest one, with the downbeat folded in. Orbit
        // speed, dot brightness (the render path reads brightness
        // as opacity against the black base) and the tail ghosts
        // all spend pulse_.
        pulse_ = 0;
        for (int b = 0; b < 3; ++b) {
            const float jump = bandEnergy[b] - smoothE_[b];
            float a = 1.5f * dt; if (a > 1.f) a = 1.f;
            smoothE_[b] += (bandEnergy[b] - smoothE_[b]) * a;
            trans_[b] *= std::exp(-dt * 4.f);
            if (jump > 0.f) {
                const float t = jump * 1.7f;
                if (t > trans_[b]) trans_[b] = t > 1.f ? 1.f : t;
            }
            if (trans_[b] > pulse_) pulse_ = trans_[b];
        }
        const float beat = beatEnv < 0.f ? 0.f : beatEnv > 1.f ? 1.f : beatEnv;
        if (beat * 0.9f > pulse_) pulse_ = beat * 0.9f;
        // Downbeat IMPULSE (deliverable: field objects "speed up
        // or slow down and impulse with the downbeats"): on the
        // envelope's rising edge through 0.65, every dot takes a
        // radial kick away from its own vortex centre.
        const bool downbeatKick = beat >= 0.65f && prevBeatEnv_ < 0.65f;
        prevBeatEnv_ = beat;
        int cap = st.particleCap < 16 ? 16 : (st.particleCap > kFieldHardCap ? kFieldHardCap : st.particleCap);
        if (cap > kFieldTarget) cap = kFieldTarget;
        while ((int)dots_.size() < cap) dots_.push_back(spawnDot(bandEnergy));
        // tail history: shift every ~45 ms of model time
        trailTimer_ += dt;
        const bool shiftTrail = trailTimer_ >= 0.045f;
        if (shiftTrail) trailTimer_ = 0;
        for (auto& d : dots_) {
            d.age += dt;
            float cx, cy;
            vortexCentre(d.vortex, cx, cy);
            const float dx = cx - d.x, dy = cy - d.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 0.015f || d.age > d.life) { d = spawnDot(bandEnergy); continue; }
            if (shiftTrail) {
                d.trailX[1] = d.trailX[0]; d.trailY[1] = d.trailY[0];
                d.trailX[0] = d.x; d.trailY[0] = d.y;
                d.trailSet = true;
            }
            if (downbeatKick && dist > 0.02f) {
                const float kick = (0.22f + 0.30f * pulse_) / dist;
                d.vx += -dx * kick; d.vy += -dy * kick; // away from the centre
            }
            // Orbit: tangential flow around the vortex, weak
            // inward pull; speed follows the feeding band's energy
            // — and, v0.26.0, that band's transients and the beat.
            const int bi = d.band < 0 ? 0 : d.band > 2 ? 2 : d.band;
            const float e = bandEnergy[bi];
            const float orbit = (0.10f + 0.30f * e) * (1.f + 1.3f * trans_[bi]) * (1.f + 0.8f * beat);
            d.vx += ((-dy / dist) * orbit + (dx / dist) * 0.03f) * dt;
            d.vy += ((dx / dist) * orbit + (dy / dist) * 0.03f) * dt;
            d.vx *= 0.97f; d.vy *= 0.97f;
            d.x += d.vx * dt; d.y += d.vy * dt;
        }
    }

    // Vortex i's centre right now: a slow Lissajous wander, each
    // vortex on detuned periods/phases — deterministic, so every
    // linked bar drawing the same model agrees on the field.
    void vortexCentre(int i, float& x, float& y) const {
        static constexpr float kPhase[kVortices] = { 0.0f, 1.7f, 3.4f, 5.1f };
        static constexpr float kWx[kVortices] = { 0.21f, 0.16f, 0.26f, 0.13f };
        static constexpr float kWy[kVortices] = { 0.17f, 0.23f, 0.14f, 0.19f };
        const int k = i < 0 ? 0 : i >= kVortices ? kVortices - 1 : i;
        const float t = (float)time_;
        x = 0.5f + 0.30f * std::sin(t * kWx[k] + kPhase[k]);
        y = 0.5f + 0.28f * std::sin(t * kWy[k] + kPhase[k] * 1.7f);
    }

    const std::vector<VizFieldDot>& particles() const { return dots_; }
    double time() const { return time_; }
    float visibility() const { // the family gate: full at 0.30, zero in silence
        const float v = activity_ / 0.30f;
        return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    }
    float pulse() const { return pulse_; } // v0.26.0: transient/downbeat envelope 0..1 from the last update
    // Palette like the burst dots: the feeding band fans across
    // the spread from the base hue; same age envelope. v0.26.0:
    // the value also rides pulse_ — against the black render base
    // that brightness IS the field's opacity, so this is the
    // "pulse more in transparency" Jackson asked for.
    VizRgb colorFor(const VizFieldDot& d, const VisualizerSettings& st) const {
        float cycle = 0.f;
        if (st.cycleSeconds > 0.01f) cycle = (float)std::fmod(time_ / (double)st.cycleSeconds, 1.0);
        const float h = st.baseHue + cycle + (float)(d.band) * 0.5f * st.paletteSpread;
        return vizHsvToRgb(h, st.saturation, valueFor(d) * (0.70f + 0.55f * pulse_));
    }
    // A tail ghost's colour: same hue as its dot, dimmer the older
    // the ghost, and brightened by pulse_ so the tail itself
    // pulses with the transients.
    VizRgb trailColorFor(const VizFieldDot& d, const VisualizerSettings& st, int k) const {
        float cycle = 0.f;
        if (st.cycleSeconds > 0.01f) cycle = (float)std::fmod(time_ / (double)st.cycleSeconds, 1.0);
        const float h = st.baseHue + cycle + (float)(d.band) * 0.5f * st.paletteSpread;
        const float ghost = 0.50f - 0.18f * (float)(k < 0 ? 0 : k > 1 ? 1 : k);
        return vizHsvToRgb(h, st.saturation, valueFor(d) * ghost * (0.55f + 0.65f * pulse_));
    }

private:
    // The dot's own brightness envelope (age fades x the silence
    // gate), shared by colorFor and trailColorFor.
    float valueFor(const VizFieldDot& d) const {
        const float fadeIn = d.age < 0.4f ? 0.4f + 0.6f * (d.age / 0.4f) : 1.f;
        const float fadeOut = d.age > d.life * 0.7f ? 1.f - (d.age - d.life * 0.7f) / (d.life * 0.3f + 1e-6f) : 1.f;
        return (0.30f + 0.70f * fadeOut) * fadeIn * visibility();
    }
    std::uint32_t rng_ = 0x27d4eb2fu;
    float next01() { // splitmix32, own stream (deterministic)
        rng_ += 0x9e3779b9u;
        std::uint32_t z = rng_;
        z = (z ^ (z >> 16)) * 0x85ebca6bu;
        z = (z ^ (z >> 13)) * 0xc2b2ae35u;
        return (float)((z ^ (z >> 16)) >> 8) / 16777216.f;
    }
    VizFieldDot spawnDot(const float bandEnergy[3]) {
        // Band pick weighted by energy (uniform when all quiet —
        // the gate hides the field in silence anyway).
        const float total = bandEnergy[0] + bandEnergy[1] + bandEnergy[2];
        int band = 1;
        if (total > 0.001f) {
            const float r = next01() * total;
            band = r < bandEnergy[0] ? 0 : r < bandEnergy[0] + bandEnergy[1] ? 1 : 2;
        } else {
            band = (int)(next01() * 3.f); if (band > 2) band = 2;
        }
        VizFieldDot d;
        d.band = band;
        d.x = vizBandFieldX(band, balance_) + (next01() - 0.5f) * 0.16f;
        d.y = 0.08f + next01() * 0.84f;
        d.x = d.x < 0.02f ? 0.02f : d.x > 0.98f ? 0.98f : d.x;
        d.vx = 0; d.vy = 0;
        d.age = 0; d.life = 2.5f + next01() * 3.f;
        d.size = 1.4f + next01() * 2.0f;
        d.trailSet = false; d.trailX[0] = d.trailX[1] = d.x; d.trailY[0] = d.trailY[1] = d.y;
        // Join the vortex nearest the birthplace (x-led: the
        // field positions are horizontal by design).
        int best = 0; float bestScore = 1e9f;
        for (int i = 0; i < kVortices; ++i) {
            float cx, cy; vortexCentre(i, cx, cy);
            const float score = std::abs(cx - d.x) + 0.5f * std::abs(cy - d.y);
            if (score < bestScore) { bestScore = score; best = i; }
        }
        d.vortex = best;
        return d;
    }
    std::vector<VizFieldDot> dots_;
    float activity_ = 0;
    float balance_ = 0;
    float smoothE_[3] = { 0, 0, 0 }; // v0.26.0: slow per-band level (transient reference)
    float trans_[3] = { 0, 0, 0 };   // v0.26.0: per-band transient envelopes
    float pulse_ = 0;                // v0.26.0: field-wide pulse envelope
    float prevBeatEnv_ = 0;          // v0.26.0: downbeat edge detection
    float trailTimer_ = 0;           // v0.26.0: tail shift cadence
    double time_ = 0;
};

// Per-band energies (0..1) from a spectrum dB vector: mean level per
// band (low < 250 Hz, mid 250 Hz–4 kHz, high > 4 kHz), mapped through
// the window -70..-6 dB -> 0..1. Empty bands and empty input read 0.
inline std::array<float, 3> spectrumBandEnergies(const std::vector<float>& dbPerBin,
                                                 int fftSize, double sampleRate) {
    std::array<float, 3> out{ 0.f, 0.f, 0.f };
    if (dbPerBin.empty() || fftSize <= 0 || sampleRate <= 0) return out;
    double sum[3] = { 0, 0, 0 }; int cnt[3] = { 0, 0, 0 };
    for (size_t b = 1; b < dbPerBin.size(); ++b) { // skip DC
        const double f = (double)b * sampleRate / fftSize;
        const int band = f < 250.0 ? 0 : f < 4000.0 ? 1 : 2;
        float t = (dbPerBin[b] + 70.f) / 64.f;
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        sum[band] += t; ++cnt[band];
    }
    for (int i = 0; i < 3; ++i) if (cnt[i] > 0) out[(size_t)i] = (float)(sum[i] / cnt[i]);
    return out;
}
} // namespace meterbar
