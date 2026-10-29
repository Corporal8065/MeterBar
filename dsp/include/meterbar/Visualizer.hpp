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

    // Advance the model. bandEnergy = [low, mid, high], each 0..1
    // (see spectrumBandEnergies). timeSec = a free-running clock for
    // palette cycling (the engine also tracks it from dt if negative).
    void update(float dt, const float bandEnergy[3], const VisualizerSettings& st) {
        if (dt <= 0) return;
        time_ += dt;
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
                p.vx *= 0.995f; p.vy *= 0.995f;
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

    // Colour of one particle under the palette settings. Ambient
    // particles are WHITE by design (fade by age only). Burst dots
    // sweep baseHue -> baseHue + spread across the stroke (hueT),
    // drifted by the cycle clock.
    VizRgb colorFor(const VizParticle& p, const VisualizerSettings& st) const {
        if (!p.burst) {
            const float fade = p.age < 0.5f ? 0.4f + 0.6f * (p.age / 0.5f) : 1.f;
            const auto v = (std::uint8_t)std::lround(255.f * fade);
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
