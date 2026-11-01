// MeterBar DSP core tests — plain asserts, no framework.
#define _USE_MATH_DEFINES // MSVC: expose M_PI from <cmath> (kit fix v0.6)
#include <cstdio>
#include <cstdint>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include "../dsp/include/meterbar/Fft.hpp"
#include "../dsp/include/meterbar/Loudness.hpp"
#include "../dsp/include/meterbar/Modules.hpp"
#include "../dsp/include/meterbar/ColorMap.hpp"
#include "../dsp/include/meterbar/RingBuffer.hpp"
#include "../dsp/include/meterbar/Arrange.hpp"
#include "../dsp/include/meterbar/ReserveState.hpp"
#include "../dsp/include/meterbar/SemVer.hpp"
#include "../dsp/include/meterbar/ReleaseParse.hpp"
#include "../dsp/include/meterbar/State.hpp"
#include "../dsp/include/meterbar/Visualizer.hpp"
#include "../dsp/include/meterbar/BarLink.hpp"
#include "../dsp/include/meterbar/Recordings.hpp"
#include "../dsp/include/meterbar/WindowGeom.hpp"
#include "../dsp/include/meterbar/BoundedThread.hpp"
#include "../dsp/include/meterbar/Glyphs.hpp"
#include "../dsp/include/meterbar/Captions.hpp"
static int failures = 0, checks = 0;
#define CHECK(cond) do{ ++checks; if(!(cond)){ ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);} else { std::printf("ok   %s\n", #cond);} }while(0)
int main() {
    using namespace meterbar; const double sr = 48000;
    // --- FFT: 1 kHz sine at sr 48k, FFT 4096 -> peak bin should be round(1000*4096/48000)=85
    {
        SpectrumAnalyser an; an.prepare(4096); an.setSmoothing(0.0f); an.setTiltDb(0.0f);
        std::vector<float> x(4096); for (int i = 0; i < 4096; ++i) x[i] = 0.5f * std::sin(2*M_PI*1000*i/sr);
        auto& db = an.process(x.data(), 4096); int peak = 0; for (int b = 1; b < an.numBins(); ++b) if (db[b] > db[peak]) peak = b;
        std::printf("FFT peak bin=%d expected=85 value=%.2f dB\n", peak, db[peak]);
        CHECK(peak == 85); CHECK(db[peak] > -8.0f && db[peak] < -4.0f); // 0.5 amp sine ~= -6 dBFS
    }
    // --- RMS: full-scale sine RMS = -3.01 dBFS
    {
        LoudnessEngine le; le.prepare(sr);
        std::vector<float> L(48000), R(48000); for (int i = 0; i < 48000; ++i){ L[i]=std::sin(2*M_PI*440*i/sr); R[i]=L[i]; }
        for (int rep = 0; rep < 6; ++rep) le.pushBlock(L.data(), R.data(), 48000); // let 1 s slow ballistics settle
        double rms = le.rmsDb(true, false); std::printf("RMS slow full-scale sine = %.3f dBFS (expect ~-3.01)\n", rms);
        CHECK(std::abs(rms - (-3.01)) < 0.15);
        double plus3 = le.rmsDb(true, true); CHECK(std::abs(plus3 - rms - 3.0) < 1e-9);
    }
    // --- LUFS: 997 Hz sine at -23 dBFS-ish amplitude -> momentary near -23 LUFS (K shelf adds a bit)
    {
        LoudnessEngine le; le.prepare(sr); float amp = std::pow(10.0f, -23.0f/20.0f);
        std::vector<float> L(48000), R(48000); for (int i = 0; i < 48000; ++i){ L[i]=amp*std::sin(2*M_PI*997*i/sr); R[i]=L[i]; }
        le.pushBlock(L.data(), R.data(), 48000);
        double m = le.momentaryLufs(), st = le.shortTermLufs();
        std::printf("LUFS momentary=%.2f short-term=%.2f (expect sane: -30..-16)\n", m, st);
        CHECK(m > -30.0 && m < -16.0); CHECK(st > -30.0 && st < -16.0);
    }
    // --- Mid/Side math round trip + selectChan
    {
        float m, s, l, r; toMidSide(0.8f, 0.2f, m, s); CHECK(std::abs(m-0.5f)<1e-6 && std::abs(s-0.3f)<1e-6);
        fromMidSide(m, s, l, r); CHECK(std::abs(l-0.8f)<1e-6 && std::abs(r-0.2f)<1e-6);
        CHECK(std::abs(selectChan(1.f, -1.f, ChanSel::Side) - 1.0f) < 1e-6);
    }
    // --- Correlation: mono signal = +1, anti-phase = -1
    {
        StereometerEngine se; std::vector<float> L(1024), R(1024), Rn(1024);
        for (int i = 0; i < 1024; ++i){ L[i]=std::sin(2*M_PI*220*i/sr); R[i]=L[i]; Rn[i]=-L[i]; }
        auto f1 = se.analyse(L.data(), R.data(), 1024); auto f2 = se.analyse(L.data(), Rn.data(), 1024);
        std::printf("corr mono=%.3f anti=%.3f\n", f1.correlation, f2.correlation);
        CHECK(std::abs(f1.correlation - 1.0f) < 1e-3); CHECK(std::abs(f2.correlation + 1.0f) < 1e-3);
        auto pr = StereometerEngine::project(0.5f, 0.5f, StereoMode::LinearBipolar); CHECK(std::abs(pr.second-0.5f)<1e-6);
    }
    // --- Oscilloscope pitch estimate on 220 Hz
    {
        std::vector<float> x(8192); for (int i = 0; i < 8192; ++i) x[i] = std::sin(2*M_PI*220*i/sr);
        int p = OscilloscopeEngine::estimatePeriod(x.data(), 8192, sr);
        double f = p ? sr / p : 0; std::printf("scope period=%d -> %.1f Hz (expect ~220)\n", p, f);
        CHECK(std::abs(f - 220.0) < 12.0);
    }
    // --- Color maps: endpoints differ, plasma ends bright yellow-ish
    {
        for (auto id : {ColorMapId::Plasma, ColorMapId::Rainbow, ColorMapId::Inferno, ColorMapId::Viridis, ColorMapId::Magma, ColorMapId::Turbo}) {
            ColorMap cm(id); auto a = cm.at(0.f), b = cm.at(1.f);
            CHECK(a.r != b.r || a.g != b.g || a.b != b.b);
        }
        ColorMap plasma(ColorMapId::Plasma); auto top = plasma.at(1.f); CHECK(top.r > 200 && top.g > 200);
        auto g = paletteGradient(0.0f); CHECK(g.b > g.r); // teal start: blue > red
    }
    // --- Ring buffer: write 3 s stereo, clip 1 s returns most recent
    {
        StereoRingBuffer rb; rb.prepare(48000 * 4);
        for (int i = 0; i < 48000 * 3; ++i) rb.push((float)i / 48000.f, -(float)i / 48000.f);
        std::vector<float> cl(48000), cr(48000); int got = rb.readClip(cl.data(), cr.data(), 1.0, sr);
        CHECK(got == 48000); CHECK(std::abs(cl[47999] - 3.0f) < 0.01f); CHECK(std::abs(cl[0] - 2.0f) < 0.01f);
    }
    // --- Spectrogram + waveform history + freq scales
    {
        SpectrogramHistory sg; sg.prepare(64, 32); std::vector<float> row(64, -20.f);
        for (int i = 0; i < 40; ++i) sg.pushRow(row, 5.f);
        CHECK(std::abs(sg.at(0, 10) - (-20.f)) < 0.01f);
        CHECK(SpectrogramHistory::fadeGain(0, 32, 0.5f) == 1.0f); CHECK(SpectrogramHistory::fadeGain(31, 32, 0.5f) < 0.55f);
        WaveformHistory wh; wh.prepare(64); std::vector<float> L(512, 0.25f), R(512, 0.25f);
        wh.pushBlock(L.data(), R.data(), 512, ChanSel::Left, ChanSel::Off, HistoryMode::Off, 5.f); CHECK(wh.ch1().back() > 0.2f);
        CHECK(std::abs(SpectrumAnalyser::freqToX(1000, 20, 20000, FreqScale::Log) - 0.566f) < 0.02f);
        CHECK(SpectrumAnalyser::freqToX(1000, 20, 20000, FreqScale::Linear) < SpectrumAnalyser::freqToX(1000, 20, 20000, FreqScale::Log));
    }
    // --- Preset defaults vs SPEC (recipe values updated v0.17)
    {
        PresetState p;
        CHECK(p.fftSize == 1024); CHECK(p.spectrumSmoothingPct == 50.0f); CHECK(p.spectrumTiltDb == 5.6f);
        CHECK(p.fade == 0.5f); CHECK(p.fftScale == FreqScale::Log); CHECK(p.vsync == true);
        CHECK(p.stereoPointSize == 1.0f); CHECK(p.themeName == "Midnight Kids"); CHECK(isValidFftSize(16384) && !isValidFftSize(1000));
        CHECK(p.backgroundOpacity == 0.15f); CHECK(p.spectrumColorMode == SpectrumColorMode::Level);
        CHECK(p.outputDeviceId.empty());
    }
    // --- Spectrum colour modes (v0.13): level heat cool->hot red, frequency gradient, blend between
    {
        auto lo = spectrumColor(SpectrumColorMode::Level, 0.f, 0.f);
        auto hi = spectrumColor(SpectrumColorMode::Level, 1.f, 0.f);
        CHECK(lo.b > lo.r);                                    // quiet = cool violet (blue-dominant)
        CHECK(hi.r > 200 && hi.r > hi.g && hi.r > hi.b);       // loud = hot red
        auto fLo = spectrumColor(SpectrumColorMode::Frequency, 0.f, 0.f);
        auto fHi = spectrumColor(SpectrumColorMode::Frequency, 0.f, 1.f);
        CHECK(fLo.b > fLo.r && fHi.r > fHi.b);                 // teal lows -> pink highs
        auto bl = spectrumColor(SpectrumColorMode::Blend, 1.f, 0.f);
        CHECK(bl.r < hi.r && bl.r > 100);                      // blend sits between the two pure colours
    }
    // --- v0.14 colour range: level->heat spreads the palette; spectrogram heat is dark at silence, full palette up top
    {
        CHECK(spectrumLevelHeatT(-90.f) == 0.0f); CHECK(spectrumLevelHeatT(0.f) == 1.0f);
        float t60 = spectrumLevelHeatT(-60.f), t30 = spectrumLevelHeatT(-30.f), t12 = spectrumLevelHeatT(-12.f);
        std::printf("levelHeatT: -60dB=%.3f -30dB=%.3f -12dB=%.3f\n", t60, t30, t12);
        CHECK(t60 > 0.15f && t60 < 0.35f);                     // quiet programme material lands in the COOL half
        CHECK(t30 > 0.5f && t30 < 0.65f);                      // mid level = mid palette (green/yellow)
        CHECK(t12 > 0.75f && t12 < 0.9f);                      // loud = orange, true red reserved for near-0 dB
        CHECK(t60 < t30 && t30 < t12);                         // monotonic
        auto sg0 = spectrogramHeat(0.f);
        CHECK(sg0.r < 30 && sg0.g < 30 && sg0.b < 30);         // silence is DARK (not glowing violet)
        CHECK(sg0.b > sg0.r);                                  // ...but still on the violet end of the ramp
        auto sgMid = spectrogramHeat(0.55f);
        CHECK(sgMid.g > sgMid.r && sgMid.g > sgMid.b);         // mid level = green-dominant (full palette, not maroon)
        auto sgTop = spectrogramHeat(1.f);
        CHECK(sgTop.r > 200 && sgTop.r > sgTop.g);             // top = the same hot red as spectrumHeat
    }
    // --- v0.15 spectrogram sensitivity: its OWN window — near-full-scale bins reach the RED heat zone, quiet stays dark
    //     (v0.17: window is now linear -70..-4 dB; the anchors below still hold — see the v0.17 block for the full sweep)
    {
        float tHot = spectrogramLevelT(-12.f), tFull = spectrogramLevelT(-3.f), tQuiet = spectrogramLevelT(-70.f), tMid = spectrogramLevelT(-30.f);
        std::printf("spectrogramLevelT: -12dB=%.3f -3dB=%.3f -30dB=%.3f -70dB=%.3f\n", tHot, tFull, tMid, tQuiet);
        CHECK(tFull >= 0.95f);                                  // near full scale -> top of the ramp
        auto hot = spectrogramHeat(tHot);
        CHECK(hot.r > 200 && hot.r > hot.g);                    // a strong music bin (-12 dB) renders RED
        CHECK(tQuiet < 0.15f);                                  // quiet stays in the dark/low zone (rendered <40 RGB, checked next)
        auto quiet = spectrogramHeat(tQuiet);
        CHECK(quiet.r < 40 && quiet.g < 40 && quiet.b < 40);     // ...and dark on screen
        CHECK(tMid > 0.6f && tMid < 0.8f);                      // mid programme level = yellow zone, not red
        CHECK(spectrogramLevelT(-75.f) == 0.0f && spectrogramLevelT(0.f) == 1.0f);
    }
    // --- v0.15 spectrum DISPLAY tilt (Pro-Q style): an equal-per-octave (pink-like) spectrum falling
    //     -4.5 dB/oct draws FLAT at +4.5 dB/oct compensation and sloped at tilt 0. Display-only math.
    //     (Function-level checks; the v0.17 Default preset uses 1.15 — see the recipe block.)
    {
        const int fftSz = 4096; SpectrumAnalyser an2; an2.prepare(fftSz);
        std::vector<float> pink((size_t)an2.numBins());
        for (int b = 0; b < an2.numBins(); ++b) {
            double f = SpectrumAnalyser::binFreq(b, fftSz, sr);
            pink[(size_t)b] = (float)(-10.0 - 4.5 * std::log2(std::max(f, 20.0) / 1000.0));
        }
        auto slopeOf = [&](const std::vector<float>& v) { // least-squares slope in dB per octave, 100 Hz..10 kHz
            double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
            for (int b = 1; b < (int)v.size(); ++b) {
                double f = SpectrumAnalyser::binFreq(b, fftSz, sr);
                if (f < 100.0 || f > 10000.0) continue;
                double x = std::log2(f / 1000.0), y = v[(size_t)b];
                sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
            }
            return (n * sxy - sx * sy) / (n * sxx - sx * sx);
        };
        auto drawn0 = applySpectrumDisplayTilt(pink, fftSz, sr, 0.0f);
        auto drawnDef = applySpectrumDisplayTilt(pink, fftSz, sr, 4.5f);
        auto drawnMax = applySpectrumDisplayTilt(pink, fftSz, sr, 6.0f);
        double s0 = slopeOf(drawn0), sDef = slopeOf(drawnDef), sMax = slopeOf(drawnMax);
        std::printf("display tilt slopes: tilt0=%.3f default4.5=%.3f max6=%.3f dB/oct\n", s0, sDef, sMax);
        CHECK(std::abs(s0 - (-4.5)) < 0.15);                    // no compensation: the raw -4.5 dB/oct slope
        CHECK(std::abs(sDef) < 0.15);                           // default tilt: drawn flat
        CHECK(std::abs(sMax) < std::abs(s0));                   // max tilt: measurably flatter than tilt 0
        CHECK(spectrumDisplayTiltCompDb(1000.0, 4.5f) == 0.0f); // pivot at 1 kHz
        CHECK(std::abs(spectrumDisplayTiltCompDb(2000.0, 4.5f) - 4.5f) < 1e-4); // +1 octave = +tilt dB
    }
    // --- v0.15 stereo vectorscope: L/R recovery from Mid/Side (Lissajous point)
    {
        auto mono = StereometerEngine::lissajousPoint(0.5f, 0.0f);   // L = R = 0.5 -> M .5, S 0
        CHECK(std::abs(mono.first - 0.5f) < 1e-6 && std::abs(mono.second - 0.5f) < 1e-6); // on the L=R diagonal
        float m, s; toMidSide(0.8f, 0.2f, m, s);
        auto pt = StereometerEngine::lissajousPoint(m, s);
        CHECK(std::abs(pt.first - 0.2f) < 1e-6 && std::abs(pt.second - 0.8f) < 1e-6);     // {x=R, y=L} round-trips
        auto anti = StereometerEngine::lissajousPoint(0.0f, 0.5f);   // L = -R
        CHECK(std::abs(anti.first + anti.second) < 1e-6);            // opposite diagonal
    }
    // --- v0.15 colour-map wiring: two maps give measurably different spectrogram colours for the same level;
    //     the new Midnight map IS the v0.14 palette (so the default look is unchanged), and it is the default.
    {
        ColorMap plasma(ColorMapId::Plasma), rainbow(ColorMapId::Rainbow), midnight(ColorMapId::Midnight);
        auto a = spectrogramMapColour(plasma, 0.5f), b = spectrogramMapColour(rainbow, 0.5f);
        int delta = std::abs((int)a.r - (int)b.r) + std::abs((int)a.g - (int)b.g) + std::abs((int)a.b - (int)b.b);
        std::printf("spectrogram map delta plasma-vs-rainbow @0.5 = %d (need > 60)\n", delta);
        CHECK(delta > 60);
        // v0.17 REGRESSION (the check whose absence let the LUT-index bug
        // ship in v0.15/v0.16): ONE map at SEVERAL levels must give its
        // several colours. at() cast the clamped float to size_t BEFORE
        // the *255, so every level < 1 returned lut_[0] — Rainbow was
        // blue at every musical level on every map-driven surface.
        auto r0 = rainbow.at(0.f), r25 = rainbow.at(0.25f), r50 = rainbow.at(0.5f), r75 = rainbow.at(0.75f), r100 = rainbow.at(1.f);
        CHECK(r0.b == 255 && r0.r == 0 && r0.g == 0);                    // blue
        CHECK(r25.g > 200 && r25.b > 200 && r25.r < 80);                 // cyan
        CHECK(r50.g == 255 && r50.r == 0 && r50.b < 80);                 // green
        CHECK(r75.r > 200 && r75.g > 200 && r75.b < 80);                 // yellow
        CHECK(r100.r == 255 && r100.g == 0 && r100.b == 0);              // red
        CHECK(!(r0.r == r50.r && r0.g == r50.g && r0.b == r50.b));       // levels genuinely differ
        auto mLo = midnight.at(0.f), mHi = midnight.at(1.f);
        CHECK(mLo.b > mLo.r); CHECK(mHi.r > 200 && mHi.r > mHi.g);   // Midnight LUT = spectrumHeat ends (violet -> hot red)
        auto dark = spectrogramMapColour(midnight, 0.f);
        CHECK(dark.r < 40 && dark.g < 40 && dark.b < 40);            // silence stays dark under the selected map too
        PresetState p0; CHECK(p0.colorMap == ColorMapId::Midnight);
    }
    // --- v0.15 state defaults
    {
        PresetState p;
        CHECK(p.scopeWindowMs == 100.0f); CHECK(p.spectrumDisplayTiltDb == 1.15f); // display tilt: v0.17 recipe value
        bool flexOk = true; for (float f : p.moduleFlex) flexOk = flexOk && (f == 1.0f);
        CHECK(flexOk);
    }
    // === v0.16 ===
    // --- Default preset recipe: SUPERSEDED by the v0.17 recipe block below
    //     (v0.16 introduced "C++ defaults = the Default preset"; v0.17
    //     revised the values at Jackson's dictation).
    // --- Loudness pack: integrated gating on a known-level signal; true peak finds the inter-sample peak
    {
        const double sr = 48000; const int n = (int)(3.0 * sr);
        std::vector<float> x(n), y(n);
        for (int i = 0; i < n; ++i) { double v = 0.2236 * std::sin(2 * M_PI * 1000.0 * i / sr); x[i] = (float)v; y[i] = (float)v; } // ~-13 dBFS sine
        LoudnessEngine le; le.prepare(sr); le.pushBlock(x.data(), y.data(), n);
        double integ = le.integratedLufs(), mom = le.momentaryLufs();
        std::printf("integrated=%.2f momentary=%.2f (constant signal: gated integrated must track momentary)\n", integ, mom);
        CHECK(std::abs(integ - mom) < 1.0);        // constant programme: gating keeps (nearly) all blocks
        CHECK(integ > -16.0 && integ < -10.0);     // and lands at the known level of the test tone
        // True peak: 12 kHz sine sampled at +/-0.7071 — sample peak -3.01 dB, true peak 0 dB.
        LoudnessEngine tp; tp.prepare(sr);
        std::vector<float> a(n), b(n);
        for (int i = 0; i < n; ++i) { double v = std::sin(2 * M_PI * 12000.0 * i / sr + M_PI / 4.0); a[i] = (float)v; b[i] = (float)v; }
        tp.pushBlock(a.data(), b.data(), n);
        std::printf("truepeak: sample peak=%.4f true peak=%.4f (%.2f dBTP)\n", tp.peak(), tp.truePeak(), tp.truePeakDb());
        CHECK(tp.peak() < 0.75f);                  // samples really do miss the crest
        CHECK(tp.truePeak() > 0.95);               // interpolated peak recovered
        CHECK(tp.truePeakDb() > -0.5 && tp.truePeakDb() <= 0.1); // ≈ 0 dBTP, above the -3.01 dB sample peak
    }
    // --- Chroma: A4 vs the semitone above differ; an octave up is the SAME pitch class
    {
        auto a4 = chromaRgb(440.0f, 1.0f), as4 = chromaRgb(466.16f, 1.0f), a5 = chromaRgb(880.0f, 1.0f);
        int delta = std::abs((int)a4.r - (int)as4.r) + std::abs((int)a4.g - (int)as4.g) + std::abs((int)a4.b - (int)as4.b);
        std::printf("chroma delta A4-vs-A#4 = %d (need > 60)\n", delta);
        CHECK(delta > 60);
        CHECK(a4.r == a5.r && a4.g == a5.g && a4.b == a5.b);
    }
    // --- 1/6-octave smoothing crushes bin-to-bin variance on a noisy spectrum
    {
        const int fftSz = 8192, nB = fftSz / 2;
        std::vector<float> noisy(nB);
        for (int i = 0; i < nB; ++i) noisy[i] = -40.0f + (i % 2 ? 10.0f : -10.0f); // +/-10 dB zigzag
        auto sm = smoothSixthOctave(noisy, fftSz, 48000.0);
        auto varOf = [](const std::vector<float>& v) { double s = 0; for (size_t i = 100; i + 1 < v.size() && i < 3000; ++i) s += std::abs(v[i + 1] - v[i]); return s; };
        double vRaw = varOf(noisy), vSm = varOf(sm);
        std::printf("sixth-octave variance: raw=%.1f smoothed=%.1f (need < 40%%)\n", vRaw, vSm);
        CHECK(vSm < 0.4 * vRaw);
    }
    // --- Beat pulse: click train fires, steady tone doesn't
    {
        const double sr = 48000; const int blk = 256;
        std::vector<float> L(blk), R(blk);
        BeatPulse bp; bp.prepare(sr);
        auto pushSine = [&](float amp, double freq, double secs) {
            int blocks = (int)(secs * sr / blk);
            for (int b = 0; b < blocks; ++b) { for (int i = 0; i < blk; ++i) { double ph = 2 * M_PI * freq * (b * blk + i) / sr; L[i] = (float)(amp * std::sin(ph)); R[i] = L[i]; } bp.pushBlock(L.data(), R.data(), blk); }
        };
        pushSine(0.01f, 440.0, 1.2);                       // quiet bed (warm-up + baseline)
        uint64_t beforeClicks = bp.onsetCount();
        for (int c = 0; c < 6; ++c) {                      // 6 clicks, 0.5 s apart
            for (int i = 0; i < blk; ++i) { L[i] = 0.9f; R[i] = 0.9f; }
            bp.pushBlock(L.data(), R.data(), blk);
            pushSine(0.01f, 440.0, 0.47);
        }
        std::printf("beatpulse: onsets on click train = %llu (need >= 5), glow=%.2f\n",
                    (unsigned long long)(bp.onsetCount() - beforeClicks), bp.level());
        CHECK(bp.onsetCount() - beforeClicks >= 5);
        BeatPulse steady; steady.prepare(sr);
        for (int b = 0; b < (int)(2.0 * sr / blk); ++b) { for (int i = 0; i < blk; ++i) { double ph = 2 * M_PI * 440.0 * (b * blk + i) / sr; L[i] = (float)(0.5 * std::sin(ph)); R[i] = L[i]; } steady.pushBlock(L.data(), R.data(), blk); }
        std::printf("beatpulse: onsets on steady tone = %llu (need 0)\n", (unsigned long long)steady.onsetCount());
        CHECK(steady.onsetCount() == 0);
    }
    // --- Stereo width: mono = 0%, anti-phase saturates (correlation checks already exist above)
    {
        const int n = 4800; std::vector<float> L(n), R(n);
        for (int i = 0; i < n; ++i) L[i] = R[i] = (float)(0.5 * std::sin(2 * M_PI * 220.0 * i / 48000.0));
        auto fMono = StereometerEngine().analyse(L.data(), R.data(), n);
        for (int i = 0; i < n; ++i) R[i] = -L[i];
        auto fAnti = StereometerEngine().analyse(L.data(), R.data(), n);
        std::printf("width: mono=%.1f%% anti=%.1f%% corr mono=%.2f anti=%.2f\n", fMono.widthPct, fAnti.widthPct, fMono.correlation, fAnti.correlation);
        CHECK(fMono.widthPct < 0.5f); CHECK(fAnti.widthPct > 150.0f);
        CHECK(std::abs(fMono.correlation - 1.0f) < 0.01f); CHECK(std::abs(fAnti.correlation + 1.0f) < 0.01f);
    }
    // === v0.17 ===
    // --- Default preset recipe (Jackson, 2026-10-28): a fresh PresetState IS the recipe
    {
        PresetState p;
        CHECK(p.name == "Default");
        CHECK(p.themeName == "Midnight Kids"); CHECK(p.colorMap == ColorMapId::Midnight);
        CHECK(p.backgroundStyle == BackgroundStyle::CleanDark); CHECK(p.backgroundOpacity == 0.15f);
        CHECK(p.spectrumStyle == PresetState::SpectrumStyle::Both);
        CHECK(p.spectrumSmoothingPct == 50.0f);
        CHECK(p.spectrumDisplayTiltDb == 1.15f);                 // display tilt
        CHECK(p.spectrumTiltDb == 5.6f);                         // analyser tilt
        CHECK(p.spectrumColorMode == SpectrumColorMode::Level);
        CHECK(p.spectrumShowTarget == false);                    // spectrum target section off
        CHECK(p.loudnessMode == LoudnessMode::LufsMomentary);
        CHECK(p.loudnessShowTargets == false);                   // streaming target lines off (his correction)
        CHECK(p.beatPulseEnabled); CHECK(p.animeClickSounds);
        CHECK(p.scopeWindowMs == 100.0f);
        CHECK(p.audioSource == "System Loopback");               // OS-default output, NOT the test tone
        CHECK(p.outputDeviceName.empty() && p.outputDeviceId.empty()); // empty = default render endpoint
        CHECK(std::abs(p.trimDb - 10.18f) < 1e-6);               // +10.18 dB exactly
        CHECK(p.spectrogramEnabled && p.waveformEnabled && p.loudnessEnabled
              && p.stereometerEnabled && p.oscilloscopeEnabled && p.spectrumEnabled);
        bool flexOk = true; for (float f : p.moduleFlex) flexOk = flexOk && (f == 1.0f);
        CHECK(flexOk);
    }
    // --- Rainbow treatment acceptance (Jackson's clarification): LEVEL drives colour
    //     through the rainbow on BOTH surfaces — quiet = cool end, loudest = RED.
    //     Sampled at -40 / -20 / -6 dB; hue must progress cool -> warm -> red.
    {
        ColorMap rainbow(ColorMapId::Rainbow), midnight(ColorMapId::Midnight);
        auto hueOf = [](Rgb c) { // HSV hue in degrees, -1 = grey
            float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
            float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
            if (d < 1e-6f) return -1.0;
            if (mx == r) return 60.0 * std::fmod((g - b) / d + 6.0, 6.0);
            if (mx == g) return 60.0 * ((b - r) / d + 2.0);
            return 60.0 * ((r - g) / d + 4.0);
        };
        // Spectrogram surface (cell colour = spectrogramMapColour over spectrogramLevelT)
        auto sg40 = spectrogramMapColour(rainbow, spectrogramLevelT(-40.f));
        auto sg20 = spectrogramMapColour(rainbow, spectrogramLevelT(-20.f));
        auto sg06 = spectrogramMapColour(rainbow, spectrogramLevelT(-6.f));
        std::printf("SG rainbow: -40dB=(%d,%d,%d) -20dB=(%d,%d,%d) -6dB=(%d,%d,%d)\n",
                    sg40.r, sg40.g, sg40.b, sg20.r, sg20.g, sg20.b, sg06.r, sg06.g, sg06.b);
        CHECK(sg40.g > sg40.r && sg40.b < 90 && sg40.r < 100);        // cool half (green), no red in it
        CHECK(sg20.r > 150 && sg20.g > 150 && sg20.b < 60);           // warm (yellow)
        CHECK(sg06.r > 200 && sg06.g < 110 && sg06.b < 60);           // red
        double h40 = hueOf(sg40), h20 = hueOf(sg20), h06 = hueOf(sg06);
        std::printf("SG rainbow hues: -40=%.0f -20=%.0f -6=%.0f deg\n", h40, h20, h06);
        CHECK(h40 > h20 && h20 > h06);                                // hue marches cool -> warm -> red
        // Full travel: hue distance swept across the map's working range
        double prev = -1, travel = 0; bool first = true;
        for (float t = 0.05f; t <= 1.001f; t += 0.05f) {
            double h = hueOf(spectrogramMapColour(rainbow, t));
            if (!first && h >= 0 && prev >= 0) travel += std::abs(h - prev);
            if (h >= 0) prev = h; first = false;
        }
        std::printf("SG rainbow hue travel t=0.05..1.0: %.0f deg (need > 180)\n", travel);
        CHECK(travel > 180.0);
        // Spectrum surface (bar colour = spectrumMapColour over the bin's dB)
        auto sp40 = spectrumMapColour(rainbow, -40.f);
        auto sp20 = spectrumMapColour(rainbow, -20.f);
        auto sp06 = spectrumMapColour(rainbow, -6.f);
        std::printf("SP rainbow: -40dB=(%d,%d,%d) -20dB=(%d,%d,%d) -6dB=(%d,%d,%d)\n",
                    sp40.r, sp40.g, sp40.b, sp20.r, sp20.g, sp20.b, sp06.r, sp06.g, sp06.b);
        CHECK(sp40.g == 255 && sp40.r == 0 && sp40.b >= 30);          // green-cyan (cool)
        CHECK(sp20.r > 180 && sp20.g == 255 && sp20.b == 0);          // yellow-green (warm)
        CHECK(sp06.r == 255 && sp06.g < 140 && sp06.b == 0);          // orange-red (red family)
        // Midnight (the Default map) stays in ITS family at the same levels:
        // teal-green at -40, warm gold at -20, hot orange-red at -6.
        auto md40 = spectrogramMapColour(midnight, spectrogramLevelT(-40.f));
        auto md06 = spectrogramMapColour(midnight, spectrogramLevelT(-6.f));
        std::printf("SG midnight: -40dB=(%d,%d,%d) -6dB=(%d,%d,%d)\n", md40.r, md40.g, md40.b, md06.r, md06.g, md06.b);
        CHECK(md40.g > md40.r && md40.g >= md40.b);                   // teal/green family
        CHECK(md06.r > 200 && md06.r > md06.g);                       // hot end of the midnight ramp
    }
    // === v0.18 ===
    // --- Arrange math (pure function behind the Windows "Arrange windows"
    //     sweep): fit a window into the monitor area BELOW a reserved strip.
    //     Monitor M = 1920x1080 at origin; strip 170 -> free {0,170,1920,910}.
    {
        const IntRect M{0, 0, 1920, 1080};
        // A window already fully below the strip is returned UNCHANGED —
        // arranging must never shuffle windows that are not in the way.
        auto keep = arrangeFit({100, 300, 800, 600}, M, 170);
        CHECK(keep == (IntRect{100, 300, 800, 600}));
        // Top poking under the strip: pushed down to just under it, size kept.
        auto ov = arrangeFit({100, 50, 800, 600}, M, 170);
        CHECK(ov.y == 170); CHECK(ov.x == 100 && ov.w == 800 && ov.h == 600);
        // Taller than the free area: height capped to the free height, top at the strip.
        auto tall = arrangeFit({0, 0, 800, 1200}, M, 170);
        CHECK(tall.h == 910); CHECK(tall.y == 170);
        // Wider than the monitor: width capped, pulled to the left edge.
        auto wide = arrangeFit({-100, 200, 2500, 600}, M, 170);
        CHECK(wide.w == 1920 && wide.x == 0);
        // Hanging off the right edge: pulled back so its right edge is the monitor's.
        auto rp = arrangeFit({1500, 300, 800, 600}, M, 170);
        CHECK(rp.x == 1120 && rp.right() == 1920);
        // Hanging off the left edge.
        auto lp = arrangeFit({-300, 300, 800, 600}, M, 170);
        CHECK(lp.x == 0);
        // Hanging off the bottom: pulled up so its bottom is the monitor's.
        auto bp = arrangeFit({100, 700, 800, 600}, M, 170);
        CHECK(bp.y == 480 && bp.bottom() == 1080);
        // No strip at all: a full-monitor window is already fine.
        CHECK(arrangeFit({0, 0, 1920, 1080}, M, 0) == (IntRect{0, 0, 1920, 1080}));
        // Strip swallowing the whole monitor (and beyond): window untouched.
        CHECK(arrangeFit({100, 50, 800, 600}, M, 5000) == (IntRect{100, 50, 800, 600}));
        // Second monitor to the right: its own x origin is respected.
        const IntRect M2{1920, 0, 1920, 1080};
        auto m2 = arrangeFit({2000, 100, 900, 700}, M2, 170);
        CHECK(m2.x == 2000 && m2.y == 170 && m2.w == 900 && m2.h == 700);
        // Monitor at negative coordinates (left of the primary): same rules.
        const IntRect M3{-1920, 0, 1920, 1080};
        auto m3 = arrangeFit({-1900, 20, 1000, 500}, M3, 170);
        CHECK(m3.x == -1900 && m3.y == 170);
        // --- maximized windows (v0.21.4): plan + restore plan ---
        {
            const IntRect zoomedRect{ 0, 0, 1920, 1080 };  // a maximized window's live rect
            const IntRect normalRect{ 100, 50, 800, 600 }; // its rcNormalPosition — pokes under the strip
            auto zp = planArrangeWindow(true, false, true, zoomedRect, normalRect);
            CHECK(zp.participate && zp.restoreFirst && zp.fitInput == normalRect); // NORMAL rect, never the zoomed one
            CHECK(arrangeFit(zp.fitInput, M, 170).y == 170);                       // fitted clear of the strip
            auto np = planArrangeWindow(true, false, false, normalRect, normalRect);
            CHECK(np.participate && !np.restoreFirst && np.fitInput == normalRect);
            CHECK(!planArrangeWindow(false, false, true, zoomedRect, normalRect).participate); // invisible stays skipped
            CHECK(!planArrangeWindow(true, true, true, zoomedRect, normalRect).participate);   // iconic stays skipped
            CHECK(!planArrangeWindow(true, false, true, zoomedRect, IntRect{ 0, 0, 0, 0 }).participate); // degenerate normal rect: left alone
            auto rp = planRestoreWindow(true);
            CHECK(rp.applyRect && rp.remaximize);   // was maximized -> re-maximize after the rect
            auto rp2 = planRestoreWindow(false);
            CHECK(rp2.applyRect && !rp2.remaximize);
        }
        // Exactly the free area: unchanged.
        CHECK(arrangeFit({0, 170, 1920, 910}, M, 170) == (IntRect{0, 170, 1920, 910}));
        // Degenerate monitor: unchanged rather than garbage.
        CHECK(arrangeFit({100, 50, 800, 600}, {0, 0, 0, 0}, 170) == (IntRect{100, 50, 800, 600}));
        // Negative strip clamps to zero.
        CHECK(arrangeFit({0, 0, 500, 500}, M, -50) == (IntRect{0, 0, 500, 500}));
    }
    // --- Reserve-screen-space default: OFF in the recipe (opt-in feature)
    {
        PresetState p; CHECK(p.reserveScreenSpace == false);
    }
    // === v0.18.1 (API converted to the v0.19.1 edge-aware form; the
    //     scenarios are the originals, played on the TOP edge) ===
    // --- Reserve state machine (the decision layer behind the AppBar):
    //     transitions only, with a 2 px dock / 8 px undock hysteresis
    //     band. The v0.18 design re-synced from a ~4x/sec timer poll and
    //     flapped the reservation (and every maximised window with it);
    //     these checks pin the behaviour that makes flapping impossible.
    {
        using RA = ReserveAction;
        const IntRect mon{0, 0, 1920, 1080};
        const IntRect bar{0, 0, 1920, 120};
        auto gapsTop = [](int top) { return EdgeGaps{ top, 900, 900, 900 }; };
        ReserveMachine m;
        CHECK(m.reserved() == false);
        // Toggle off: never any action, however docked the bar is.
        CHECK(m.update(false, gapsTop(0), bar, mon, 1) == RA::None);
        CHECK(m.update(false, gapsTop(0), bar, mon, 1) == RA::None);
        // Toggle on but floating well off the top: nothing.
        CHECK(m.update(true, gapsTop(40), bar, mon, 1) == RA::None);
        // The dead zone does not dock: 3..8 px away is not docked.
        CHECK(m.update(true, gapsTop(5), bar, mon, 1) == RA::None);
        CHECK(m.update(true, gapsTop(8), bar, mon, 1) == RA::None);
        CHECK(m.reserved() == false);
        // Dock -> Register, exactly once; identical repeats are None.
        CHECK(m.update(true, gapsTop(2), bar, mon, 1) == RA::Register);
        CHECK(m.reserved() == true);
        CHECK(m.update(true, gapsTop(2), bar, mon, 1) == RA::None);
        CHECK(m.update(true, gapsTop(0), bar, mon, 1) == RA::None);
        // Jitter inside the hysteresis band: no state change, no action,
        // however long it wobbles (the v0.18 flicker scenario).
        for (int gap : { 3, 8, 5, 2, 7, 4, 8, 3 })
            CHECK(m.update(true, gapsTop(gap), bar, mon, 1) == RA::None);
        CHECK(m.reserved() == true);
        // A real geometry change while reserved: exactly one Update.
        const IntRect taller{0, 0, 1920, 160};
        CHECK(m.update(true, gapsTop(0), taller, mon, 1) == RA::Update);
        CHECK(m.update(true, gapsTop(0), taller, mon, 1) == RA::None);
        // Moving to another monitor: exactly one Update.
        CHECK(m.update(true, gapsTop(0), taller, mon, 2) == RA::Update);
        CHECK(m.update(true, gapsTop(0), taller, mon, 2) == RA::None);
        // Dragged clear of the top (past the undock threshold): Release.
        CHECK(m.update(true, gapsTop(9), taller, mon, 2) == RA::Release);
        CHECK(m.reserved() == false);
        // Sitting in the dead zone after release does NOT re-register.
        CHECK(m.update(true, gapsTop(5), taller, mon, 2) == RA::None);
        // Docking again works, and toggle-off then releases exactly once.
        CHECK(m.update(true, gapsTop(1), taller, mon, 2) == RA::Register);
        CHECK(m.update(false, gapsTop(1), taller, mon, 2) == RA::Release);
        CHECK(m.update(false, gapsTop(1), taller, mon, 2) == RA::None);
        // A degenerate bar rect never registers.
        ReserveMachine m2;
        CHECK(m2.update(true, gapsTop(0), IntRect{0, 0, 0, 0}, mon, 1) == RA::None);
        CHECK(m2.reserved() == false);
    }
    // === v0.19.1 ===
    // --- Edge-aware reserve: the bar may dock to ANY edge (Jackson's
    //     bar lives at the BOTTOM; v0.18/18.1 reserved the top only).
    //     Same machine, same hysteresis, judged per registered edge.
    {
        using RA = ReserveAction;
        using RE = ReserveEdge;
        const IntRect mon{0, 0, 1920, 1080};
        const IntRect barTop{0, 0, 1920, 120};
        const IntRect barBottom{0, 960, 1920, 120};
        const IntRect barLeft{0, 100, 120, 800};
        const IntRect barRight{1800, 100, 120, 800};
        auto gapsFor = [](int top, int bottom, int left, int right) {
            return EdgeGaps{ top, bottom, left, right };
        };
        // Gaps as the caller computes them for each docked bar (a
        // full-width bar is also flush with left/right — value 960 etc.
        // is the far-edge distance on the strip axis).
        const EdgeGaps gTop = gapsFor(0, 960, 0, 0);
        const EdgeGaps gBottom = gapsFor(960, 0, 0, 0);
        const EdgeGaps gLeft = gapsFor(100, 180, 0, 1800);
        const EdgeGaps gRight = gapsFor(100, 180, 1800, 0);

        // stripRect: the full edge strip, flush on each edge.
        CHECK(ReserveMachine::stripRect(RE::Top, barTop, mon) == (IntRect{0, 0, 1920, 120}));
        CHECK(ReserveMachine::stripRect(RE::Bottom, barBottom, mon) == (IntRect{0, 960, 1920, 120}));
        CHECK(ReserveMachine::stripRect(RE::Left, barLeft, mon) == (IntRect{0, 0, 120, 1080}));
        CHECK(ReserveMachine::stripRect(RE::Right, barRight, mon) == (IntRect{1800, 0, 120, 1080}));
        // Floating inside the dead zone, the strip still reaches the
        // bar's far edge (thickness = gap + bar size).
        CHECK(ReserveMachine::stripRect(RE::Top, IntRect{0, 5, 1920, 120}, mon) == (IntRect{0, 0, 1920, 125}));
        CHECK(ReserveMachine::stripRect(RE::Bottom, IntRect{0, 955, 1920, 120}, mon) == (IntRect{0, 955, 1920, 125}));

        // BOTTOM docking (Jackson's actual layout).
        ReserveMachine mb;
        CHECK(mb.update(true, gapsFor(960, 5, 0, 0), barBottom, mon, 1) == RA::None);   // dead zone does not dock
        CHECK(mb.update(true, gBottom, barBottom, mon, 1) == RA::Register);
        CHECK(mb.reserved() == true);
        CHECK(mb.edge() == RE::Bottom);
        CHECK(mb.requestedRect() == (IntRect{0, 960, 1920, 120}));
        CHECK(mb.update(true, gBottom, barBottom, mon, 1) == RA::None);                  // repeats are silent
        CHECK(mb.update(true, gapsFor(955, 5, 0, 0), barBottom, mon, 1) == RA::None);    // dead-zone wobble held
        // Bar grows taller while bottom-docked: exactly one Update,
        // and the requested strip follows the bar's new top edge.
        const IntRect barBottomTall{0, 920, 1920, 160};
        CHECK(mb.update(true, gapsFor(920, 0, 0, 0), barBottomTall, mon, 1) == RA::Update);
        CHECK(mb.requestedRect() == (IntRect{0, 920, 1920, 160}));
        CHECK(mb.update(true, gapsFor(920, 0, 0, 0), barBottomTall, mon, 1) == RA::None);
        // Dragged clear of the bottom (past 8 px, no other edge near):
        // Release, exactly once.
        CHECK(mb.update(true, gapsFor(900, 60, 60, 60), barBottomTall, mon, 1) == RA::Release);
        CHECK(mb.reserved() == false);

        // LEFT and RIGHT docking.
        ReserveMachine ml;
        CHECK(ml.update(true, gLeft, barLeft, mon, 1) == RA::Register);
        CHECK(ml.edge() == RE::Left);
        CHECK(ml.requestedRect() == (IntRect{0, 0, 120, 1080}));
        CHECK(ml.update(false, gLeft, barLeft, mon, 1) == RA::Release);  // toggle-off from a side edge
        CHECK(ml.update(false, gLeft, barLeft, mon, 1) == RA::None);
        ReserveMachine mr;
        CHECK(mr.update(true, gRight, barRight, mon, 1) == RA::Register);
        CHECK(mr.edge() == RE::Right);
        CHECK(mr.requestedRect() == (IntRect{1800, 0, 120, 1080}));

        // Nearest edge wins when several are plausible; orientation
        // breaks the flush-corner ties (wide bar -> its top/bottom).
        ReserveMachine mn;
        CHECK(mn.update(true, gapsFor(30, 0, 400, 400), barBottom, mon, 1) == RA::Register);
        CHECK(mn.edge() == RE::Bottom);
        ReserveMachine mt;
        CHECK(mt.update(true, gTop, barTop, mon, 1) == RA::Register);
        CHECK(mt.edge() == RE::Top);   // NOT Left/Right, though a full-width bar is flush there too
        // A tall bar flush with the top-left corner is a LEFT bar.
        ReserveMachine mc;
        const IntRect barTallLeft{0, 0, 120, 1080};
        CHECK(mc.update(true, gapsFor(0, 0, 0, 1800), barTallLeft, mon, 1) == RA::Register);
        CHECK(mc.edge() == RE::Left);

        // EDGE SWITCH: bottom-docked bar dragged straight to the top
        // edge — one Switch (the caller performs exactly one Release
        // of the old edge + one Register of the new), then silence.
        ReserveMachine ms;
        CHECK(ms.update(true, gBottom, barBottom, mon, 1) == RA::Register);
        CHECK(ms.update(true, gTop, barTop, mon, 1) == RA::Switch);
        CHECK(ms.reserved() == true);
        CHECK(ms.edge() == RE::Top);
        CHECK(ms.requestedRect() == (IntRect{0, 0, 1920, 120}));
        CHECK(ms.update(true, gTop, barTop, mon, 1) == RA::None);
        CHECK(ms.update(true, gTop, barTop, mon, 1) == RA::None);
        CHECK(ms.update(false, gTop, barTop, mon, 1) == RA::Release);
        CHECK(ms.update(false, gTop, barTop, mon, 1) == RA::None);
    }
    // --- arrangeFit on the other three edges (the Top outcomes are
    //     graded by the v0.18 block above; the 3-arg form IS Top).
    {
        using RE = ReserveEdge;
        const IntRect M{0, 0, 1920, 1080};
        CHECK(arrangeFit({100, 300, 800, 600}, M, 170) == arrangeFit({100, 300, 800, 600}, M, RE::Top, 170));
        // BOTTOM strip 120 -> free {0,0,1920,960}.
        CHECK(arrangeFit({100, 300, 800, 600}, M, RE::Bottom, 120) == (IntRect{100, 300, 800, 600})); // already clear
        auto bb = arrangeFit({100, 1000, 800, 100}, M, RE::Bottom, 120);
        CHECK(bb.y == 860 && bb.bottom() == 960);                                                     // poking under: pulled up
        auto bt = arrangeFit({0, 0, 800, 1200}, M, RE::Bottom, 120);
        CHECK(bt.h == 960 && bt.y == 0);                                                              // taller than free: capped
        CHECK(arrangeFit({0, 0, 1920, 960}, M, RE::Bottom, 120) == (IntRect{0, 0, 1920, 960}));        // exact free area
        CHECK(arrangeFit({100, 50, 800, 600}, M, RE::Bottom, 5000) == (IntRect{100, 50, 800, 600}));   // strip overflow: untouched
        // LEFT strip 120 -> free {120,0,1800,1080}.
        auto lf = arrangeFit({50, 100, 800, 600}, M, RE::Left, 120);
        CHECK(lf.x == 120 && lf.w == 800);                                                            // poking under: pushed right
        CHECK(arrangeFit({300, 100, 800, 600}, M, RE::Left, 120) == (IntRect{300, 100, 800, 600}));   // already clear
        auto lw = arrangeFit({0, 0, 2500, 600}, M, RE::Left, 120);
        CHECK(lw.w == 1800 && lw.x == 120);                                                           // wider than free: capped
        // RIGHT strip 120 -> free {0,0,1800,1080}.
        auto rt = arrangeFit({1500, 100, 800, 600}, M, RE::Right, 120);
        CHECK(rt.x == 1000 && rt.right() == 1800);                                                    // poking under: pulled left
        CHECK(arrangeFit({100, 100, 800, 600}, M, RE::Right, 120) == (IntRect{100, 100, 800, 600}));  // already clear
        // Second monitor at negative coordinates, bottom edge.
        const IntRect M3{-1920, 0, 1920, 1080};
        auto m3 = arrangeFit({-1900, 1000, 1000, 100}, M3, RE::Bottom, 120);
        CHECK(m3.x == -1900 && m3.bottom() == 960);
    }
    // === v0.19 ===
    // --- SemVer (the updater's version compare): NUMERIC ordering —
    //     as raw strings "0.9.0" would sort after "0.19.0" and the
    //     updater would miss (or reverse) real updates.
    {
        SemVer a, b;
        CHECK(parseSemVer("0.19.0", a) && a.major == 0 && a.minor == 19 && a.patch == 0);
        CHECK(parseSemVer("v0.19.0", b) && a == b);              // GitHub tag form == bare form
        CHECK(parseSemVer("0.19", b) && a == b);                 // missing patch = 0
        CHECK(parseSemVer("1", b) && b.major == 1 && b.minor == 0 && b.patch == 0);
        CHECK(!parseSemVer("", b)); CHECK(!parseSemVer("abc", b)); CHECK(!parseSemVer("1.2.3.4", b));
        CHECK(!parseSemVer("1..2", b)); CHECK(!parseSemVer("1.", b));
        CHECK(semVerIsNewer("0.19.0", "0.18.1"));                // this version over the last
        CHECK(semVerIsNewer("0.19.0", "0.9.0"));                 // the string-compare trap: 19 > 9 numerically
        CHECK(!semVerIsNewer("0.9.0", "0.19.0"));                // ...and never the reverse
        CHECK(!semVerIsNewer("0.19.0", "0.19.0"));               // equal is not newer
        CHECK(semVerIsNewer("1.0.0", "0.99.99"));                // major dominates
        CHECK(semVerIsNewer("v0.20.0", "0.19.9"));               // tags compare against bare versions
        CHECK(!semVerIsNewer("garbage", "0.19.0"));              // unparseable is never an update
        CHECK(compareSemVer(SemVer{0, 19, 0}, SemVer{0, 19, 0}) == 0);
        CHECK(compareSemVer(SemVer{0, 18, 1}, SemVer{0, 19, 0}) == -1);
        CHECK(compareSemVer(SemVer{2, 0, 0}, SemVer{1, 9, 9}) == 1);
    }
    // === v0.19.2 ===
    // --- ReleaseParse (the updater's GitHub-release field extraction):
    //     tag, notes (fully unescaped), and ONLY the MeterBar-Setup.exe
    //     asset's URL/size/digest — a neighbouring asset's fields must
    //     never bleed into it.
    {
        const std::string doc = R"JSON({"url":"https://api.github.com/repos/x/y/releases/1","tag_name":"v0.20.0","name":"MeterBar v0.20.0","draft":false,"body":"Line one\nLine two with \"quotes\" and a \\ backslash.\n\n- bullet","assets":[{"url":"https://api.github.com/a/1","name":"notes.txt","size":12,"browser_download_url":"https://example.com/notes.txt"},{"url":"https://api.github.com/a/2","name":"MeterBar-Setup.exe","size":3456789,"digest":"sha256:0123abcdef","browser_download_url":"https://example.com/MeterBar-Setup.exe"}]})JSON";
        ReleaseFields f;
        CHECK(parseReleaseJson(doc, f));
        CHECK(f.tag == "v0.20.0");
        CHECK(f.notes == std::string("Line one\nLine two with \"quotes\" and a \\ backslash.\n\n- bullet"));
        CHECK(f.hasAsset);
        CHECK(f.assetSize == 3456789);
        CHECK(f.assetDigest == "sha256:0123abcdef");
        CHECK(f.assetUrl == "https://example.com/MeterBar-Setup.exe"); // not notes.txt's URL
        // body null (a release with no notes) parses, with empty notes:
        ReleaseFields g;
        CHECK(parseReleaseJson(R"JSON({"tag_name":"v1.0.0","body":null,"assets":[]})JSON", g));
        CHECK(g.notes.empty() && !g.hasAsset);
        // \u escapes become UTF-8 (é = U+00E9):
        ReleaseFields h;
        CHECK(parseReleaseJson(R"JSON({"tag_name":"v1.0.1","body":"caf\u00e9","assets":[]})JSON", h));
        CHECK(h.notes == std::string("caf\xc3\xa9"));
        // no usable tag -> no parse; garbage -> no parse:
        ReleaseFields bad;
        CHECK(!parseReleaseJson(R"JSON({"body":"x","assets":[]})JSON", bad));
        CHECK(!parseReleaseJson("this is not json", bad));
        CHECK(!parseReleaseJson("", bad));
    }
    // --- httpStatusBlocksAuth (the private-repo signal): GitHub shows
    //     a private repo to unauthenticated callers as 401/403/404 —
    //     anything else is a different kind of failure.
    {
        CHECK(httpStatusBlocksAuth(401));
        CHECK(httpStatusBlocksAuth(403));
        CHECK(httpStatusBlocksAuth(404));
        CHECK(!httpStatusBlocksAuth(200));
        CHECK(!httpStatusBlocksAuth(301));
        CHECK(!httpStatusBlocksAuth(500));
        CHECK(!httpStatusBlocksAuth(0));
    }
    // ---------- v0.20.0 (i): exact spectrum mapping — binAtX ----------
    {
        const int nb = 4097, fft = 8192; const double sr = 48000.0;
        for (auto scale : { FreqScale::Log, FreqScale::Mel, FreqScale::Linear }) {
            // left edge: the walk returns the LAST bin whose own freqToX
            // position is <= frac (the spectrum view's exact LUT semantics);
            // bins below minF clamp to x=0, so at frac 0 the next bin up
            // must be the first with a positive position.
            const int b0 = SpectrumAnalyser::binAtX(0.f, nb, fft, sr, 20.0, 20000.0, scale);
            CHECK(b0 >= 1
                  && SpectrumAnalyser::freqToX(SpectrumAnalyser::binFreq(b0, fft, sr), 20.0, 20000.0, scale) <= 0.f
                  && SpectrumAnalyser::freqToX(SpectrumAnalyser::binFreq(b0 + 1, fft, sr), 20.0, 20000.0, scale) > 0.f);
            CHECK(SpectrumAnalyser::binAtX(1.f, nb, fft, sr, 20.0, 20000.0, scale) == nb - 1);   // right edge: last bin
            bool mono = true; int prev = 1;
            for (int i = 0; i <= 100; ++i) {
                int b = SpectrumAnalyser::binAtX(i / 100.f, nb, fft, sr, 20.0, 20000.0, scale);
                if (b < prev) mono = false;
                prev = b;
            }
            CHECK(mono); // never walks backwards
            // round-trip: the position OF bin k selects bin k
            for (int k : { 10, 100, 500, 2000, 3000 }) { // all within [minF, maxF]
                float fx = SpectrumAnalyser::freqToX(SpectrumAnalyser::binFreq(k, fft, sr), 20.0, 20000.0, scale);
                CHECK(SpectrumAnalyser::binAtX(fx, nb, fft, sr, 20.0, 20000.0, scale) == k);
            }
        }
    }
    // ---------- v0.20.0 (i): scope 3-band split ----------
    {
        const double sr = 48000.0; const int n = 8192;
        auto sine = [&](double hz) {
            std::vector<float> x((size_t)n);
            for (int i = 0; i < n; ++i) x[(size_t)i] = (float)std::sin(2.0 * 3.141592653589793 * hz * i / sr);
            return x;
        };
        auto rms = [](const std::vector<float>& v, int skip) {
            double s2 = 0; int c = 0;
            for (size_t i = (size_t)skip; i < v.size(); ++i) { s2 += (double)v[i] * v[i]; ++c; }
            return std::sqrt(s2 / std::max(1, c));
        };
        auto lo = splitScopeBands(sine(100.0), sr);
        CHECK(rms(lo.low, 1024) > rms(lo.mid, 1024) && rms(lo.low, 1024) > rms(lo.high, 1024));   // 100 Hz lives in LOW
        auto md = splitScopeBands(sine(1000.0), sr);
        CHECK(rms(md.mid, 1024) > rms(md.low, 1024) && rms(md.mid, 1024) > rms(md.high, 1024));   // 1 kHz lives in MID
        auto hi = splitScopeBands(sine(8000.0), sr);
        CHECK(rms(hi.high, 1024) > rms(hi.low, 1024) && rms(hi.high, 1024) > rms(hi.mid, 1024)); // 8 kHz lives in HIGH
        // bands sum back to the input exactly (construction guarantee)
        auto mix = sine(440.0);
        auto sp = splitScopeBands(mix, sr);
        double maxErr = 0;
        for (int i = 0; i < n; ++i)
            maxErr = std::max(maxErr, (double)std::abs(sp.low[(size_t)i] + sp.mid[(size_t)i] + sp.high[(size_t)i] - mix[(size_t)i]));
        CHECK(maxErr < 1e-5);
        CHECK(splitScopeBands({}, sr).low.empty());
    }
    // ---------- v0.20.0 (l): visualizer model ----------
    {
        VisualizerSettings vs; // defaults
        CHECK(vs.enabled == false);                       // the toggle ships OFF
        CHECK(vs.particleCap == 600);
        VisualizerEngine eng;
        const float silence[3] = { 0, 0, 0 };
        for (int i = 0; i < 30; ++i) eng.update(1.f / 30.f, silence, vs);
        CHECK((int)eng.particles().size() == VisualizerEngine::kAmbientTarget); // ambient population present
        CHECK(eng.burstCount() == 0);                     // silence never bursts
        bool allAmbient = true;
        for (auto& pt : eng.particles()) if (pt.burst) allAmbient = false;
        CHECK(allAmbient);
        // convergence: individual particles move INWARD (the population
        // mean is stationary by design — converged particles respawn at
        // the rim, so the flow is continuous). Snapshot the cohort after
        // one frame; 1 s later, of those not yet respawned, most must be
        // closer to the centre than they started.
        {
            VisualizerEngine eng2;
            eng2.update(1.f / 30.f, silence, vs);
            std::vector<double> start;
            for (auto& pt : eng2.particles()) start.push_back(std::hypot(pt.x - 0.5, pt.y - 0.5));
            for (int i = 0; i < 30; ++i) eng2.update(1.f / 30.f, silence, vs);
            int alive = 0, inward = 0;
            for (size_t i = 0; i < start.size() && i < eng2.particles().size(); ++i) {
                auto& pt = eng2.particles()[i];
                if (pt.burst || pt.age < 0.9f) continue; // respawned this second: skip
                ++alive;
                if (std::hypot(pt.x - 0.5, pt.y - 0.5) < start[i]) ++inward;
            }
            CHECK(alive > 50);
            CHECK(inward > alive * 4 / 5);
        }
        // a loud low band fires bursts; the cap is never exceeded, ever
        const float loud[3] = { 0.9f, 0, 0 };
        for (int i = 0; i < 600; ++i) {
            eng.update(1.f / 30.f, loud, vs);
            CHECK((int)eng.particles().size() <= vs.particleCap);
        }
        CHECK(eng.burstCount() > 0);
        // band energies from a synthetic spectrum: hot 100 Hz bin -> low band leads
        std::vector<float> db(4097, -120.f);
        for (int b = 1; b <= 42; ++b) db[(size_t)b] = -8.f; // the whole <250 Hz band hot (8192/48k)
        auto e = spectrumBandEnergies(db, 8192, 48000.0);
        CHECK(e[0] > 0.5f && e[0] > e[1] && e[0] > e[2]);
        CHECK(spectrumBandEnergies({}, 8192, 48000.0)[0] == 0.f);
        // palette: saturation 0 = grey; ambient is always white-ish
        VisualizerSettings grey = vs; grey.saturation = 0.f;
        VizParticle bp; bp.burst = true; bp.hueT = 0.3f; bp.life = 1.f;
        auto gc = eng.colorFor(bp, grey);
        CHECK(gc.r == gc.g && gc.g == gc.b);
        VizParticle ap; ap.burst = false; ap.age = 1.f;
        auto ac = eng.colorFor(ap, vs);
        CHECK(ac.r == ac.g && ac.g == ac.b); // ambient stays white at any palette setting
        // hue wraps: baseHue near 1 + spread still yields a saturated colour
        VisualizerSettings wrap = vs; wrap.baseHue = 0.97f;
        auto wc = eng.colorFor(bp, wrap);
        CHECK(!(wc.r == wc.g && wc.g == wc.b));
    }
    // ---------- v0.25.0: distributed emitters + Field style + BLK5 ----------
    {
        VisualizerSettings vs;
        // burst birthplaces follow the band's field position: low
        // left, mid centre, high right (the centre is a coordinate
        // reference, not a spawn point — Jackson, 2026-11-01)
        auto burstMeanX = [&](int band, float balance, int frames) {
            VisualizerEngine e2;
            float hot[3] = { 0, 0, 0 }; hot[(size_t)band] = 0.95f;
            for (int i = 0; i < frames; ++i) e2.update(1.f / 30.f, hot, vs, balance);
            double sx = 0; int n = 0;
            for (auto& p : e2.particles()) if (p.burst) { sx += p.x; ++n; }
            CHECK(n > 0);
            return n ? sx / n : 0.5;
        };
        const double lowX = burstMeanX(0, 0.f, 3), midX = burstMeanX(1, 0.f, 3), highX = burstMeanX(2, 0.f, 3);
        CHECK(lowX < 0.42);
        CHECK(midX > 0.40 && midX < 0.60);
        CHECK(highX > 0.58);
        CHECK(lowX < midX && midX < highX);
        // stereo balance shifts the same band's births sideways
        CHECK(burstMeanX(1, 0.9f, 10) > burstMeanX(1, -0.9f, 10) + 0.05);
        // the mapping itself: ordered bands, balance clamped in-field
        CHECK(vizBandFieldX(0, 0.f) < vizBandFieldX(1, 0.f) && vizBandFieldX(1, 0.f) < vizBandFieldX(2, 0.f));
        CHECK(vizBandFieldX(2, 1.f) <= 0.94f && vizBandFieldX(0, -1.f) >= 0.06f);
        // Field style: population fills to its target and never
        // exceeds it; the gate opens with music; vortices wander
        VisualizerField fld;
        const float loud[3] = { 0.8f, 0.6f, 0.4f };
        for (int i = 0; i < 120; ++i) { fld.update(1.f / 30.f, loud, vs); CHECK((int)fld.particles().size() <= VisualizerField::kFieldTarget); }
        CHECK((int)fld.particles().size() == VisualizerField::kFieldTarget);
        CHECK(fld.visibility() == 1.f);
        float wx0, wy0, wx1, wy1;
        fld.vortexCentre(0, wx0, wy0);
        for (int i = 0; i < 600; ++i) fld.update(1.f / 30.f, loud, vs); // +20 s
        fld.vortexCentre(0, wx1, wy1);
        CHECK(std::hypot(wx1 - wx0, wy1 - wy0) > 0.05);
        // silence: the gate closes completely, like the other styles
        VisualizerField quiet;
        const float sil[3] = { 0, 0, 0 };
        for (int i = 0; i < 600; ++i) quiet.update(1.f / 30.f, sil, vs);
        CHECK(quiet.visibility() == 0.f);
        // BLK5: the magic moved with the widened domain, and the
        // stereo balance rides the audio packet at [4]
        CHECK(kLinkMagic == 0x424C4B35u);
        LinkSlot slot; slot.appId = 1; slot.heartbeatMs = 1000; slot.audioSeq = 3;
        slot.audioLow = 0.1f; slot.audioMid = 0.2f; slot.audioHigh = 0.3f; slot.audioLevel = 0.4f; slot.audioBalance = 0.7f;
        float pkt[5] = { 0, 0, 0, 0, 0 };
        CHECK(audioPacket(slot, 1000, pkt));
        CHECK(pkt[0] == 0.1f && pkt[3] == 0.4f && pkt[4] == 0.7f);
    }
    // ---------- v0.21.5: calmed whites + Lines style ----------
    {
        VisualizerSettings vs;
        VisualizerEngine eng;
        const float loudMid[3] = { 0, 0.9f, 0 };
        for (int i = 0; i < 90; ++i) eng.update(1.f / 30.f, loudMid, vs); // 3 s of music: gate fully open
        CHECK(eng.ambientVisibility() == 1.f);
        // the whites' peak is capped: no ambient particle renders
        // brighter than kAmbientPeak of full white (they used to
        // sit at a full 255 — the loudest discs in the bar)
        int peak = 0; bool sawAmbient = false;
        for (auto& pt : eng.particles()) {
            if (pt.burst) continue;
            sawAmbient = true;
            auto c = eng.colorFor(pt, vs);
            if ((int)c.r > peak) peak = (int)c.r;
        }
        CHECK(sawAmbient);
        CHECK(peak <= (int)std::lround(255.f * VisualizerEngine::kAmbientPeak) + 1);
        CHECK(peak > 60); // still present, not deleted
        // and they fade OUT at end of life, like the burst dots:
        // a white at 95% of its life is dimmer than at mid-life
        VizParticle mid; mid.burst = false; mid.age = 2.f; mid.life = 4.f;
        VizParticle old; old.burst = false; old.age = 3.8f; old.life = 4.f;
        CHECK(eng.colorFor(old, vs).r < eng.colorFor(mid, vs).r);

        // Lines: silence = still, thin, invisible
        VisualizerLines lines;
        const float silence[3] = { 0, 0, 0 };
        for (int i = 0; i < 60; ++i) lines.update(1.f / 30.f, silence);
        CHECK(lines.visibility() == 0.f);
        CHECK(lines.level(1) == 0.f);
        CHECK(lines.halfThick(1, 24) <= 0.0045f); // hairline at rest (0.004 x breathe <= 1)
        bool inRange = true;
        for (int b = 0; b < 3; ++b)
            for (int i = 0; i < VisualizerLines::kPoints; ++i) {
                float y = lines.centreY(b, i);
                if (!(y > 0.f && y < 1.f)) inRange = false;
            }
        CHECK(inRange);
        // a loud band swells its ribbon: the level springs up, the
        // thickness grows, the silence gate opens
        const float loudLow[3] = { 0.95f, 0, 0 };
        const float thickRest = lines.halfThick(0, 24);
        for (int i = 0; i < 60; ++i) lines.update(1.f / 30.f, loudLow);
        CHECK(lines.level(0) > 0.5f);
        CHECK(lines.level(1) == 0.f); // other bands untouched
        CHECK(lines.halfThick(0, 24) > thickRest * 2.f);
        CHECK(lines.visibility() == 1.f);
        // spring, not a copy: one frame of loud must NOT slam the
        // level to target (the raw-copy jitter this style avoids)
        VisualizerLines one;
        one.update(1.f / 30.f, loudLow);
        CHECK(one.level(0) < 0.2f);
        // geometry stays in the unit square even at full level
        for (int i = 0; i < 30; ++i) lines.update(1.f / 30.f, loudLow);
        inRange = true;
        for (int b = 0; b < 3; ++b)
            for (int i = 0; i < VisualizerLines::kPoints; ++i) {
                float y = lines.centreY(b, i);
                if (!(y > 0.f && y < 1.f)) inRange = false;
            }
        CHECK(inRange);
    }

// ---- BarLink protocol geometry (v0.2.0, item k) ----
{
    using namespace meterbar;
    LinkShared sh; const long long now = 100000;
    auto& peer = sh.slots[1]; peer.appId = 1; peer.linked = 1; peer.heartbeatMs = now; peer.rect = { 100, 0, 800, 40 };
    // snap: left edge 6 px from the peer's left edge aligns exactly
    auto s1 = snapToPeers({ 106, 300, 500, 40 }, sh, 0, now);
    CHECK(s1.x == 100);
    // snap: top edge 8 px above the peer's bottom stacks flush
    auto s2 = snapToPeers({ 400, 48, 500, 40 }, sh, 0, now);
    CHECK(s2.y == 40);
    // beyond the threshold: untouched
    auto s3 = snapToPeers({ 130, 300, 500, 40 }, sh, 0, now);
    CHECK(s3.x == 130 && s3.y == 300);
    // unlinked or stale peers never snap
    peer.linked = 0; CHECK(snapToPeers({ 106, 300, 500, 40 }, sh, 0, now).x == 106);
    peer.linked = 1; peer.heartbeatMs = now - kPeerStaleMs - 1;
    CHECK(snapToPeers({ 106, 300, 500, 40 }, sh, 0, now).x == 106);
    // never snap to self
    peer.heartbeatMs = now; peer.appId = 0;
    CHECK(snapToPeers({ 106, 300, 500, 40 }, sh, 0, now).x == 106);
    // follow: pure translation, size preserved
    auto f = followRect({ 50, 500, 300, 40 }, { 0, 0, 800, 40 }, { 25, -10, 800, 40 });
    CHECK(f.x == 75 && f.y == 490 && f.w == 300 && f.h == 40);
    // group dock stacking on all four edges
    IntRect lead{ 0, 0, 800, 40 }, fol{ 200, 300, 600, 36 };
    CHECK(groupDockRect(lead, fol, ReserveEdge::Top).y == 40);
    IntRect leadB{ 0, 1040, 800, 40 };
    CHECK(groupDockRect(leadB, fol, ReserveEdge::Bottom).y == 1040 - 36);
    CHECK(groupDockRect(lead, fol, ReserveEdge::Left).x == 800);
    IntRect leadR{ 1120, 0, 800, 40 };
    CHECK(groupDockRect(leadR, fol, ReserveEdge::Right).x == 1120 - 600);
    // v0.2.1: the cross axis ALIGNS to the leader (field fix — a
    // follower stacked under a sideways-moved peer used to keep its
    // own x and no longer sat under the peer). Vertical stacks align
    // left edges; horizontal stacks align top edges.
    IntRect leadM{ 350, 0, 800, 40 };
    CHECK(groupDockRect(leadM, fol, ReserveEdge::Top).x == 350);
    IntRect leadMB{ 350, 1040, 800, 40 };
    auto gdB = groupDockRect(leadMB, fol, ReserveEdge::Bottom);
    CHECK(gdB.y == 1040 - 36 && gdB.x == 350);
    IntRect leadL{ 0, 120, 40, 800 };
    auto gdL = groupDockRect(leadL, fol, ReserveEdge::Left);
    CHECK(gdL.x == 40 && gdL.y == 120);
    IntRect leadRM{ 1880, 120, 40, 800 };
    auto gdR = groupDockRect(leadRM, fol, ReserveEdge::Right);
    CHECK(gdR.x == 1880 - 600 && gdR.y == 120);
    // size is never touched by stacking
    CHECK(gdB.w == 600 && gdB.h == 36);
    // drag liveness is staleness-based
    LinkSlot d; d.dragSeq = 3; d.dragStampMs = now;
    CHECK(dragLive(d, now)); CHECK(!dragLive(d, now + kLeaderStaleMs + 1));
}
// ---- Recordings menu model (v0.21.4) ----
{
    using namespace meterbar;
    CHECK(isRecordingFileName("record-20261031-141500.wav"));
    CHECK(isRecordingFileName("clip10s-3.wav"));
    CHECK(isRecordingFileName("clip60s-12.wav"));
    CHECK(!isRecordingFileName("notes.txt"));
    CHECK(!isRecordingFileName("random.wav"));       // a wav, but not one of ours
    CHECK(!isRecordingFileName("record-2026.mp3"));  // right prefix, wrong format
    std::vector<RecordingFileInfo> files = {
        { "record-20261031-120000.wav", 1000, 5000 },
        { "record-20261031-130000.wav", 3000, 7000 },  // newest take
        { "clip10s-1.wav",              2000, 900  },
        { "clip60s-2.wav",              4000, 3000 },  // newest overall
        { "unrelated.wav",              9000, 100  },  // newest of all, still not listed
        { "record-20261031-140000.wav", 5000, 1000 },  // the take being written right now
    };
    auto plan = planRecordingsMenu(files, "record-20261031-140000.wav");
    CHECK(plan.size() == 4);                            // unrelated + active take excluded
    CHECK(plan[0].name == "clip60s-2.wav" && plan[0].clip);
    CHECK(plan[1].name == "record-20261031-130000.wav" && !plan[1].clip);
    CHECK(plan[2].name == "clip10s-1.wav" && plan[2].clip);
    CHECK(plan[3].name == "record-20261031-120000.wav");
    CHECK(plan[0].sizeBytes == 3000);                   // facts ride through untouched
    // no active recording: the newest take is listed
    CHECK(planRecordingsMenu(files, "").size() == 5);
    // cap: 30 takes -> the 25 newest, oldest dropped
    std::vector<RecordingFileInfo> many;
    for (int i = 0; i < 30; ++i) many.push_back({ "record-x" + std::to_string(i) + ".wav", 100 + i, 1 });
    auto capped = planRecordingsMenu(many, "");
    CHECK(capped.size() == 25);
    CHECK(capped.front().mtimeMs == 129 && capped.back().mtimeMs == 105);
    // empty in, empty out (the app renders "No recordings yet")
    CHECK(planRecordingsMenu({}, "").empty());
}
// ---- Background sync decisions (v0.2.1, item k hardening) ----
{
    using namespace meterbar;
    const long long now = 200000;
    BgSyncState sync;
    // first sight of our own background is a change (nothing published yet)
    CHECK(bgIsLocalChange(sync, 1, 100, 0));
    sync.publishedStyle = 1; sync.publishedPct = 100; sync.publishedEffect = 0;
    CHECK(!bgIsLocalChange(sync, 1, 100, 0));
    CHECK(bgIsLocalChange(sync, 1, 55, 0));  // opacity moved
    CHECK(bgIsLocalChange(sync, 2, 100, 0)); // style moved
    CHECK(bgIsLocalChange(sync, 1, 100, 1)); // effect moved (BLK3)
    // a peer publishes a new background: adopted once
    LinkShared sh;
    auto& peer = sh.slots[1]; peer.appId = 1; peer.linked = 1; peer.heartbeatMs = now;
    peer.bgSeq = 7; peer.bgStyle = 2; peer.bgOpacityPct = 40; peer.bgEffect = 2;
    BgPacket out;
    CHECK(bgAdoption(sh, 0, sync, now, out));
    CHECK(out.style == 2 && out.opacityPct == 40 && out.effect == 2);
    // the adopted values are now OUR published values: no echo publish
    CHECK(!bgIsLocalChange(sync, 2, 40, 2));
    // the same seq is never adopted twice
    CHECK(!bgAdoption(sh, 0, sync, now, out));
    // a genuinely new seq adopts again
    peer.bgSeq = 8; peer.bgStyle = 0; peer.bgOpacityPct = 90; peer.bgEffect = 1;
    CHECK(bgAdoption(sh, 0, sync, now, out));
    CHECK(out.style == 0 && out.opacityPct == 90 && out.effect == 1);
    // BLK4: effect 3 (visualizer Lines) rides the same packet,
    // adopted verbatim like every other ordinal
    peer.bgSeq = 12; peer.bgEffect = 3;
    CHECK(bgAdoption(sh, 0, sync, now, out));
    CHECK(out.effect == 3);
    CHECK(!bgIsLocalChange(sync, 0, 90, 3)); // adopted = published: no echo
    // unlinked peers, stale peers, seq 0 and self never supply a background
    peer.linked = 0; peer.bgSeq = 9;
    CHECK(!bgAdoption(sh, 0, sync, now, out));
    peer.linked = 1; peer.heartbeatMs = now - kPeerStaleMs - 1;
    CHECK(!bgAdoption(sh, 0, sync, now, out));
    peer.heartbeatMs = now; peer.bgSeq = 0;
    CHECK(!bgAdoption(sh, 0, sync, now, out));
    peer.bgSeq = 11; peer.appId = 0; // the peer slot carries OUR id: self, ignored
    CHECK(!bgAdoption(sh, 0, sync, now, out));
    // two peers changed at once: the LAST in slot order wins, both are marked seen
    LinkShared sh2; BgSyncState sync2;
    auto& p1 = sh2.slots[1]; p1.appId = 1; p1.linked = 1; p1.heartbeatMs = now; p1.bgSeq = 3; p1.bgStyle = 1; p1.bgOpacityPct = 70;
    auto& p2 = sh2.slots[2]; p2.appId = 2; p2.linked = 1; p2.heartbeatMs = now; p2.bgSeq = 4; p2.bgStyle = 2; p2.bgOpacityPct = 30;
    CHECK(bgAdoption(sh2, 0, sync2, now, out));
    CHECK(out.style == 2 && out.opacityPct == 30);
    CHECK(sync2.seenSeq[1] == 3 && sync2.seenSeq[2] == 4);
    CHECK(!bgAdoption(sh2, 0, sync2, now, out));
}


    // ---------- v0.20.1: WindowGeom clamp (shared off-screen fix) ----------
    {
        std::vector<IntRect> areas{ IntRect{0, 0, 1920, 1040} };
        IntRect inside{ 100, 100, 1060, 56 };
        CHECK(clampRectToWorkAreas(inside, areas) == inside);
        auto c = clampRectToWorkAreas(IntRect{ -300, 2000, 1060, 56 }, areas);
        CHECK(c.x == 0 && c.y == 1040 - 56);
        auto c2 = clampRectToWorkAreas(IntRect{ 3000, 10, 500, 56 }, areas);
        CHECK(c2.x == 1920 - 500 && c2.y == 10);
        CHECK(clampRectToWorkAreas(inside, {}) == inside);
    }
    // ---------- v0.21.1: WindowGeom visibility floor (link applies) ----------
    {
        std::vector<IntRect> areas{ IntRect{0, 0, 1920, 1040} };
        std::vector<IntRect> two{ IntRect{0, 0, 1920, 1040}, IntRect{1920, 0, 1920, 1040} };
        // fully off-screen: the floor fires — size kept, primary area,
        // centred horizontally, near the top
        IntRect lost{ 5000, 3000, 1060, 56 };
        CHECK(!rectMeaningfullyVisible(lost, areas));
        auto f = ensureRectVisible(lost, areas);
        CHECK(f.x == (1920 - 1060) / 2 && f.y == 24 && f.w == 1060 && f.h == 56);
        CHECK(rectMeaningfullyVisible(f, areas));
        // genuinely on the second display: untouched
        IntRect onSecond{ 2000, 50, 1060, 56 };
        CHECK(rectMeaningfullyVisible(onSecond, two));
        CHECK(ensureRectVisible(onSecond, two) == onSecond);
        // partially visible ABOVE the floor (120x56 showing): untouched
        IntRect partial{ 1800, 100, 1060, 56 };
        CHECK(rectMeaningfullyVisible(partial, areas));
        CHECK(ensureRectVisible(partial, areas) == partial);
        // a 20 px sliver is below the floor: rescued to the primary
        IntRect sliver{ 1900, 100, 1060, 56 };
        CHECK(!rectMeaningfullyVisible(sliver, areas));
        CHECK(ensureRectVisible(sliver, areas).x == (1920 - 1060) / 2);
        // sunk so only 10 px of height shows: below the floor too
        CHECK(!rectMeaningfullyVisible(IntRect{ 100, 1030, 1060, 56 }, areas));
        // straddling two monitors with a real chunk on each: untouched
        IntRect straddle{ 1700, 100, 1060, 56 };
        CHECK(ensureRectVisible(straddle, two) == straddle);
        // no areas = no opinion
        CHECK(ensureRectVisible(lost, {}) == lost);
    }
    // ---------- v0.20.1: BarLink BLK2 — stack planner + audio broadcast ----------
    {
        const long long now = 777000;
        LinkShared sh;
        auto& m = sh.slots[0]; m.appId = 0; m.linked = 1; m.heartbeatMs = now; m.rect = IntRect{40, 0, 900, 56};
        m.stackEdge = 0; m.stackAnchor = m.rect;
        auto& t = sh.slots[1]; t.appId = 1; t.linked = 1; t.heartbeatMs = now; t.rect = IntRect{500, 500, 900, 96};
        auto& s2 = sh.slots[2]; s2.appId = 2; s2.linked = 1; s2.heartbeatMs = now; s2.rect = IntRect{5, 5, 1060, 108};
        IntRect targets[3];
        CHECK(planStackAll(sh, now, 0, targets) == 3);
        CHECK(targets[0] == m.rect);
        CHECK(targets[1].y == 56 && targets[1].x == 40);
        CHECK(targets[2].y == 56 + 96 && targets[2].x == 40);
        t.linked = 0;
        CHECK(planStackAll(sh, now, 0, targets) == 2);
        CHECK(targets[1].w == 0);
        // audio: unpublished slot reads false; published values round-trip
        float pkt[4] = { -1, -1, -1, -1 };
        CHECK(!audioPacket(m, now, pkt));
        m.audioSeq = 3; m.audioLow = 0.75f; m.audioMid = 0.5f; m.audioHigh = 0.25f; m.audioLevel = 0.6f;
        CHECK(audioPacket(m, now, pkt));
        CHECK(pkt[0] == 0.75f && pkt[1] == 0.5f && pkt[2] == 0.25f && pkt[3] == 0.6f);
        CHECK(!audioPacket(m, now + kPeerStaleMs + 1, pkt));
        BgSyncState bs; bs.publishedStyle = 3; bs.publishedPct = 40; bs.publishedEffect = 1;
        CHECK(!bgIsLocalChange(bs, 3, 40, 1));
        bgForceRepublish(bs);
        CHECK(bgIsLocalChange(bs, 3, 40, 1));
    }
    // --- group field map: one visualizer field across the linked bars
    {
        // Unlinked / alone: group == own -> the historical bar-local map.
        IntRect own{ 100, 200, 800, 40 };
        auto m0 = fieldMap(own, own, 400, 20);
        CHECK(!m0.grouped);
        float lx = -1, ly = -1;
        fieldToLocal(m0, 0.5f, 0.5f, lx, ly);
        CHECK(lx == 200.f && ly == 10.f);
        fieldToLocal(m0, 0.f, 0.f, lx, ly);
        CHECK(lx == 0.f && ly == 0.f);
        // Degenerate rects fall back to the local map, never a wild scale.
        auto mD = fieldMap(own, IntRect{}, 400, 20);
        CHECK(!mD.grouped && mD.kx == 400.f && mD.ky == 20.f);
        // Two bars side by side: the field centre is the seam — it maps
        // to the left bar's right cache edge and the right bar's left
        // cache edge (the same screen point from both sides).
        IntRect grp{ 0, 0, 1600, 40 };
        IntRect left{ 0, 0, 800, 40 }, right{ 800, 0, 800, 40 };
        auto mL = fieldMap(left, grp, 400, 20);
        auto mR = fieldMap(right, grp, 400, 20);
        CHECK(mL.grouped && mR.grouped);
        fieldToLocal(mL, 0.5f, 0.5f, lx, ly); CHECK(lx == 400.f);
        fieldToLocal(mR, 0.5f, 0.5f, lx, ly); CHECK(lx == 0.f);
        fieldToLocal(mL, 0.25f, 0.5f, lx, ly); CHECK(lx == 200.f); // quarter point: centre of the left bar
        fieldToLocal(mR, 0.75f, 0.5f, lx, ly); CHECK(lx == 200.f); // three-quarter: centre of the right bar
        // Three bars: the middle bar's cache spans the middle third.
        IntRect grp3{ 0, 0, 2400, 40 };
        IntRect mid3{ 800, 0, 800, 40 };
        auto mM = fieldMap(mid3, grp3, 400, 20);
        fieldToLocal(mM, 0.5f, 0.5f, lx, ly); CHECK(lx == 200.f);
        fieldToLocal(mM, 1.f / 3.f, 0.5f, lx, ly); CHECK(lx > -0.01f && lx < 0.01f);
        // Vertical stack: the same on the y axis.
        IntRect vgrp{ 0, 0, 800, 80 };
        IntRect top{ 0, 0, 800, 40 }, bot{ 0, 40, 800, 40 };
        auto mT = fieldMap(top, vgrp, 400, 20);
        auto mB = fieldMap(bot, vgrp, 400, 20);
        fieldToLocal(mT, 0.5f, 0.5f, lx, ly); CHECK(ly == 20.f);
        fieldToLocal(mB, 0.5f, 0.5f, lx, ly); CHECK(ly == 0.f);
        // Visibility culling: a dot deep in the left slice is invisible
        // to the right bar; a dot straddling an edge still draws.
        CHECK(fieldDotVisible(200.f, 10.f, 3.f, 400, 20));
        CHECK(!fieldDotVisible(-60.f, 10.f, 3.f, 400, 20));
        CHECK(!fieldDotVisible(460.f, 10.f, 3.f, 400, 20));
        CHECK(fieldDotVisible(-2.f, 10.f, 3.f, 400, 20));
    }
    // --- v0.20.2: BoundedThread — the bounded stop behind the loopback fix
    {
        BoundedThread idle;                       // never started
        CHECK(idle.stopBounded(50));
        BoundedThread quick;                      // finishes at once: joined
        quick.start([]{});
        CHECK(quick.stopBounded(2000));
        CHECK(quick.finished());
        // Worker stuck in a "driver call" (never looks at any stop flag):
        // stopBounded must give up in bounded time, detach and report
        // false — the caller then leaks its state instead of freezing
        // the UI on an unbounded join. The gate is shared so the
        // detached worker can still exit cleanly afterwards.
        auto gate = std::make_shared<std::atomic<bool>>(false);
        BoundedThread stuck;
        stuck.start([gate]{ while (!gate->load()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
        auto t0 = std::chrono::steady_clock::now();
        bool joined = stuck.stopBounded(100);
        auto waitedMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(!joined);
        CHECK(waitedMs >= 50 && waitedMs < 1000); // waited ~the timeout, NOT the worker's lifetime
        gate->store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // --- v0.20.2: SpectrogramHistory::head — the renderer's skip bookkeeping
    {
        SpectrogramHistory sg; sg.prepare(4, 8);
        std::vector<float> row(4, -30.f);
        CHECK(sg.head() == 0);
        sg.pushRow(row, 5.f);                     // speed 5 -> exactly one row per call
        CHECK(sg.head() == 1);
        for (int i = 0; i < 7; ++i) sg.pushRow(row, 5.f);
        CHECK(sg.head() == 0);                    // wrapped at rows
        SpectrogramHistory sg2; sg2.prepare(2, 4);
        sg2.pushRow(std::vector<float>{ -10.f, -11.f }, 5.f);
        sg2.pushRow(std::vector<float>{ -20.f, -21.f }, 5.f);
        CHECK(sg2.head() == 2);
        CHECK(sg2.at(0, 0) == -20.f);             // age 0 = newest row
        CHECK(sg2.at(1, 0) == -10.f);
    }
    // --- v0.20.2: group-dock participation rule + the mutual-dock chase
    {
        CHECK(groupDockFollows(-1));
        CHECK(!groupDockFollows(0));
        CHECK(!groupDockFollows((int)ReserveEdge::Bottom));
        // Simulate the client dock rule for two bars that are BOTH
        // reserve-docked at the bottom edge and linked (each tick, each
        // bar stacks against the other's latest rect). Unguarded — the
        // v0.20.1 behaviour — the positions never settle: the chase
        // that flooded the message thread in the 2026-10-29 field
        // report. Guarded by groupDockFollows, neither bar ever moves.
        auto simTick = [](IntRect& self, const IntRect& peer, bool guarded) {
            if (guarded && !groupDockFollows((int)ReserveEdge::Bottom)) return false;
            IntRect t = groupDockRect(peer, self, ReserveEdge::Bottom);
            if (t == self) return false;
            self = t; return true;
        };
        {
            IntRect a{ 100, 1000, 800, 50 }, b{ 100, 940, 800, 60 };
            bool moved = true;
            for (int i = 0; i < 50 && moved; ++i) {
                bool ma = simTick(a, b, false), mb = simTick(b, a, false);
                moved = ma || mb;
            }
            CHECK(moved); // v0.20.1: still moving after 50 ticks — no fixed point
        }
        {
            IntRect a{ 100, 1000, 800, 50 }, b{ 100, 940, 800, 60 };
            bool moved = true;
            for (int i = 0; i < 50 && moved; ++i) {
                bool ma = simTick(a, b, true), mb = simTick(b, a, true);
                moved = ma || mb;
            }
            CHECK(!moved);
            CHECK(a == (IntRect{ 100, 1000, 800, 50 }));
            CHECK(b == (IntRect{ 100, 940, 800, 60 }));
        }
        // Floating follower + docked peer: one step to the target, then
        // a fixed point (the target depends only on the peer's rect).
        {
            IntRect peer{ 100, 940, 800, 60 }, me{ 300, 300, 700, 50 };
            IntRect t1 = groupDockRect(peer, me, ReserveEdge::Bottom);
            CHECK(t1 == (IntRect{ 100, 890, 700, 50 }));
            CHECK(groupDockRect(peer, t1, ReserveEdge::Bottom) == t1);
        }
    }

    // v0.20.3 silence gate (Jackson's ask): the ambient whites fade
    // out when the bands carry no energy; colored content is
    // unaffected. Same check as TickerBar's core suite.
    {
        meterbar::VisualizerEngine viz;
        meterbar::VisualizerSettings vs;
        const float zero[3] = { 0.f, 0.f, 0.f };
        for (int i = 0; i < 240; ++i) viz.update(1.f / 60.f, zero, vs);
        CHECK(viz.activity() < 0.02f);
        int brightest = 0;
        for (auto& p : viz.particles()) if (!p.burst) { auto c = viz.colorFor(p, vs); brightest = std::max(brightest, (int)c.r); }
        CHECK(brightest < 12);
        const float loud[3] = { 0.5f, 0.4f, 0.3f };
        for (int i = 0; i < 30; ++i) viz.update(1.f / 60.f, loud, vs);
        CHECK(viz.activity() > 0.3f);
        brightest = 0;
        for (auto& p : viz.particles()) if (!p.burst) { auto c = viz.colorFor(p, vs); brightest = std::max(brightest, (int)c.r); }
        // v0.21.5 retune: the intent is unchanged (whites clearly
        // present once music plays) but the scale is the calmed
        // peak now — kAmbientPeak x 255 = 140, not the old
        // full-brightness 255 this check was written against.
        CHECK(brightest > 100);
        CHECK(brightest <= (int)std::lround(255.f * meterbar::VisualizerEngine::kAmbientPeak) + 1);
    }

    // ---- Glyphs (v0.22.2): the toolbar icon glyphs must be
    // EXACTLY the UTF-8 encoding of their codepoints. The field
    // failure was mojibake — MSVC decoded the raw literals as
    // Windows-1252, so each sequence's first byte surfaced as a
    // Latin-1 char (0xF0, 0xE2; Jackson's v0.22.1
    // screenshot). Pin every glyph two ways: against an
    // independent codepoint->UTF-8 encoder written here, and
    // against the byte sequences themselves.
    {
        auto encodeUtf8 = [](std::uint32_t cp) {
            std::string out;
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
            return out;
        };
        CHECK(std::string(meterbar::glyphLinkUtf8()) == encodeUtf8(0x1F517));
        CHECK(std::string(meterbar::glyphLinkUtf8()) == std::string("\xF0\x9F\x94\x97", 4));
        CHECK(std::string(meterbar::glyphRecordUtf8()) == encodeUtf8(0x1F534));
        CHECK(std::string(meterbar::glyphRecordUtf8()) == std::string("\xF0\x9F\x94\xB4", 4));
        CHECK(std::string(meterbar::glyphPauseUtf8()) == encodeUtf8(0x23F8));
        CHECK(std::string(meterbar::glyphPauseUtf8()) == std::string("\xE2\x8F\xB8", 3));
        CHECK(std::string(meterbar::glyphPlayUtf8()) == encodeUtf8(0x25B6));
        CHECK(std::string(meterbar::glyphPlayUtf8()) == std::string("\xE2\x96\xB6", 3));
        CHECK(std::string(meterbar::glyphSettingsUtf8()) == encodeUtf8(0x2699));
        CHECK(std::string(meterbar::glyphSettingsUtf8()) == std::string("\xE2\x9A\x99", 3));
        CHECK(std::string(meterbar::glyphMenuUtf8()) == encodeUtf8(0x2630));
        CHECK(std::string(meterbar::glyphMenuUtf8()) == std::string("\xE2\x98\xB0", 3));
    }

    // ---- Live Captions framing (v0.23.0): MBC1 round-trip, partial
    // reads, multiple packets per feed, desync latching, audio prefix.
    {
        CaptionPacket p; p.type = CaptionPacketType::Config; p.epoch = 7; p.sequence = 42;
        p.payload = { 'h', 'i' };
        auto bytes = encodeCaptionPacket(p);
        CHECK(bytes.size() == kCaptionHeaderBytes + 2);
        CHECK(bytes[0] == 'M' && bytes[1] == 'B' && bytes[2] == 'C' && bytes[3] == '1');
        CaptionPacketDecoder dec; CaptionPacket out;
        dec.feed(bytes.data(), 10);                 // partial header
        CHECK(!dec.next(out));
        dec.feed(bytes.data() + 10, bytes.size() - 10);
        CHECK(dec.next(out));
        CHECK(out.type == CaptionPacketType::Config && out.epoch == 7 && out.sequence == 42);
        CHECK(out.payload.size() == 2 && out.payload[0] == 'h');
        CHECK(!dec.next(out));                      // drained
        // two packets in one feed
        auto b2 = encodeCaptionPacket(p);
        std::vector<uint8_t> both = bytes; both.insert(both.end(), b2.begin(), b2.end());
        CaptionPacketDecoder dec2; dec2.feed(both.data(), both.size());
        CHECK(dec2.next(out)); CHECK(dec2.next(out)); CHECK(!dec2.next(out));
        // bad magic latches the error state
        auto bad = bytes; bad[0] = 'X';
        CaptionPacketDecoder dec3; dec3.feed(bad.data(), bad.size());
        CHECK(!dec3.next(out)); CHECK(dec3.error());
        // audio payload prefix round-trip
        CaptionAudioPrefix pre; pre.firstNativeSample = 123456789; pre.sampleRate = 44100; pre.frameSamples = 3;
        int16_t samples[3] = { -32768, 0, 32767 };
        auto ap = buildCaptionAudioPayload(pre, samples);
        CaptionAudioPrefix pre2; size_t off = 0;
        CHECK(parseCaptionAudioPayload(ap.data(), ap.size(), pre2, off));
        CHECK(pre2.firstNativeSample == 123456789 && pre2.sampleRate == 44100 && pre2.frameSamples == 3);
        CHECK(off == kCaptionAudioPrefixBytes);
        int16_t s0 = (int16_t)((uint16_t)ap[off] | ((uint16_t)ap[off + 1] << 8));
        CHECK(s0 == -32768);
        CHECK(!parseCaptionAudioPayload(ap.data(), 10, pre2, off)); // truncated
    }
    // ---- Live Captions display model (v0.23.0): provisional replace,
    // final immutability, demotion to previous, expiry rules.
    {
        CaptionDisplay d;
        auto blk = [](int64_t start, const char* text, bool fin) {
            CaptionBlock b; b.epoch = 1; b.startMs = start; b.endMs = start + 1000;
            b.id = captionBlockId(1, start); b.text = text; b.final = fin; return b;
        };
        CHECK(d.empty(0));
        d.apply(blk(0, "hello wor", false), 1000);
        auto v = d.view(1000);
        CHECK(v.hasCurrent && !v.current.final && v.current.text == "hello wor");
        d.apply(blk(0, "hello world", false), 1500);   // provisional revises in place
        CHECK(d.view(1500).current.text == "hello world");
        d.apply(blk(0, "hello world.", true), 1800);   // final locks
        CHECK(d.view(1800).current.final);
        d.apply(blk(0, "hello world REWRITTEN", true), 2000); // final is immutable
        CHECK(d.view(2000).current.text == "hello world.");
        d.apply(blk(0, "late provisional", false), 2100);     // ...against provisionals too
        CHECK(d.view(2100).current.text == "hello world.");
        d.apply(blk(2000, "next line", true), 3000);   // new final demotes the old to previous
        v = d.view(3000);
        CHECK(v.hasCurrent && v.current.text == "next line");
        CHECK(v.hasPrevious && v.previous.text == "hello world.");
        v = d.view(3000 + kCaptionPreviousHoldMs + 1); // previous expires after 4 s
        CHECK(!v.hasPrevious && v.hasCurrent);
        v = d.view(3000 + kCaptionFinalHoldMs + 1);    // current final expires after 6 s
        CHECK(!v.hasCurrent);
        CHECK(d.empty(3000 + kCaptionFinalHoldMs + 1));
        // an unfinalised provisional is discarded, never promoted to history
        CaptionDisplay d2;
        d2.apply(blk(0, "draft", false), 0);
        d2.apply(blk(5000, "real", true), 1000);
        v = d2.view(1000);
        CHECK(v.hasCurrent && !v.hasPrevious);
        // a stale provisional (no final within 3 s) disappears
        CaptionDisplay d3;
        d3.apply(blk(0, "fading", false), 0);
        CHECK(d3.view(kCaptionProvisionalStaleMs).hasCurrent);
        CHECK(!d3.view(kCaptionProvisionalStaleMs + 1).hasCurrent);
        CHECK(captionBlockId(3, 2500) == "cap-3-2500");
    }
    // ---- Live Captions two-line planning (v0.23.0): wrap, overflow
    // keeps the newest words, over-long words hard-split.
    {
        auto p1 = planCaptionLines("one two three", 20);
        CHECK(p1.lines.size() == 1 && p1.lines[0] == "one two three" && !p1.overflowed);
        auto p2 = planCaptionLines("aaa bbb ccc ddd eee fff", 11);
        CHECK(p2.lines.size() == 2 && !p2.overflowed);
        auto p3 = planCaptionLines("w1 w2 w3 w4 w5 w6 w7 w8 w9 wa wb wc", 8);
        CHECK(p3.lines.size() == 2 && p3.overflowed);
        CHECK(p3.lines[1].find("wc") != std::string::npos); // newest words survive
        auto p4 = planCaptionLines("abcdefghijklmnop", 6);
        CHECK(p4.overflowed && p4.lines.size() == 2 && p4.lines[1] == "mnop");
        auto p5 = planCaptionLines("", 20);
        CHECK(p5.lines.empty() && !p5.overflowed);
    }

    // ---------- v0.24.0: caption overlay homing (WindowGeom) ----------
    // Two 1920x1040 work areas side by side; the bar sits bottom-
    // docked on the SECOND display. The overlay must home to the
    // bar's display, not the primary's and not a stale saved spot.
    {
        const std::vector<IntRect> two{ {0, 0, 1920, 1040}, {1920, 0, 1920, 1040} };
        const IntRect barD2{ 2220, 870, 1100, 170 }; // bottom edge 1040 = D2 bottom
        // Saved on the OTHER display -> relocate to bottom-centre of
        // the bar's display, raised above the bar it would overlap.
        auto p = captionHomeRect(IntRect{ 300, 800, 900, 132 }, true, barD2, two, 132);
        const IntRect wantHome{ 2430, 726, 900, 132 }; // x centred on D2; y = bar.y - 12 - h
        CHECK(p.relocate);
        CHECK(p.rect == wantHome);
        // Saved mainly on the bar's own display -> keep placement.
        auto keep = captionHomeRect(IntRect{ 2500, 700, 900, 132 }, true, barD2, two, 132);
        CHECK(!keep.relocate);
        // Saved straddling, >= half on the bar's display -> keep.
        auto strad = captionHomeRect(IntRect{ 1700, 800, 900, 132 }, true, barD2, two, 132);
        CHECK(!strad.relocate);
        // Bar on D1, saved mostly on D2 -> relocate onto D1.
        const IntRect barD1{ 400, 870, 1100, 170 };
        auto toD1 = captionHomeRect(IntRect{ 1700, 800, 900, 132 }, true, barD1, two, 132);
        CHECK(toD1.relocate && toD1.rect.right() <= 1920 && toD1.rect.x >= 0);
        // No saved position -> default 900 wide, centred on the bar's display.
        auto fresh = captionHomeRect(IntRect{}, false, barD2, two, 132);
        CHECK(fresh.relocate && fresh.rect.w == 900 && fresh.rect.x == 2430);
        // Oversized saved width clamps to the 1200 ceiling.
        auto wide = captionHomeRect(IntRect{ 300, 800, 2000, 132 }, true, barD2, two, 132);
        CHECK(wide.relocate && wide.rect.w == 1200);
        // No displays known -> no opinion, keep whatever exists.
        auto none = captionHomeRect(IntRect{ 300, 800, 900, 132 }, true, barD2, {}, 132);
        CHECK(!none.relocate);
        // Home rect that clears the bar keeps the 24 px bottom margin.
        const IntRect barHigh{ 2220, 100, 1100, 170 };
        auto clear = captionHomeRect(IntRect{ 300, 800, 900, 132 }, true, barHigh, two, 132);
        CHECK(clear.relocate && clear.rect.bottom() == 1040 - 24);
    }

    // ---------- v0.24.0: caption dock growth + reservation/link ----------
    // The docked strip grows the bar by the strip height, anchored at
    // its docked edge. The reservation (strip derived from the bar's
    // REAL rect) and the link stack (plans derived from the anchor's
    // CURRENT rect) must follow the grown bounds exactly.
    {
        const IntRect bar{ 2220, 910, 1100, 170 };
        const int stripH = 76;
        auto grownB = captionGrownBarRect(bar, stripH, ReserveEdge::Bottom);
        const IntRect wantGrownB{ 2220, 910 - stripH, 1100, 170 + stripH };
        CHECK(grownB == wantGrownB);
        CHECK(grownB.bottom() == bar.bottom()); // docked edge never moves
        auto grownT = captionGrownBarRect(IntRect{ 100, 0, 1100, 170 }, stripH, ReserveEdge::Top);
        const IntRect wantGrownT{ 100, 0, 1100, 170 + stripH };
        const IntRect wantShrunkT{ 100, 0, 1100, 170 };
        CHECK(grownT == wantGrownT);
        CHECK(captionShrunkBarRect(grownB, stripH, ReserveEdge::Bottom) == bar);
        CHECK(captionShrunkBarRect(grownT, stripH, ReserveEdge::Top) == wantShrunkT);
        // Shrink floor: a bar manually shrunk while docked keeps 48 px.
        auto floored = captionShrunkBarRect(IntRect{ 0, 500, 800, 100 }, stripH, ReserveEdge::Bottom);
        CHECK(floored.h == 48 && floored.bottom() == 600);
        // Reservation: bottom-docked bar registers with its own height
        // as the strip thickness; after growth the machine emits
        // exactly one Update and the strip IS the grown thickness —
        // the reservation matches the real (grown) bounds.
        const IntRect mon{ 1920, 0, 1920, 1080 };
        ReserveMachine rm;
        const EdgeGaps g0{ 910, 0, 300, 500 };
        const IntRect wantStrip0{ 1920, 910, 1920, 170 };
        const IntRect wantStrip1{ 1920, 910 - stripH, 1920, 170 + stripH };
        CHECK(rm.update(true, g0, bar, mon, 7) == ReserveAction::Register);
        CHECK(rm.edge() == ReserveEdge::Bottom);
        CHECK(rm.requestedRect() == wantStrip0);
        const EdgeGaps g1{ 910 - stripH, 0, 300, 500 }; // bottom gap unchanged by growth
        CHECK(rm.update(true, g1, grownB, mon, 7) == ReserveAction::Update);
        CHECK(rm.requestedRect() == wantStrip1);
        CHECK(rm.update(true, g1, grownB, mon, 7) == ReserveAction::None); // no flap
        CHECK(ReserveMachine::stripRect(ReserveEdge::Bottom, grownB, mon).h == 170 + stripH);
        // Link stack: a follower group-docks flush against the GROWN
        // leader rect, and a one-click stack plan from the grown
        // anchor puts the follower above the grown bar, not the old.
        auto gd = groupDockRect(grownB, IntRect{ 0, 0, 900, 56 }, ReserveEdge::Bottom);
        CHECK(gd.y == grownB.y - 56 && gd.x == grownB.x);
        LinkShared sh;
        const long long now = 100000;
        sh.slots[0].appId = kLinkAppMeter; sh.slots[0].heartbeatMs = now; sh.slots[0].linked = 1;
        sh.slots[0].rect = grownB; sh.slots[0].stackSeq = 1;
        sh.slots[0].stackEdge = (int)ReserveEdge::Bottom; sh.slots[0].stackAnchor = grownB;
        sh.slots[1].appId = kLinkAppTicker; sh.slots[1].heartbeatMs = now; sh.slots[1].linked = 1;
        sh.slots[1].rect = IntRect{ 2220, 910 - 56, 900, 56 }; // stacked vs the UNgrown bar
        IntRect targets[3];
        const IntRect wantTarget{ 2220, grownB.y - 56, 900, 56 };
        CHECK(planStackAll(sh, now, kLinkAppMeter, targets) == 2);
        CHECK(targets[1] == wantTarget);
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

