#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <meterbar/Modules.hpp>
#include <meterbar/Fft.hpp>
#include <meterbar/ColorMap.hpp>
#include <BinaryData.h>
#include "Theme.h"
// Data prepared once per frame by MainComponent, drawn by every module view.
struct AnalysisData {
    const meterbar::PresetState* st = nullptr;
    Theme theme; meterbar::ColorMap cmap;
    std::vector<float> spectrumDb;           // smoothed + tilted dB per bin
    const meterbar::SpectrogramHistory* spectrogram = nullptr;
    std::vector<float> waveCh1, waveCh2;     // scrolling peak columns 0..1
    std::vector<float> latestL, latestR;     // recent samples (scope / stereo)
    double loudDb = -120, peakLin = 0; meterbar::StereoFrame stereo;
    double loudDbL = -120, loudDbR = -120;   // v0.15: per-channel display levels for the vertical loudness bars
    double loudPeakDbL = -120, loudPeakDbR = -120; // v0.15: per-channel peak holds (dBFS)
    double loudIntegrated = -120, loudShortTerm = -120, loudTruePeakDb = -120; // v0.16 loudness pack
    float beatGlow = 0;                      // v0.16: onset envelope 0..1 (Beat Pulse + mascot)
    std::vector<float> scopeBuf; float scopeFreq = 0;
    double sampleRate = 48000;
};
class ModuleView : public juce::Component {
public:
    enum class Kind { Spectrum, Spectrogram, Waveform, Loudness, Stereometer, Oscilloscope };
    Kind kind; const AnalysisData* data = nullptr;
    int flexIndex = 0; // v0.15: index into PresetState::moduleFlex (display order)
    explicit ModuleView(Kind k) : kind(k) { setInterceptsMouseClicks(false, false); setOpaque(false); }
    juce::Image sgCache; // spectrogram render cache (v0.11)
    void paint(juce::Graphics& g) override {
        if (!data || !data->st) return;
        auto& st = *data->st; auto& th = data->theme; auto r = getLocalBounds().toFloat();
        // v0.13: the section background honours the Background Opacity
        // setting — below 1.0 the (non-opaque) window lets the desktop
        // show through the panel fill. Traces/text stay full alpha.
        const float bgOp = juce::jlimit(0.15f, 1.0f, st.backgroundOpacity);
        // v0.16: panels paint the selected Background Style (Flat keeps
        // the exact old th.panel fill).
        if (st.backgroundStyle == meterbar::BackgroundStyle::Flat) g.fillAll(th.panel.withAlpha(bgOp));
        else fillStyledBackground(g, r, st.backgroundStyle, th, bgOp);
        // v0.12: softer chrome — rounded border in the theme border colour,
        // and a header that wears the module's own accent: a colour dot +
        // the title in that accent (thin caps feel, bold 10pt).
        g.setColour(th.border); g.drawRoundedRectangle(r.reduced(0.5f), 6.f, 1.f);
        // v0.16 Beat Pulse: a SUBTLE onset glow — a whisper of accent fill
        // plus a slightly brighter border while the envelope is up. It
        // never re-colours data and decays in ~120 ms by DSP design.
        if (st.beatPulseEnabled && data->beatGlow > 0.01f) {
            auto acc0 = th.moduleAccent((int)kind);
            g.setColour(acc0.withAlpha(0.055f * data->beatGlow));
            g.fillRoundedRectangle(r.reduced(1.f), 6.f);
            g.setColour(acc0.brighter(0.35f).withAlpha(0.5f * data->beatGlow));
            g.drawRoundedRectangle(r.reduced(0.5f), 6.f, 1.4f);
        }
        juce::String title = kind == Kind::Spectrum ? "Spectrum" : kind == Kind::Spectrogram ? "Spectrogram" :
            kind == Kind::Waveform ? "Waveform" : kind == Kind::Loudness ? "Loudness" :
            kind == Kind::Stereometer ? "Stereometer" : "Oscilloscope";
        auto acc = th.moduleAccent((int)kind);
        auto hdr = r.reduced(4).removeFromTop(13);
        g.setColour(acc); g.fillEllipse(hdr.getX() + 1, hdr.getCentreY() - 2.5f, 5.f, 5.f);
        g.setFont(juce::Font(10.f, juce::Font::bold));
        g.drawText(title.toUpperCase(), hdr.withTrimmedLeft(10), juce::Justification::topLeft);
        auto plot = r.reduced(4).withTrimmedTop(14);
        switch (kind) {
        case Kind::Spectrum: drawSpectrum(g, plot, st); break;
        case Kind::Spectrogram: drawSpectrogram(g, plot, st); break;
        case Kind::Waveform: drawWaveform(g, plot, st); break;
        case Kind::Loudness: drawLoudness(g, plot, st); break;
        case Kind::Stereometer: drawStereo(g, plot, st); break;
        case Kind::Oscilloscope: drawScope(g, plot, st); break;
        }
    }
private:
    static float dbToY(float db, juce::Rectangle<float> p) { // -90..0 dB window
        return p.getBottom() - juce::jmap(juce::jlimit(-90.f, 0.f, db), -90.f, 0.f, 0.f, p.getHeight());
    }
    void drawSpectrum(juce::Graphics& g, juce::Rectangle<float> p, const meterbar::PresetState& st) {
        auto& th = data->theme; int nb = (int)data->spectrumDb.size(); if (nb < 2) return;
        // faint level grid (-30 / -60 dB) so the trace reads against something
        g.setColour(th.grid.withAlpha(0.35f));
        for (float gdb : { -30.f, -60.f }) { float gy = dbToY(gdb, p); g.drawLine(p.getX(), gy, p.getRight(), gy, 0.5f); }
        if (st.spectrumShowTarget) { // flat target line at the tilt-compensated 0 line
            g.setColour(th.warn.withAlpha(0.7f)); float y = dbToY(-18.f, p); g.drawLine(p.getX(), y, p.getRight(), y, 1.f);
        }
        // v0.12: exact pixel->bin mapping via the DSP core's tested freqToX
        // (the old ^2.2 power approximation ignored the Mel/Linear scales and
        // misplaced peaks vs the scale the analyser actually uses). freqToX
        // is monotonic in bin frequency, so one walking pass builds the LUT.
        const double sr = data->sampleRate > 0 ? data->sampleRate : 48000.0;
        const double maxF = std::min(20000.0, sr * 0.5);
        const int fftSz = (nb - 1) * 2, W = (int)p.getWidth();
        std::vector<int> binAt((size_t)std::max(1, W), 1);
        for (int px = 0, b = 1; px < W; ++px) {
            float fx = (float)px / (float)W;
            while (b < nb - 1 && meterbar::SpectrumAnalyser::freqToX(
                       meterbar::SpectrumAnalyser::binFreq(b + 1, fftSz, sr), 20.0, maxF, st.fftScale) <= fx) ++b;
            binAt[(size_t)px] = b;
        }
        auto style = st.spectrumStyle;
        juce::Path path; bool started = false;
        int bars = 48;
        // v0.13: per-bin colour from the DSP spectrumColor() — by LEVEL
        // (quiet = cool violet, loud = hot red; Jackson: "the higher it is
        // the redder it is"), by FREQUENCY (palette gradient across the
        // axis), or a BLEND of both. ColorMap mode keeps the classic
        // selected-ColorMap behaviour. The accent trace still strokes on top.
        // v0.14: the LEVEL fed to the heat ramp now comes from
        // spectrumLevelHeatT() (a 1.35 power curve over the dB window), not
        // the old linear map — programme bins live in the top of the -90..0
        // window, so linear mapping rendered a loud tone almost entirely
        // orange/red and the cool half never showed.
        // v0.17: ColorMap mode joins the same level language — DSP
        // spectrumMapColour feeds the selected map with spectrumLevelHeatT
        // instead of the old plain linear level, which bunched musical
        // bars into the map's middle (and, through the broken LUT index
        // fixed in this version, rendered one hue entirely). With Rainbow
        // selected a bar's colour now follows its own height through the
        // rainbow: quiet = cool end, loudest = red.
        auto specColour = [&](float db, int px, int bin) {
            if (st.spectrumColorMode == meterbar::SpectrumColorMode::ColorMap) {
                auto mc = meterbar::spectrumMapColour(data->cmap, db);
                return juce::Colour(mc.r, mc.g, mc.b);
            }
            if (st.spectrumColorMode == meterbar::SpectrumColorMode::Chroma) {
                // v0.16 Chroma: colour = pitch class of the bin's frequency
                auto c = meterbar::chromaRgb((float)meterbar::SpectrumAnalyser::binFreq(bin, fftSz, sr),
                                             meterbar::spectrumLevelHeatT(db));
                return juce::Colour(c.r, c.g, c.b);
            }
            auto c = meterbar::spectrumColor(st.spectrumColorMode, meterbar::spectrumLevelHeatT(db), W > 1 ? (float)px / (float)(W - 1) : 0.f);
            return juce::Colour(c.r, c.g, c.b);
        };
        // v0.16: optional 1/6-octave smoothing of the DRAWN curve (display-only).
        std::vector<float> smoothBuf; const std::vector<float>* drawSrc = &data->spectrumDb;
        if (st.spectrumSixthOctave) { smoothBuf = meterbar::smoothSixthOctave(data->spectrumDb, fftSz, sr); drawSrc = &smoothBuf; }
        // v0.15: DISPLAY tilt (Pro-Q style slope compensation) is applied
        // HERE, to the drawn values only — data->spectrumDb itself (which
        // the spectrogram also consumes) is never modified. Pivot 1 kHz.
        auto drawnDb = [&](int bin) {
            double f = meterbar::SpectrumAnalyser::binFreq(bin, fftSz, sr);
            return (*drawSrc)[(size_t)bin] + meterbar::spectrumDisplayTiltCompDb(f, st.spectrumDisplayTiltDb);
        };
        for (int px = 0; px < W; ++px) {
            int bin = binAt[(size_t)px];
            float db = drawnDb(bin); float y = dbToY(db, p); float x = p.getX() + px;
            if (style == meterbar::PresetState::SpectrumStyle::Bars || style == meterbar::PresetState::SpectrumStyle::Both) {
                int bw = std::max(1, (int)p.getWidth() / bars);
                if (px % bw == 0) { g.setColour(specColour(db, px, bin)); g.fillRect(x, y, (float)bw - 1, p.getBottom() - y); }
            }
            if (style == meterbar::PresetState::SpectrumStyle::Fft || style == meterbar::PresetState::SpectrumStyle::Both ||
                style == meterbar::PresetState::SpectrumStyle::Color) {
                g.setColour(specColour(db, px, bin)); g.fillRect(x, y, 1.f, 2.f);
                if (!started) { path.startNewSubPath(x, y); started = true; } else path.lineTo(x, y);
            }
        }
        if (style == meterbar::PresetState::SpectrumStyle::Fft || style == meterbar::PresetState::SpectrumStyle::Both) {
            g.setColour(th.moduleAccent((int)Kind::Spectrum)); g.strokePath(path, juce::PathStrokeType(1.2f));
        }
        g.setColour(th.dim); g.setFont(9.f);
        g.drawText("20", p.removeFromBottom(10), juce::Justification::bottomLeft);
        g.drawText("20k", p, juce::Justification::bottomRight);
    }
    // Level heat adapter (v0.14): the DSP spectrumHeat ramp — the full
    // palette (violet -> teal -> green -> yellow -> orange -> hot red) —
    // shared by the waveform and oscilloscope so every trace module speaks
    // the same colour language as the spectrum's Level mode.
    static juce::Colour levelHeat(float t) {
        auto c = meterbar::spectrumHeat(juce::jlimit(0.f, 1.f, t));
        return juce::Colour(c.r, c.g, c.b);
    }
    // Spectrogram heat: the SELECTED Color Map's LUT with the low end
    // pulled toward black so silence stays dark. v0.14 replaced the old
    // private maroon-red ramp with the fixed spectrumHeat palette — and
    // silently dropped the colour-map feed, so Menu -> Color Map stopped
    // changing anything on the default settings (Jackson's v0.15 bug
    // report). v0.15 restores the feed via DSP spectrogramMapColour; the
    // default map "Midnight Kids" IS the v0.14 palette as a LUT, so the
    // approved look is unchanged until a different map is picked.
    static juce::Colour heatColour(const meterbar::ColorMap& cm, float t) {
        auto c = meterbar::spectrogramMapColour(cm, t);
        return juce::Colour(c.r, c.g, c.b);
    }
    void drawSpectrogram(juce::Graphics& g, juce::Rectangle<float> p, const meterbar::PresetState& st) {
        auto* sg = data->spectrogram; if (!sg || sg->bins() == 0) return;
        bool vert = st.spectrogramOrientation == meterbar::Orientation::Vertical;
        // v0.11: render into a half-res cached image, then one blit. The old
        // per-cell fillRect storm (thousands per frame) could starve mouse
        // events during window moves on the UI thread.
        int iw = std::max(1, (int)p.getWidth() / 2), ih = std::max(1, (int)p.getHeight() / 2);
        if (!sgCache.isValid() || sgCache.getWidth() != iw || sgCache.getHeight() != ih)
            sgCache = juce::Image(juce::Image::RGB, iw, ih, true);
        {
            juce::Graphics ig(sgCache);
            for (int x = 0; x < iw; ++x) for (int y = 0; y < ih; ++y) {
                int age = vert ? (int)((float)y / ih * sg->rows()) : (int)((float)x / iw * sg->rows());
                int bin = vert ? (int)((float)x / iw * (sg->bins() - 1))
                               : (int)((1.f - (float)y / ih) * (sg->bins() - 1));
                float db = sg->at(age, juce::jlimit(0, sg->bins() - 1, bin));
                // v0.15: the spectrogram has its OWN sensitivity mapping
                // (DSP spectrogramLevelT) — at the trim where the other
                // modules peak properly, strong bins reach the RED band.
                // v0.17: the window is linear -70..-4 dB, so a cell's
                // level sweeps the FULL selected colour map (quiet =
                // cool end, loudest = red) — and the map lookup itself
                // was fixed (it returned the map's first colour at every
                // level below full scale; see ColorMap::at). Fade applies.
                float t = juce::jlimit(0.f, 1.f, meterbar::spectrogramLevelT(db)
                            * meterbar::SpectrogramHistory::fadeGain(age, sg->rows(), st.fade));
                ig.setColour(heatColour(data->cmap, t));
                ig.fillRect(x, y, 1, 1);
            }
        }
        g.drawImage(sgCache, p);
    }
    void drawWaveform(juce::Graphics& g, juce::Rectangle<float> p, const meterbar::PresetState&) {
        auto& th = data->theme; int n = (int)data->waveCh1.size(); if (n == 0) return;
        float midY = p.getCentreY();
        for (int x = 0; x < (int)p.getWidth(); ++x) {
            int i = (int)((float)x / p.getWidth() * (n - 1));
            float v = juce::jlimit(0.f, 1.f, data->waveCh1[i]);
            // Colour follows amplitude on the same sqrt curve as the height
            // (MiniMeters-style: wider/louder = hotter colour).
            // v0.14: per-column peak HEAT from the shared full palette
            // (spectrumHeat) instead of the palette gradient — a loud
            // column now climbs violet -> teal -> green -> yellow ->
            // orange -> red with its peak, like every other trace module.
            auto c = levelHeat(std::sqrt(v)); g.setColour(c);
            // Perceptual (sqrt) height: mic-level peaks (~-25 dB = 0.06 linear)
            // were a 1-2 px line on a linear scale and looked dead next to the
            // dB-scaled spectrum. Sqrt makes quiet sources visible (field fix).
            float h = juce::jmax(1.f, std::sqrt(v) * p.getHeight() * 0.5f);
            g.fillRect(p.getX() + x, midY - h, 1.f, h * 2);
            if (!data->waveCh2.empty()) { float v2 = data->waveCh2[i]; g.setColour(th.moduleAccent((int)Kind::Waveform).withAlpha(0.85f));
                float h2 = juce::jmax(1.f, std::sqrt(juce::jlimit(0.f, 1.f, v2)) * p.getHeight() * 0.5f); g.fillRect(p.getX() + x, midY - h2, 1.f, 2.f); }
        }
    }
    void drawLoudness(juce::Graphics& g, juce::Rectangle<float> p, const meterbar::PresetState& st) {
        // v0.15: VERTICAL meter (Jackson's ask) — L and R as up-down columns
        // like a real meter, and the live numeric readout sits immediately
        // to the RIGHT of the bars, moving with the level. Scale -60..0 dB,
        // fill = the shared palette level language, peak tick per channel.
        auto& th = data->theme; double v = data->loudDb;
        auto plot = p; plot.removeFromBottom(2);
        const float barW = 20.f, gap = 7.f;
        auto barsArea = plot.removeFromLeft(barW * 2 + gap);
        auto chanBar = [&](juce::Rectangle<float> bar, double db, double peakDb) {
            g.setColour(th.deepest.withAlpha(juce::jlimit(0.15f, 1.0f, st.backgroundOpacity)));
            g.fillRect(bar); // track (background fill -> honours bg opacity)
            float frac = juce::jmap(juce::jlimit(-60.f, 0.f, (float)db), -60.f, 0.f, 0.f, 1.f);
            float fillH = bar.getHeight() * frac;
            // segmented fill: colour per height step, same level language
            // as every other module (quiet teal at the bottom -> hot up top)
            for (float y = bar.getBottom(); y > bar.getBottom() - fillH; y -= 3.f) {
                float lev = (bar.getBottom() - y) / std::max(1.f, bar.getHeight());
                g.setColour(paletteGrad(lev * 0.95f));
                g.fillRect(bar.getX(), y - 2.f, bar.getWidth(), 2.f);
            }
            if (peakDb > -119) {
                float pf = juce::jmap(juce::jlimit(-60.f, 0.f, (float)peakDb), -60.f, 0.f, 0.f, 1.f);
                float py = bar.getBottom() - bar.getHeight() * pf;
                g.setColour(peakDb > -1.0 ? th.bad : th.text);
                g.fillRect(bar.getX() - 1.f, py - 0.75f, bar.getWidth() + 2.f, 1.5f);
            }
        };
        auto barL = barsArea.removeFromLeft(barW); barsArea.removeFromLeft(gap);
        auto barR = barsArea.removeFromLeft(barW);
        // reserve a label strip under the bars by trimming the plot first
        auto plotTop = plot.getY();
        auto mkBar = [&](juce::Rectangle<float> b) { return b.withTop(plotTop).withBottom(b.getBottom() - 11.f); };
        chanBar(mkBar(barL), data->loudDbL, data->loudPeakDbL);
        chanBar(mkBar(barR), data->loudDbR, data->loudPeakDbR);
        g.setColour(th.dim); g.setFont(9.f);
        g.drawText("L", juce::Rectangle<float>(barL.getX(), barL.getBottom() - 11.f, barW, 10.f), juce::Justification::centred);
        g.drawText("R", juce::Rectangle<float>(barR.getX(), barR.getBottom() - 11.f, barW, 10.f), juce::Justification::centred);
        // numeric readout immediately right of the bars, level-live
        auto num = plot.withTrimmedLeft(6);
        g.setColour(th.text); g.setFont(juce::Font(22.f, juce::Font::bold));
        g.drawText(v <= -119 ? juce::String("--") : juce::String(v, 1), num.removeFromTop(28), juce::Justification::centredLeft);
        g.setColour(th.dim); g.setFont(10.f);
        g.drawText(juce::String(meterbar::loudnessModeName(st.loudnessMode)), num.removeFromTop(13), juce::Justification::topLeft);
        g.drawText("Peak " + (data->peakLin > 0 ? juce::String(20 * std::log10(data->peakLin), 1) + " dBFS" : juce::String("-inf")),
                   num.removeFromTop(13), juce::Justification::topLeft);
        // v0.16 loudness pack: Integrated + Short-Term LUFS, True Peak,
        // and the streaming-target read against the integrated value.
        num.removeFromTop(2);
        g.setFont(9.5f);
        auto fmtDb = [](double x) { return x <= -119 ? juce::String("--") : juce::String(x, 1); };
        g.setColour(th.text);
        g.drawText("Integrated " + fmtDb(data->loudIntegrated) + " LUFS", num.removeFromTop(12), juce::Justification::topLeft);
        g.drawText("Short-Term " + fmtDb(data->loudShortTerm) + " LUFS", num.removeFromTop(12), juce::Justification::topLeft);
        g.drawText("True Peak " + fmtDb(data->loudTruePeakDb) + " dBTP", num.removeFromTop(12), juce::Justification::topLeft);
        // v0.17: the two streaming-target lines are a SECTION, off in the
        // Default recipe (Settings -> Loudness -> "Streaming target
        // lines"). The Integrated/Short-Term/True Peak lines above always
        // show; only these comparisons hide.
        if (data->loudIntegrated > -119 && st.loudnessShowTargets) {
            auto vs = [](double integ, double target) {
                double d = integ - target;
                if (std::abs(d) <= 0.5) return juce::String("at target");
                return d > 0 ? "over by " + juce::String(d, 1) : "under by " + juce::String(-d, 1);
            };
            g.setColour(th.dim);
            g.drawText("Spotify/YT -14: " + vs(data->loudIntegrated, -14.0), num.removeFromTop(12), juce::Justification::topLeft);
            g.drawText("Apple Music -16: " + vs(data->loudIntegrated, -16.0), num.removeFromTop(12), juce::Justification::topLeft);
        }
    }
    void drawStereo(juce::Graphics& g, juce::Rectangle<float> p, const meterbar::PresetState& st) {
        // v0.15: conventional STEREO VECTORSCOPE (Jackson's ask) — a
        // Lissajous plot of L vs R, Ozone-Imager style. The old mode
        // projections (Scaled/Linear, unipolar/bipolar) read like a
        // surround/all-around display on stereo music; they are retired
        // from the view (the DSP correlation/balance computation is
        // unchanged and still shown below the plot). x = R, y = L: mono
        // draws the main diagonal, width spreads points off it.
        auto& th = data->theme; auto& f = data->stereo;
        // v0.13: the plot square is a background fill -> honours bg opacity.
        g.setColour(th.deepest.withAlpha(juce::jlimit(0.15f, 1.0f, st.backgroundOpacity))); auto sq = p.withSizeKeepingCentre(std::min(p.getWidth(), p.getHeight()), std::min(p.getWidth(), p.getHeight()));
        g.fillRect(sq); g.setColour(th.grid.withAlpha(0.6f)); g.drawRect(sq, 0.5f);
        g.drawLine(sq.getX(), sq.getCentreY(), sq.getRight(), sq.getCentreY(), 0.5f);
        g.drawLine(sq.getCentreX(), sq.getY(), sq.getCentreX(), sq.getBottom(), 0.5f);
        // diagonal guides: main diagonal = mono (L = R); anti-diagonal = anti-phase
        g.setColour(th.grid.withAlpha(0.45f));
        g.drawLine(sq.getX(), sq.getBottom(), sq.getRight(), sq.getY(), 0.75f);
        g.setColour(th.grid.withAlpha(0.22f));
        g.drawLine(sq.getX(), sq.getY(), sq.getRight(), sq.getBottom(), 0.5f);
        float ps = st.stereoPointAuto ? 1.5f : juce::jlimit(1.f, 20.f, st.stereoPointSize);
        size_t n = std::min(f.xs.size(), f.ys.size());
        auto lowC = paletteGrad(0.1f), midC = paletteGrad(0.55f), highC = paletteGrad(0.95f);
        for (size_t i = 0; i < n; ++i) {
            auto pt = meterbar::StereometerEngine::lissajousPoint(f.ys[i], f.xs[i]); // {x = R, y = L}
            float x = sq.getCentreX() + juce::jlimit(-1.f, 1.f, pt.first) * sq.getWidth() * 0.5f;
            float y = sq.getCentreY() - juce::jlimit(-1.f, 1.f, pt.second) * sq.getHeight() * 0.5f;
            juce::Colour c = th.moduleAccent((int)Kind::Stereometer);
            if (st.stereoColor == meterbar::StereoColor::Rgb) c = (i % 3 == 0 ? juce::Colours::red : i % 3 == 1 ? juce::Colours::limegreen : juce::Colours::blue);
            else if (st.stereoColor == meterbar::StereoColor::MultiBand) c = (i < n / 3 ? lowC : i < 2 * n / 3 ? midC : highC);
            g.setColour(c.withAlpha(0.7f)); g.fillRect(x, y, ps, ps);
        }
        g.setColour(th.dim); g.setFont(9.f);
        g.drawText("L", sq.withTrimmedLeft(3).withTrimmedTop(1), juce::Justification::topLeft);
        g.drawText("R", sq.withTrimmedRight(3), juce::Justification::centredRight);
        g.setColour(th.text); g.setFont(10.f);
        // v0.16: width % joins the correlation readout under the vectorscope
        // (correlation itself landed in v0.15; both update live per frame).
        g.drawText("corr " + juce::String(f.correlation, 2) + "   width " + juce::String((int)std::lround(f.widthPct)) + "%   bal " + juce::String(f.balance, 2),
                   getLocalBounds().toFloat().removeFromBottom(12).withTrimmedLeft(4), juce::Justification::bottomLeft);
    }
    void drawScope(juce::Graphics& g, juce::Rectangle<float> p, const meterbar::PresetState& st) {
        auto& th = data->theme; int n = (int)data->scopeBuf.size(); if (n == 0) return;
        float midY = p.getCentreY(); g.setColour(th.dim.withAlpha(0.5f)); g.drawLine(p.getX(), midY, p.getRight(), midY, 0.5f);
        auto drawChan = [&](const std::vector<float>& buf, juce::Colour c) {
            juce::Path path; int m = (int)buf.size(); if (m == 0) return;
            for (int x = 0; x < (int)p.getWidth(); ++x) { int i = (int)((float)x / p.getWidth() * (m - 1));
                float y = midY - buf[i] * p.getHeight() * 0.48f;
                if (x == 0) path.startNewSubPath(p.getX(), y); else path.lineTo(p.getX() + x, y); }
            g.setColour(c); g.strokePath(path, juce::PathStrokeType(1.1f));
        };
        // Main trace: multicolour segments, colour = amplitude at that point
        // (palette gradient teal -> yellow -> orange), per Jackson 2026-10-13.
        // v0.14: segments now take the shared level HEAT (spectrumHeat) with
        // a mild 0.8 power lift, so a loud passage visibly reddens through
        // the full palette instead of topping out at the gradient's pink.
        {
            auto& buf = data->scopeBuf; int m = (int)buf.size();
            if (m > 1) {
                auto yAt = [&](int idx){ return midY - buf[idx] * p.getHeight() * 0.48f; };
                int prevI = 0; float prevY = yAt(0);
                for (int x = 1; x < (int)p.getWidth(); ++x) {
                    int i = (int)((float)x / p.getWidth() * (m - 1));
                    float amp = std::max(std::abs(buf[prevI]), std::abs(buf[i]));
                    g.setColour(levelHeat(std::pow(juce::jlimit(0.f, 1.f, amp), 0.8f)));
                    g.drawLine(p.getX() + x - 1, prevY, p.getX() + x, yAt(i), 1.2f);
                    prevI = i; prevY = yAt(i);
                }
            }
        }
        if (st.scopeDisplay == meterbar::ScopeDisplay::Multi) { // pseudo multi-band: also draw latestL faintly
            drawChan(data->latestL, th.accent2.withAlpha(0.5f));
        }
        if (data->scopeFreq > 0) { g.setColour(th.dim); g.setFont(9.f);
            g.drawText(juce::String(data->scopeFreq, 1) + " Hz", p, juce::Justification::bottomRight); }
    }
};

// Anime Mode mascot (v0.16 item 7): the transparent-cutout character,
// drawn directly on the window background in her own resizable cell at
// the END of the module strip (she is NOT a ModuleView — no meter data
// lives under her, and she is hidden entirely outside the Anime theme).
// MainComponent drives the two public fields each frame: excited swaps
// the frame; bobPx is the vertical bounce offset from BeatPulse onsets.
class MascotView : public juce::Component {
public:
    bool excited = false;
    float bobPx = 0.f; // <= 0 (up) during a hop
    MascotView() { setInterceptsMouseClicks(false, false); setOpaque(false); }
    void paint(juce::Graphics& g) override {
        ensureLoaded();
        auto& img = excited ? imgExcited : imgCalm;
        if (!img.isValid()) return;
        auto b = getLocalBounds().toFloat();
        float scale = std::min(b.getWidth() / (float)img.getWidth(), b.getHeight() / (float)img.getHeight());
        float w = img.getWidth() * scale, h = img.getHeight() * scale;
        float x = b.getCentreX() - w * 0.5f;
        float y = b.getBottom() - h + bobPx;
        // soft ground shadow so the cutout sits on the background
        g.setColour(juce::Colour(0x28000000));
        g.fillEllipse(b.getCentreX() - w * 0.30f, b.getBottom() - 6.f, w * 0.60f, 5.f);
        g.drawImage(img, x, y, w, h, 0, 0, img.getWidth(), img.getHeight());
    }
private:
    juce::Image imgCalm, imgExcited; bool loaded = false;
    void ensureLoaded() {
        if (loaded) return; loaded = true;
        imgCalm = juce::ImageFileFormat::loadFrom(BinaryData::animemascotcalmclear_png,
                                                  (size_t)BinaryData::animemascotcalmclear_pngSize);
        imgExcited = juce::ImageFileFormat::loadFrom(BinaryData::animemascotexcitedclear_png,
                                                     (size_t)BinaryData::animemascotexcitedclear_pngSize);
    }
};
