#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <BinaryData.h>
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
        juce::AudioBuffer<float> buf((int)reader->numChannels, (int)reader->lengthInSamples);
        reader->read(&buf, 0, (int)reader->lengthInSamples, 0, true, true);
        // Quiet UI-feedback level, never sample level: the buffer itself
        // peaks ~0.5 and the player gain trims it further.
        buf.applyGain(0.6f);
        mixer.addInputSource(new juce::MemoryAudioSource(buf, true, false), true);
    }
    void hush() { mixer.removeAllInputs(); }
private:
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
