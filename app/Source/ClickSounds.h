#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <BinaryData.h>
#include <cmath>
// Anime Mode cute click sounds (v0.16). The three WAVs are synthesized
// (assets/synth_clicks.py — pure sine gestures, no external samples) and
// embedded via BinaryData. Playback uses its OWN output-only device
// manager + AudioSourcePlayer, initialised lazily on the first click, so
// the capture path (input-only device manager in AudioEngine) is never
// touched. The caller (MainComponent) gates every play on
// theme == Anime && the "Cute click sounds" toggle; hush() additionally
// drops anything queued the moment the theme switches away.
class ClickSounds {
public:
    enum class Which { Pop, Bloop, Sparkle };
    ~ClickSounds() {
        player.setSource(nullptr);
        dm.removeAudioCallback(&player);
        dm.closeAudioDevice();
    }
    void play(Which w) {
        ensureInit();
        if (!ready) return;
        const void* data = nullptr; int size = 0;
        switch (w) {
        case Which::Pop:     data = BinaryData::clickpop_wav;     size = BinaryData::clickpop_wavSize; break;
        case Which::Bloop:   data = BinaryData::clickbloop_wav;   size = BinaryData::clickbloop_wavSize; break;
        case Which::Sparkle: data = BinaryData::clicksparkle_wav; size = BinaryData::clicksparkle_wavSize; break;
        }
        if (!data || size <= 0) return;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(
            new juce::MemoryInputStream(data, (size_t)size, false), true));
        if (!reader) return;
        juce::AudioBuffer<float> raw((int)reader->numChannels, (int)reader->lengthInSamples);
        reader->read(&raw, 0, (int)reader->lengthInSamples, 0, true, true);
        auto buf = renderClick(raw, reader->sampleRate);
        mixer.addInputSource(new juce::MemoryAudioSource(buf, true, false), true);
    }
    void hush() { mixer.removeAllInputs(); }
private:
    // Click render (family fix — SessionBar v0.4.12 / TickerBar v0.6.4):
    // the source WAVs are MONO, and juce::MemoryAudioSource maps a mono
    // buffer onto destination channel 0 only and actively CLEARS the
    // other channels (juce_MemoryAudioSource.cpp) — so the clicks
    // played in the LEFT EAR ONLY. The render below fixes the image
    // and applies Jackson's spec (intake 2026-10-31, item 3): both
    // ears, softer, high-cut + a touch of reverb.
    static juce::AudioBuffer<float> renderClick(const juce::AudioBuffer<float>& src, double sampleRate) {
        const int n = src.getNumSamples();
        const int sr = sampleRate > 0 ? (int)sampleRate : 44100;
        // 1) Centred stereo image: mono folds to both ears equally; a
        //    stereo source keeps its own channels.
        juce::AudioBuffer<float> flat(2, n);
        if (src.getNumChannels() == 1) {
            flat.copyFrom(0, 0, src, 0, 0, n);
            flat.copyFrom(1, 0, src, 0, 0, n);
        } else {
            flat.copyFrom(0, 0, src, 0, 0, n);
            flat.copyFrom(1, 0, src, 1, 0, n);
        }
        // 2) High-cut: one-pole lowpass, fc ~4 kHz — takes the brittle
        //    edge off the sine gestures without dulling them.
        const float a = 1.0f - std::exp(-2.0f * 3.14159265f * 4000.0f / (float)sr);
        for (int ch = 0; ch < 2; ++ch) {
            auto* d = flat.getWritePointer(ch);
            float y = 0.f;
            for (int i = 0; i < n; ++i) { y += a * (d[i] - y); d[i] = y; }
        }
        // 3) A touch of reverb: short feedforward taps (small-room
        //    fingerprint) over an extended tail, wet kept low.
        const int tail = (int)(0.10 * sr);
        juce::AudioBuffer<float> out(2, n + tail);
        out.clear();
        static const float taps[][2] = { {17.f, 0.20f}, {27.f, 0.15f}, {39.f, 0.11f}, {53.f, 0.07f} };
        for (int ch = 0; ch < 2; ++ch) {
            auto* d = out.getWritePointer(ch);
            const auto* s = flat.getReadPointer(ch);
            for (int i = 0; i < n; ++i) d[i] = s[i];
            for (auto& t : taps) {
                const int off = (int)(t[0] * 0.001f * (float)sr);
                const float g = t[1];
                for (int i = 0; i < n; ++i) d[i + off] += g * s[i];
            }
        }
        // 4) Quieter: the old path applied 0.6 here with the player at
        //    0.5 (0.30 of sample level overall); now 0.38 x 0.5 = 0.19,
        //    about -4 dB — a UI tick, not a beep.
        out.applyGain(0.38f);
        return out;
    }
    void ensureInit() {
        if (ready || tried) return;
        tried = true;
        auto err = dm.initialise(0, 2, nullptr, true); // output only, default device
        if (err.isNotEmpty()) return;
        player.setSource(&mixer);
        player.setGain(0.5f);
        dm.addAudioCallback(&player);
        ready = true;
    }
    juce::AudioDeviceManager dm;
    juce::AudioSourcePlayer player;
    juce::MixerAudioSource mixer;
    bool ready = false, tried = false;
};
