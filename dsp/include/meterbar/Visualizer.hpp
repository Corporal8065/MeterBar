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
    void update(float dt, const float bandEnergy[3], const VisualizerSettings& st) {
        if (dt <= 0) return;
        time_ += dt;
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
        for (int band = 0; band < 3; ++band) {
            refractory_[band] -= dt;
            if (bandEnergy[band] >= kTrigger && refractory_[band] <= 0) {
                refractory_[band] = kRefractorySec;
                spawnBurst(band, bandEnergy[band], cap);
            }
        }
        // --- integrate ---
        for (auto& p : ps_) {
            p.age += dt;
            if (!p.burst) {
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
        return vizHsvToRgb(h, st.saturation, 0.35f + 0.65f * fadeOut);
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
        const float ang = next01() * 6.2831853f;
        const float rad = 0.55f + next01() * 0.25f;
        p.x = 0.5f + std::cos(ang) * rad * 0.5f + (next01() - 0.5f) * 0.1f;
        p.y = 0.5f + std::sin(ang) * rad * 0.35f + (next01() - 0.5f) * 0.1f;
        p.vx = 0; p.vy = 0;
        p.age = 0; p.life = 6.f + next01() * 8.f;
        p.size = 1.2f + next01() * 1.6f;
        p.burst = false;
        return p;
    }
    void spawnBurst(int band, float energy, int cap) {
        // Band anchors: low = lower centre, mid = centre, high = upper centre.
        const float ax = 0.5f, ay = band == 0 ? 0.72f : band == 1 ? 0.5f : 0.28f;
        const float ang = next01() * 6.2831853f;
        const float dx = std::cos(ang), dy = std::sin(ang); // stroke direction
        const float halfLen = 0.10f + 0.08f * energy;
        int want = (int)std::lround(26.f + 34.f * energy);
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
            const float out = 0.10f + 0.25f * energy * (1.f - std::abs(t - 0.5f));
            p.vx = (p.x - ax) * out * 8.f + (next01() - 0.5f) * 0.05f;
            p.vy = (p.y - ay) * out * 8.f + (next01() - 0.5f) * 0.05f;
            p.age = 0; p.life = 0.9f + next01() * 0.8f;
            p.size = 1.6f + next01() * 2.2f;
            p.hueT = t;
            p.burst = true;
            ps_.push_back(p);
        }
    }
    std::vector<VizParticle> ps_;
    float refractory_[3] = { 0, 0, 0 };
    int bursts_ = 0;
    float activity_ = 0; // v0.20.3: smoothed max band energy (the silence gate)
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
            // Damped spring: stiff enough to answer a hit, soft
            // enough to settle like ink — never a raw copy of the
            // frame energy (that jitter is what this style avoids).
            vel_[b] += ((target - level_[b]) * 42.f - vel_[b] * 9.f) * dt;
            level_[b] += vel_[b] * dt;
            if (level_[b] < 0.f) { level_[b] = 0.f; if (vel_[b] < 0.f) vel_[b] = 0.f; }
            if (level_[b] > 1.15f) { level_[b] = 1.15f; if (vel_[b] > 0.f) vel_[b] = 0.f; }
            // Near-still when quiet: the creep floor is 0.15 rad/s
            // and the amplitude (centreY) collapses with the level.
            phase_[b] += dt * (0.15f + 1.35f * level_[b]) * kSpeed[b];
        }
    }

    float level(int band) const { return level_[band]; }
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
    float halfThick(int band, int i) const {
        const float u = kPoints > 1 ? (float)i / (float)(kPoints - 1) : 0.f;
        const float breathe = 0.75f + 0.25f * std::sin(phase_[band] * 0.9f + u * 3.1f + (float)band * 1.7f);
        return (0.004f + 0.030f * level_[band]) * breathe;
    }
    // Band colour from the same palette as the burst dots: the
    // three bands fan across the palette spread from the base hue.
    VizRgb colorFor(int band, const VisualizerSettings& st) const {
        float cycle = 0.f;
        if (st.cycleSeconds > 0.01f) cycle = (float)std::fmod(time_ / (double)st.cycleSeconds, 1.0);
        const float h = st.baseHue + cycle + (float)band * st.paletteSpread * 0.33f;
        return vizHsvToRgb(h, st.saturation, 0.9f);
    }

private:
    float level_[3] = { 0, 0, 0 };
    float vel_[3] = { 0, 0, 0 };
    float phase_[3] = { 0, 0, 0 };
    float wob_[3][kPoints] = {};
    float activity_ = 0;
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
