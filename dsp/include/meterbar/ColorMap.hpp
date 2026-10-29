#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace meterbar {
// Midnight is APPENDED (value 6), never inserted: presets store the map as
// an int, and existing saved values 0..5 must keep their meaning.
enum class ColorMapId { Plasma, Rainbow, Inferno, Viridis, Magma, Turbo, Midnight };
inline const char* colorMapName(ColorMapId id) {
    switch (id) { case ColorMapId::Plasma: return "Plasma"; case ColorMapId::Rainbow: return "Rainbow";
    case ColorMapId::Inferno: return "Inferno"; case ColorMapId::Viridis: return "Viridis";
    case ColorMapId::Magma: return "Magma"; case ColorMapId::Midnight: return "Midnight Kids"; default: return "Turbo"; }
}
struct Rgb { uint8_t r, g, b; };
inline Rgb spectrumHeat(float t); // forward declaration — defined below; ColorMap::build(Midnight) samples it
// Generated LUTs: each map is defined by control stops (sampled from the well-known
// public-domain matplotlib-style maps / rainbow & turbo definitions) and linearly
// interpolated to 256 entries. No third-party assets are copied.
class ColorMap {
public:
    static std::array<Rgb, 256> build(ColorMapId id) {
        if (id == ColorMapId::Midnight) {
            // The v0.14 full-palette heat ramp (violet -> teal -> green ->
            // yellow -> orange -> hot red) sampled into a LUT, so the
            // built-in palette is itself a selectable Color Map and the
            // default spectrogram look is unchanged when it is selected.
            std::array<Rgb, 256> lut{};
            for (int i = 0; i < 256; ++i) lut[(size_t)i] = spectrumHeat(i / 255.0f);
            return lut;
        }
        using Stop = std::pair<float, Rgb>;
        std::vector<Stop> stops;
        auto S = [&](float t, int r, int g, int b){ stops.push_back({t, {(uint8_t)r,(uint8_t)g,(uint8_t)b}}); };
        switch (id) {
        case ColorMapId::Plasma: S(0,13,8,135); S(.25,126,3,168); S(.5,204,71,120); S(.75,248,149,64); S(1,240,249,33); break;
        case ColorMapId::Inferno: S(0,0,0,4); S(.25,87,16,110); S(.5,188,55,84); S(.75,249,142,9); S(1,252,255,164); break;
        case ColorMapId::Magma: S(0,0,0,4); S(.25,81,18,124); S(.5,183,54,121); S(.75,252,136,97); S(1,252,253,191); break;
        case ColorMapId::Viridis: S(0,68,1,84); S(.25,59,82,139); S(.5,33,145,140); S(.75,94,201,98); S(1,253,231,37); break;
        case ColorMapId::Turbo: S(0,48,18,59); S(.2,70,107,227); S(.4,26,180,160); S(.6,120,220,60); S(.8,240,160,20); S(1,122,4,3); break;
        default: /* Rainbow */ S(0,0,0,255); S(.25,0,255,255); S(.5,0,255,0); S(.75,255,255,0); S(1,255,0,0); break;
        }
        std::array<Rgb, 256> lut{};
        for (int i = 0; i < 256; ++i) {
            float t = i / 255.0f; size_t k = 0;
            while (k + 1 < stops.size() && stops[k + 1].first < t) ++k;
            if (k + 1 >= stops.size()) { lut[i] = stops.back().second; continue; }
            float span = std::max(stops[k + 1].first - stops[k].first, 1e-6f);
            float f = (t - stops[k].first) / span;
            auto a = stops[k].second, b = stops[k + 1].second;
            lut[i] = { (uint8_t)(a.r + (b.r - a.r) * f), (uint8_t)(a.g + (b.g - a.g) * f), (uint8_t)(a.b + (b.b - a.b) * f) };
        }
        return lut;
    }
    explicit ColorMap(ColorMapId id = ColorMapId::Plasma) : lut_(build(id)), id_(id) {}
    // v0.17 ROOT-CAUSE FIX: the cast used to bind to the clamped float
    // BEFORE the *255 — (size_t)(0.577f) is 0 — so at() returned lut_[0]
    // (the map's darkest colour) for EVERY level below 1.0, and only an
    // exactly-full-scale input ever reached lut_[255]. Every map-driven
    // surface (spectrogram cells, spectrum Color Map mode) therefore
    // rendered ONE hue at varying brightness: Rainbow read as flat blue
    // at all musical levels, which is Jackson's "not rainbowy" report.
    // The v0.15 test missed it because it compared different MAPS at one
    // level (their index-0 colours differ) instead of one map at several
    // levels. Multiply first, then cast.
    Rgb at(float t01) const { return lut_[(size_t)(std::clamp(t01, 0.f, 1.f) * 255.0f)]; }
    ColorMapId id() const { return id_; }
private: std::array<Rgb, 256> lut_; ColorMapId id_;
};
// Jackson's palette gradient — the level language used across every module
// (v0.12, "midnight kids" family from the icon/logo): teal -> green -> yellow
// -> orange -> pink. Quiet = teal, hot = pink. 5-stop linear interpolation.
inline Rgb paletteGradient(float t) {
    struct C { float r,g,b; };
    static const C stops[5] = { {64,170,192}, {61,220,132}, {250,217,39}, {254,138,10}, {224,76,137} };
    t = std::clamp(t, 0.0f, 1.0f) * 4.0f;
    int k = std::min(3, (int)t); float f = t - (float)k;
    C o{ stops[k].r + (stops[k+1].r - stops[k].r) * f,
         stops[k].g + (stops[k+1].g - stops[k].g) * f,
         stops[k].b + (stops[k+1].b - stops[k].b) * f };
    return { (uint8_t)o.r, (uint8_t)o.g, (uint8_t)o.b };
}
// Spectrum colouring (v0.13, Jackson's ask): colour each bin by its LEVEL
// (quiet = cool, loud = hot RED — "the higher it is the redder it is") and/or
// by its FREQUENCY (low frequencies one hue family -> high another).
// Level uses spectrumHeat: every stop is from the midnight-kids family
// (violet / teal / green / yellow / orange) except the top stop, which is a
// true hot red. Frequency uses paletteGradient (teal lows -> pink highs).
// Blend averages the two. ColorMap mode is NOT resolved here — the view
// keeps the classic selected-ColorMap behaviour for that mode.
// Chroma is APPENDED (value 4): presets store this enum as an int.
enum class SpectrumColorMode { Level, Frequency, Blend, ColorMap, Chroma };
inline Rgb spectrumHeat(float t) {
    struct C { float r,g,b; };
    static const C stops[6] = { {88,60,180}, {64,170,192}, {61,220,132}, {250,217,39}, {254,138,10}, {254,64,36} };
    static const float pos[6] = { 0.0f, 0.30f, 0.55f, 0.75f, 0.90f, 1.0f };
    t = std::clamp(t, 0.0f, 1.0f);
    int k = 0; while (k < 4 && t > pos[k + 1]) ++k;
    float span = std::max(pos[k + 1] - pos[k], 1e-6f);
    float f = (t - pos[k]) / span;
    C o{ stops[k].r + (stops[k+1].r - stops[k].r) * f,
         stops[k].g + (stops[k+1].g - stops[k].g) * f,
         stops[k].b + (stops[k+1].b - stops[k].b) * f };
    return { (uint8_t)o.r, (uint8_t)o.g, (uint8_t)o.b };
}
// Level (dB) -> heat position for the Spectrum Level/Blend modes (v0.14).
// The v0.13 view mapped the -90..0 dB window LINEARLY, so programme
// material — whose bins live in the top third of that window, tilt boost
// included — rendered almost entirely orange/red and the cool half of the
// palette never appeared (Jackson's loud-tone screenshot was all pink/red).
// A 1.35 power curve spreads the working range across the whole ramp:
// -60 dB -> ~0.23 (teal), -30 dB -> ~0.58 (green/yellow), -12 dB -> ~0.82
// (orange), only near 0 dB reaches the true-red top stop.
inline float spectrumLevelHeatT(float db) {
    float lvl = std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
    return std::pow(lvl, 1.35f);
}
// Spectrogram heat (v0.14): the SAME full spectrumHeat palette (violet ->
// teal -> green -> yellow -> orange -> red) with the low end pulled toward
// black, so silence/quiet cells stay dark instead of glowing violet. This
// replaces the spectrogram's old private maroon-red ramp (deep red ->
// orange -> white), which never showed the cool half of the palette.
inline Rgb spectrogramHeat(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto c = spectrumHeat(t);
    float gain = 0.10f + 0.90f * t;
    return { (uint8_t)(c.r * gain), (uint8_t)(c.g * gain), (uint8_t)(c.b * gain) };
}
// Spectrogram cell colour from the SELECTED Color Map (v0.15 colour-map
// fix): the spectrogram lost its colour-map feed in v0.14, when its
// private ramp was replaced by the fixed spectrumHeat palette — from then
// on the Menu -> Color Map selector fed ONLY the spectrum's Colour By =
// Color Map mode, so on the default Level mode switching maps changed
// nothing visible anywhere (Jackson's bug report). The spectrogram once
// again colours its cells from the selected map's LUT, with the same
// low-end darkening spectrogramHeat used (silence stays dark).
inline Rgb spectrogramMapColour(const ColorMap& cm, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto c = cm.at(t);
    float gain = 0.10f + 0.90f * t;
    return { (uint8_t)(c.r * gain), (uint8_t)(c.g * gain), (uint8_t)(c.b * gain) };
}
// Spectrogram level mapping (v0.15, Jackson's field feedback): at the trim
// where every OTHER module reads correctly, the spectrogram never reached
// red. The other modules use wide windows (spectrum/loudness integrate or
// peak near full scale); a single spectrogram CELL is one FFT bin, and a
// strong bin in real music sits around -30..-12 dB, not near 0. The old
// view mapped cells over -90..-10 dB linearly, so programme cells landed
// mid-palette at best. The spectrogram got its OWN window (floor -75 dB,
// ceiling -8 dB, 0.85 gamma).
// v0.17: the window is widened and made LINEAR — floor -70 dB, ceiling
// -4 dB, no gamma — so a cell's level sweeps the FULL selected map:
// equal dB steps take equal steps through the colours, the cool end
// covers genuinely quiet cells, and red is reserved for the top of the
// range (Jackson's clarified ask: "the more volume on that frequency at
// that time, the more red it is, given from the spectrum of colors").
// Anchors (Rainbow map): -40 dB -> 0.45 (green-cyan, cool half),
// -20 dB -> 0.76 (yellow), -6 dB -> 0.97 (red). Midnight anchors stay in
// family: -30 dB -> 0.61 (green-yellow), -12 dB -> 0.88 (orange).
// This touches ONLY the spectrogram's cell colour path — no other module's
// gain, trim or mapping changes.
inline float spectrogramLevelT(float db) {
    constexpr float kFloorDb = -70.0f, kCeilDb = -4.0f;
    return std::clamp((db - kFloorDb) / (kCeilDb - kFloorDb), 0.0f, 1.0f);
}
// Spectrum bar colour from the SELECTED Color Map (v0.17): the spectrum's
// Colour By = Color Map mode used to feed the LUT a plain LINEAR level
// over the -90..0 dB draw window, so musical bars (-50..-15 dB) bunched
// into the middle of the map and the cool and red ends never appeared —
// the same compression Jackson reported on the spectrogram. The map is
// now fed by spectrumLevelHeatT, the spectrum's own level language (the
// curve the Level mode uses), so with Rainbow selected a bar's colour
// follows its height through the rainbow: quiet = cool end, rising =
// yellow/orange, loudest = red. No darkening: bars sit on the panel at
// full map brightness, like the Level mode.
inline Rgb spectrumMapColour(const ColorMap& cm, float db) {
    return cm.at(spectrumLevelHeatT(db));
}
inline Rgb spectrumColor(SpectrumColorMode mode, float level01, float freq01) {
    switch (mode) {
    case SpectrumColorMode::Frequency: return paletteGradient(freq01);
    case SpectrumColorMode::Blend: {
        auto a = spectrumHeat(level01), b = paletteGradient(freq01);
        return { (uint8_t)(((int)a.r + (int)b.r) / 2), (uint8_t)(((int)a.g + (int)b.g) / 2), (uint8_t)(((int)a.b + (int)b.b) / 2) };
    }
    default: return spectrumHeat(level01); // Level (and ColorMap/Chroma, which the view overrides)
    }
}
// Chroma colouring (v0.16): colour by PITCH CLASS — 12 hues, one per note
// name, cycling every octave (A is the same hue in every octave).
// Brightness follows the level so loud notes glow and quiet bins recede.
inline Rgb chromaRgb(float freqHz, float level01) {
    if (!(freqHz > 1.0f)) freqHz = 1.0f;
    double midi = 69.0 + 12.0 * std::log2((double)freqHz / 440.0);
    long pc = ((std::lround(midi) % 12) + 12) % 12;   // 0=C .. 9=A .. 11=B
    float hue = (float)pc / 12.0f, sat = 0.72f, val = 0.30f + 0.70f * std::clamp(level01, 0.0f, 1.0f);
    float c = val * sat, x = c * (1.0f - std::fabs(std::fmod(hue * 6.0f, 2.0f) - 1.0f)), m = val - c;
    float r = 0, g = 0, b = 0; int seg = (int)std::floor(hue * 6.0f) % 6;
    if (seg == 0) { r = c; g = x; } else if (seg == 1) { r = x; g = c; } else if (seg == 2) { g = c; b = x; }
    else if (seg == 3) { g = x; b = c; } else if (seg == 4) { r = x; b = c; } else { r = c; b = x; }
    return { (uint8_t)((r + m) * 255.0f), (uint8_t)((g + m) * 255.0f), (uint8_t)((b + m) * 255.0f) };
}
} // namespace meterbar
