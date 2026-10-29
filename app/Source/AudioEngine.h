#pragma once
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <deque>
#include <meterbar/RingBuffer.hpp>
#include <meterbar/Loudness.hpp>
#include <meterbar/Modules.hpp>
#include <meterbar/State.hpp>
#include "WindowsLoopback.h"

// Owns: JUCE device input, WASAPI loopback, a test-tone generator thread,
// the shared stereo ring buffer (analysis source), the loudness engine,
// WAV recording and clip export. One audio source is active at a time.
class AudioEngine : private juce::AudioIODeviceCallback, private juce::Thread {
public:
    AudioEngine() : juce::Thread("TestTone") {
        ring.prepare(48000 * 65); // >60 s for clip capture
        formatManager.registerBasicFormats();
        deviceManager.initialiseWithDefaultDevices(2, 0);
        loudness.prepare(deviceManager.getCurrentAudioDevice() ?
            deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 48000);
    }
    ~AudioEngine() override { stopTestTone(); loopback.stop(); deviceManager.removeAudioCallback(this); shutdownRecorder(); }

    meterbar::PresetState state; // single source of truth (UI edits, engine reads trim/routing/source)
    meterbar::StereoRingBuffer ring;
    meterbar::LoudnessEngine loudness;
    std::atomic<bool> paused{false};
    std::atomic<double> activeSampleRate{48000};
    std::atomic<int64_t> buffersReceived{0};
    juce::AudioDeviceManager deviceManager;

    void applySource() {
        // A recording is bound to one source at one sample rate: the writer's
        // rate header is fixed at start, so a source switch mid-record would
        // silently produce a wrong-rate WAV. End the take cleanly instead
        // (v0.12 audit). The Rec button label re-syncs from the UI timer.
        stopRecording();
        deviceManager.removeAudioCallback(this); stopTestTone(); loopback.stop();
        if (state.audioSource == "Input") {
            // Honor the saved/chosen input device (unified picker / presets).
            if (!state.audioDeviceName.empty()) {
                auto setup = deviceManager.getAudioDeviceSetup();
                if (setup.inputDeviceName != juce::String(state.audioDeviceName)) {
                    setup.inputDeviceName = juce::String(state.audioDeviceName);
                    setup.useDefaultInputChannels = true;
                    deviceManager.setAudioDeviceSetup(setup, true);
                }
            }
            auto* dev = deviceManager.getCurrentAudioDevice();
            if (dev) { activeSampleRate = dev->getCurrentSampleRate(); loudness.prepare(activeSampleRate.load()); }
            deviceManager.addAudioCallback(this);
        } else if (state.audioSource == "System Loopback") {
            // Capture the CHOSEN output device — by endpoint ID when known
            // (v0.13), by friendly name as fallback (empty = default output).
            loopback.start([this](const float* L, const float* R, int n, double sr){ pushStereo(L, R, n, sr); }, state.outputDeviceName, state.outputDeviceId);
        } else { // Test Tone
            startTestTone();
        }
    }
    // Common ingest: routing (channel pair / ch selects are applied by the device
    // layer for multichannel input; stereo path uses pair 0), trim, then fan-out.
    void pushStereo(const float* L, const float* R, int n, double sr) {
        if (paused.load()) return;
        if (sr > 0 && std::abs(sr - activeSampleRate.load()) > 1.0) { activeSampleRate = sr; }
        float gain = std::pow(10.0f, state.trimDb / 20.0f);
        scratchL.resize(n); scratchR.resize(n);
        for (int i = 0; i < n; ++i) { scratchL[i] = (L ? L[i] : 0.f) * gain; scratchR[i] = (R ? R[i] : 0.f) * gain; }
        ring.pushBlock(scratchL.data(), scratchR.data(), n);
        { const juce::ScopedLock sl(loudLock); loudness.pushBlock(scratchL.data(), scratchR.data(), n); }
        // v0.16: beat/onset detection on the same ingest (drives the Beat
        // Pulse glow and the mascot's bob). Fed in 256-sample sub-blocks —
        // the detector's time constants are expressed per block duration.
        if (sr > 0) {
            if (std::abs(sr - beatSr_) > 1.0) { beatPulse.prepare(sr); beatSr_ = sr; }
            for (int off = 0; off < n; off += 256)
                beatPulse.pushBlock(scratchL.data() + off, scratchR.data() + off, std::min(256, n - off));
        }
        feedRecorder(scratchL.data(), scratchR.data(), n);
        buffersReceived.fetch_add(1);
    }
    // v0.16 accessors
    float beatLevel() { return beatPulse.level(); }
    uint64_t beatOnsets() { return beatPulse.onsetCount(); }
    double loudnessIntegrated() { const juce::ScopedLock sl(loudLock); return loudness.integratedLufs(); }
    double loudnessShortTerm() { const juce::ScopedLock sl(loudLock); return loudness.shortTermLufs(); }
    double loudnessTruePeakDb() { const juce::ScopedLock sl(loudLock); return loudness.truePeakDb(); }
    juce::String loopbackDeviceName() { return juce::String(loopback.activeDeviceName()); }
    juce::String loopbackDeviceId() { return juce::String(loopback.activeDeviceId()); }
    juce::String loopbackWantedName() { return juce::String(loopback.wantedDeviceName()); }
    juce::String loopbackWantedId() { return juce::String(loopback.wantedDeviceId()); }
    juce::String loopbackPickHow() { return juce::String(loopback.pickHow()); }
    long long loopbackPackets() { return loopback.packetsReceived(); }
    juce::String loopbackFormat() { return juce::String(loopback.formatInfo()); }
    juce::String loopbackStatus() { return juce::String(loopback.statusInfo()); }
    float loopbackPeak() { return loopback.peakSeen(); }
    double loudnessValue() { const juce::ScopedLock sl(loudLock); return loudness.valueFor(state.loudnessMode); }
    float peakValue() { const juce::ScopedLock sl(loudLock); return loudness.peak(); }
    void resetPeak() { const juce::ScopedLock sl(loudLock); loudness.resetPeak(); }

    // ---- Recording (v0.11 rewrite, v0.12 hand-off hardening) ----
    // The v0.10 ThreadedWriter design still froze-then-crashed on Stop in the
    // field. This design has ONE owner: a dedicated writer thread creates the
    // WAV writer's lifetime — it alone writes and it alone closes the writer
    // (in run(), after draining its queue). Stop = flip the flag, signal exit,
    // join. The audio path only copies blocks into a bounded lock-held queue.
    // v0.12 audit found one hole in the v0.11 code: feedRecorder() read the
    // recorder_ unique_ptr on the AUDIO thread with no synchronisation while
    // the UI thread could reset() it in stopRecording() — a check-then-use
    // use-after-free window exactly on the record->stop path. The pointer is
    // now handed over under recPtrLock (held only for the short push), and
    // stopRecording moves the thread out under the same lock BEFORE joining,
    // so a live object is never freed under a feeder or a writer.
    class RecorderThread : public juce::Thread {
    public:
        explicit RecorderThread(juce::AudioFormatWriter* w) : juce::Thread("MeterBarRecWriter"), writer_(w) {}
        ~RecorderThread() override { stopThread(4000); } // reached only after a confirmed stop (or app teardown)
        std::atomic<bool> accepting{true};
        void push(const float* L, const float* R, int n) {
            if (!accepting.load() || n <= 0) return;
            std::vector<float> l(L, L + n), r(R, R + n);
            const juce::ScopedLock sl(lock_);
            if (queue_.size() < 400) queue_.emplace_back(std::move(l), std::move(r));
        }
        void run() override {
            while (true) {
                std::pair<std::vector<float>, std::vector<float>> block; bool have = false;
                { const juce::ScopedLock sl(lock_);
                  if (!queue_.empty()) { block = std::move(queue_.front()); queue_.pop_front(); have = true; } }
                if (have && writer_) {
                    int frames = (int)block.first.size();
                    juce::AudioBuffer<float> buf(2, frames);
                    buf.copyFrom(0, 0, block.first.data(), frames);
                    buf.copyFrom(1, 0, block.second.data(), frames);
                    writer_->writeFromAudioSampleBuffer(buf, 0, frames);
                }
                bool empty; { const juce::ScopedLock sl(lock_); empty = queue_.empty(); }
                if (threadShouldExit() && empty) break;
                if (!have) wait(10);
            }
            writer_.reset(); // flush + finalize the WAV on this thread
        }
    private:
        std::unique_ptr<juce::AudioFormatWriter> writer_;
        juce::CriticalSection lock_;
        std::deque<std::pair<std::vector<float>, std::vector<float>>> queue_;
    };
    bool startRecording(const juce::File& file) {
        stopRecording();
        auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream().release());
        if (!stream) return false;
        auto* writer = formatManager.findFormatForFileExtension("wav")->createWriterFor(
            stream.get(), activeSampleRate.load(), 2, 16, {}, 0);
        if (!writer) return false;
        stream.release();
        auto rec = std::make_unique<RecorderThread>(writer);
        rec->startThread();
        { const juce::ScopedLock sl(recPtrLock); recorder_ = std::move(rec); } // publish under the hand-off lock
        recording_.store(true); return true;
    }
    void stopRecording() {
        recording_.store(false);
        std::unique_ptr<RecorderThread> rec;
        { const juce::ScopedLock sl(recPtrLock); rec = std::move(recorder_); } // unpublish first: feeders now skip
        if (rec) {
            rec->accepting.store(false);
            rec->signalThreadShouldExit();
            if (!rec->stopThread(4000)) {
                // The drain is bounded (<=400 queued blocks of local disk
                // writes), so this path should never be taken — but if the
                // thread is somehow still alive, destroying the object would
                // free writer_/lock_/queue_ under its running run() loop.
                // Leak the (small) thread object instead: it exits on its
                // own once the queue drains and harms nothing. Never free
                // under a live thread. (v0.12 audit)
                rec.release();
                DBG("MeterBar: recorder thread did not stop in 4s — leaked rather than freed under a live thread");
            }
        }
    }
    bool isRecording() const { return recording_.load(); }
    bool writeClip(double seconds, const juce::File& file) {
        int maxF = (int)(seconds * activeSampleRate.load());
        std::vector<float> L(maxF), R(maxF);
        int got = ring.readClip(L.data(), R.data(), seconds, activeSampleRate.load());
        if (got <= 0) return false;
        juce::AudioBuffer<float> buf(2, got); buf.copyFrom(0, 0, L.data(), got); buf.copyFrom(1, 0, R.data(), got);
        juce::WavAudioFormat wav; std::unique_ptr<juce::FileOutputStream> out(file.createOutputStream().release());
        if (!out) return false;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(out.get(), activeSampleRate.load(), 2, 16, {}, 0));
        if (!w) return false; out.release(); return w->writeFromAudioSampleBuffer(buf, 0, got);
    }
    juce::String currentDeviceName() {
        auto* d = deviceManager.getCurrentAudioDevice(); return d ? d->getName() : juce::String("(none)");
    }
private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override {
        for (int c = 0; c < numOut; ++c) if (out[c]) std::fill(out[c], out[c] + numSamples, 0.f);
        if (numIn <= 0 || in == nullptr) return;
        int c1 = juce::jlimit(0, numIn - 1, state.routingCh1 + state.routingPair * 2);
        int c2 = juce::jlimit(0, numIn - 1, state.routingCh2 + state.routingPair * 2);
        pushStereo(in[c1], numIn > 1 ? in[c2] : in[c1], numSamples, activeSampleRate.load());
    }
    void audioDeviceAboutToStart(juce::AudioIODevice* dev) override {
        if (dev) { activeSampleRate = dev->getCurrentSampleRate(); const juce::ScopedLock sl(loudLock); loudness.prepare(activeSampleRate.load()); }
    }
    void audioDeviceStopped() override {}
    // Test tone: 220 Hz + 440 Hz stereo sines with slow amplitude swell, so every
    // meter visibly moves with no working capture at all.
    void run() override {
        double sr = 48000; activeSampleRate = sr; { const juce::ScopedLock sl(loudLock); loudness.prepare(sr); }
        double phase = 0, phase2 = 0; int64_t t = 0;
        std::vector<float> L(256), R(256);
        while (!threadShouldExit()) {
            for (int i = 0; i < 256; ++i) {
                double env = 0.55 + 0.4 * std::sin(2 * M_PI * 0.11 * (t + i) / sr);
                L[i] = (float)(0.5 * env * std::sin(phase) + 0.2 * std::sin(phase2));
                R[i] = (float)(0.5 * env * std::sin(phase * 0.999) + 0.2 * std::sin(phase2 * 1.003));
                phase += 2 * M_PI * 220 / sr; phase2 += 2 * M_PI * 440 / sr;
            }
            t += 256; pushStereo(L.data(), R.data(), 256, sr); wait(5);
        }
    }
    void startTestTone() { if (!isThreadRunning()) startThread(); }
    void stopTestTone() { if (isThreadRunning()) stopThread(500); }
    void feedRecorder(const float* L, const float* R, int n) {
        if (!recording_.load()) return;
        // Hand-off lock held only for the push itself (two small vector
        // copies + the recorder's own queue lock): stopRecording() can never
        // free the RecorderThread while a feeder is inside it. Lock order is
        // always recPtrLock -> RecorderThread::lock_, never the reverse.
        const juce::ScopedLock sl(recPtrLock);
        if (recorder_) recorder_->push(L, R, n);
    }
    void shutdownRecorder() { stopRecording(); }
    WindowsLoopback loopback;
    meterbar::BeatPulse beatPulse; double beatSr_ = 0; // v0.16
    juce::AudioFormatManager formatManager;
    std::unique_ptr<RecorderThread> recorder_;
    juce::CriticalSection recPtrLock; // guards recorder_ hand-off between feeder threads and the UI thread
    std::atomic<bool> recording_{false};
    juce::CriticalSection loudLock;
    std::vector<float> scratchL, scratchR;
};
