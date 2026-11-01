#include "CaptionController.h"

namespace {
int64_t nowMs() { return juce::Time::currentTimeMillis(); }
juce::String upper2(const juce::String& s) { return s.substring(0, 2).toUpperCase(); }
} // namespace

void CaptionController::initialise(
        AudioEngine& engine,
        std::function<juce::Rectangle<int>()> barBoundsProvider) {
    engine_ = &engine;
    settings_ = CaptionSettings::load();
    client_.setTap(&engine.captionTap);
    overlay_.barBoundsProvider = std::move(barBoundsProvider);
    overlay_.onBoundsChanged = [this] { persistBounds(); };
    overlay_.restoreBounds(settings_.x, settings_.y, settings_.w, settings_.hasPosition);
    overlay_.setAlwaysOnTopFlag(settings_.alwaysOnTop);
    lastPaused_ = engine.paused.load();
    lastEpoch_ = engine.captionEpoch();
    lastSource_ = juce::String(engine.state.audioSource);
    if (settings_.mode != meterbar::CaptionMode::Off
        && lastSource_ != "Test Tone") {
        helperWanted_ = true;
        startHelper();
    }
}

void CaptionController::shutdown() {
    if (helperWanted_ || client_.isRunning()) stopHelper(true);
    if (engine_) engine_->captionTap.setEnabled(false);
    overlay_.hide();
    persistSettings();
}

void CaptionController::persistSettings() { CaptionSettings::save(settings_); }

void CaptionController::persistBounds() {
    const auto b = overlay_.bounds();
    settings_.hasPosition = overlay_.hasPosition();
    settings_.x = b.getX();
    settings_.y = b.getY();
    settings_.w = b.getWidth();
    persistSettings();
}

juce::String CaptionController::sourceClass() const {
    if (!engine_) return "system";
    return engine_->state.audioSource == "Input" ? "microphone" : "system";
}
juce::String CaptionController::sourceLabel() const {
    if (!engine_) return "SYSTEM AUDIO";
    return engine_->state.audioSource == "Input" ? "MICROPHONE" : "SYSTEM AUDIO";
}

void CaptionController::setMode(meterbar::CaptionMode m) {
    if (m == settings_.mode) return;
    settings_.mode = m;
    persistSettings();
    if (m == meterbar::CaptionMode::Off) {
        helperWanted_ = false;
        stopHelper(true);
        if (engine_) engine_->captionTap.setEnabled(false);
        display_.clear();
        helperState_ = "stopped";
        setupNeeded_ = false;
        overlay_.hide();
        return;
    }
    if (lastSource_ == "Test Tone") return; // poll() starts it when the source changes
    helperWanted_ = true;
    restartCount_ = 0;
    startHelper();
}

void CaptionController::startHelper() {
    if (!engine_) return;
    stoppingIntentionally_ = false;
    engine_->captionTap.setEnabled(true);
    helperState_ = "starting";
    client_.sendConfig(juce::String(meterbar::captionModeName(settings_.mode)),
                       sourceClass());
    client_.start(juce::String(meterbar::captionModeName(settings_.mode)));
}

void CaptionController::stopHelper(bool intentional) {
    stoppingIntentionally_ = intentional;
    if (client_.isConnected()) client_.sendControl("stop");
    client_.stop();
    helperState_ = "stopped";
}

void CaptionController::setOverlayAlwaysOnTop(bool on) {
    settings_.alwaysOnTop = on;
    overlay_.setAlwaysOnTopFlag(on);
    persistSettings();
}

void CaptionController::moveCaptionWindow() { overlay_.enterMoveMode(); }
void CaptionController::resetCaptionPosition() { overlay_.resetPosition(); }

void CaptionController::launchSetup() {
#if JUCE_WINDOWS
    const auto bat = CaptionClient::helperScriptFile()
                         .getSiblingFile("setup_captions_runtime.bat");
    if (bat.existsAsFile()) bat.startAsProcess();
#endif
}

juce::String CaptionController::statusText() const {
    using namespace meterbar;
    if (settings_.mode == CaptionMode::Off) return "Status: Off";
    if (lastSource_ == "Test Tone") return "Status: Unavailable for Test Tone";
    if (helperState_ == "starting") return "Status: Starting…";
    if (helperState_ == "loading_model") return "Status: Loading model…";
    if (helperState_ == "game_hold") return "Status: Paused — game running";
    if (helperState_ == "gpu_busy") return "Status: Paused — GPU busy";
    if (helperState_ == "setup_needed") return "Status: Setup needed";
    if (helperState_ == "error")
        return "Status: Error" + (lastError_.isNotEmpty() ? " — " + lastError_ : juce::String());
    if (helperState_ == "listening" || helperState_ == "speech"
        || helperState_ == "cpu_degraded") {
        juce::String via = helperDevice_ == "cpu" ? "CPU" : "CUDA";
        juce::String s = "Status: Listening — " + via;
        if (helperModel_.isNotEmpty()) s += " " + helperModel_;
        if (helperDevice_ == "cpu") s += " (degraded)";
        return s;
    }
    return "Status: " + helperState_;
}

juce::String CaptionController::statusBadge() const {
    using namespace meterbar;
    if (engine_ && engine_->paused.load()) return "PAUSED";
    if (helperState_ == "game_hold") return "PAUSED — GAME";
    if (helperState_ == "gpu_busy") return "PAUSED — GPU BUSY";
    if (helperDevice_ == "cpu" && asrLagMs_ > 2500)
        return "DELAYED " + juce::String(asrLagMs_ / 1000) + "s — CPU";
    if (settings_.mode == CaptionMode::Translate) {
        if (detectedLanguage_.isEmpty()) return "AUTO → EN — " + sourceLabel();
        if (detectedLanguage_ == "en") return "EN — " + sourceLabel();
        return upper2(detectedLanguage_) + " → EN — " + sourceLabel();
    }
    return "LIVE — " + sourceLabel();
}

void CaptionController::applyEvent(const CaptionEvent& e) {
    using namespace meterbar;
    switch (e.kind) {
        case CaptionEvent::Kind::Ready:
            helperState_ = "listening";
            readyMs_ = nowMs();
            break;
        case CaptionEvent::Kind::Status: {
            const auto st = e.json.getProperty("state", "").toString();
            if (st.isNotEmpty()) helperState_ = st;
            helperModel_ = e.json.getProperty("model", "").toString();
            helperDevice_ = e.json.getProperty("device", "").toString();
            helperCompute_ = e.json.getProperty("compute_type", "").toString();
            asrLagMs_ = (int)e.json.getProperty("asr_lag_ms", 0);
            const auto lang = e.json.getProperty("language", "").toString();
            if (lang.isNotEmpty()) detectedLanguage_ = lang;
            setupNeeded_ = (helperState_ == "setup_needed");
            break;
        }
        case CaptionEvent::Kind::Caption: {
            CaptionBlock b;
            b.id = e.json.getProperty("id", "").toString().toStdString();
            b.epoch = (uint32_t)(int)e.json.getProperty("epoch", 0);
            b.startMs = (int)e.json.getProperty("start_ms", 0);
            b.endMs = (int)e.json.getProperty("end_ms", 0);
            b.text = e.json.getProperty("text", "").toString().toStdString();
            b.final = (bool)e.json.getProperty("final", false);
            b.mode = settings_.mode;
            b.language = e.json.getProperty("language", "").toString().toStdString();
            b.languageProb = (float)(double)e.json.getProperty("language_probability", 0.0);
            if (!b.language.empty()) detectedLanguage_ = juce::String(b.language);
            // Stale-epoch captions never splice into the new source.
            if (engine_ && b.epoch == engine_->captionEpoch())
                display_.apply(b, nowMs());
            break;
        }
        case CaptionEvent::Kind::Error: {
            const auto code = e.json.getProperty("code", "").toString();
            lastError_ = e.json.getProperty("message", "").toString();
            helperState_ = "error";
            if (code == "protocol_mismatch" || code == "worker_crashed") {
                // Spec 2.3/3.4: not retried silently — the feature
                // turns off until Jackson toggles it again.
                settings_.mode = CaptionMode::Off;
                helperWanted_ = false;
                persistSettings();
                if (engine_) engine_->captionTap.setEnabled(false);
            }
            break;
        }
        case CaptionEvent::Kind::LaunchFailed:
            lastError_ = e.message;
            helperState_ = "error";
            setupNeeded_ = e.message.containsIgnoreCase("not found")
                        || e.message.containsIgnoreCase("Could not start");
            break;
        case CaptionEvent::Kind::Disconnected: {
            if (!helperWanted_ || stoppingIntentionally_) break;
            // One automatic restart; a second disconnect within
            // 60 s of the first turns the feature off (spec 2.3).
            const auto t = nowMs();
            if (t - firstRestartMs_ > 60000) { restartCount_ = 0; firstRestartMs_ = t; }
            ++restartCount_;
            if (restartCount_ >= 2) {
                lastError_ = "Caption helper stopped twice";
                helperState_ = "error";
                settings_.mode = CaptionMode::Off;
                helperWanted_ = false;
                persistSettings();
                if (engine_) engine_->captionTap.setEnabled(false);
                overlay_.hide();
            } else {
                startHelper();
            }
            break;
        }
    }
}

void CaptionController::poll() {
    if (!engine_) return;
    CaptionEvent e;
    bool gotEvents = false;
    while (client_.pollEvent(e)) { applyEvent(e); gotEvents = true; }

    // --- Pause edges: finalise the utterance, then hold. ---
    const bool paused = engine_->paused.load();
    if (paused != lastPaused_) {
        lastPaused_ = paused;
        if (client_.isConnected())
            client_.sendControl(paused ? "source_paused" : "source_resumed");
    }
    // --- Epoch edges: a new source epoch restarts the timeline. ---
    const auto epoch = engine_->captionEpoch();
    if (epoch != lastEpoch_) {
        lastEpoch_ = epoch;
        display_.clear();
        if (client_.isConnected()) client_.sendControl("source_changed");
    }
    // --- Source edges: Test Tone has no captions; leaving it with a
    // --- mode selected (re)starts the helper. ---
    const juce::String source = engine_->state.audioSource;
    if (source != lastSource_) {
        const bool wasTone = lastSource_ == "Test Tone";
        lastSource_ = source;
        if (source == "Test Tone") {
            if (client_.isRunning()) stopHelper(true);
        } else {
            if (client_.isConnected())
                client_.sendConfig(
                    juce::String(meterbar::captionModeName(settings_.mode)),
                    sourceClass());
            if (wasTone && helperWanted_ && !client_.isRunning()) {
                restartCount_ = 0;
                startHelper();
            }
        }
    }
    pushOverlay(gotEvents);
}

void CaptionController::pushOverlay(bool force) {
    using namespace meterbar;
    const auto t = nowMs();
    if (!force && t - lastOverlayPushMs_ < 250) return;
    lastOverlayPushMs_ = t;
    if (settings_.mode == CaptionMode::Off) { overlay_.hide(); return; }

    const auto view = display_.view(t);
    juce::String key;
    if (view.hasCurrent || view.hasPrevious) {
        key = "cap|" + juce::String(view.hasPrevious ? view.previous.text : "")
            + "|" + juce::String(view.hasCurrent ? view.current.text : "")
            + "|" + (view.hasCurrent && view.current.final ? "f" : "p")
            + "|" + statusBadge();
        if (key != lastPushedKey_) {
            lastPushedKey_ = key;
            overlay_.showCaptions(
                view.hasPrevious ? juce::String(view.previous.text) : juce::String(),
                view.hasCurrent ? juce::String(view.current.text) : juce::String(),
                view.hasCurrent && view.current.final, statusBadge());
        }
        return;
    }
    // No blocks: a status pill only when there is something to say.
    juce::String pill;
    if (lastSource_ == "Test Tone") pill = ""; // menu carries the explanation
    else if (helperState_ == "starting" || helperState_ == "loading_model")
        pill = "Live captions: loading model…";
    else if (helperState_ == "game_hold") pill = "Live captions paused — game running";
    else if (helperState_ == "gpu_busy") pill = "Live captions paused — GPU busy";
    else if (helperState_ == "setup_needed") pill = "Live captions: setup needed — see the Menu";
    else if (helperState_ == "error") pill = "Live captions: error — see the Menu";
    else if (engine_->paused.load()) pill = "Live captions: paused";
    else if ((helperState_ == "listening" || helperState_ == "cpu_degraded")
             && readyMs_ > 0 && t - readyMs_ < 3000)
        pill = "Live captions: listening…";
    key = "pill|" + pill;
    if (key != lastPushedKey_) {
        lastPushedKey_ = key;
        if (pill.isNotEmpty()) overlay_.showPill(pill);
        else overlay_.hide();
    }
}
