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
#include "../dsp/include/meterbar/BeatTrack.hpp"
#include "../dsp/include/meterbar/Undertow.hpp"
#include "../dsp/include/meterbar/Performance.hpp"
#include "../dsp/include/meterbar/CrashWatch.hpp"
#include "../dsp/include/meterbar/PinSource.hpp" // v0.28.7
#include "../dsp/include/meterbar/CloseSave.hpp" // v0.28.8
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
        CHECK(p.stereoPointSize == 2.0f); CHECK(p.stereoColor == StereoColor::Rainbow); // v0.28.6 recipe: point size 2, Rainbow default (was 1.0f / Single)
        CHECK(p.themeName == "Midnight Kids"); CHECK(isValidFftSize(16384) && !isValidFftSize(1000));
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
        CHECK(p.backgroundStyle == BackgroundStyle::MidnightKids); CHECK(p.backgroundOpacity == 0.15f); // v0.28.0 recipe (was CleanDark)
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
        CHECK(std::abs(p.trimDb - 27.0f) < 1e-6);                 // v0.28.0 recipe: +27 dB (was +10.18 through v0.27.x)
        CHECK(p.visualizerEnabled);                              // v0.28.0 recipe: visualizer background ON
        CHECK(p.visualizerStyle == 3);                           // v0.28.1 recipe: Undertow is the default background (auto-engage; v0.28.0 shipped Field/2)
        CHECK(p.undertowBloomQuarter && p.undertowBloomEighth && p.undertowPreArm); // grading hooks at the committed design
        CHECK(std::abs(p.undertowKickTauMs - 65.0f) < 1e-6);     // committed kick release τ
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
        // BLK5 widened the domain (BLK6, below in the file, moved
        // the magic again for the linkSeq field); the stereo
        // balance rides the audio packet at [4]
        CHECK(kLinkMagic == 0x424C4B37u); // BLK7 (v0.28.4): appearance extension + lastClaim/linkAll appends moved the magic again
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
    // v0.25.1 parent ownership: adoption flows DOWNHILL only, from
    // the group parent (lowest appId). The packet mechanics are
    // exercised from a follower's seat (self = 2, parent = 0)...
    LinkShared sh;
    auto& peer = sh.slots[0]; peer.appId = 0; peer.linked = 1; peer.heartbeatMs = now;
    peer.bgSeq = 7; peer.bgStyle = 2; peer.bgOpacityPct = 40; peer.bgEffect = 2;
    BgPacket out;
    CHECK(bgAdoption(sh, 2, sync, now, out));
    CHECK(out.style == 2 && out.opacityPct == 40 && out.effect == 2);
    CHECK(sync.adoptedFrom == 0);
    // the adopted values are now OUR published values: no echo publish
    CHECK(!bgIsLocalChange(sync, 2, 40, 2));
    // the same seq is never adopted twice
    CHECK(!bgAdoption(sh, 2, sync, now, out));
    // a genuinely new seq adopts again
    peer.bgSeq = 8; peer.bgStyle = 0; peer.bgOpacityPct = 90; peer.bgEffect = 1;
    CHECK(bgAdoption(sh, 2, sync, now, out));
    CHECK(out.style == 0 && out.opacityPct == 90 && out.effect == 1);
    // BLK4: effect 3 (visualizer Lines) rides the same packet,
    // adopted verbatim like every other ordinal
    peer.bgSeq = 12; peer.bgEffect = 3;
    CHECK(bgAdoption(sh, 2, sync, now, out));
    CHECK(out.effect == 3);
    CHECK(!bgIsLocalChange(sync, 0, 90, 3)); // adopted = published: no echo
    // unlinked peers, stale peers, seq 0 and self never supply a background
    peer.linked = 0; peer.bgSeq = 9;
    CHECK(!bgAdoption(sh, 2, sync, now, out));
    peer.linked = 1; peer.heartbeatMs = now - kPeerStaleMs - 1;
    CHECK(!bgAdoption(sh, 2, sync, now, out));
    peer.heartbeatMs = now; peer.bgSeq = 0;
    CHECK(!bgAdoption(sh, 2, sync, now, out));
    peer.bgSeq = 11; peer.appId = 2; // the peer slot carries OUR id: self, ignored
    CHECK(!bgAdoption(sh, 2, sync, now, out));
    // ...and from the parent's own seat (self = 0, MeterBar's seat):
    // the parent adopts from NOBODY, however fresh the followers'
    // packets are — the pre-v0.25.1 join swap, pinned shut.
    LinkShared sh2; BgSyncState sync2;
    auto& p1 = sh2.slots[1]; p1.appId = 1; p1.linked = 1; p1.heartbeatMs = now; p1.bgSeq = 3; p1.bgStyle = 1; p1.bgOpacityPct = 70;
    auto& p2 = sh2.slots[2]; p2.appId = 2; p2.linked = 1; p2.heartbeatMs = now; p2.bgSeq = 4; p2.bgStyle = 2; p2.bgOpacityPct = 30;
    CHECK(!bgAdoption(sh2, 0, sync2, now, out));
    CHECK(sync2.adoptedFrom == -1);
    // a middle bar (self = 1) between the same two peers takes the
    // parent's (appId 0 is absent here, so with only appId 2 live it
    // is itself the parent) — with appId 0 present it takes 0's:
    auto& p0 = sh2.slots[0]; p0.appId = 0; p0.linked = 1; p0.heartbeatMs = now; p0.bgSeq = 5; p0.bgStyle = 1; p0.bgOpacityPct = 70;
    BgSyncState sync3;
    CHECK(bgAdoption(sh2, 1, sync3, now, out));
    CHECK(out.style == 1 && out.opacityPct == 70);
    CHECK(sync3.seenSeq[0] == 5 && sync3.seenSeq[2] == 0);
    CHECK(sync3.adoptedFrom == 0);
    CHECK(!bgAdoption(sh2, 1, sync3, now, out));
}
// ---- BLK6: first-clicked-wins parentage + member-drag leadership ----
{
    using namespace meterbar;
    const long long now = 300000;
    BgPacket out;
    // FIRST-CLICKED WINS: appId 2 claimed linkSeq 1 (clicked first),
    // appId 0 claimed 3 — the parent is 2, NOT the lowest appId (the
    // v0.25.1 fixed-slot rule Jackson's 2026-11-01 field test rejected).
    LinkShared sh;
    auto& s0 = sh.slots[0]; s0.appId = 0; s0.linked = 1; s0.heartbeatMs = now; s0.linkSeq = 3; s0.bgSeq = 4; s0.bgStyle = 1; s0.bgOpacityPct = 70;
    auto& s1 = sh.slots[1]; s1.appId = 1; s1.linked = 1; s1.heartbeatMs = now; s1.linkSeq = 2; s1.bgSeq = 5; s1.bgStyle = 0; s1.bgOpacityPct = 90;
    auto& s2 = sh.slots[2]; s2.appId = 2; s2.linked = 1; s2.heartbeatMs = now; s2.linkSeq = 1; s2.bgSeq = 6; s2.bgStyle = 2; s2.bgOpacityPct = 30;
    CHECK(bgParentApp(sh, 1, 2, now) == 2);
    BgSyncState sy1;
    CHECK(bgAdoption(sh, 1, sy1, now, out));
    CHECK(out.style == 2 && out.opacityPct == 30 && sy1.adoptedFrom == 2);
    CHECK(sy1.seenSeq[0] == 0); // the lowest-appId non-parent packet is never consumed
    // the first-clicked bar itself adopts from nobody
    BgSyncState sy2;
    CHECK(!bgAdoption(sh, 2, sy2, now, out));
    // the parent's SLOT is resolved by appId after selection: with
    // self's own slot sitting between the parent's and a follower's,
    // adoption must still read the PARENT's packet (a first draft of
    // bgParentApp returned self's slot index here and adopted self).
    {
        LinkShared shS; BgSyncState syS;
        auto& q0 = shS.slots[0]; q0.appId = 0; q0.linked = 1; q0.heartbeatMs = now; q0.linkSeq = 1; q0.bgSeq = 21; q0.bgStyle = 3; q0.bgOpacityPct = 25;
        auto& q1 = shS.slots[1]; q1.appId = 1; q1.linked = 1; q1.heartbeatMs = now; q1.linkSeq = 0; q1.bgSeq = 22; // self unclaimed: the geometry that exposed the slot bug q1.bgStyle = 0; q1.bgOpacityPct = 99;
        auto& q2 = shS.slots[2]; q2.appId = 2; q2.linked = 1; q2.heartbeatMs = now; q2.linkSeq = 3; q2.bgSeq = 23; q2.bgStyle = 1; q2.bgOpacityPct = 55;
        CHECK(bgAdoption(shS, 1, syS, now, out));
        CHECK(out.style == 3 && out.opacityPct == 25 && syS.adoptedFrom == 0);
        CHECK(syS.seenSeq[0] == 21 && syS.seenSeq[1] == 0 && syS.seenSeq[2] == 0);
    }
    // LATE JOINER NEVER PARENT: appId 0 re-claims with the largest
    // linkSeq and a fresh packet — the group still follows appId 2,
    // and the joiner adopts the parent's packet from its own seat.
    LinkShared shJ = sh; shJ.slots[0].linkSeq = 9; shJ.slots[0].bgSeq = 40;
    BgSyncState syJ;
    CHECK(bgAdoption(shJ, 0, syJ, now, out));
    CHECK(syJ.adoptedFrom == 2 && out.opacityPct == 30);
    // PARENT PACKET ARRIVES AFTER THE FOLLOWER'S FIRST SCAN: the
    // follower scanned while the first-clicked bar's slot was not yet
    // in the block (its app started later); when the parent's packet
    // appears, adoption fires on that tick.
    LinkShared shL; BgSyncState syL;
    auto& l1 = shL.slots[1]; l1.appId = 1; l1.linked = 1; l1.heartbeatMs = now; l1.linkSeq = 2; l1.bgSeq = 3; l1.bgStyle = 0; l1.bgOpacityPct = 90;
    CHECK(!bgAdoption(shL, 1, syL, now, out)); // alone in the block: self is parent
    auto& l0 = shL.slots[0]; l0.appId = 0; l0.linked = 1; l0.heartbeatMs = now; l0.linkSeq = 1; l0.bgSeq = 2; l0.bgStyle = 3; l0.bgOpacityPct = 15;
    CHECK(bgAdoption(shL, 1, syL, now, out));
    CHECK(out.style == 3 && out.opacityPct == 15 && syL.adoptedFrom == 0);
    // HANDOFF: the parent unlinks — the earliest-linked REMAINING
    // member becomes parent, and its standing packet is adopted even
    // though that bgSeq was already seen (identity change re-fires).
    LinkShared shH; BgSyncState syH;
    auto& h0 = shH.slots[0]; h0.appId = 0; h0.linked = 1; h0.heartbeatMs = now; h0.linkSeq = 1; h0.bgSeq = 7; h0.bgStyle = 1; h0.bgOpacityPct = 60;
    auto& h1 = shH.slots[1]; h1.appId = 1; h1.linked = 1; h1.heartbeatMs = now; h1.linkSeq = 2; h1.bgSeq = 8; h1.bgStyle = 2; h1.bgOpacityPct = 45;
    auto& h2 = shH.slots[2]; h2.appId = 2; h2.linked = 1; h2.heartbeatMs = now; h2.linkSeq = 3; h2.bgSeq = 9; h2.bgStyle = 0; h2.bgOpacityPct = 95;
    CHECK(bgAdoption(shH, 2, syH, now, out)); CHECK(syH.adoptedFrom == 0);
    syH.seenSeq[1] = 8; // h1's packet was consumed under the old regime
    h0.linked = 0;
    CHECK(bgParentApp(shH, 2, 3, now) == 1);
    CHECK(bgAdoption(shH, 2, syH, now, out));
    CHECK(out.style == 2 && out.opacityPct == 45 && syH.adoptedFrom == 1);
    // A RELINKED PARENT RETURNS AS A LATE JOINER: it claims a fresh
    // (largest) linkSeq, so it now ADOPTS from the standing parent.
    h0.linked = 1; h0.linkSeq = 10;
    BgSyncState syH0;
    CHECK(bgAdoption(shH, 0, syH0, now, out));
    CHECK(syH0.adoptedFrom == 1 && out.opacityPct == 45);
    // MEMBER-DRAG LEADERSHIP: whichever member leads a drag session,
    // every other member's follow rect translates by the leader's
    // delta with size preserved — the pure half of Jackson's rule
    // "dragging ANY linked bar moves the whole stack" (the client
    // half — every bar's geometry feed stamps a session — is pinned
    // by the TickerBar v0.6.7 wiring fix; it was the missing half).
    for (int leader = 0; leader < 3; ++leader) {
        LinkShared shD;
        for (int a = 0; a < 3; ++a) {
            auto& sl = shD.slots[a]; sl.appId = a; sl.linked = 1; sl.heartbeatMs = now;
            sl.rect = IntRect{ 100 + a * 700, 200, 600, 40 };
        }
        auto& ld = shD.slots[leader];
        ld.dragSeq = 5; ld.dragStart = ld.rect; ld.dragStampMs = now;
        ld.rect = IntRect{ ld.rect.x + 30, ld.rect.y - 12, ld.rect.w, ld.rect.h };
        CHECK(dragLive(ld, now));
        for (int a = 0; a < 3; ++a) {
            if (a == leader) continue;
            auto fr = followRect(shD.slots[a].rect, ld.dragStart, ld.rect);
            CHECK(fr.x == shD.slots[a].rect.x + 30 && fr.y == shD.slots[a].rect.y - 12);
            CHECK(fr.w == 600 && fr.h == 40);
        }
    }
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

    // === v0.26.0 ===
    // --- xToFreq is freqToX's exact inverse (all three scales)
    {
        for (auto sc : { FreqScale::Log, FreqScale::Mel, FreqScale::Linear })
            for (double f : { 20.0, 55.0, 440.0, 1000.0, 5000.0, 19999.0 }) {
                float frac = SpectrumAnalyser::freqToX(f, 20.0, 20000.0, sc);
                float back = SpectrumAnalyser::xToFreq(frac, 20.0, 20000.0, sc);
                CHECK(std::abs(back - f) / f < 0.01);
            }
    }
    // --- BeatPulse v0.26.0 flux path: a moderate hit inside LOUD
    // steady material (level ratio under the old 2.4 bar, and under
    // the new 1.95 bar too) still fires on its sharp attack.
    {
        const double sr = 48000; const int blk = 256;
        std::vector<float> L(blk), R(blk);
        BeatPulse bp; bp.prepare(sr);
        auto pushAmp = [&](float amp, int blocks) {
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) { double ph = 2 * M_PI * 440.0 * (b * blk + i) / sr; L[i] = (float)(amp * std::sin(ph)); R[i] = L[i]; }
                bp.pushBlock(L.data(), R.data(), blk);
            }
        };
        pushAmp(0.5f, (int)(2.0 * sr / blk));            // loud bed, baseline settles high
        uint64_t before = bp.onsetCount();
        for (int h = 0; h < 4; ++h) {                    // 4 hits: ms ratio ~1.85 (no ratio hit), flux ~1.85 (flux hit)
            pushAmp(0.68f, 1);
            pushAmp(0.5f, (int)(0.45 * sr / blk));
        }
        std::printf("beatpulse flux: onsets in loud bed = %llu (need >= 3)\n", (unsigned long long)(bp.onsetCount() - before));
        CHECK(bp.onsetCount() - before >= 3);
    }
    // --- Bubbles/Lines impact envelopes (v0.26.0 rework)
    {
        VisualizerSettings vs;
        VisualizerEngine eng;
        const float sil[3] = { 0, 0, 0 };
        for (int i = 0; i < 10; ++i) eng.update(1.f / 30.f, sil, vs);
        CHECK(eng.impact() == 0.f);
        const float hit[3] = { 0.95f, 0, 0 };
        eng.update(1.f / 30.f, hit, vs);
        CHECK(eng.impact() > 0.5f);                      // a hard jump registers as impact
        CHECK(eng.burstCount() >= 1);
        VisualizerLines lines;
        lines.update(1.f / 30.f, hit);
        CHECK(lines.impact(0) > 0.5f);
        CHECK(lines.impact(1) == 0.f);                   // other bands untouched
        for (int i = 0; i < 60; ++i) lines.update(1.f / 30.f, sil);
        CHECK(lines.impact(0) < 0.05f);                  // and it decays away
    }
    // --- Field: transient pulse, downbeat fold-in, trails stamp
    {
        VisualizerSettings vs;
        VisualizerField fld;
        const float sil[3] = { 0, 0, 0 };
        for (int i = 0; i < 30; ++i) fld.update(1.f / 30.f, sil, vs);
        CHECK(fld.pulse() == 0.f);
        const float hit[3] = { 0.95f, 0.2f, 0.1f };
        fld.update(1.f / 30.f, hit, vs);
        CHECK(fld.pulse() > 0.3f);                       // a transient pulses the field
        for (int i = 0; i < 60; ++i) fld.update(1.f / 30.f, hit, vs);
        int trailed = 0, moved = 0;
        for (auto& d : fld.particles()) if (d.trailSet) { ++trailed; if (d.trailX[0] != d.x || d.trailY[0] != d.y) ++moved; }
        CHECK(trailed > (int)fld.particles().size() / 2);
        CHECK(moved > (int)fld.particles().size() / 2);  // the tail lags the dot
        VisualizerField beat;
        for (int i = 0; i < 30; ++i) beat.update(1.f / 30.f, hit, vs, 0.f, 0.f);
        beat.update(1.f / 30.f, hit, vs, 0.f, 1.0f);     // a downbeat lands
        CHECK(beat.pulse() >= 0.85f);                    // and folds into the pulse
    }
    // --- BeatTracker: locks to a kick train, counts the bar, holds
    // the count through a breakdown, re-locks on a real tempo
    // change; silence never locks; 6/8 subdivides the kick period.
    {
        const double sr = 48000; const int blk = 256;
        std::vector<float> L(blk), R(blk);
        BeatTracker tr; tr.prepare(sr);
        double tNow = 0, nextKick = 0.35, kickPeriod = 0.5; bool kicksOn = true;
        auto feed = [&](double secs) {
            int blocks = (int)std::lround(secs * sr / blk);
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) {
                    double tt = tNow + (double)i / sr;
                    double v = 0.01 * std::sin(2 * M_PI * 440.0 * tt); // bed the kick band can't hear
                    if (kicksOn && tt >= nextKick && tt < nextKick + 0.09)
                        v += 0.85 * std::sin(2 * M_PI * 60.0 * (tt - nextKick)) * (1.0 - (tt - nextKick) / 0.09);
                    L[(size_t)i] = (float)v; R[(size_t)i] = (float)v;
                }
                tr.pushBlock(L.data(), R.data(), blk);
                tNow += blk / sr;
                while (nextKick < tNow) nextKick += kickPeriod;
            }
        };
        feed(6.0);                                       // 120 BPM kicks
        std::printf("beattrack: locked=%d bpm=%.1f conf=%.2f onsets=%llu\n",
                    (int)tr.locked(), tr.bpm(), (double)tr.confidence(), (unsigned long long)tr.onsetCount());
        CHECK(tr.locked());
        CHECK(tr.bpm() > 114.0 && tr.bpm() < 126.0);
        CHECK(tr.confidence() > 0.5f);
        {   // the count cycles the whole bar
            bool seen[5] = { false, false, false, false, false };
            uint64_t g0 = tr.gridBeatCount();
            for (int i = 0; i < 400; ++i) { feed(blk / sr); int c = tr.countInBar(); if (c >= 1 && c <= 4) seen[c] = true; }
            CHECK(seen[1] && seen[2] && seen[3] && seen[4]);
            CHECK(tr.gridBeatCount() > g0);
            // recent-beats overlay data: ages ascend, counts step back
            float ages[16]; int counts[16];
            int nR = tr.collectRecentBeats(1.6f, ages, counts, 16);
            CHECK(nR >= 3);
            CHECK(ages[0] < 0.55f && ages[1] > ages[0] && ages[2] > ages[1]);
            CHECK(counts[0] == tr.countInBar());
        }
        {   // breakdown: kicks stop, the count must NOT
            kicksOn = false;
            uint64_t g0 = tr.gridBeatCount();
            feed(5.0);
            CHECK(tr.locked());
            CHECK(tr.gridBeatCount() >= g0 + 8);
            kicksOn = true; nextKick = tNow + 0.2;
        }
        kickPeriod = 60.0 / 90.0;                        // the song slows to 90 BPM
        feed(7.0);
        std::printf("beattrack after tempo change: bpm=%.1f locked=%d\n", tr.bpm(), (int)tr.locked());
        CHECK(tr.locked());
        CHECK(tr.bpm() > 85.0 && tr.bpm() < 95.0);
        // silence: no lock, no onsets
        BeatTracker quiet; quiet.prepare(sr);
        std::vector<float> Z(blk, 0.f);
        for (int b = 0; b < (int)(2.0 * sr / blk); ++b) quiet.pushBlock(Z.data(), Z.data(), blk);
        CHECK(!quiet.locked());
        CHECK(quiet.onsetCount() == 0);
        // 6/8: kicks on the dotted quarter (0.6 s) -> grid pulses
        // every 0.2 s, counts run 1..6, bpm reads the KICK tempo
        BeatTracker six; six.prepare(sr); six.setMeter(6, 3);
        {
            double t2 = 0, nk = 0.35;
            int blocks = (int)std::lround(10.0 * sr / blk);
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) {
                    double tt = t2 + (double)i / sr;
                    double v = 0.01 * std::sin(2 * M_PI * 440.0 * tt);
                    if (tt >= nk && tt < nk + 0.09)
                        v += 0.85 * std::sin(2 * M_PI * 60.0 * (tt - nk)) * (1.0 - (tt - nk) / 0.09);
                    L[(size_t)i] = (float)v; R[(size_t)i] = (float)v;
                }
                six.pushBlock(L.data(), R.data(), blk);
                t2 += blk / sr;
                while (nk < t2) nk += 0.6;
            }
            CHECK(six.locked());
            CHECK(six.bpm() > 95.0 && six.bpm() < 105.0);
            bool seen1 = false, seen6 = false;
            for (int i = 0; i < 400; ++i) {
                for (int j = 0; j < blk; ++j) { L[(size_t)j] = 0.f; R[(size_t)j] = 0.f; }
                six.pushBlock(L.data(), R.data(), blk);
                if (six.countInBar() == 1) seen1 = true;
                if (six.countInBar() == 6) seen6 = true;
            }
            CHECK(seen1 && seen6);
        }
    }
    // --- BeatTracker at slow tempos (v0.26.1 field fix): Jackson's
    // ~80 BPM song would not lock. Bare kick trains at 80/60 always
    // locked — the failure needs what a real slow song's kick band
    // carries besides the kick: bass playing eighth notes in the
    // same <=140 Hz band. Those weaker crossings became onsets, and
    // at song tempos under ~107 BPM their spacing sits inside the
    // fold window whole, so v0.26.0 read 160 BPM on an 80 BPM song
    // (and ~89 on a soft 60). The peak-relative onset gate (>= 45%
    // of the band's recent peak) admits the kick — which sets the
    // peak — and refuses the bass under it.
    {
        const double sr = 48000; const int blk = 256;
        std::vector<float> L(blk), R(blk);
        // One feeder for every train below: kicks at `period`
        // (envelope: raised-cosine attack, (1-u)^1.6 body), an
        // optional sustained bass bed, and optional bass plucks on
        // every eighth note in the kick band.
        auto feedTrain = [&](BeatTracker& tr, double period, double secs,
                             double kickFreq, double kickAmp, double attack, double dur,
                             double bedFreq, double bedAmp, double pluckAmp) {
            double tNow = 0, nextKick = 0.35, curK = -1, nextPluck = 0.35 + period / 2, curP = -1;
            int blocks = (int)std::lround(secs * sr / blk);
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) {
                    double tt = tNow + (double)i / sr;
                    while (tt >= nextKick) { curK = nextKick; nextKick += period; }
                    while (tt >= nextPluck) { curP = nextPluck; nextPluck += period / 2; }
                    double v = 0.01 * std::sin(2 * M_PI * 440.0 * tt);
                    if (bedAmp > 0) v += bedAmp * std::sin(2 * M_PI * bedFreq * tt);
                    if (curK >= 0) { double lt = tt - curK;
                        if (lt >= 0 && lt < dur) {
                            double env = lt < attack ? 0.5 - 0.5 * std::cos(M_PI * lt / attack)
                                                     : std::pow(1.0 - (lt - attack) / (dur - attack), 1.6);
                            v += kickAmp * std::sin(2 * M_PI * kickFreq * lt) * env;
                        } }
                    if (pluckAmp > 0 && curP >= 0) { double lt = tt - curP;
                        if (lt >= 0 && lt < 0.12) v += pluckAmp * std::sin(2 * M_PI * 98.0 * lt) * (1.0 - lt / 0.12); }
                    L[(size_t)i] = (float)v; R[(size_t)i] = (float)v;
                }
                tr.pushBlock(L.data(), R.data(), blk);
                tNow += blk / sr;
            }
        };
        {   // 80 BPM bare kicks — the slow control
            BeatTracker tr; tr.prepare(sr);
            feedTrain(tr, 60.0 / 80.0, 16.0, 60, 0.85, 0.004, 0.30, 0, 0, 0);
            std::printf("beattrack slow bare 80: locked=%d bpm=%.1f\n", (int)tr.locked(), tr.bpm());
            CHECK(tr.locked());
            CHECK(tr.bpm() > 74.0 && tr.bpm() < 86.0);
        }
        {   // 60 BPM bare kicks — the slow control
            BeatTracker tr; tr.prepare(sr);
            feedTrain(tr, 1.0, 16.0, 60, 0.85, 0.004, 0.30, 0, 0, 0);
            std::printf("beattrack slow bare 60: locked=%d bpm=%.1f\n", (int)tr.locked(), tr.bpm());
            CHECK(tr.locked());
            CHECK(tr.bpm() > 55.0 && tr.bpm() < 65.0);
        }
        {   // 80 BPM + bass eighth plucks: v0.26.0 read ~160 here
            BeatTracker tr; tr.prepare(sr);
            feedTrain(tr, 60.0 / 80.0, 16.0, 60, 0.85, 0.004, 0.30, 0, 0, 0.45);
            std::printf("beattrack slow bass8 80: locked=%d bpm=%.1f onsets=%llu\n",
                        (int)tr.locked(), tr.bpm(), (unsigned long long)tr.onsetCount());
            CHECK(tr.locked());
            CHECK(tr.bpm() > 74.0 && tr.bpm() < 86.0);
        }
        {   // 60 BPM + bass eighth plucks: must read ~60, not ~120
            BeatTracker tr; tr.prepare(sr);
            feedTrain(tr, 1.0, 20.0, 60, 0.85, 0.004, 0.30, 0, 0, 0.45);
            std::printf("beattrack slow bass8 60: locked=%d bpm=%.1f onsets=%llu\n",
                        (int)tr.locked(), tr.bpm(), (unsigned long long)tr.onsetCount());
            CHECK(tr.locked());
            CHECK(tr.bpm() > 55.0 && tr.bpm() < 65.0);
        }
        {   // 60 BPM soft-attack kicks over a sustained bass bed:
            // v0.26.0 read ~89 here (bed/kick crossings polluted the
            // intervals); the gate keeps only the kicks.
            BeatTracker tr; tr.prepare(sr);
            feedTrain(tr, 1.0, 20.0, 55, 0.80, 0.16, 0.55, 52, 0.35, 0);
            std::printf("beattrack slow softbed 60: locked=%d bpm=%.1f onsets=%llu\n",
                        (int)tr.locked(), tr.bpm(), (unsigned long long)tr.onsetCount());
            CHECK(tr.locked());
            CHECK(tr.bpm() > 55.0 && tr.bpm() < 65.0);
        }
    }

    // === v0.27.0 ===
    // --- BeatTracker on BUSY tracks (item 13): kick + snare +
    // hats + bassline at ~80 / ~100 / ~128 BPM must read the TRUE
    // tempo. The v0.27.0 cascaded kick-band lowpass (two one-pole
    // stages at 140 Hz) isolates the kick from the snare's low
    // body and the bassline's harmonics in a dense mix.
    {
        const double sr = 48000; const int blk = 256;
        std::vector<float> L(blk), R(blk);
        auto feedBusy = [&](BeatTracker& tr, double bpm, double secs) {
            const double period = 60.0 / bpm;
            double tNow = 0; int blocks = (int)std::lround(secs * sr / blk);
            auto env = [](double lt, double dur, double atk) {
                if (lt < 0 || lt >= dur) return 0.0;
                return lt < atk ? 0.5 - 0.5 * std::cos(M_PI * lt / atk)
                                : std::pow(1.0 - (lt - atk) / (dur - atk), 1.6);
            };
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) {
                    double tt = tNow + (double)i / sr;
                    double v = 0.008 * std::sin(2 * M_PI * 440.0 * tt);
                    double klt = std::fmod(tt - 0.35, period); if (klt < 0) klt += period;
                    v += 0.85 * std::sin(2 * M_PI * 55.0 * klt) * env(klt, 0.28, 0.004);   // kick, every beat
                    double barPos = std::fmod(tt / period, 4.0);
                    double slt = (barPos - 1.0) * period; if (barPos < 1.0) slt = (barPos + 3.0) * period;
                    double slt2 = (barPos - 3.0) * period; if (barPos < 3.0) slt2 = (barPos + 1.0) * period;
                    double sEnv = env(slt, 0.16, 0.002) + env(slt2, 0.16, 0.002);
                    v += 0.55 * std::sin(2 * M_PI * 190.0 * tt) * sEnv + 0.30 * std::sin(2 * M_PI * 1800.0 * tt) * sEnv; // snare 2 & 4
                    double hlt = std::fmod(tt - 0.35, period / 2); if (hlt < 0) hlt += period / 2;
                    v += 0.22 * std::sin(2 * M_PI * 8000.0 * tt) * env(hlt, 0.05, 0.001);  // hats, eighths
                    int step = (int)std::floor((tt - 0.35) / (period / 2));
                    double bf = (step % 4 == 0) ? 55.0 : (step % 4 == 2) ? 65.4 : 49.0;
                    v += 0.42 * std::sin(2 * M_PI * bf * tt) * env(hlt, period / 2 * 0.9, 0.01); // bassline, eighths
                    L[(size_t)i] = (float)v; R[(size_t)i] = (float)v;
                }
                tr.pushBlock(L.data(), R.data(), blk);
                tNow += blk / sr;
            }
        };
        for (double bpm : { 80.0, 100.0, 128.0 }) {
            BeatTracker tr; tr.prepare(sr);
            feedBusy(tr, bpm, 24.0);
            std::printf("beattrack busy %.0f: locked=%d read=%.1f onsets=%llu\n",
                        bpm, (int)tr.locked(), tr.bpm(), (unsigned long long)tr.onsetCount());
            CHECK(tr.locked());
            CHECK(tr.bpm() > bpm * 0.94 && tr.bpm() < bpm * 1.06);
        }
    }
    // --- BeatPulse sensitivity (item 16): a hit at energy ratio
    // ~1.9 over the baseline fires (the v0.26.x bar was 1.95).
    {
        const double sr = 48000; const int blk = 256;
        std::vector<float> L(blk), R(blk);
        BeatPulse bp; bp.prepare(sr);
        auto pushAmp = [&](float amp, int blocks) {
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) { double ph = 2 * M_PI * 440.0 * (b * blk + i) / sr; L[i] = (float)(amp * std::sin(ph)); R[i] = L[i]; }
                bp.pushBlock(L.data(), R.data(), blk);
            }
        };
        pushAmp(0.5f, (int)(2.0 * sr / blk));
        uint64_t before = bp.onsetCount();
        for (int h = 0; h < 4; ++h) { pushAmp(0.69f, 1); pushAmp(0.5f, (int)(0.45 * sr / blk)); } // ms ratio ~1.90
        std::printf("beatpulse v0.27 sensitivity: onsets = %llu (need >= 3)\n", (unsigned long long)(bp.onsetCount() - before));
        CHECK(bp.onsetCount() - before >= 3);
    }
    // --- Field v0.27.0: stereo lookback + size scale (item 9),
    // more bubbles, screen bursts (item 10), sub body inertia
    // (item 17), bridge state (item 20), sub band honesty.
    {
        VisualizerSettings vs;
        CHECK(VisualizerField::kFieldTarget > 150);      // MORE bubbles than v0.26.x's 150
        VisualizerField fld;
        const float bands[3] = { 0.6f, 0.5f, 0.4f };
        for (int i = 0; i < 90; ++i) fld.update(1.f / 60.f, bands, vs, 0.f, 0.f, 0.9f, 0.f);
        CHECK(fld.widthLookback() > 0.7f);               // the lookback holds the wide image
        CHECK(fld.sizeScale() > 1.2f);                   // and bubbles swell with it
        CHECK((int)fld.particles().size() == VisualizerField::kFieldTarget);
        bool huesDiffer = false;                          // different colours by default: per-dot jitter spreads hues
        if (fld.particles().size() > 10) {
            auto c0 = fld.colorFor(fld.particles()[0], vs);
            for (size_t i = 1; i < fld.particles().size(); ++i) {
                auto c = fld.colorFor(fld.particles()[i], vs);
                if (c.r != c0.r || c.g != c0.g || c.b != c0.b) { huesDiffer = true; break; }
            }
        }
        CHECK(huesDiffer);
        // Sub body: heavy + slow — a 0.25 s blast of sub barely
        // moves it; 3 s of sustained sub swells it; silence lets
        // it settle back down slowly (not instantly).
        VisualizerField sub;
        const float noBands[3] = { 0, 0, 0 };
        for (int i = 0; i < 15; ++i) sub.update(1.f / 60.f, noBands, vs, 0.f, 0.f, 0.f, 1.0f);
        const float afterBlast = sub.subLevel();
        for (int i = 0; i < 180; ++i) sub.update(1.f / 60.f, noBands, vs, 0.f, 0.f, 0.f, 1.0f);
        const float afterSustain = sub.subLevel();
        for (int i = 0; i < 30; ++i) sub.update(1.f / 60.f, noBands, vs, 0.f, 0.f, 0.f, 0.0f);
        const float afterHalfSec = sub.subLevel();
        std::printf("subbody: blast=%.3f sustain=%.3f release0.5s=%.3f\n", afterBlast, afterSustain, afterHalfSec);
        CHECK(afterBlast < 0.35f);                       // inertia: no snap
        CHECK(afterSustain > 0.8f);                      // sustained sub swells it
        CHECK(afterHalfSec > 0.4f && afterHalfSec < afterSustain); // slow release
        // Screen bursts: transients in exactly 2 bands fire 3
        // bursts of different colours with random transparency.
        VisualizerField fb;
        const float calm[3] = { 0.1f, 0.1f, 0.1f };
        for (int i = 0; i < 30; ++i) fb.update(1.f / 30.f, calm, vs);
        const float twoBand[3] = { 0.95f, 0.9f, 0.1f };
        fb.update(1.f / 30.f, twoBand, vs);
        std::printf("screenbursts: n=%d\n", (int)fb.screenBursts().size());
        CHECK(fb.screenBursts().size() == 3);
        if (fb.screenBursts().size() == 3) {
            auto& b0 = fb.screenBursts()[0]; auto& b1 = fb.screenBursts()[1]; auto& b2 = fb.screenBursts()[2];
            CHECK(std::abs(b0.hue - b1.hue) > 0.15f && std::abs(b1.hue - b2.hue) > 0.15f); // different colours
            CHECK(b0.alpha >= 0.2f && b0.alpha <= 0.8f && b1.alpha >= 0.2f && b1.alpha <= 0.8f); // random transparency in range
        }
        // Bridge: wide + mid/high present + low/sub ~zero, held.
        VisualizerField br;
        const float bridgeBands[3] = { 0.02f, 0.5f, 0.45f };
        for (int i = 0; i < 12; ++i) br.update(1.f / 60.f, bridgeBands, vs, 0.f, 0.f, 0.9f, 0.0f);
        CHECK(!br.bridgeActive());                      // not yet — the condition must SUSTAIN
        for (int i = 0; i < 60; ++i) br.update(1.f / 60.f, bridgeBands, vs, 0.f, 0.f, 0.9f, 0.0f);
        CHECK(br.bridgeActive());
        const float bassyBands[3] = { 0.7f, 0.5f, 0.45f };
        for (int i = 0; i < 30; ++i) br.update(1.f / 60.f, bassyBands, vs, 0.f, 0.f, 0.9f, 0.0f);
        CHECK(!br.bridgeActive());                      // bass back -> bridge state ends
        // Sub band honesty: energy at 30 Hz reads; at 200 Hz doesn't.
        {
            std::vector<float> db(513, -70.f);
            db[1] = -10.f; // bin 1 = 46.9 Hz centre, span 23-70 Hz: the only bin with under-45 Hz content at this FFT size
            CHECK(subBandEnergy(db, 1024, 48000.0) > 0.8f);
            std::vector<float> db2(513, -70.f);
            for (int b = 1; b < 513; ++b) { double f = b * 48000.0 / 1024; if (f > 150.0 && f < 300.0) db2[(size_t)b] = -10.f; }
            CHECK(subBandEnergy(db2, 1024, 48000.0) < 0.05f);
        }
    }

    // === v0.28.0 — Undertow (visualizer council DESIGN.md) ===
    // --- committed envelope law + follower maths
    {
        CHECK(kUndertowKickReleaseTau == 0.065f);            // τ 65 ms, no hold (the council's settled argument)
        CHECK(kUndertowPipelineCompSec == 0.034f);           // 34 ms pipeline compensation
        CHECK(kUndertowPredictedCap == 0.45f);               // unconfirmed prediction cap
        CHECK(kUndertowConfirmWindowSec == 0.060f);          // ±60 ms confirmation window
        const float v150 = decayTau(1.f, 0.150f, kUndertowKickReleaseTau);
        CHECK(v150 > 0.07f && v150 < 0.13f);                 // ~10% of peak inside 150 ms (the gap reads empty)
        CHECK(decayTau(1.f, 0.255f, kUndertowKickReleaseTau) < 0.02f);
        float stepped = 1.f; for (int i = 0; i < 10; ++i) stepped = decayTau(stepped, 0.016f, 0.065f);
        CHECK(std::abs(stepped - decayTau(1.f, 0.16f, 0.065f)) < 0.001f); // dt-corrected: step size must not matter
        // tails: born at full factor, quadratic alpha / 1.5-power width, dead at 220 ms
        CHECK(tailAlphaFactor(0.f) == 1.f && tailWidthFactor(0.f) == 1.f);
        CHECK(std::abs(tailAlphaFactor(0.11f) - 0.25f) < 0.01f);
        CHECK(tailWidthFactor(0.11f) > 0.30f && tailWidthFactor(0.11f) < 0.40f);
        CHECK(tailAlphaFactor(0.22f) == 0.f && tailWidthFactor(0.30f) == 0.f);
        UndertowTrail trl; trl.reset(0.1f, 0.2f);
        trl.advance(0.15f); CHECK(trl.n == 1 && trl.age[0] > 0.14f);
        trl.push(0.3f, 0.4f); CHECK(trl.n == 2 && trl.age[0] == 0.f && trl.age[1] > 0.14f);
        trl.advance(0.10f); CHECK(trl.n == 1);               // the 250 ms-old point is pruned; the 100 ms one stays
        CHECK(softClipUnit(0.f) == 0.f && std::abs(softClipUnit(1.f) - 1.f) < 1e-5f);
        CHECK(softClipUnit(0.5f) > 0.60f && softClipUnit(0.5f) < 0.72f);
        CHECK(undertowDbToLevel(-60.f) == 0.f && undertowDbToLevel(-6.f) == 1.f);
        CHECK(std::abs(undertowDbToLevel(-33.f) - 0.5f) < 1e-6f);
        CHECK(undertowDbToLevel(-100.f) == 0.f && undertowDbToLevel(0.f) == 1.f);
        // palette: the fixed Midnight Kits band colours, exact hexes (DESIGN §(e))
        auto c0 = UndertowModel::bandColour(0); CHECK(c0.r == 0x4A && c0.g == 0x5F && c0.b == 0xD9);
        auto c1 = UndertowModel::bandColour(1); CHECK(c1.r == 0x33 && c1.g == 0x50 && c1.b == 0x7A);
        auto c2 = UndertowModel::bandColour(2); CHECK(c2.r == 0xE8 && c2.g == 0xEE && c2.b == 0xF5);
        auto c3 = UndertowModel::bandColour(3); CHECK(c3.r == 0x9F && c3.g == 0xD8 && c3.b == 0xE8);
        auto c4 = UndertowModel::bandColour(4); CHECK(c4.r == 0xCF && c4.g == 0xC8 && c4.b == 0xE8);
        auto ce = UndertowModel::membraneEdgeColour(); CHECK(ce.r == 0x7C && ce.g == 0x8C && ce.b == 0xF0);
        auto cw = UndertowModel::ringWarmColour(); CHECK(cw.r == 0xE8 && cw.g == 0xD9 && cw.b == 0xB0);
    }
    // --- Path A: FiveBandOnsets selectivity (time-domain; no FFT anywhere near timing)
    {
        const int blk = 256;
        std::vector<float> L(blk), R(blk);
        double tNow = 0;
        auto tone = [&](FiveBandOnsets& ob, double freq, double amp, double secs) {
            int n = (int)std::lround(secs * sr / blk);
            for (int b = 0; b < n; ++b) {
                for (int i = 0; i < blk; ++i) { double tt = tNow + (double)i / sr; L[(size_t)i] = R[(size_t)i] = (float)(amp * std::sin(2 * M_PI * freq * tt)); }
                ob.pushBlock(L.data(), R.data(), blk); tNow += blk / sr;
            }
        };
        auto silence = [&](FiveBandOnsets& ob, double secs) {
            std::vector<float> z(blk, 0.f);
            int n = (int)std::lround(secs * sr / blk);
            for (int b = 0; b < n; ++b) { ob.pushBlock(z.data(), z.data(), blk); tNow += blk / sr; }
        };
        // kicks: 60 Hz bursts; optional softer off-beat bass (the v0.26.1 trap)
        auto kickTrain = [&](FiveBandOnsets& ob, double secs, double period, double kickAmp, double bassAmp) {
            int n = (int)std::lround(secs * sr / blk);
            for (int b = 0; b < n; ++b) {
                for (int i = 0; i < blk; ++i) {
                    double tt = tNow + (double)i / sr;
                    double v = 0.01 * std::sin(2 * M_PI * 440.0 * tt);
                    double pos = std::fmod(tt, period);
                    if (pos < 0.09) v += kickAmp * std::sin(2 * M_PI * 60.0 * pos) * (1.0 - pos / 0.09);
                    if (bassAmp > 0) { double pb = std::fmod(tt - period * 0.5, period); if (pb < 0) pb += period;
                        if (pb < 0.12) v += bassAmp * std::sin(2 * M_PI * 55.0 * pb) * (1.0 - pb / 0.12); }
                    L[(size_t)i] = R[(size_t)i] = (float)v;
                }
                ob.pushBlock(L.data(), R.data(), blk); tNow += blk / sr;
            }
        };
        {   // silence: nothing fires, ever
            FiveBandOnsets ob; ob.prepare(sr); tNow = 0; silence(ob, 2.0);
            uint64_t tot = 0; for (int b = 0; b < 5; ++b) tot += ob.onsetCount(b);
            CHECK(tot == 0);
        }
        {   // a steady tone is not an onset: count frozen after warm-up
            FiveBandOnsets ob; ob.prepare(sr); tNow = 0; tone(ob, 60.0, 0.5, 3.0);
            uint64_t c0[5]; for (int b = 0; b < 5; ++b) c0[b] = ob.onsetCount(b);
            tone(ob, 60.0, 0.5, 2.0);
            bool frozen = true; for (int b = 0; b < 5; ++b) frozen = frozen && ob.onsetCount(b) == c0[b];
            CHECK(frozen);
        }
        {   // 120 BPM kick train: the sub band tracks the kicks
            FiveBandOnsets ob; ob.prepare(sr); tNow = 0; kickTrain(ob, 3.0, 0.5, 0.9, 0.0);
            const uint64_t before = ob.onsetCount(0);
            kickTrain(ob, 3.0, 0.5, 0.9, 0.0);
            const uint64_t got = ob.onsetCount(0) - before;
            std::printf("undertow onsets: sub kicks in 3 s = %llu (6 fed)\n", (unsigned long long)got);
            CHECK(got >= 5 && got <= 7);
        }
        {   // peak gate: softer off-beat bass must NOT double the sub count
            FiveBandOnsets ob; ob.prepare(sr); tNow = 0; kickTrain(ob, 4.0, 0.5, 0.9, 0.30);
            std::printf("undertow onsets: sub with bass train = %llu (8 kicks fed)\n", (unsigned long long)ob.onsetCount(0));
            CHECK(ob.onsetCount(0) >= 5 && ob.onsetCount(0) <= 11);
        }
        {   // absolute floor: inaudible kicks never fire
            FiveBandOnsets ob; ob.prepare(sr); tNow = 0; kickTrain(ob, 3.0, 0.5, 0.0001, 0.0);
            CHECK(ob.onsetCount(0) == 0);
        }
        {   // high ticks over a steady sub bed: high fires, sub stays silent
            FiveBandOnsets ob; ob.prepare(sr); tNow = 0;
            int n = (int)std::lround(4.0 * sr / blk);
            for (int b = 0; b < n; ++b) {
                for (int i = 0; i < blk; ++i) {
                    double tt = tNow + (double)i / sr;
                    double v = 0.02 * std::sin(2 * M_PI * 60.0 * tt);
                    double pos = std::fmod(tt, 0.5);
                    if (pos < 0.04) v += 0.5 * std::sin(2 * M_PI * 6000.0 * pos);
                    L[(size_t)i] = R[(size_t)i] = (float)v;
                }
                ob.pushBlock(L.data(), R.data(), blk); tNow += blk / sr;
            }
            std::printf("undertow onsets: high ticks = %llu (8 fed), sub = %llu\n",
                        (unsigned long long)ob.onsetCount(3), (unsigned long long)ob.onsetCount(0));
            CHECK(ob.onsetCount(3) >= 5 && ob.onsetCount(3) <= 10);
            CHECK(ob.onsetCount(0) == 0);
        }
        {   // presence: sparse 12 kHz ticks clear the strictest ratio
            FiveBandOnsets ob; ob.prepare(sr); tNow = 0;
            int n = (int)std::lround(4.0 * sr / blk);
            for (int b = 0; b < n; ++b) {
                for (int i = 0; i < blk; ++i) {
                    double tt = tNow + (double)i / sr;
                    double pos = std::fmod(tt, 1.0);
                    double v = pos < 0.03 ? 0.6 * std::sin(2 * M_PI * 12000.0 * pos) : 0.0;
                    L[(size_t)i] = R[(size_t)i] = (float)v;
                }
                ob.pushBlock(L.data(), R.data(), blk); tNow += blk / sr;
            }
            CHECK(ob.onsetCount(4) >= 3);
        }
    }
    // --- Path B: levels from the unsmoothed spectrum
    {
        std::vector<float> db(1025, -100.f);
        db[1] = -14.f; db[2] = -8.f; // 23.4 + 46.9 Hz: sub energy
        auto lv = fiveBandLevelsFromRawDb(db, 2048, 48000.0);
        CHECK(lv.band[0] > 0.7f);
        CHECK(lv.band[2] < 0.05f);
        int pk = 0; for (int i = 1; i < FiveBandLevels::kEdgePoints; ++i) if (lv.edge[i] > lv.edge[pk]) pk = i;
        const double pkHz = 20.0 * std::pow(12.5, ((double)pk + 0.5) / FiveBandLevels::kEdgePoints);
        CHECK(pkHz > 35.0 && pkHz < 60.0); // the membrane edge peaks at the tone, not at DC or the band edge
        std::vector<float> db2(1025, -100.f);
        for (int b = 42; b <= 46; ++b) db2[(size_t)b] = -8.f; // ~1 kHz cluster
        auto lv2 = fiveBandLevelsFromRawDb(db2, 2048, 48000.0);
        CHECK(lv2.band[2] > 0.65f);
        CHECK(lv2.band[0] < 0.05f);
        std::vector<float> db3(1025, -100.f);
        auto lv3 = fiveBandLevelsFromRawDb(db3, 2048, 48000.0);
        bool allZero = true; for (int b = 0; b < 5; ++b) allZero = allZero && lv3.band[b] < 0.02f;
        CHECK(allZero);
    }
    // --- the model: envelope law, beat states, Governor
    {
        FiveBandLevels loud; for (int b = 0; b < 5; ++b) loud.band[b] = 0.8f;
        FiveBandLevels zero{};
        UndertowBeatFeed noBeat; UndertowOnsetFeed none;
        {   // a sub onset fires the kick on the committed envelope
            UndertowModel m;
            UndertowOnsetFeed fire; fire.count[0] = 1; fire.strength[0] = 1.f; fire.lastSec[0] = 50.0;
            m.update(0.016f, loud, -20.f, noBeat, fire);
            CHECK(m.kickValue() > 0.7f);                       // fired this frame (one frame of τ65 decay applied)
            CHECK(m.kickSpikeDisplay() > m.kickValue());       // the render-only easeOutBack overshoot is live
            for (int i = 0; i < 3; ++i) m.update(0.016f, loud, -20.f, noBeat, none);
            CHECK(std::abs(m.kickSpikeDisplay() - m.kickValue()) < 1e-5f); // overshoot window (40 ms) over: display IS the envelope
            for (int i = 0; i < 6; ++i) m.update(0.016f, loud, -20.f, noBeat, none);
            CHECK(m.kickValue() > 0.04f && m.kickValue() < 0.14f);  // ~0.16 s after the hit: the gap is nearly empty
            for (int i = 0; i < 6; ++i) m.update(0.016f, loud, -20.f, noBeat, none);
            CHECK(m.kickValue() < 0.03f);                      // ~0.26 s: gone
            CHECK(m.beatState() == UndertowBeatState::Reactive);
        }
        {   // silence: the gate hard-zeros; no grid + no sub for 10 s -> AMBIENT; a kick revives it
            UndertowModel m;
            for (int i = 0; i < 750; ++i) m.update(0.016f, zero, -60.f, noBeat, none); // 12 s
            CHECK(m.silenceGate() == 0.f);
            CHECK(m.displayGain() == 0.f);
            CHECK(m.beatState() == UndertowBeatState::Ambient);
            UndertowOnsetFeed fire; fire.count[0] = 1; fire.strength[0] = 1.f; fire.lastSec[0] = 999.0;
            m.update(0.016f, zero, -60.f, noBeat, fire);
            CHECK(m.beatState() == UndertowBeatState::Reactive);
        }
        {   // Still: low activity sustained -> rest state
            UndertowModel m;
            FiveBandLevels low; for (int b = 0; b < 5; ++b) low.band[b] = 0.05f;
            for (int i = 0; i < 125; ++i) m.update(0.016f, low, -20.f, noBeat, none); // 2 s
            CHECK(m.still());
        }
        // a simulated 120 BPM grid in 4/4 (beats at 0.5 s spacing)
        struct GridSim {
            double now = 0, nextBeat = 0.5; uint64_t beats = 0;
            UndertowBeatFeed f;
            GridSim(float conf) { f.locked = true; f.confidence = conf; f.bpm = 120.f; f.beatsPerBar = 4; f.lastGridBeatSec = -1; }
            void step(UndertowModel& m, const FiveBandLevels& lv, const UndertowOnsetFeed& onf) {
                now += 0.016;
                while (now >= nextBeat) {
                    f.lastGridBeatSec = nextBeat; ++beats; nextBeat += 0.5;
                    f.gridBeatCount = beats;
                    f.countInBar = (int)((beats - 1) % 4) + 1;
                }
                f.nextIsDownbeat = (beats % 4) == 0;
                f.trackerNowSec = now; f.secondsToNextBeat = nextBeat - now;
                m.update(0.016f, lv, -20.f, f, onf);
            }
        };
        {   // LOCKED: two grid beats at confidence >= 0.60 engage prediction;
            // the downbeat pulse fires 34 ms early, capped, then confirms to full
            GridSim g(0.8f); UndertowModel m;
            for (int i = 0; i < 80; ++i) g.step(m, loud, none);       // 1.28 s: beats at 0.5, 1.0
            CHECK(m.beatState() == UndertowBeatState::Locked);
            while (g.now < 2.40) g.step(m, loud, none);
            float maxPre = 0;
            while (g.now < 2.49) { g.step(m, loud, none); maxPre = std::max(maxPre, m.kickValue()); }
            std::printf("undertow predict: pre-beat kick peak = %.3f (cap 0.45 x downbeat 1.25)\n", (double)maxPre);
            CHECK(maxPre > 0.35f);                                    // the prediction really fired early
            CHECK(maxPre <= 0.58f);                                   // ... and stayed under the cap x downbeat
            UndertowOnsetFeed conf; conf.count[0] = 1; conf.strength[0] = 0.9f; conf.lastSec[0] = 2.5;
            g.step(m, loud, conf);                                    // the onset lands on the beat
            CHECK(m.kickValue() > 0.65f);                             // confirmed: upgraded to full strength
        }
        {   // REACTIVE (confidence under the bar): the grid alone never fires a pulse
            GridSim g(0.4f); UndertowModel m;
            for (int i = 0; i < 160; ++i) g.step(m, loud, none);      // 2.56 s of grid, zero onsets
            CHECK(m.beatState() == UndertowBeatState::Reactive);
            CHECK(m.kickValue() < 0.01f);
        }
        {   // pre-arm: mid onsets at count 2 in two consecutive bars arm that position
            GridSim g(0.8f); UndertowModel m;
            UndertowOnsetFeed feed;
            int injectedBar = -1;
            while (g.now < 4.6) {
                g.step(m, loud, feed);
                const int bar = (int)((g.beats - 1) / 4);              // model bars run 0-based here; beats 1-4 = bar 0
                if (g.f.countInBar == 2 && bar <= 1 && injectedBar != bar) {
                    injectedBar = bar;
                    feed.count[2] += 1; feed.strength[2] = 0.8f; feed.lastSec[2] = g.now;
                    g.step(m, loud, feed);                            // drain the latch on the next frame
                }
                if (g.f.countInBar == 1 && g.beats >= 9) break;       // third bar begins: position 2 is upcoming
            }
            CHECK(m.preArm(2));
            CHECK(std::abs(m.thresholdScale(2) - 0.88f) < 1e-6f);
            CHECK(!m.preArm(3));                                      // a band that never fired there stays unarmed
            CHECK(m.thresholdScale(3) == 1.f);
        }
        {   // the limiter never retimes an envelope: identical onsets into a
            // limiter-hot model and a cold model give identical kick curves
            UndertowModel hot, cold;
            FiveBandLevels maxLv; for (int b = 0; b < 5; ++b) maxLv.band[b] = 1.f;
            for (int i = 0; i < 375; ++i) { hot.update(0.016f, maxLv, -6.f, noBeat, none); cold.update(0.016f, zero, -60.f, noBeat, none); }
            std::printf("undertow limiter: est=%.3f dark=%.3f dots=%d\n",
                        (double)hot.estimatedLuminance(), (double)hot.darkFraction(), hot.liveDots());
            CHECK(hot.estimatedLuminance() > 0.19f && hot.estimatedLuminance() <= (double)kUndertowLuminanceCap + 0.01); // v0.28.12: rolling average under the raised cap (0.26; was <=0.19 under the 18% cap)
            CHECK(hot.darkFraction() >= 0.60f);                       // the soak rule: >=60% near-black even driven hard
            CHECK(hot.liveDots() <= UndertowModel::kDotHardCap);
            CHECK(hot.displayGain() > 0.f && hot.displayGain() <= 1.f);
            UndertowOnsetFeed fire; fire.count[0] = 1; fire.strength[0] = 1.f; fire.lastSec[0] = 100.0;
            bool same = true;
            for (int i = 0; i < 20; ++i) {
                hot.update(0.016f, maxLv, -6.f, noBeat, fire);
                cold.update(0.016f, zero, -60.f, noBeat, fire);
                if (std::abs(hot.kickValue() - cold.kickValue()) > 1e-6f) same = false;
            }
            CHECK(same);
        }
    }


    // ============================================================
    // v0.28.1 — scored kick detection, measure phase, stereo enums
    // (Jackson's v0.28.0 grading fixes). The kick tests SCORE the
    // detectors against known kick placements (TP/FP/FN with a
    // -30..+120 ms match window) instead of trusting adjectives.
    // Measured on the development machine at the v0.28.1 freeze;
    // the CHECKs carry headroom for platform float differences.
    // ============================================================
    {
        const int blk = 256; const double sr = 48000.0;
        auto kEnv281 = [](double lt, double dur, double atk) {
            if (lt < 0 || lt >= dur) return 0.0;
            return lt < atk ? 0.5 - 0.5 * std::cos(M_PI * lt / atk)
                            : std::pow(1.0 - (lt - atk) / (dur - atk), 1.6);
        };
        struct Sc281 { int tp = 0, fp = 0, fn = 0; };
        auto score281 = [](const std::vector<double>& kicks, const std::vector<double>& onsets) {
            Sc281 sc; std::vector<bool> used(onsets.size(), false);
            for (double k : kicks) {
                int best = -1; double bd = 1e9;
                for (size_t i = 0; i < onsets.size(); ++i) {
                    if (used[i]) continue;
                    const double d = onsets[i] - k;
                    if (d >= -0.03 && d <= 0.12 && std::abs(d) < bd) { bd = std::abs(d); best = (int)i; }
                }
                if (best >= 0) { used[(size_t)best] = true; ++sc.tp; } else ++sc.fn;
            }
            for (bool u : used) if (!u) ++sc.fp;
            return sc;
        };
        // Verse/chorus material: 100 BPM kicks, amplitude alternating
        // 0.90 / 0.40 every two bars, over an eighth-note bassline.
        // v0.28.0 SCORES on this signal (the failure Jackson graded):
        // BeatTracker TP 36 / FN 11 — the flat 45%-of-peak gate can
        // never hear the quiet verse under the chorus's peak;
        // FiveBandOnsets sub TP 23 / FN 24 (half the kicks gone).
        auto verseChorus = [&](auto&& sink, double secs) {
            const double period = 0.6;
            std::vector<double> kicks;
            for (double t = 0.35; t < 28.5; t += period) kicks.push_back(t);
            std::vector<float> L(blk), R(blk);
            double tNow = 0;
            const int blocks = (int)std::lround(secs * sr / blk);
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) {
                    const double tt = tNow + i / sr;
                    double v = 0.008 * std::sin(2 * M_PI * 440.0 * tt);
                    const int bar = (int)std::floor((tt - 0.35) / (period * 4));
                    const double amp = ((bar / 2) % 2 == 0) ? 0.90 : 0.40;
                    for (double k : kicks) v += amp * std::sin(2 * M_PI * 58.0 * (tt - k)) * kEnv281(tt - k, 0.30, 0.004);
                    double hlt = std::fmod(tt - 0.35, period / 2); if (hlt < 0) hlt += period / 2;
                    const int step = (int)std::floor((tt - 0.35) / (period / 2));
                    const double bf = (step % 4 == 0) ? 55.0 : (step % 4 == 2) ? 65.4 : 49.0;
                    v += 0.35 * std::sin(2 * M_PI * bf * tt) * kEnv281(hlt, period / 2 * 0.9, 0.01);
                    L[(size_t)i] = R[(size_t)i] = (float)v;
                }
                sink(L.data(), R.data(), tNow);
                tNow += blk / sr;
            }
            return kicks;
        };
        {   // BeatTracker on verse/chorus: v0.28.1 measured TP 44 / FP 1 / FN 3
            BeatTracker tr; tr.prepare(sr);
            std::vector<double> onsets;
            auto kicks = verseChorus([&](const float* L, const float* R, double t0) {
                if (tr.pushBlock(L, R, blk)) onsets.push_back(t0 + blk / sr);
            }, 29.0);
            const auto sc = score281(kicks, onsets);
            std::printf("v0.28.1 tracker verse/chorus: TP=%d FP=%d FN=%d (of %d kicks)\n", sc.tp, sc.fp, sc.fn, (int)kicks.size());
            CHECK(sc.tp >= 42); CHECK(sc.fn <= 5); CHECK(sc.fp <= 2);
            CHECK(tr.locked()); CHECK(std::abs(tr.bpm() - 100.0) < 3.0);
        }
        {   // FiveBandOnsets sub on the same signal: v0.28.1 measured TP 39 / FP 0 / FN 8
            FiveBandOnsets ob; ob.prepare(sr);
            std::vector<double> onsets; uint64_t seen = 0;
            auto kicks = verseChorus([&](const float* L, const float* R, double) {
                ob.pushBlock(L, R, blk);
                if (ob.onsetCount(0) != seen) { seen = ob.onsetCount(0); onsets.push_back(ob.lastOnsetSec(0)); }
            }, 29.0);
            const auto sc = score281(kicks, onsets);
            std::printf("v0.28.1 fiveband verse/chorus: TP=%d FP=%d FN=%d (of %d kicks)\n", sc.tp, sc.fp, sc.fn, (int)kicks.size());
            CHECK(sc.tp >= 36); CHECK(sc.fn <= 11); CHECK(sc.fp <= 1);
        }
        {   // Breakdown honesty: kicks stop for 6 s mid-song, the
            // bassline plays on. The adaptive gate buys verse hearing
            // at a measured price — a few on-beat bass notes fire as
            // grid confirmations (v0.28.1 measured 4; v0.28.0 measured
            // 0 but could not hear verses at all). Bound it, and pin
            // that the grid survives at the true tempo.
            BeatTracker tr; tr.prepare(sr);
            const double period = 0.6;
            std::vector<float> L(blk), R(blk);
            double tNow = 0; int breakOnsets = 0;
            const int blocks = (int)std::lround(22.0 * sr / blk);
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) {
                    const double tt = tNow + i / sr;
                    double v = 0.008 * std::sin(2 * M_PI * 440.0 * tt);
                    const bool kickTime = (tt < 8.0 || tt >= 14.0);
                    double klt = std::fmod(tt - 0.35, period); if (klt < 0) klt += period;
                    if (kickTime) v += 0.85 * std::sin(2 * M_PI * 58.0 * klt) * kEnv281(klt, 0.30, 0.004);
                    double hlt = std::fmod(tt - 0.35, period / 2); if (hlt < 0) hlt += period / 2;
                    const int step = (int)std::floor((tt - 0.35) / (period / 2));
                    const double bf = (step % 4 == 0) ? 55.0 : (step % 4 == 2) ? 65.4 : 49.0;
                    v += 0.42 * std::sin(2 * M_PI * bf * tt) * kEnv281(hlt, period / 2 * 0.9, 0.01);
                    L[(size_t)i] = R[(size_t)i] = (float)v;
                }
                if (tr.pushBlock(L.data(), R.data(), blk) && tNow + blk / sr >= 8.6 && tNow + blk / sr < 13.8) ++breakOnsets;
                tNow += blk / sr;
            }
            std::printf("v0.28.1 tracker breakdown: onsets inside the kick-less window = %d (bound 6)\n", breakOnsets);
            CHECK(breakOnsets <= 6);
            CHECK(tr.locked()); CHECK(std::abs(tr.bpm() - 100.0) < 3.0);
        }
        // --- Measure phase: accent evidence or silence ---
        auto accentRun = [&](int bpb, int accentBeat, double secs, BeatTracker& tr) {
            tr.prepare(sr); tr.setMeter(bpb, 1);
            const double period = 0.6;
            std::vector<float> L(blk), R(blk);
            double tNow = 0;
            const int blocks = (int)std::lround(secs * sr / blk);
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < blk; ++i) {
                    const double tt = tNow + i / sr;
                    double v = 0.008 * std::sin(2 * M_PI * 440.0 * tt);
                    double klt = std::fmod(tt - 0.35, period); if (klt < 0) klt += period;
                    const long beatIdx = (long)std::floor((tt - 0.35) / period);
                    const int songBeat = (int)(beatIdx % bpb) + 1;
                    const double amp = accentBeat == 0 ? 0.85 : (songBeat == accentBeat ? 0.95 : 0.55);
                    v += amp * std::sin(2 * M_PI * 58.0 * klt) * kEnv281(klt, 0.30, 0.004);
                    double hlt = std::fmod(tt - 0.35, period / 2); if (hlt < 0) hlt += period / 2;
                    v += 0.30 * std::sin(2 * M_PI * 55.0 * tt) * kEnv281(hlt, period / 2 * 0.9, 0.01);
                    L[(size_t)i] = R[(size_t)i] = (float)v;
                }
                tr.pushBlock(L.data(), R.data(), blk);
                tNow += blk / sr;
            }
        };
        {   // Accents on song beat 1 (4/4): a phase IS proven, and —
            // the part that matters — the beats the DRAW path would
            // emphasise (collectRecentBeats counts == downbeatCount)
            // are exactly the accented song beats. The count NUMBER
            // itself is frame-relative and not asserted.
            BeatTracker tr; accentRun(4, 1, 30.0, tr);
            std::printf("v0.28.1 phase accents-on-1: downbeatCount=%d margin=%.3f\n", tr.downbeatCount(), (double)tr.phaseMargin());
            CHECK(tr.downbeatCount() > 0);
            CHECK(tr.phaseMargin() >= 0.15f);
            float ages[64]; int counts[64];
            const int nb = tr.collectRecentBeats(12.f, ages, counts, 64);
            CHECK(nb >= 12);
            const double now = tr.nowSec();
            int emphasised = 0, emphasisedOnAccent = 0;
            for (int i = 0; i < nb; ++i) {
                if (counts[i] != tr.downbeatCount()) continue;
                ++emphasised;
                const double beatT = now - ages[i];
                const long beatIdx = std::lround((beatT - 0.35) / 0.6);
                if ((int)(beatIdx % 4) + 1 == 1) ++emphasisedOnAccent;
            }
            std::printf("v0.28.1 phase draw consistency: %d emphasised beats, %d on the accented song beat\n", emphasised, emphasisedOnAccent);
            CHECK(emphasised >= 2);
            CHECK(emphasisedOnAccent == emphasised);
        }
        {   // Uniform kicks: NO phase may be proven (the v0.26.1 rule
            // survives as the tracker's silence when it has no evidence).
            BeatTracker tr; accentRun(4, 0, 30.0, tr);
            std::printf("v0.28.1 phase uniform: downbeatCount=%d margin=%.3f (must stay unproven)\n", tr.downbeatCount(), (double)tr.phaseMargin());
            CHECK(tr.downbeatCount() == 0);
        }
        {   // 3/4 with accents on 1: proven in the 3-count frame.
            BeatTracker tr; accentRun(3, 1, 30.0, tr);
            std::printf("v0.28.1 phase 3/4 accents-on-1: downbeatCount=%d margin=%.3f\n", tr.downbeatCount(), (double)tr.phaseMargin());
            CHECK(tr.downbeatCount() > 0);
        }
        // --- Stereo colour enum: stored ints must never renumber ---
        CHECK((int)StereoColor::Single == 0 && (int)StereoColor::Rgb == 1 && (int)StereoColor::MultiBand == 2);
        CHECK((int)StereoColor::Rainbow == 3 && (int)StereoColor::Sunset == 4 && (int)StereoColor::Ice == 5);
        // --- Undertow composite arithmetic (the v0.28.1 visibility
        // fix, model side): a loud frame's model alphas, multiplied
        // by the NEW crisp opacity (0.85, no scrim), must clear a
        // conservative visibility floor; multiplied by the OLD
        // stack (0.30 crisp x 0.82 scrim) the same frame sat under
        // it — the arithmetic half of the invisibility diagnosis.
        // (The renderer constants themselves are app-side and ride
        // the Windows build; the on-screen result is Jackson's to
        // grade — see the round receipt's NOT VERIFIED section.)
        {
            FiveBandLevels lv; for (int b = 0; b < 5; ++b) lv.band[b] = 0.8f;
            UndertowModel m; UndertowOnsetFeed none;
            for (int i = 0; i < 240; ++i) {
                UndertowBeatFeed bf; bf.locked = true; bf.confidence = 0.9f; bf.bpm = 100.f;
                bf.secondsToNextBeat = 0.3f; bf.nextIsDownbeat = (i % 60) == 0;
                bf.countInBar = (i % 4) + 1; bf.beatsPerBar = 4;
                bf.trackerNowSec = i / 60.0; bf.lastGridBeatSec = i / 60.0;
                m.update(1.f / 60.f, lv, -8.f, bf, none);
            }
            const float memA = m.membraneFillAlpha() * 0.55f * m.displayGain();
            std::printf("v0.28.1 visibility arithmetic: membrane alpha %.3f -> effective %.3f (new) vs %.3f (old stack)\n",
                        (double)memA, (double)(memA * 0.85f), (double)(memA * 0.30f * 0.82f));
            CHECK(memA * 0.85f >= 0.10f);        // new stack: visible over the black base
            CHECK(memA * 0.30f * 0.82f < 0.10f); // old stack: under the floor — the bug, in numbers
        }
    }

    // === v0.28.2 — Performance Mode (low-end laptop round, scope B:
    // toggle + Auto). Every effective limit is pinned here; the app
    // side carries no policy of its own. ===
    {
        // Default recipe: performanceMode is APPENDED; the default
        // is AUTO (2) — the urgent 2026-11-03 change: on Jackson's
        // laptop the app out-lags Settings itself within ~10 s, so
        // the rescue cannot wait for a click. Auto starts at Full.
        PresetState p; CHECK(p.performanceMode == 2);
        // The core's Full caps ARE the Undertow model's own constants.
        CHECK(kPerfFullSwarmCap == UndertowModel::kSwarmCap);
        CHECK(kPerfFullDustCap == UndertowModel::kDustCap);
        // --- Full: the user's own settings pass through unchanged ---
        { auto e = effectiveLimits(false, 60, true, true);
          CHECK(!e.lowActive); CHECK(e.fpsCap == 60);
          CHECK(e.bloomQuarter && e.bloomEighth);
          CHECK(e.vizMaxW == 480 && e.vizMaxH == 270);
          CHECK(e.scopePitchEvery == 1);
          CHECK(e.swarmCap == 240 && e.dustCap == 60);
          CHECK(e.sgScale == 1.0f); } // v0.28.6: full-res spectrogram cache at Full
        { auto e = effectiveLimits(false, 120, false, true);
          CHECK(e.fpsCap == 120); CHECK(!e.bloomQuarter && e.bloomEighth); }
        { auto e = effectiveLimits(false, 0, true, false);
          CHECK(e.fpsCap == 60); } // unset reads as the historical 60
        // --- Low-End: exactly the contract's caps ---
        { auto e = effectiveLimits(true, 60, true, true);
          CHECK(e.lowActive); CHECK(e.fpsCap == 30);
          CHECK(!e.bloomQuarter && !e.bloomEighth);
          CHECK(e.vizMaxW == 320 && e.vizMaxH == 180);
          CHECK(e.scopePitchEvery == 3);
          CHECK(e.swarmCap == 120 && e.dustCap == 30);
          CHECK(e.sgScale == 0.5f); } // v0.28.6: half-res spectrogram cache under Low-End (bilinear render keeps it smooth)
        { auto e = effectiveLimits(true, 120, true, true); CHECK(e.fpsCap == 30); }
        { auto e = effectiveLimits(true, 15, true, true); CHECK(e.fpsCap == 15); } // never raises a lower user limit
        // --- mode resolution ---
        CHECK(!perfLowActive(0, true));   // Full ignores the latch
        CHECK(perfLowActive(1, false));   // Low-End is immediate
        CHECK(!perfLowActive(2, false));  // Auto before the latch
        CHECK(perfLowActive(2, true));    // Auto after the latch
        CHECK(!perfLowActive(7, true));   // unknown persisted value -> Full
        // --- visualizer layer size under the caps ---
        { int w, h; auto f = effectiveLimits(false, 60, true, true);
          perfVizLayerSize(2200, 340, f, w, h); CHECK(w == 480 && h == 170);
          auto l = effectiveLimits(true, 60, true, true);
          perfVizLayerSize(2200, 340, l, w, h); CHECK(w == 320 && h == 170);
          perfVizLayerSize(400, 100, l, w, h);  CHECK(w == 200 && h == 50); }
        // --- scope pitch cadence ---
        { auto f = effectiveLimits(false, 60, true, true);
          CHECK(perfScopeCaptureDue(0, f) && perfScopeCaptureDue(1, f) && perfScopeCaptureDue(2, f));
          auto l = effectiveLimits(true, 60, true, true);
          CHECK(perfScopeCaptureDue(0, l)); CHECK(!perfScopeCaptureDue(1, l));
          CHECK(!perfScopeCaptureDue(2, l)); CHECK(perfScopeCaptureDue(3, l));
          CHECK(perfScopeCaptureDue(6, l)); }
        // --- Auto latch: 3 s of CONTINUOUS sag under 85% of target ---
        { PerfAutoLatch a;
          for (int i = 0; i < 100; ++i) a.update(59.0, 60.0, 0.1); // healthy: never
          CHECK(!a.latched()); CHECK(a.sagSeconds() == 0);
          for (int i = 0; i < 29; ++i) a.update(40.0, 60.0, 0.1);  // 2.9 s of sag: not yet
          CHECK(!a.latched());
          a.update(60.0, 60.0, 0.1);                              // one good frame resets the sag
          for (int i = 0; i < 29; ++i) a.update(40.0, 60.0, 0.1);
          CHECK(!a.latched());
          a.update(40.0, 60.0, 0.2);                              // crosses 3.0 s continuous
          CHECK(a.latched());
          for (int i = 0; i < 600; ++i) a.update(30.0, 30.0, 0.1); // session latch: no step-up, no flap
          CHECK(a.latched());
          a.reset(); CHECK(!a.latched()); }
        { PerfAutoLatch a; // persisted step-down: forceLatched re-arms
          a.forceLatched();                      // it at the next launch
          CHECK(a.latched()); CHECK(perfLowActive(2, a.latched()));
          for (int i = 0; i < 600; ++i) a.update(30.0, 30.0, 0.1); // clean Low-End frames never release it
          CHECK(a.latched()); }
        { PerfAutoLatch a; // unwarm EMA samples are ignored
          for (int i = 0; i < 100; ++i) a.update(0.0, 60.0, 0.1);
          CHECK(!a.latched()); }
        // --- Undertow runtime caps bind in the model itself ---
        { UndertowModel m; CHECK(m.swarmCap() == 240 && m.dustCap() == 60);
          m.setSwarmCap(9999); CHECK(m.swarmCap() == 240);
          m.setDustCap(-5);    CHECK(m.dustCap() == 0);
          m.setSwarmCap(120); m.setDustCap(30);
          CHECK(m.swarmCap() == 120 && m.dustCap() == 30);
          FiveBandLevels lv; for (int b = 0; b < 5; ++b) lv.band[b] = 0.8f;
          UndertowOnsetFeed none;
          int maxAlive = 0;
          for (int i = 0; i < 600; ++i) {
              UndertowBeatFeed bf; bf.locked = true; bf.confidence = 0.9f; bf.bpm = 100.f;
              bf.secondsToNextBeat = 0.3f; bf.nextIsDownbeat = (i % 60) == 0;
              bf.countInBar = (i % 4) + 1; bf.beatsPerBar = 4;
              bf.trackerNowSec = i / 60.0; bf.lastGridBeatSec = i / 60.0;
              m.update(1.f / 60.f, lv, -8.f, bf, none);
              int alive = 0; for (auto& d : m.swarm()) if (d.alive) ++alive;
              if (alive > maxAlive) maxAlive = alive;
          }
          std::printf("v0.28.2 undertow caps: max swarm alive=%d (cap 120), dust=%d (cap 30)\n", maxAlive, m.dustCount());
          CHECK(maxAlive <= 120); CHECK(maxAlive > 30); // the cap binds and the swarm still lives
          CHECK(m.dustCount() <= 30); }
        { UndertowModel m; // the same loud feed at Full caps: dust runs past the Low-End cap
          FiveBandLevels lv; for (int b = 0; b < 5; ++b) lv.band[b] = 0.8f;
          UndertowOnsetFeed none;
          for (int i = 0; i < 120; ++i) {
              UndertowBeatFeed bf; bf.locked = true; bf.confidence = 0.9f; bf.bpm = 100.f;
              bf.secondsToNextBeat = 0.3f; bf.countInBar = (i % 4) + 1; bf.beatsPerBar = 4;
              bf.trackerNowSec = i / 60.0; bf.lastGridBeatSec = i / 60.0;
              m.update(1.f / 60.f, lv, -8.f, bf, none);
          }
          std::printf("v0.28.2 undertow full: dust=%d (must exceed the Low-End cap 30)\n", m.dustCount());
          CHECK(m.dustCount() > 30); }
    }

    // === v0.28.3 — CrashWatch: crash-record naming + next-launch
    // notice logic (family crash-log contract). The handler writes
    // crash-YYYY-MM-DD_HHMMSS.txt (+ .dmp); a crash is UNSEEN until
    // a "<name>.seen" sibling exists.
    {
        CrashTimeParts t; t.year = 2026; t.month = 11; t.day = 3; t.hour = 11; t.minute = 48; t.second = 6;
        CHECK(crashFileStem(t) == "crash-2026-11-03_114806");
        CHECK(crashTextName(t) == "crash-2026-11-03_114806.txt");
        CHECK(crashDumpName(t) == "crash-2026-11-03_114806.dmp");
        CrashTimeParts pad; pad.year = 2027; pad.month = 1; pad.day = 2; pad.hour = 3; pad.minute = 4; pad.second = 5;
        CHECK(crashFileStem(pad) == "crash-2027-01-02_030405"); // zero padding everywhere
        CHECK(isCrashTextName("crash-2026-11-03_114806.txt"));
        CHECK(!isCrashTextName("crash-2026-11-03_114806.dmp"));
        CHECK(!isCrashTextName("capture-log.txt"));
        CHECK(!isCrashTextName("crash-2026-11-03_114806.txt.seen")); // the marker is not itself a crash
        CHECK(seenMarkerFor("crash-2026-11-03_114806.txt") == "crash-2026-11-03_114806.txt.seen");
        CHECK(isSeenMarkerName("crash-2026-11-03_114806.txt.seen"));
        CHECK(!isSeenMarkerName("notes.txt.seen"));
        // unseen: only crash .txt files lacking a .seen sibling, oldest first
        auto unseen = unseenCrashes({ "capture-log.txt",
                                      "crash-2026-11-03_114806.txt",
                                      "crash-2026-11-02_090000.txt", "crash-2026-11-02_090000.txt.seen",
                                      "crash-2026-11-03_120000.txt",
                                      "crash-2026-11-03_114806.dmp", "perf-auto.json" });
        CHECK(unseen.size() == 2);
        if (unseen.size() == 2) { // guard: CHECK does not abort — never index an unchecked vector
            CHECK(unseen[0] == "crash-2026-11-03_114806.txt");
            CHECK(unseen[1] == "crash-2026-11-03_120000.txt");
        }
        CHECK(unseenCrashes({}).empty());
        CHECK(unseenCrashes({ "crash-2026-11-03_114806.txt", "crash-2026-11-03_114806.txt.seen" }).empty());
        // record text: carries version, windows, time, exception, snapshot
        auto rec = crashRecordText("0.28.3", "10.0 build 26200", "2026-11-03_114806", 0xC0000374ul,
                                   "0x00007fff00000000", "audio source: System Loopback (app source 3)\n");
        CHECK(rec.find("app version: 0.28.3") != std::string::npos);
        CHECK(rec.find("windows: 10.0 build 26200") != std::string::npos);
        CHECK(rec.find("exception: 0xc0000374") != std::string::npos);
        CHECK(rec.find("audio source: System Loopback") != std::string::npos);
        auto recEmpty = crashRecordText("0.28.3", "10.0 build 26200", "2026-11-03_114806", 0ul, "0x0", "");
        CHECK(recEmpty.find("(none recorded)") != std::string::npos);
    }

    // === v0.28.4 — CrashWatch WER harvest: the handler never fires
    // for heap-corruption deaths (ntdll fast-fail -> WER), so new
    // MeterBar dumps in %LOCALAPPDATA%\CrashDumps are harvested on
    // launch into crash-record names. Pinned here: source-name
    // recognition, harvest keys (copy exactly once), +1s carry,
    // collision-free destination stems.
    {
        // source names: WER writes MeterBar.exe.<pid>.dmp (or bare)
        CHECK(isWerDumpName("MeterBar.exe.39596.dmp"));
        CHECK(isWerDumpName("MeterBar.exe.dmp"));
        CHECK(!isWerDumpName("MeterBar.exe.dmp.tmp"));
        CHECK(!isWerDumpName("MeterBar.exe.abc.dmp"));       // pid suffix must be digits
        CHECK(!isWerDumpName("MeterBar.exe..dmp"));          // empty pid
        CHECK(!isWerDumpName("OtherApp.exe.39596.dmp"));     // not our dumps
        CHECK(!isWerDumpName("crash-2026-11-03_114806.dmp")); // handler names are not sources
        CHECK(!isWerDumpName("MeterBar.exe.39596.txt"));
        // keys: name + size + write time; whole-line membership
        CrashTimeParts mt; mt.year = 2026; mt.month = 11; mt.day = 3; mt.hour = 14; mt.minute = 4; mt.second = 59;
        const auto k1 = werHarvestKey("MeterBar.exe.39596.dmp", 3803091, mt);
        CHECK(k1 == "MeterBar.exe.39596.dmp|3803091|2026-11-03_140459");
        CHECK(werHarvestKey("MeterBar.exe.39596.dmp", 3803092, mt) != k1); // size differs
        CHECK(werHarvestKey("MeterBar.exe.39597.dmp", 3803091, mt) != k1); // pid differs
        const std::string log = "MeterBar.exe.111.dmp|100|2026-11-03_010203\n" + k1 + "\n";
        CHECK(werHarvestKeySeen(log, k1));
        CHECK(werHarvestKeySeen(log, "MeterBar.exe.111.dmp|100|2026-11-03_010203"));
        CHECK(!werHarvestKeySeen(log, "MeterBar.exe.39596.dmp|3803091|2026-11-03_14045"));  // prefix is not a line
        CHECK(!werHarvestKeySeen(log, "MeterBar.exe.222.dmp|100|2026-11-03_010203"));
        CHECK(!werHarvestKeySeen("", k1));
        // +1s carry across every boundary
        CrashTimeParts a; a.year = 2026; a.month = 11; a.day = 3; a.hour = 14; a.minute = 4; a.second = 58;
        CHECK(crashFileStem(addOneSecond(a)) == "crash-2026-11-03_140459");
        a.second = 59;
        CHECK(crashFileStem(addOneSecond(a)) == "crash-2026-11-03_140500"); // minute carry
        CrashTimeParts b; b.year = 2026; b.month = 1; b.day = 31; b.hour = 23; b.minute = 59; b.second = 59;
        CHECK(crashFileStem(addOneSecond(b)) == "crash-2026-02-01_000000"); // month carry
        CrashTimeParts c; c.year = 2028; c.month = 2; c.day = 28; c.hour = 23; c.minute = 59; c.second = 59;
        CHECK(crashFileStem(addOneSecond(c)) == "crash-2028-02-29_000000"); // leap year
        CrashTimeParts d; d.year = 2026; d.month = 2; d.day = 28; d.hour = 23; d.minute = 59; d.second = 59;
        CHECK(crashFileStem(addOneSecond(d)) == "crash-2026-03-01_000000"); // non-leap
        CrashTimeParts e; e.year = 2026; e.month = 12; e.day = 31; e.hour = 23; e.minute = 59; e.second = 59;
        CHECK(crashFileStem(addOneSecond(e)) == "crash-2027-01-01_000000"); // year carry
        // stems: free -> as-is; clash on either sibling -> bump
        CrashTimeParts t; t.year = 2026; t.month = 11; t.day = 3; t.hour = 14; t.minute = 4; t.second = 59;
        CHECK(harvestStem({}, t) == "crash-2026-11-03_140459");
        CHECK(harvestStem({ "crash-2026-11-03_140459.txt" }, t) == "crash-2026-11-03_140500");
        CHECK(harvestStem({ "crash-2026-11-03_140459.dmp" }, t) == "crash-2026-11-03_140500");
        CHECK(harvestStem({ "crash-2026-11-03_140459.txt", "crash-2026-11-03_140500.dmp" }, t)
              == "crash-2026-11-03_140501"); // double clash bumps twice
        // a harvested record is an ordinary unseen crash to the notice
        auto unseenH = unseenCrashes({ harvestStem({}, t) + ".txt", harvestStem({}, t) + ".dmp" });
        CHECK(unseenH.size() == 1);
    }

    // === v0.28.4 — activation construction pin (D-SEQ-1). The core
    // cannot call Windows APIs, but it CAN read the app source when
    // run from a tree checkout (the release CI runs this suite from
    // the repo root): the segment between the activation call and
    // the wait must contain NO PropVariantClear(&prop) — the blob
    // payload is the caller's stack struct and is never ours to
    // clear — while the unrelated GetValue clear in friendlyName
    // (payload owned by the caller there) must still be present,
    // proving the scan is reading the right file. If the source is
    // not reachable the pin SKIPS with a printed note (checked by
    // the round's local run + CI, where it does execute).
    {
        std::string src;
        const char* candidates[] = { "app/Source/WindowsLoopback.cpp",
                                     "../app/Source/WindowsLoopback.cpp",
                                     "../../app/Source/WindowsLoopback.cpp" };
        for (auto* path : candidates) {
            std::FILE* fh = std::fopen(path, "rb");
            if (!fh) continue;
            char chunk[8192]; std::size_t n;
            while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) src.append(chunk, n);
            std::fclose(fh);
            if (!src.empty()) break;
        }
        if (src.empty()) {
            std::printf("v0.28.4 activation pin SKIPPED (WindowsLoopback.cpp not reachable from CWD)\n");
        } else {
            const auto callPos = src.find("ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK");
            const auto waitPos = src.find("WaitForSingleObject(handler->done", callPos);
            CHECK(callPos != std::string::npos && waitPos != std::string::npos && waitPos > callPos);
            if (callPos != std::string::npos && waitPos != std::string::npos && waitPos > callPos) {
                const std::string seg = src.substr(callPos, waitPos - callPos);
                CHECK(seg.find("PropVariantClear(&prop)") == std::string::npos); // D-SEQ-1 stays gone
            }
            CHECK(src.find("PropVariantClear(&v)") != std::string::npos); // the owned-payload clear stays
        }
    }

    // === BLK7 (v0.28.4) — link-direction fix: nine-field packet,
    // diverged gate, claim restore, group-dock parent exemption,
    // planLinkAll, parent-untouched scenario ===
    {
        using namespace meterbar;
        const long long now = 400000;
        CHECK(kLinkMagic == 0x424C4B37u);
        // helper: a live linked slot with a full nine-field packet
        auto mkSlot = [&](LinkShared& sh, int idx, int app, unsigned lseq, unsigned bgseq) {
            auto& sl = sh.slots[idx]; sl.appId = app; sl.linked = 1; sl.heartbeatMs = now; sl.linkSeq = lseq;
            sl.rect = { app * 100, 0, 800, 40 };
            sl.bgSeq = bgseq; sl.bgStyle = 2; sl.bgOpacityPct = 40; sl.bgEffect = 3;
            sl.bgTheme = 1; sl.bgScheme = 5;
            sl.bgPalHue = 0.25f; sl.bgPalSat = 0.5f; sl.bgPalSpread = 0.75f; sl.bgPalCycle = 12.f;
        };
        // --- nine-field adoption downhill; parent never adopts ---
        {
            LinkShared sh; mkSlot(sh, 0, 0, 1, 7); mkSlot(sh, 2, 2, 2, 9);
            BgSyncState sy; BgPacket out;
            CHECK(bgAdoption(sh, 2, sy, now, out));
            CHECK(out.style == 2 && out.opacityPct == 40 && out.effect == 3);
            CHECK(out.theme == 1 && out.scheme == 5);
            CHECK(out.palHue == 0.25f && out.palSat == 0.5f && out.palSpread == 0.75f && out.palCycle == 12.f);
            CHECK(!bgIsLocalChange(sy, out)); // adopted = published: no echo on any of the nine
            CHECK(!bgAdoption(sh, 2, sy, now, out)); // converged: gate shut
            BgSyncState parentSy;
            CHECK(!bgAdoption(sh, 0, parentSy, now, out)); // the parent adopts from NOBODY
            // nine-field local-change gate: each extension field moves it
            BgPacket v = out;
            v.theme = 2; CHECK(bgIsLocalChange(sy, v)); v = out;
            v.scheme = 3; CHECK(bgIsLocalChange(sy, v)); v = out;
            v.palCycle = 99.f; CHECK(bgIsLocalChange(sy, v));
        }
        // --- diverged gate (defect-6 port): a follower-side local
        // change is re-converged to the parent's standing packet ---
        {
            LinkShared sh; mkSlot(sh, 0, 0, 1, 7); mkSlot(sh, 2, 2, 2, 9);
            BgSyncState sy; BgPacket out;
            CHECK(bgAdoption(sh, 2, sy, now, out));
            // follower changes its own theme locally (published markers diverge)
            sy.publishedTheme = 4; sy.publishedPalHue = 0.9f;
            CHECK(bgAdoption(sh, 2, sy, now, out)); // same bgSeq, still re-adopts
            CHECK(out.theme == 1 && out.palHue == 0.25f);
            CHECK(!bgAdoption(sh, 2, sy, now, out));
            // bgForceRepublish drops ALL nine markers to the sentinel
            bgForceRepublish(sy);
            CHECK(sy.publishedTheme == -999 && sy.publishedScheme == -999 && sy.publishedPalCycle == -999.f);
            CHECK(bgAdoption(sh, 2, sy, now, out)); // forced republish state also re-converges
        }
        // --- claim restore (DESIGN §4) ---
        {
            LinkShared sh;
            CHECK(claimFor(sh, 0, now) == 0); // no record -> fresh claim
            sh.lastClaim[0] = 5;
            CHECK(claimFor(sh, 0, now) == 5); // recorded, nobody holds it -> restore
            auto& holder = sh.slots[1]; holder.appId = 1; holder.linked = 1; holder.heartbeatMs = now; holder.linkSeq = 5;
            CHECK(claimFor(sh, 0, now) == 0); // LIVE holder collision -> fresh claim
            holder.heartbeatMs = now - kPeerStaleMs - 1;
            CHECK(claimFor(sh, 0, now) == 5); // stale holder does not block restore
            sh.lastClaim[0] = 0; // deliberate-unlink clear (client OFF edge)
            CHECK(claimFor(sh, 0, now) == 0);
            CHECK(claimFor(sh, 7, now) == 0); // out-of-range app
        }
        // --- group-dock parent exemption (DESIGN §5) ---
        {
            LinkShared sh; mkSlot(sh, 0, 0, 1, 7); mkSlot(sh, 1, 1, 2, 3);
            CHECK(!groupDockMovesSelf(sh, 0, 1, now)); // parent: no automatic move
            CHECK(groupDockMovesSelf(sh, 1, 2, now));  // follower: moves (unchanged)
        }
        // --- planLinkAll (DESIGN §6c) ---
        {
            LinkShared sh; IntRect targets[3];
            auto live = [&](int i, int h) { auto& sl = sh.slots[i]; sl.appId = i; sl.linked = 0; sl.heartbeatMs = now; sl.rect = { 50, 50, 300, h }; };
            live(0, 40); live(1, 36); live(2, 44); // running-but-unlinked bars ARE participants
            IntRect area{ 100, 200, 1920, 1080 };
            CHECK(planLinkAll(sh, now, 0, area, targets) == 3); // Top
            CHECK(targets[0].x == 100 && targets[0].y == 200 && targets[0].w == 1920 && targets[0].h == 40);
            CHECK(targets[1].y == 240 && targets[1].h == 36);
            CHECK(targets[2].y == 276 && targets[2].h == 44); // contiguous, heights preserved
            CHECK(planLinkAll(sh, now, 1, area, targets) == 3); // Bottom
            CHECK(targets[2].bottom() == 1280 && targets[2].y == 1236);
            CHECK(targets[1].bottom() == targets[2].y && targets[0].bottom() == targets[1].y); // order stays 0,1,2 top-to-bottom
            CHECK(targets[0].w == 1920 && targets[1].w == 1920 && targets[2].w == 1920);
            // middle bar absent
            sh.slots[1].heartbeatMs = now - kPeerStaleMs - 1;
            CHECK(planLinkAll(sh, now, 0, area, targets) == 2);
            CHECK(targets[1].w == 0);
            CHECK(targets[2].y == targets[0].bottom()); // gap closed
            // only MeterBar
            sh.slots[2].heartbeatMs = now - kPeerStaleMs - 1;
            CHECK(planLinkAll(sh, now, 1, area, targets) == 1);
            CHECK(targets[0].bottom() == 1280 && targets[0].h == 40);
            CHECK(planLinkAll(sh, now, 0, IntRect{}, targets) == 0); // degenerate area
        }
        // --- pulse packet (DESIGN §7) ---
        {
            LinkSlot sl; sl.appId = 0; sl.heartbeatMs = now;
            unsigned seq = 0; int col = 0; float str = 0.f;
            CHECK(!pulsePacket(sl, now, seq, col, str)); // never stamped
            sl.pulseSeq = 3; sl.pulseColour = 0xE04C89; sl.pulseStrength = 0.7f;
            CHECK(pulsePacket(sl, now, seq, col, str));
            CHECK(seq == 3 && col == 0xE04C89 && str == 0.7f);
        }
        // --- PARENT-UNTOUCHED SCENARIO (DESIGN §10): join ->
        // follower-change -> parent scenario; the parent's sync
        // state and published packet stay untouched, no adoption
        // ever fires for it ---
        {
            LinkShared sh;
            mkSlot(sh, 0, 0, 1, 7); // parent (MeterBar, first-clicked)
            // the parent's own published packet, stamped in its slot
            BgSyncState parentSy;
            parentSy.publishedStyle = 2; parentSy.publishedPct = 40; parentSy.publishedEffect = 3;
            parentSy.publishedTheme = 1; parentSy.publishedScheme = 5;
            parentSy.publishedPalHue = 0.25f; parentSy.publishedPalSat = 0.5f;
            parentSy.publishedPalSpread = 0.75f; parentSy.publishedPalCycle = 12.f;
            const BgSyncState parentBefore = parentSy;
            const LinkSlot parentSlotBefore = sh.slots[0];
            // follower joins and adopts
            mkSlot(sh, 1, 1, 2, 1);
            sh.slots[1].bgStyle = 0; sh.slots[1].bgTheme = 3; // follower's own (different) packet
            BgSyncState folSy; BgPacket out;
            CHECK(bgAdoption(sh, 1, folSy, now, out));
            CHECK(out.theme == 1); // downhill: parent's theme wins
            // follower changes locally; parent re-checks — nothing fires,
            // nothing in the parent's state or slot moves
            folSy.publishedTheme = 0;
            CHECK(!bgAdoption(sh, 0, parentSy, now, out));
            CHECK(parentSy.publishedStyle == parentBefore.publishedStyle
               && parentSy.publishedTheme == parentBefore.publishedTheme
               && parentSy.publishedPalCycle == parentBefore.publishedPalCycle
               && parentSy.adoptedFrom == -1);
            CHECK(sh.slots[0].bgSeq == parentSlotBefore.bgSeq && sh.slots[0].bgTheme == parentSlotBefore.bgTheme);
            CHECK(!groupDockMovesSelf(sh, 0, 1, now)); // and the automatic dock cannot move it either
        }
    }

    // === v0.28.6 — feel-fixes round (decided subset) ===
    {
        using namespace meterbar;
        // --- bilinear spectrogram sampling (item: blockiness fix).
        // A 4-bin x 3-row history with known rows; pushRow at speed 5
        // advances exactly one row per push. Newest row = last push.
        SpectrogramHistory h; h.prepare(4, 3);
        h.pushRow({ 0.f, 10.f, 20.f, 30.f }, 5.f);     // oldest
        h.pushRow({ 40.f, 50.f, 60.f, 70.f }, 5.f);
        h.pushRow({ 80.f, 90.f, 100.f, 110.f }, 5.f);  // newest (age 0)
        CHECK(h.at(0, 0) == 80.f && h.at(2, 3) == 30.f); // fixture sanity
        // integer coordinates sample their own cell exactly
        CHECK(std::abs(spectrogramSampleBilinear(h, 0.f, 0.f) - 80.f) < 0.001f);
        CHECK(std::abs(spectrogramSampleBilinear(h, 2.f, 3.f) - 30.f) < 0.001f);
        // midpoints blend the two neighbours (the anti-blocky property)
        CHECK(std::abs(spectrogramSampleBilinear(h, 0.5f, 0.f) - 60.f) < 0.001f);  // (80+40)/2 along age
        CHECK(std::abs(spectrogramSampleBilinear(h, 0.f, 1.5f) - 95.f) < 0.001f);  // (90+100)/2 along bin
        CHECK(std::abs(spectrogramSampleBilinear(h, 0.25f, 0.5f) - 75.f) < 0.001f); // full bilinear blend
        // out-of-range coordinates clamp to the edge cells
        CHECK(std::abs(spectrogramSampleBilinear(h, -3.f, 99.f) - 110.f) < 0.001f);
        CHECK(std::abs(spectrogramSampleBilinear(h, 99.f, -1.f) - 0.f) < 0.001f);
        // an unprepared history reads as silence, never garbage
        SpectrogramHistory empty;
        CHECK(spectrogramSampleBilinear(empty, 0.f, 0.f) == -120.f);
    }
    {
        // --- app-source pins for the items the core cannot execute
        // (same pattern as the v0.28.4 activation pin: read the app
        // source when run from a tree checkout; SKIP with a note
        // when unreachable).
        auto readApp = [](const char* rel) {
            std::string src;
            const std::string base(rel);
            const char* prefixes[] = { "", "../", "../../" };
            for (auto* pre : prefixes) {
                std::FILE* fh = std::fopen((std::string(pre) + base).c_str(), "rb");
                if (!fh) continue;
                char chunk[8192]; std::size_t n;
                while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) src.append(chunk, n);
                std::fclose(fh);
                if (!src.empty()) break;
            }
            return src;
        };
        const std::string mv = readApp("app/Source/ModuleViews.h");
        const std::string tp = readApp("app/Source/TextPop.h");
        const std::string sh = readApp("app/Source/Settings.h");
        const std::string mc = readApp("app/Source/MainComponent.cpp");
        if (mv.empty() || tp.empty() || sh.empty() || mc.empty()) {
            std::printf("v0.28.6 app-source pins SKIPPED (app sources not reachable from CWD)\n");
        } else {
            // item 1: textpop::drawSingle exists, and drawLoudness's
            // readout column uses ONLY single draws (no drawPoppedText
            // between drawLoudness and drawStereo)
            CHECK(tp.find("drawSingle") != std::string::npos);
            const auto l0 = mv.find("void drawLoudness");
            const auto l1 = mv.find("void drawStereo", l0);
            CHECK(l0 != std::string::npos && l1 != std::string::npos && l1 > l0);
            if (l0 != std::string::npos && l1 != std::string::npos && l1 > l0) {
                const std::string seg = mv.substr(l0, l1 - l0);
                CHECK(seg.find("drawSingleText") != std::string::npos);
                CHECK(seg.find("drawPoppedText") == std::string::npos);
            }
            // item 4: the spectrogram render samples bilinearly
            CHECK(mv.find("spectrogramSampleBilinear") != std::string::npos);
            // item 5: the stereometer plot is the full cell rect
            CHECK(mv.find("auto sq = p;") != std::string::npos);
            // item 6: Settings headings paint through the text pop
            CHECK(sh.find("PoppedHeading") != std::string::npos);
            CHECK(sh.find("textpop::draw") != std::string::npos);
            // items 2+3: the bar trim slider exists, and BOTH Settings
            // open paths (toolbar gear + menu item) route the toggle
            CHECK(mc.find("trimSlider_") != std::string::npos);
            CHECK(mc.find("void MainComponent::toggleSettings()") != std::string::npos);
            CHECK(mc.find("idOpenSettings) { toggleSettings(); }") != std::string::npos);
            CHECK(mc.find("settingsBtn.onClick = [this]{ uiClick(ClickSounds::Which::Pop); toggleSettings(); }") != std::string::npos);
        }
    }

    // === v0.28.7 — pinned-app source (appSource 4): exe-name
    // policy, the attach/detach watch machine, and the state
    // default. The Windows behaviour itself (activation, process
    // enumeration) cannot run in this harness; the app-source
    // scan pins below cover the wiring it depends on.
    {
        using namespace meterbar;
        // exe names: a chooser hands back a full path; the pin
        // stores and matches the NAME part only
        CHECK(exeBaseName("C:\\Program Files\\Ableton\\Live 12 Suite\\Program\\Ableton Live 12 Suite.exe") == "Ableton Live 12 Suite.exe");
        CHECK(exeBaseName("C:/Program Files/Ableton/Ableton Live 12 Suite.exe") == "Ableton Live 12 Suite.exe");
        CHECK(exeBaseName("Spotify.exe") == "Spotify.exe");
        CHECK(exeBaseName("  TIDAL.exe  ") == "TIDAL.exe");
        CHECK(exeBaseName("").empty());
        CHECK(exeBaseName("   ").empty());
        // display label: base name minus the .exe suffix
        CHECK(pinDisplayLabel("Ableton Live 12 Suite.exe") == "Ableton Live 12 Suite");
        CHECK(pinDisplayLabel("ABLETON.EXE") == "ABLETON");
        CHECK(pinDisplayLabel("C:\\Apps\\My Tool.exe") == "My Tool");
        CHECK(pinDisplayLabel("noext") == "noext");
        CHECK(pinDisplayLabel("").empty());
        // the resolver's match rule: base names, case-insensitive
        CHECK(exeNamesMatch("spotify.exe", "Spotify.EXE"));
        CHECK(exeNamesMatch("C:\\x\\TIDAL.exe", "tidal.exe"));
        CHECK(!exeNamesMatch("Spotify.exe", "TIDAL.exe"));
        CHECK(!exeNamesMatch("", "Spotify.exe"));
        CHECK(!exeNamesMatch("Spotify.exe", ""));
        // state default: no pin, System source
        PresetState freshState;
        CHECK(freshState.pinnedExe.empty());
        CHECK(freshState.appSource == 0);
        // --- the watch machine: one action per edge, no churn ---
        {
            PinWatch w;
            // absent: the waiting state — no action, unattached
            CHECK(w.step(false, false, true) == PinAction::None);
            CHECK(!w.attached);
            // appeared inside the settle window: not yet
            CHECK(w.step(true, false, false) == PinAction::None);
            CHECK(!w.attached);
            // appeared and settled: attach exactly once
            CHECK(w.step(true, false, true) == PinAction::Attach);
            CHECK(w.attached);
            // activation in flight or failed: NO re-attach churn
            CHECK(w.step(true, false, true) == PinAction::None);
            // capture reports running: belief confirmed silently
            CHECK(w.step(true, true, true) == PinAction::None);
            CHECK(w.attached);
            // disappeared: detach exactly once, back to waiting
            CHECK(w.step(false, false, true) == PinAction::Detach);
            CHECK(!w.attached);
            CHECK(w.step(false, false, true) == PinAction::None);
            // reappeared: attach again
            CHECK(w.step(true, false, true) == PinAction::Attach);
            CHECK(w.attached);
        }
        {
            // startup: the constructor's applySource attached
            // before the first step (capture already running) —
            // latch the belief with NO action, and a later
            // disappearance still detaches
            PinWatch w;
            CHECK(w.step(true, true, false) == PinAction::None);
            CHECK(w.attached);
            CHECK(w.step(false, false, false) == PinAction::Detach);
            CHECK(!w.attached);
        }
    }
    {
        // --- app-source pins for the wiring (v0.28.4 pattern:
        // read the app source from a tree checkout; SKIP with a
        // note when unreachable — CI and local runs execute them).
        auto readApp = [](const char* rel) {
            std::string src;
            const std::string base(rel);
            const char* prefixes[] = { "", "../", "../../" };
            for (auto* pre : prefixes) {
                std::FILE* fh = std::fopen((std::string(pre) + base).c_str(), "rb");
                if (!fh) continue;
                char chunk[8192]; std::size_t n;
                while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) src.append(chunk, n);
                std::fclose(fh);
                if (!src.empty()) break;
            }
            return src;
        };
        const std::string pio = readApp("app/Source/PresetIO.h");
        const std::string sth = readApp("dsp/include/meterbar/State.hpp");
        const std::string eng = readApp("app/Source/AudioEngine.h");
        const std::string set = readApp("app/Source/Settings.h");
        const std::string mc  = readApp("app/Source/MainComponent.cpp");
        if (pio.empty() || sth.empty() || eng.empty() || set.empty() || mc.empty()) {
            std::printf("v0.28.7 app-source pins SKIPPED (app sources not reachable from CWD)\n");
        } else {
            // persistence: the pin rides PresetIO BOTH ways and
            // the appSource clamp admits 4 (an old clamp at 3
            // would silently un-pin every saved preset)
            CHECK(pio.find("set(\"pinnedExe\"") != std::string::npos);
            CHECK(pio.find("g(\"pinnedExe\")") != std::string::npos);
            CHECK(pio.find("as > 4 ? 4") != std::string::npos);
            CHECK(sth.find("std::string pinnedExe") != std::string::npos);
            // reuse: applySource sends the pin through the SAME
            // process-loopback activation as Spotify/Tidal, and
            // the pinned branch contains NO endpoint capture —
            // pinned mode can never fall back to the system mix
            const auto b0 = eng.find("state.appSource == 4) {");
            CHECK(b0 != std::string::npos);
            if (b0 != std::string::npos) {
                const auto b1 = eng.find("}", b0);
                const std::string seg = eng.substr(b0, b1 - b0);
                CHECK(seg.find("startProcessLoopback(cb, state.pinnedExe)") != std::string::npos);
                CHECK(seg.find("loopback.start(cb") == std::string::npos);
                CHECK(seg.find("state.pinnedExe.empty()") != std::string::npos); // empty pin starts nothing
            }
            // the waiting state is real UI text
            CHECK(eng.find("waiting for") != std::string::npos);
            CHECK(eng.find("never another app or the system mix") != std::string::npos);
            // Settings: the source menu shows the pin state and
            // carries the Pin / Unpin controls
            CHECK(set.find("Pinned app only (") != std::string::npos);
            CHECK(set.find("no pin set") != std::string::npos);
            CHECK(set.find("Pin an app...") != std::string::npos);
            CHECK(set.find("choosePinnedExe") != std::string::npos);
            CHECK(set.find("never falls back to another app or the system mix") != std::string::npos);
            // the watcher is wired: ~1 s step, attach via the
            // shared applySource, detach a bare stop, and the
            // pin's own persistence file (restart survival)
            CHECK(mc.find("pollPinnedSource(now)") != std::string::npos);
            CHECK(mc.find("PinAction::Attach") != std::string::npos);
            CHECK(mc.find("engine.applySource()") != std::string::npos);
            CHECK(mc.find("PinAction::Detach") != std::string::npos);
            CHECK(mc.find("engine.stopLoopback()") != std::string::npos);
            CHECK(mc.find("pin-source.json") != std::string::npos);
            CHECK(mc.find("loadPinPersist()") != std::string::npos);
            CHECK(mc.find("savePinPersist()") != std::string::npos);
        }
    }
    {
        // --- CloseSave (v0.28.8): the exit save prompt's dirty
        // check — the complete PresetState, exact field-by-field
        // (SessionBar parity). Identical states are NOT a change;
        // a mutation of ANY single field IS — one line per field,
        // so this block is the checklist that "complete" is real.
        const PresetState base;
        CHECK(sameState(base, base));
        CHECK(!changedSinceOpen(base, base));
        auto changed = [&base](auto fn) { PresetState c = base; fn(c); return changedSinceOpen(base, c); };
        CHECK(changed([](PresetState& c) { c.name = "Other"; }));
        CHECK(changed([](PresetState& c) { c.spectrogramEnabled = !c.spectrogramEnabled; }));
        CHECK(changed([](PresetState& c) { c.waveformEnabled = !c.waveformEnabled; }));
        CHECK(changed([](PresetState& c) { c.loudnessEnabled = !c.loudnessEnabled; }));
        CHECK(changed([](PresetState& c) { c.stereometerEnabled = !c.stereometerEnabled; }));
        CHECK(changed([](PresetState& c) { c.oscilloscopeEnabled = !c.oscilloscopeEnabled; }));
        CHECK(changed([](PresetState& c) { c.spectrumEnabled = !c.spectrumEnabled; }));
        CHECK(changed([](PresetState& c) { c.spectrogramOrientation = Orientation::Vertical; }));
        CHECK(changed([](PresetState& c) { c.fade += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.loopEnabled = !c.loopEnabled; }));
        CHECK(changed([](PresetState& c) { c.linkedSpeed += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.waveCh1 = ChanSel::Right; }));
        CHECK(changed([](PresetState& c) { c.waveCh2 = ChanSel::Left; }));
        CHECK(changed([](PresetState& c) { c.waveHistory = HistoryMode::Fast; }));
        CHECK(changed([](PresetState& c) { c.loudnessMode = LoudnessMode::RmsFast; }));
        CHECK(changed([](PresetState& c) { c.loudnessShowTargets = !c.loudnessShowTargets; }));
        CHECK(changed([](PresetState& c) { c.stereoMode = StereoMode::Lissajous; }));
        CHECK(changed([](PresetState& c) { c.stereoColor = StereoColor::Single; }));
        CHECK(changed([](PresetState& c) { c.stereoPointAuto = !c.stereoPointAuto; }));
        CHECK(changed([](PresetState& c) { c.stereoPointSize += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.scopeTrigger = ScopeTrigger::Free; }));
        CHECK(changed([](PresetState& c) { c.scopeDisplay = ScopeDisplay::Multi; }));
        CHECK(changed([](PresetState& c) { c.scopeWindowMs += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.spectrumStyle = PresetState::SpectrumStyle::Fft; }));
        CHECK(changed([](PresetState& c) { c.spectrumSmoothingPct += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.spectrumTiltDb += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.fftSize += 1; }));
        CHECK(changed([](PresetState& c) { c.fftScale = FreqScale::Linear; }));
        CHECK(changed([](PresetState& c) { c.spectrumShowReference = !c.spectrumShowReference; }));
        CHECK(changed([](PresetState& c) { c.spectrumShowTarget = !c.spectrumShowTarget; }));
        CHECK(changed([](PresetState& c) { c.spectrumColorMode = SpectrumColorMode::Frequency; }));
        CHECK(changed([](PresetState& c) { c.spectrumDisplayTiltDb += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.spectrumSixthOctave = !c.spectrumSixthOctave; }));
        CHECK(changed([](PresetState& c) { c.themeName = "Other"; }));
        CHECK(changed([](PresetState& c) { c.colorMap = ColorMapId::Plasma; }));
        CHECK(changed([](PresetState& c) { c.backgroundOpacity += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.backgroundStyle = BackgroundStyle::Flat; }));
        CHECK(changed([](PresetState& c) { c.beatPulseEnabled = !c.beatPulseEnabled; }));
        CHECK(changed([](PresetState& c) { c.beatTimeSignature += 1; }));
        CHECK(changed([](PresetState& c) { c.linkEnabled = !c.linkEnabled; }));
        CHECK(changed([](PresetState& c) { c.linkBackground = !c.linkBackground; }));
        CHECK(changed([](PresetState& c) { c.visualizerEnabled = !c.visualizerEnabled; }));
        CHECK(changed([](PresetState& c) { c.visualizerStyle += 1; }));
        CHECK(changed([](PresetState& c) { c.undertowBloomQuarter = !c.undertowBloomQuarter; }));
        CHECK(changed([](PresetState& c) { c.undertowBloomEighth = !c.undertowBloomEighth; }));
        CHECK(changed([](PresetState& c) { c.undertowPreArm = !c.undertowPreArm; }));
        CHECK(changed([](PresetState& c) { c.undertowKickTauMs += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.visualizerBaseHue += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.visualizerSaturation += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.visualizerSpread += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.visualizerCycleSec += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.animeClickSounds = !c.animeClickSounds; }));
        CHECK(changed([](PresetState& c) { c.mascotFlex += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.audioSource = "Other"; }));
        CHECK(changed([](PresetState& c) { c.appSource += 1; }));
        CHECK(changed([](PresetState& c) { c.pinnedExe = "Other.exe"; }));
        CHECK(changed([](PresetState& c) { c.audioDeviceName = "Other"; }));
        CHECK(changed([](PresetState& c) { c.outputDeviceName = "Other"; }));
        CHECK(changed([](PresetState& c) { c.outputDeviceId = "Other"; }));
        CHECK(changed([](PresetState& c) { c.trimDb += 0.25f; }));
        CHECK(changed([](PresetState& c) { c.routingPair += 1; }));
        CHECK(changed([](PresetState& c) { c.routingCh1 += 1; }));
        CHECK(changed([](PresetState& c) { c.routingCh2 += 1; }));
        CHECK(changed([](PresetState& c) { c.vsync = !c.vsync; }));
        CHECK(changed([](PresetState& c) { c.fpsLimit += 1; }));
        CHECK(changed([](PresetState& c) { c.performanceMode += 1; }));
        CHECK(changed([](PresetState& c) { c.moduleFlex[3] += 0.5f; }));
        CHECK(changed([](PresetState& c) { c.pinned = !c.pinned; }));
        CHECK(changed([](PresetState& c) { c.stick = !c.stick; }));
        CHECK(changed([](PresetState& c) { c.showToolbar = !c.showToolbar; }));
        CHECK(changed([](PresetState& c) { c.showRepositionHint = !c.showRepositionHint; }));
        CHECK(changed([](PresetState& c) { c.reserveScreenSpace = !c.reserveScreenSpace; }));
        CHECK(changed([](PresetState& c) { c.windowX += 7; })); // geometry alone is a change
        CHECK(changed([](PresetState& c) { c.windowY += 7; }));
        CHECK(changed([](PresetState& c) { c.windowW += 7; }));
        CHECK(changed([](PresetState& c) { c.windowH += 7; }));
        // an exact revert is NOT a change (floats compare exactly)
        { PresetState c = base; c.fade = 0.9f; c.fade = base.fade; c.windowX += 5; c.windowX = base.windowX;
          CHECK(!changedSinceOpen(base, c)); }
    }
    {
        // --- close-save wiring pins (v0.28.8, v0.28.4 pattern:
        // read the app source from a tree checkout; SKIP with a
        // note when unreachable — CI and local runs execute them).
        auto readApp = [](const char* rel) {
            std::string src;
            const std::string base(rel);
            const char* prefixes[] = { "", "../", "../../" };
            for (auto* pre : prefixes) {
                std::FILE* fh = std::fopen((std::string(pre) + base).c_str(), "rb");
                if (!fh) continue;
                char chunk[8192]; std::size_t n;
                while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) src.append(chunk, n);
                std::fclose(fh);
                if (!src.empty()) break;
            }
            return src;
        };
        const std::string mc  = readApp("app/Source/MainComponent.cpp");
        const std::string mn  = readApp("app/Source/Main.cpp");
        const std::string pio = readApp("app/Source/PresetIO.h");
        if (mc.empty() || mn.empty() || pio.empty()) {
            std::printf("v0.28.8 app-source pins SKIPPED (app sources not reachable from CWD)\n");
        } else {
            // the gate: every quit path routes through it, and the
            // prompt compares against the open baseline
            CHECK(mn.find("mainWindow->requestClose()") != std::string::npos);
            CHECK(mc.find("void MainComponent::requestClose()") != std::string::npos);
            CHECK(mc.find("changedSinceOpen(openBaseline_") != std::string::npos);
            CHECK(mc.find("Continue Without Saving") != std::string::npos);
            // the ONE save function serves the menu and the prompt
            CHECK(mc.find("void MainComponent::performSave(bool saveAs") != std::string::npos);
            CHECK(mc.find("performSave(saveAs, [](bool) {})") != std::string::npos);
            // startup restore + the last-state record beside the presets
            CHECK(mc.find("presetio::loadLastState(engine.state)") != std::string::npos);
            CHECK(pio.find("last-state.json") != std::string::npos);
            // the audit's latent gap closed: loop rides presets BOTH ways
            CHECK(pio.find("set(\"loop\"") != std::string::npos);
            CHECK(pio.find("gb(\"loop\"") != std::string::npos);
            // standing rule: the updater entry stays in the menu
            CHECK(mc.find("Check For Updates") != std::string::npos);
        }
    }

    // ---------- v0.28.9: dock merge (Reposition geometry, Stick =
    // Reserve, Undertow orbs) ----------
    {
        // dockedBarRect: flush + full width on the bar's OWN monitor,
        // height unchanged (Jackson's 2026-11-11 ruling).
        const IntRect monP{ 0, 0, 1920, 1080 };
        CHECK(dockedBarRect({ 300, 400, 800, 120 }, monP, true)  == (IntRect{ 0, 0, 1920, 120 }));
        CHECK(dockedBarRect({ 300, 400, 800, 120 }, monP, false) == (IntRect{ 0, 960, 1920, 120 }));
        // non-primary display: x/width follow THAT monitor
        const IntRect mon2{ 1920, 0, 2560, 1440 };
        CHECK(dockedBarRect({ 2500, 300, 900, 100 }, mon2, true)  == (IntRect{ 1920, 0, 2560, 100 }));
        CHECK(dockedBarRect({ 2500, 300, 900, 100 }, mon2, false) == (IntRect{ 1920, 1340, 2560, 100 }));
        // negative-origin monitor (left of primary)
        const IntRect monL{ -1920, 0, 1920, 1080 };
        CHECK(dockedBarRect({ -1500, 500, 700, 90 }, monL, true) == (IntRect{ -1920, 0, 1920, 90 }));
        // degenerate inputs keep the bar as-is
        CHECK(dockedBarRect({ 10, 10, 100, 50 }, { 0, 0, 0, 0 }, true) == (IntRect{ 10, 10, 100, 50 }));
        CHECK(dockedBarRect({ 10, 10, 100, 0 }, monP, true) == (IntRect{ 10, 10, 100, 0 }));

        // applyStickReserveMerge: either legacy flag on loads as
        // stuck; the pair always ends in sync; idempotent.
        { bool s = false, r = false; applyStickReserveMerge(s, r); CHECK(!s && !r); }
        { bool s = true,  r = false; applyStickReserveMerge(s, r); CHECK(s && r); }
        { bool s = false, r = true;  applyStickReserveMerge(s, r); CHECK(s && r); }
        { bool s = true,  r = true;  applyStickReserveMerge(s, r); CHECK(s && r); }

        // Performance: the orb cap halves in Low-End like the others.
        CHECK(kPerfFullOrbCap == UndertowModel::kOrbCount);
        { auto e = effectiveLimits(false, 60, true, true); CHECK(e.orbCap == 48); }
        { auto e = effectiveLimits(true, 60, true, true);  CHECK(e.orbCap == 24); }

        // Orb palette: 8 colours, wraps, first entry is the
        // historical slate (bandColour(1)).
        { auto c = UndertowModel::orbPalette(0); CHECK(c.r == 0x33 && c.g == 0x50 && c.b == 0x7A); }
        { auto a = UndertowModel::orbPalette(0), b = UndertowModel::orbPalette(8);
          CHECK(a.r == b.r && a.g == b.g && a.b == b.b); }
        { int distinct = 0;
          for (int i = 0; i < UndertowModel::kOrbPaletteSize; ++i) {
              bool seen = false; auto c = UndertowModel::orbPalette(i);
              for (int j = 0; j < i; ++j) { auto d = UndertowModel::orbPalette(j);
                  if (c.r == d.r && c.g == d.g && c.b == d.b) seen = true; }
              if (!seen) ++distinct;
          }
          CHECK(distinct == UndertowModel::kOrbPaletteSize); }

        // Model seeding: 48 orbs (30 in v0.28.9), varied colours AND
        // effects, deterministic across instances.
        CHECK(UndertowModel::kOrbCount == 48); // v0.28.12 (was 30 in v0.28.9)
        { UndertowModel m1, m2;
          int distinctColours = 0; bool fxSeen[3] = { false, false, false };
          for (int i = 0; i < UndertowModel::kOrbCount; ++i) {
              const auto& o = m1.orbs()[(size_t)i];
              fxSeen[(int)o.effect] = true;
              bool seen = false;
              for (int j = 0; j < i; ++j) { const auto& p = m1.orbs()[(size_t)j];
                  if (o.colour.r == p.colour.r && o.colour.g == p.colour.g && o.colour.b == p.colour.b) seen = true; }
              if (!seen) ++distinctColours;
              const auto& q = m2.orbs()[(size_t)i];
              CHECK(o.colour.r == q.colour.r && o.effect == q.effect && o.phase == q.phase);
          }
          CHECK(distinctColours >= 6);
          CHECK(fxSeen[0] && fxSeen[1] && fxSeen[2]); }

        // Orb cap: parked orbs sit at alpha 0 while live orbs fade
        // in on the low band; pulse orbs modulate radius off the
        // plain level mapping.
        { UndertowModel m; m.setOrbCap(5);
          CHECK(m.orbCap() == 5 && m.activeOrbCount() == 5);
          FiveBandLevels lv; lv.band[1] = 0.8f;
          UndertowBeatFeed bf; UndertowOnsetFeed of;
          for (int f = 0; f < 90; ++f) m.update(1.f / 60.f, lv, -20.f, bf, of);
          CHECK(m.orbs()[4].alpha > 0.f);
          CHECK(m.orbs()[5].alpha == 0.f && m.orbs()[47].alpha == 0.f);
          bool radiusVaries = false;
          for (int i = 1; i < 5; ++i) {
              const float r0 = m.orbs()[0].radius / m.orbs()[0].baseRadius;
              const float ri = m.orbs()[(size_t)i].radius / m.orbs()[(size_t)i].baseRadius;
              if (std::fabs(r0 - ri) > 1e-4f) radiusVaries = true;
          }
          CHECK(radiusVaries); // per-orb slice + pulse: radii are not one uniform mapping
        }
        { UndertowModel m; m.setOrbCap(1000); CHECK(m.orbCap() == UndertowModel::kOrbCount); // clamps
          m.setOrbCap(-3); CHECK(m.orbCap() == 0); }

        // --- v0.28.12: band-grouped orbs + the reactivity rework ---
        // Group assignment: per-band counts are the committed table,
        // every orb's band matches orbBandOf, and the seeded colour
        // is the palette colour tinted 45% toward the band colour.
        { int per[5] = { 0, 0, 0, 0, 0 };
          UndertowModel m;
          for (int i = 0; i < UndertowModel::kOrbCount; ++i) {
              const auto& o = m.orbs()[(size_t)i];
              CHECK(o.band == UndertowModel::orbBandOf(i));
              ++per[o.band];
              const auto mix = UndertowModel::mixRgb(UndertowModel::orbPalette(i),
                                                     UndertowModel::bandColour(o.band), 0.45f);
              CHECK(o.colour.r == mix.r && o.colour.g == mix.g && o.colour.b == mix.b);
          }
          for (int b = 0; b < 5; ++b) CHECK(per[b] == UndertowModel::kOrbBandCounts[b]);
          { const auto a = UndertowModel::orbPalette(3), b = UndertowModel::bandColour(2);
            const auto m0 = UndertowModel::mixRgb(a, b, 0.f), m1 = UndertowModel::mixRgb(a, b, 1.f);
            CHECK(m0.r == a.r && m0.g == a.g && m0.b == a.b);
            CHECK(m1.r == b.r && m1.g == b.g && m1.b == b.b); }
        }
        {   // Each band drives its OWN group: feeding only band b
            // raises band b's mean orb alpha above every other
            // group's; loud low-band drive clears the old 0.28
            // ceiling by a wide margin; silence settles every orb
            // back to the floor.
            UndertowBeatFeed bf; UndertowOnsetFeed of;
            for (int b = 0; b < 5; ++b) {
                UndertowModel m;
                FiveBandLevels lv; lv.band[b] = 0.9f;
                for (int f = 0; f < 120; ++f) m.update(1.f / 60.f, lv, -12.f, bf, of);
                double sum[5] = { 0, 0, 0, 0, 0 }; int cnt[5] = { 0, 0, 0, 0, 0 };
                for (int i = 0; i < UndertowModel::kOrbCount; ++i) {
                    const auto& o = m.orbs()[(size_t)i];
                    sum[o.band] += o.alpha; ++cnt[o.band];
                }
                for (int g = 0; g < 5; ++g) if (g != b) CHECK(sum[b] / cnt[b] > sum[g] / cnt[g]);
                if (b == 1) CHECK(sum[1] / cnt[1] > 0.40); // pre-v0.28.12 ceiling was 0.28 at ANY feed
            }
            { UndertowModel m; FiveBandLevels lv;
              for (int f = 0; f < 120; ++f) m.update(1.f / 60.f, lv, -60.f, bf, of);
              for (int i = 0; i < UndertowModel::kOrbCount; ++i)
                  CHECK(m.orbs()[(size_t)i].alpha <= 0.071f); } // floor 0.05 x worst Pulse breathe 1.4
        }
        {   // Drift speed is per-band: at equal drive the high
            // group's orbs carry far more velocity than the sub
            // group's (speed table 0.26 vs 0.055 at full level).
            UndertowBeatFeed bf; UndertowOnsetFeed of;
            FiveBandLevels lv; for (int b = 0; b < 5; ++b) lv.band[b] = 0.85f;
            UndertowModel m;
            for (int f = 0; f < 180; ++f) m.update(1.f / 60.f, lv, -12.f, bf, of);
            double vel[5] = { 0, 0, 0, 0, 0 }; int cnt[5] = { 0, 0, 0, 0, 0 };
            for (int i = 0; i < UndertowModel::kOrbCount; ++i) {
                const auto& o = m.orbs()[(size_t)i];
                vel[o.band] += std::sqrt((double)o.vx * o.vx + (double)o.vy * o.vy); ++cnt[o.band];
            }
            CHECK(vel[3] / cnt[3] > 2.0 * (vel[0] / cnt[0])); // high darts
            CHECK(vel[2] / cnt[2] > vel[0] / cnt[0]);         // mid out-drifts sub
        }
        {   // A sub onset pumps the sub orbs' radii (the swell term)
            // beyond what the same level alone produces.
            UndertowBeatFeed bf; UndertowOnsetFeed none;
            FiveBandLevels lv; lv.band[0] = 0.8f;
            UndertowModel plain, pumped;
            for (int f = 0; f < 30; ++f) { plain.update(1.f / 60.f, lv, -12.f, bf, none); pumped.update(1.f / 60.f, lv, -12.f, bf, none); }
            UndertowOnsetFeed kick; kick.count[0] = 1; kick.strength[0] = 1.f; kick.lastSec[0] = 1.0;
            pumped.update(1.f / 60.f, lv, -12.f, bf, kick);
            plain.update(1.f / 60.f, lv, -12.f, bf, none);
            double rp = 0, rq = 0;
            for (int i = 0; i < UndertowModel::kOrbBandCounts[0]; ++i) {
                rp += pumped.orbs()[(size_t)i].radius / pumped.orbs()[(size_t)i].baseRadius;
                rq += plain.orbs()[(size_t)i].radius / plain.orbs()[(size_t)i].baseRadius;
            }
            CHECK(rp > rq + 0.10 * UndertowModel::kOrbBandCounts[0]); // swell 0.38 x pulse ~1 on the kick frame
        }
        {   // v0.28.12: a high onset now latches bandPulse_[3] (it
            // was decayed on the tau table and read, but never set).
            UndertowModel m;
            UndertowBeatFeed bf; UndertowOnsetFeed of;
            of.count[3] = 1; of.strength[3] = 0.9f; of.lastSec[3] = 0.5;
            FiveBandLevels lv;
            m.update(1.f / 60.f, lv, -40.f, bf, of);
            CHECK(m.bandPulse(3) > 0.4f);
        }

        // --- dock-merge wiring pins (app sources, same pattern as
        // the v0.28.8 pins above; SKIP with a note when unreachable).
        auto readApp2 = [](const char* rel) {
            std::string src;
            const std::string base(rel);
            const char* prefixes[] = { "", "../", "../../" };
            for (auto* pre : prefixes) {
                std::FILE* fh = std::fopen((std::string(pre) + base).c_str(), "rb");
                if (!fh) continue;
                char chunk[8192]; std::size_t n;
                while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) src.append(chunk, n);
                std::fclose(fh);
                if (!src.empty()) break;
            }
            return src;
        };
        const std::string mc9  = readApp2("app/Source/MainComponent.cpp");
        const std::string pio9 = readApp2("app/Source/PresetIO.h");
        const std::string ss9  = readApp2("app/Source/ScreenSpace.cpp");
        const std::string sh9  = readApp2("app/Source/Settings.h");
        if (mc9.empty() || pio9.empty() || ss9.empty() || sh9.empty()) {
            std::printf("v0.28.9 app-source pins SKIPPED (app sources not reachable from CWD)\n");
        } else {
            // Reposition: total-area geometry through the core fn
            CHECK(mc9.find("meterbar::dockedBarRect") != std::string::npos);
            CHECK(mc9.find("disp->totalArea") != std::string::npos);
            // the merged control: menu item gone, one action path
            CHECK(mc9.find("idReserve, \"Reserve Screen Space\"") == std::string::npos);
            CHECK(mc9.find("void MainComponent::setStick(bool on)") != std::string::npos);
            CHECK(mc9.find("applyStickReserveMerge(engine.state.stick") != std::string::npos);
            CHECK(sh9.find("onStickToggled") != std::string::npos);
            // presets + last-state fold legacy pairs on load
            CHECK(pio9.find("applyStickReserveMerge(s.stick") != std::string::npos);
            // drag lock while stuck (2026-11-03 ruling)
            CHECK(mc9.find("if (engine.state.stick) return;") != std::string::npos);
            // geometry judged on the visible rect, both feed sites
            CHECK(mc9.find("screenSpace.windowVisibleRect") != std::string::npos);
            // AppBar lifecycle: same-edge moves reposition in place
            CHECK(ss9.find("g_appBar.requested = want;") != std::string::npos);
            CHECK(ss9.find("bool ScreenSpace::windowVisibleRect") != std::string::npos);
            // orbs: per-orb colour reaches the renderer, cap wired
            CHECK(mc9.find("col(o.colour)") != std::string::npos);
            CHECK(mc9.find("setOrbCap(pl.orbCap)") != std::string::npos);
        }
    }

    // ---------- v0.28.10: dock fix — placement must speak the
    // machine's language (the VISIBLE rect). Until this block, no
    // test produced the observation the shipped app produces: dock
    // via the app's own action, then read the gap the machine is
    // fed (the DIAGNOSIS coverage gap — the 0.28.9 pins were
    // substring finds that this defect sailed through). These
    // checks run that loop end to end in core: place, derive the
    // visible rect, compute the gaps exactly as syncScreenSpace
    // does, feed the ReserveMachine. ----------
    {
        using RA = ReserveAction;
        using RE = ReserveEdge;
        const IntRect mon{0, 0, 1920, 1080};
        auto iabs = [](int a, int b) { return a > b ? a - b : b - a; };
        auto gapsOf = [&iabs](IntRect r, IntRect m) {
            return EdgeGaps{ iabs(r.y, m.y), iabs(m.bottom(), r.bottom()),
                             iabs(r.x, m.x), iabs(m.right(), r.right()) };
        };

        // frameInsetsOf: window minus visible per side, clamped >= 0.
        { const auto f = frameInsetsOf({0, 0, 1936, 136}, {8, 8, 1920, 120});
          CHECK(f.top == 8 && f.bottom == 8 && f.left == 8 && f.right == 8); }
        { const auto f = frameInsetsOf({100, 100, 800, 120}, {100, 100, 800, 120});
          CHECK(f.top == 0 && f.bottom == 0 && f.left == 0 && f.right == 0); }
        { const auto f = frameInsetsOf({0, 0, 100, 100}, {-4, -4, 108, 108}); // visible beyond window: clamp
          CHECK(f.top == 0 && f.bottom == 0 && f.left == 0 && f.right == 0); }
        { const auto f = frameInsetsOf({0, 0, 1936, 140}, {8, 0, 1920, 132}); // asymmetric: no top border
          CHECK(f.top == 0 && f.bottom == 8 && f.left == 8 && f.right == 8); }
        // visibleRectFromWindowRect inverts the placement expansion.
        CHECK(visibleRectFromWindowRect({-8, -8, 1936, 136}, FrameInsets{8, 8, 8, 8}) == (IntRect{0, 0, 1920, 120}));

        // THE DEFECT, on the shipped 0.28.9 path: place the WINDOW
        // rect flush (dockedBarRect), then let the machine judge the
        // VISIBLE rect. For any inset > 2 the judged gap sits in the
        // 3..8 px no-dock dead zone (or beyond) and Register never
        // fires — Jackson's "reserve never engages from the app's own
        // dock" field failure, reproduced in core.
        for (int ins : { 3, 8, 12 }) {
            const FrameInsets in{ins, ins, ins, ins};
            const IntRect winBar{300, 400, 800, 120 + 2 * ins}; // window rect of a 120 px visible bar
            const IntRect placed = dockedBarRect(winBar, mon, true);
            const IntRect vis = visibleRectFromWindowRect(placed, in);
            ReserveMachine m;
            CHECK(m.update(true, gapsOf(vis, mon), vis, mon, 1) == RA::None);
            CHECK(!m.reserved());
        }
        // The boundary that made the field failure look
        // intermittent: insets 0 and 2 DO register on the old path.
        for (int ins : { 0, 2 }) {
            const FrameInsets in{ins, ins, ins, ins};
            const IntRect winBar{300, 400, 800, 120 + 2 * ins};
            const IntRect vis = visibleRectFromWindowRect(dockedBarRect(winBar, mon, true), in);
            ReserveMachine m;
            CHECK(m.update(true, gapsOf(vis, mon), vis, mon, 1) == RA::Register);
        }

        // THE FIX: place via dockedBarWindowRect with the live
        // insets. The visible bar lands flush, full width, height
        // kept; the machine's own observation registers on BOTH
        // edges at every realistic inset (0 = borderless, 4/8 = 100%
        // scaling, 10/12 = 125-150% scaling), with the strip EXACTLY
        // the visible bar's height (0.28.9's overshoot engagement
        // came out ~one inset thin) and a steady state of None.
        for (int ins : { 0, 4, 8, 10, 12 }) {
            const FrameInsets in{ins, ins, ins, ins};
            const IntRect winBar{300, 400, 800, 120 + 2 * ins};
            {   // top
                const IntRect placed = dockedBarWindowRect(winBar, mon, true, in);
                CHECK(placed.h == winBar.h); // window height preserved (visible height + insets)
                const IntRect vis = visibleRectFromWindowRect(placed, in);
                CHECK(vis == (IntRect{0, 0, 1920, 120}));
                CHECK(gapsOf(vis, mon).top == 0);
                ReserveMachine m;
                CHECK(m.update(true, gapsOf(vis, mon), vis, mon, 1) == RA::Register);
                CHECK(m.edge() == RE::Top);
                CHECK(m.requestedRect() == (IntRect{0, 0, 1920, 120}));
                CHECK(m.update(true, gapsOf(vis, mon), vis, mon, 1) == RA::None);
            }
            {   // bottom
                const IntRect placed = dockedBarWindowRect(winBar, mon, false, in);
                const IntRect vis = visibleRectFromWindowRect(placed, in);
                CHECK(vis == (IntRect{0, 960, 1920, 120}));
                ReserveMachine m;
                CHECK(m.update(true, gapsOf(vis, mon), vis, mon, 1) == RA::Register);
                CHECK(m.edge() == RE::Bottom);
                CHECK(m.requestedRect() == (IntRect{0, 960, 1920, 120}));
            }
        }
        // Asymmetric insets (a borderless top edge): still flush.
        { const FrameInsets in{0, 8, 8, 8};
          const IntRect placed = dockedBarWindowRect({300, 400, 800, 128}, mon, true, in);
          CHECK(visibleRectFromWindowRect(placed, in) == (IntRect{0, 0, 1920, 120})); }
        // Non-primary / negative-origin monitor through the new path.
        { const FrameInsets in{8, 8, 8, 8};
          const IntRect monL{-1920, 0, 1920, 1080};
          const IntRect placed = dockedBarWindowRect({-1500, 500, 700, 106}, monL, true, in);
          const IntRect vis = visibleRectFromWindowRect(placed, in);
          CHECK(vis == (IntRect{-1920, 0, 1920, 90}));
          ReserveMachine m;
          CHECK(m.update(true, gapsOf(vis, monL), vis, monL, 7) == RA::Register); }
        // Edge-to-edge reposition while stuck: 0.28.9 took Release
        // (bottom dock, click At the Top — judged top gap ~8 with
        // the bottom clear); the new path's judged top gap is 0, so
        // the machine SWITCHES, exactly once, and then holds steady
        // (no self-release at high scaling — the judged gap after
        // the app's own dock is 0 whatever the inset).
        { const FrameInsets in{12, 12, 12, 12};
          const IntRect winBar{300, 400, 800, 144};
          ReserveMachine m;
          const IntRect visB = visibleRectFromWindowRect(dockedBarWindowRect(winBar, mon, false, in), in);
          CHECK(m.update(true, gapsOf(visB, mon), visB, mon, 1) == RA::Register);
          CHECK(m.edge() == RE::Bottom);
          const IntRect visT = visibleRectFromWindowRect(
              dockedBarWindowRect(dockedBarWindowRect(winBar, mon, false, in), mon, true, in), in);
          CHECK(m.update(true, gapsOf(visT, mon), visT, mon, 1) == RA::Switch);
          CHECK(m.edge() == RE::Top);
          CHECK(m.requestedRect() == (IntRect{0, 0, 1920, 120}));
          CHECK(m.update(true, gapsOf(visT, mon), visT, mon, 1) == RA::None); }
        // Degenerate: insets meeting the bar fall back to the plain
        // window-rect dock; a degenerate monitor keeps the bar as-is.
        CHECK(dockedBarWindowRect({10, 10, 100, 12}, mon, true, FrameInsets{8, 8, 8, 8})
              == dockedBarRect({10, 10, 100, 12}, mon, true));
        CHECK(dockedBarWindowRect({10, 10, 100, 50}, {0, 0, 0, 0}, true, FrameInsets{8, 8, 8, 8})
              == (IntRect{10, 10, 100, 50}));

        // --- v0.28.10 wiring pins (app sources, same pattern as the
        // earlier blocks; SKIP with a note when unreachable). These
        // pin the app-side halves of fixes 1-4; the OS-facing bodies
        // (DWM measurement, AppBar calls, the TaskbarCreated
        // broadcast) are compile-proven by the Windows CI build,
        // not executable here — see the receipt's honesty note.
        auto readApp3 = [](const char* rel) {
            std::string src;
            const std::string base(rel);
            const char* prefixes[] = { "", "../", "../../" };
            for (auto* pre : prefixes) {
                std::FILE* fh = std::fopen((std::string(pre) + base).c_str(), "rb");
                if (!fh) continue;
                char chunk[8192]; std::size_t n;
                while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) src.append(chunk, n);
                std::fclose(fh);
                if (!src.empty()) break;
            }
            return src;
        };
        auto countOf = [](const std::string& hay, const std::string& needle) {
            int n = 0; std::string::size_type p = 0;
            while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
            return n;
        };
        const std::string mcA  = readApp3("app/Source/MainComponent.cpp");
        const std::string mhA  = readApp3("app/Source/MainComponent.h");
        const std::string ssA  = readApp3("app/Source/ScreenSpace.cpp");
        const std::string cmA  = readApp3("CMakeLists.txt");
        if (mcA.empty() || mhA.empty() || ssA.empty() || cmA.empty()) {
            std::printf("v0.28.10 app-source pins SKIPPED (app sources not reachable from CWD)\n");
        } else {
            // fix 1: doTopBottom measures the live insets and places
            // through the window-space dock rect
            CHECK(mcA.find("meterbar::dockedBarWindowRect") != std::string::npos);
            CHECK(mcA.find("meterbar::frameInsetsOf") != std::string::npos);
            // fix 2: the post-show hook exists and syncs
            CHECK(mhA.find("void visibilityChanged() override;") != std::string::npos);
            CHECK(mcA.find("void MainComponent::visibilityChanged()") != std::string::npos);
            CHECK(mcA.find("if (isVisible()) {") != std::string::npos); // v0.28.11: braced block — sync + deferred arm (see the v0.28.11 block)
            // fix 3: the visible-rect feed now reaches every rect
            // consumer — the three gap-judging sites (syncScreenSpace,
            // doArrangeWindows, captionAnchorEdge) plus doTopBottom's
            // fix-1 inset measurement
            CHECK(countOf(mcA, "screenSpace.windowVisibleRect") == 4);
            // fix 4: TaskbarCreated re-register in ScreenSpace
            CHECK(ssA.find("RegisterWindowMessageW(L\"TaskbarCreated\")") != std::string::npos);
            CHECK(ssA.find("reregisterAfterShellRestart") != std::string::npos);
            // the version source of truth moved
            CHECK(cmA.find("project(MeterBar VERSION 0.28.12") != std::string::npos);
        }
    }

    // ---------- v0.28.11: restore rule (S1-A) — a saved DOCKED
    // rect must survive restore verbatim. The shipped restore
    // paths (MainWindow ctor, preset apply) fed the saved rect
    // through clampRectToWorkAreas against the USER areas: a
    // 0.28.10 dock saves its window rect one frame inset OUTSIDE
    // the monitor edge, the clamp dragged it fully inside the
    // work area, and the machine judged the visible edge one
    // inset off the monitor edge — the no-dock dead zone — so a
    // Stick-on relaunch never registered. These checks run the
    // restore arithmetic itself: old path vs new rule, then the
    // restored rect through the machine. ----------
    {
        using RA = ReserveAction;
        const IntRect ua{0, 0, 1920, 1032}; // 1080p, taskbar at the bottom
        const std::vector<IntRect> areas{ ua };
        auto gapsOf = [](IntRect r, IntRect m) {
            auto iabs = [](int a, int b) { return a > b ? a - b : b - a; };
            return EdgeGaps{ iabs(r.y, m.y), iabs(m.bottom(), r.bottom()),
                             iabs(r.x, m.x), iabs(m.right(), r.right()) };
        };
        const IntRect mon{0, 0, 1920, 1080};
        const FrameInsets in8{8, 8, 8, 8};

        // Top dock, saved as the 0.28.10 placement leaves it.
        const IntRect dockedTop{-8, -8, 1936, 136};
        // THE DEFECT on the old path: the clamp pulls it inside.
        CHECK(clampRectToWorkAreas(dockedTop, areas) == (IntRect{0, 0, 1936, 136}));
        // ...and the machine judges the clamped rect's visible
        // edge 8 px off the monitor top: dead zone, never docks.
        { const IntRect vis = visibleRectFromWindowRect(clampRectToWorkAreas(dockedTop, areas), in8);
          ReserveMachine m;
          CHECK(m.update(true, gapsOf(vis, mon), vis, mon, 1) == RA::None);
          CHECK(!m.reserved()); }
        // THE FIX: the restore rule returns it verbatim...
        CHECK(restoreRectToWorkAreas(dockedTop, areas) == dockedTop);
        // ...and its visible rect registers, exactly as placed.
        { const IntRect vis = visibleRectFromWindowRect(restoreRectToWorkAreas(dockedTop, areas), in8);
          CHECK(vis == (IntRect{0, 0, 1920, 120}));
          ReserveMachine m;
          CHECK(m.update(true, gapsOf(vis, mon), vis, mon, 1) == RA::Register); }
        // Bottom dock against a bottom taskbar's user area: the
        // old clamp hauled it up by the inset; the rule keeps it.
        const IntRect dockedBot{-8, 952, 1936, 136};
        CHECK(clampRectToWorkAreas(dockedBot, areas) == (IntRect{0, 896, 1936, 136}));
        CHECK(restoreRectToWorkAreas(dockedBot, areas) == dockedBot);
        // A docked rect on a negative-origin monitor restores too.
        const std::vector<IntRect> areas2{ IntRect{-1920, 0, 1920, 1032}, ua };
        CHECK(restoreRectToWorkAreas((IntRect{-1928, -8, 1936, 136}), areas2) == (IntRect{-1928, -8, 1936, 136}));
        // Ordinary floating rects were never the clamp's victims
        // and are untouched by the rule.
        CHECK(restoreRectToWorkAreas((IntRect{300, 200, 900, 150}), areas) == (IntRect{300, 200, 900, 150}));
        // The floor is the family's own: exactly 40 px visible
        // passes verbatim; 20 px visible is a lost rect and the
        // clamp rescues it exactly as before.
        CHECK(restoreRectToWorkAreas((IntRect{100, -80, 800, 120}), areas) == (IntRect{100, -80, 800, 120}));
        CHECK(restoreRectToWorkAreas((IntRect{100, -100, 800, 120}), areas) == (IntRect{100, 0, 800, 120}));
        // Saved against a monitor that is gone: clamped home.
        CHECK(restoreRectToWorkAreas((IntRect{-3000, 200, 800, 120}), areas) == (IntRect{0, 200, 800, 120}));
        // Degenerate inputs pass through (the callers keep their
        // own fallback chains).
        CHECK(restoreRectToWorkAreas((IntRect{5, 5, 100, 50}), {}) == (IntRect{5, 5, 100, 50}));
        CHECK(restoreRectToWorkAreas((IntRect{5, 5, 0, 50}), areas) == (IntRect{5, 5, 0, 50}));

        // --- v0.28.11 wiring pins (app sources, same pattern as
        // the earlier blocks; SKIP with a note when unreachable).
        // These pin the app-side halves: the two restore call
        // sites, the deferred-sync machinery, the drag-coalesced
        // feeders, and the link Stick guard. The OS-facing bodies
        // remain compile-proven by the Windows CI build, not
        // executable here — see the receipt's honesty note.
        auto readApp4 = [](const char* rel) {
            std::string src;
            const std::string base(rel);
            const char* prefixes[] = { "", "../", "../../" };
            for (auto* pre : prefixes) {
                std::FILE* fh = std::fopen((std::string(pre) + base).c_str(), "rb");
                if (!fh) continue;
                char chunk[8192]; std::size_t n;
                while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) src.append(chunk, n);
                std::fclose(fh);
                if (!src.empty()) break;
            }
            return src;
        };
        auto countOf4 = [](const std::string& hay, const std::string& needle) {
            int n = 0; std::string::size_type p = 0;
            while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
            return n;
        };
        const std::string mcB  = readApp4("app/Source/MainComponent.cpp");
        const std::string mhB  = readApp4("app/Source/MainComponent.h");
        const std::string mnB  = readApp4("app/Source/Main.cpp");
        const std::string cmB  = readApp4("CMakeLists.txt");
        if (mcB.empty() || mhB.empty() || mnB.empty() || cmB.empty()) {
            std::printf("v0.28.11 app-source pins SKIPPED (app sources not reachable from CWD)\n");
        } else {
            // S1-A: both restore call sites use the restore rule;
            // no bare clamp call remains at either site.
            CHECK(mnB.find("meterbar::restoreRectToWorkAreas") != std::string::npos);
            CHECK(countOf4(mnB, "clampRectToWorkAreas(") == 0);
            CHECK(mcB.find("meterbar::restoreRectToWorkAreas") != std::string::npos);
            CHECK(countOf4(mcB, "clampRectToWorkAreas(") == 0);
            // S1-B guard + S2 coalescing: the deferred re-sync
            // exists, is deadline-honoured in the frame timer, and
            // is armed from the dock action and the visibility hook.
            CHECK(mcB.find("void MainComponent::scheduleDeferredSync()") != std::string::npos);
            CHECK(mhB.find("deferredSyncAtMs_") != std::string::npos);
            CHECK(mcB.find("deferredSyncAtMs_ != 0 && now >= deferredSyncAtMs_") != std::string::npos);
            CHECK(countOf4(mcB, "scheduleDeferredSync();") >= 4);
            // S2: both geometry feeders hold their feed while a
            // mouse button is down (drag/resize in progress).
            CHECK(countOf4(mcB, "juce::Component::isMouseButtonDownAnywhere()") == 2);
            // S1-B guard: remote link applies yield while stuck.
            CHECK(mcB.find("if (engine.state.stick) return;") != std::string::npos);
            // the version source of truth moved
            CHECK(cmB.find("project(MeterBar VERSION 0.28.12") != std::string::npos);
        }
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

