#include "CaptionClient.h"
#include "MeterBarVersion.h" // generated: single-source version
#include <meterbar/Captions.hpp>

namespace {
juce::String envOr(const juce::String& name, const juce::String& fallback = {}) {
    return juce::SystemStats::getEnvironmentVariable(name, fallback);
}
} // namespace

juce::File CaptionClient::runtimeDir() {
    const auto local = envOr("LOCALAPPDATA");
    juce::File base = local.isNotEmpty()
        ? juce::File(local)
        : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    return base.getChildFile("MeterBar").getChildFile("Captions");
}

juce::File CaptionClient::helperScriptFile() {
    const auto env = envOr("METERBAR_CAPTIONS_HELPER");
    if (env.isNotEmpty()) return juce::File(env);
    auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                      .getParentDirectory();
    return exeDir.getChildFile("Captions").getChildFile("meterbar_captions_helper.py");
}

juce::String CaptionClient::findPythonExe() {
    const auto env = envOr("METERBAR_CAPTIONS_PYTHON");
    if (env.isNotEmpty()) return env;
    std::vector<juce::File> candidates;
    const auto venv = runtimeDir().getChildFile(".venv");
#if JUCE_WINDOWS
    candidates.push_back(venv.getChildFile("Scripts").getChildFile("pythonw.exe"));
    candidates.push_back(venv.getChildFile("Scripts").getChildFile("python.exe"));
    // Bootstrap fallback (spec 3.5): the Room Voice Link kits' venvs,
    // newest first — same discovery idea as start-voice-link.bat.
    const auto dl = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                        .getChildFile("Downloads").getChildFile("files for milo");
    for (const char* kit : { "room-voice-link-v2.2", "room-voice-link-v2.1",
                             "room-voice-link-v2", "room-voice-link" })
        candidates.push_back(dl.getChildFile(kit).getChildFile(".venv")
                                 .getChildFile("Scripts").getChildFile("python.exe"));
#else
    candidates.push_back(venv.getChildFile("bin").getChildFile("python"));
#endif
    for (auto& f : candidates)
        if (f.existsAsFile()) return f.getFullPathName();
#if JUCE_WINDOWS
    return "python.exe"; // PATH last resort; the launch failure will say so
#else
    return "python3";
#endif
}

void CaptionClient::start(const juce::String& mode) {
    if (running_.load()) { sendConfig(mode, pendingSourceClass_); return; }
    stopping_.store(false);
    running_.store(true);
    if (launchThread_.joinable()) launchThread_.join();
    launchThread_ = std::thread([this, mode] { launchThreadMain(mode); });
}

void CaptionClient::stop() {
    stopping_.store(true);
    running_.store(false);
    connected_.store(false);
    if (socket_) socket_->close();
    if (process_.isRunning()) process_.kill();
    for (auto* t : { &launchThread_, &ioThread_, &streamerThread_ })
        if (t->joinable()) t->join();
    socket_.reset();
}

void CaptionClient::launchThreadMain(juce::String mode) {
    auto fail = [this](const juce::String& why) {
        running_.store(false);
        CaptionEvent e; e.kind = CaptionEvent::Kind::LaunchFailed; e.message = why;
        pushEvent(e);
    };
    const auto script = helperScriptFile();
    if (!script.existsAsFile())
        return fail("Caption helper not found at " + script.getFullPathName());
    const auto python = findPythonExe();
    const auto rt = runtimeDir();
    const auto runDir = rt.getChildFile("run");
    runDir.createDirectory();
    sessionId_ = "mbc-" + juce::Uuid().toString().substring(0, 8);
    const auto readyFile = runDir.getChildFile("helper-ready-" + sessionId_ + ".json");
    readyFile.deleteFile();

    juce::StringArray args;
    args.add(python);
    args.add(script.getFullPathName());
    args.add("--session"); args.add(sessionId_);
    args.add("--rundir"); args.add(runDir.getFullPathName());
    args.add("--log-dir"); args.add(rt.getChildFile("logs").getFullPathName());
    args.add("--models-dir"); args.add(envOr("METERBAR_CAPTIONS_MODELS_DIR",
                                             rt.getChildFile("models").getFullPathName()));
    args.add("--mode"); args.add(mode);
    if (envOr("METERBAR_CAPTIONS_ALLOW_MODEL_NAMES") == "1")
        args.add("--allow-model-names");
    if (!process_.start(args))
        return fail("Could not start the caption helper with " + python);

    // Poll the ready file (<= 20 s); a dead child fails fast.
    juce::var ready;
    const int64_t deadline = juce::Time::currentTimeMillis() + 20000;
    while (juce::Time::currentTimeMillis() < deadline) {
        if (stopping_.load()) return;
        if (!process_.isRunning())
            return fail("The caption helper exited during startup (see its log in "
                        + rt.getChildFile("logs").getFullPathName() + ")");
        if (readyFile.existsAsFile()) {
            ready = juce::JSON::parse(readyFile.loadFileAsString());
            if ((int)ready.getProperty("protocol", 0) == (int)meterbar::kCaptionProtocolVersion
                && ready.getProperty("session", "").toString() == sessionId_)
                break;
            ready = juce::var(); // stale file from another session — keep waiting
        }
        juce::Thread::sleep(100);
    }
    if (ready.isVoid())
        return fail("The caption helper did not become ready in time");

    socket_ = std::make_unique<juce::StreamingSocket>();
    const int port = (int)ready.getProperty("port", 0);
    if (port <= 0 || !socket_->connect("127.0.0.1", port, 5000)) {
        socket_.reset();
        return fail("Could not connect to the caption helper on 127.0.0.1");
    }
    connected_.store(true);
    // HELLO, then the pending CONFIG (the helper starts its worker on
    // CONFIG; until then it just holds the connection).
    juce::DynamicObject::Ptr hello = new juce::DynamicObject();
    hello->setProperty("app_version", juce::String(meterbar::kVersionString));
    hello->setProperty("protocol", (int)meterbar::kCaptionProtocolVersion);
    hello->setProperty("session", sessionId_);
    hello->setProperty("encoding", "pcm_s16le");
    hello->setProperty("epoch", (int)(tap_ ? tap_->epoch() : 0));
    sendJson(meterbar::CaptionPacketType::Hello, juce::var(hello),
             tap_ ? tap_->epoch() : 0);
    {
        // Read the pending config under the lock, but do NOT call
        // sendConfig while holding it — sendConfig takes the same
        // (non-recursive) mutex and the launch thread would deadlock
        // before the IO threads ever start.
        juce::String pm, psc;
        {
            const std::lock_guard<std::mutex> lock(configMutex_);
            pm = pendingMode_;
            psc = pendingSourceClass_;
        }
        if (pm.isNotEmpty()) sendConfig(pm, psc);
    }
    ioThread_ = std::thread([this] { ioThreadMain(); });
    streamerThread_ = std::thread([this] { streamerThreadMain(); });
}

void CaptionClient::ioThreadMain() {
    meterbar::CaptionPacketDecoder decoder;
    std::vector<uint8_t> buf(65536);
    while (!stopping_.load() && connected_.load()) {
        if (!socket_) break;
        const int ready = socket_->waitUntilReady(true, 100);
        if (ready < 0) break;
        if (ready == 0) continue;
        const int n = socket_->read(buf.data(), (int)buf.size(), false);
        if (n < 0) break;
        if (n == 0) continue;
        decoder.feed(buf.data(), (size_t)n);
        meterbar::CaptionPacket pkt;
        while (decoder.next(pkt)) {
            CaptionEvent e;
            switch (pkt.type) {
                case meterbar::CaptionPacketType::Ready:
                    e.kind = CaptionEvent::Kind::Ready; break;
                case meterbar::CaptionPacketType::Status:
                    e.kind = CaptionEvent::Kind::Status; break;
                case meterbar::CaptionPacketType::Caption:
                    e.kind = CaptionEvent::Kind::Caption; break;
                case meterbar::CaptionPacketType::Error:
                    e.kind = CaptionEvent::Kind::Error; break;
                default:
                    continue; // heartbeat etc. — nothing to surface
            }
            e.json = juce::JSON::parse(juce::String::fromUTF8(
                reinterpret_cast<const char*>(pkt.payload.data()),
                (int)pkt.payload.size()));
            pushEvent(e);
        }
        if (decoder.error()) break;
    }
    if (!stopping_.load()) {
        connected_.store(false);
        running_.store(false);
        CaptionEvent e; e.kind = CaptionEvent::Kind::Disconnected;
        pushEvent(e);
    }
}

void CaptionClient::streamerThreadMain() {
    CaptionTap::Frame frame;
    while (!stopping_.load() && connected_.load()) {
        bool sent = false;
        if (tap_ && tap_->pop(frame)) {
            meterbar::CaptionAudioPrefix pre;
            pre.firstNativeSample = frame.firstNativeSample;
            pre.sampleRate = frame.sampleRate;
            pre.frameSamples = (uint16_t)frame.samples.size();
            meterbar::CaptionPacket pkt;
            pkt.type = meterbar::CaptionPacketType::Audio;
            pkt.epoch = frame.epoch;
            pkt.payload = meterbar::buildCaptionAudioPayload(pre, frame.samples.data());
            sent = writePacket(pkt);
            if (!sent) break;
        }
        if (!sent) {
            // Heartbeat when the seam has been quiet for > 1 s (spec 2.3).
            if (juce::Time::currentTimeMillis() - lastWriteMs_ > 1000) {
                meterbar::CaptionPacket hb;
                hb.type = meterbar::CaptionPacketType::Heartbeat;
                hb.epoch = tap_ ? tap_->epoch() : 0;
                writePacket(hb);
            }
            juce::Thread::sleep(5);
        }
    }
}

bool CaptionClient::writePacket(const meterbar::CaptionPacket& pktIn) {
    if (!socket_ || !connected_.load()) return false;
    auto pkt = pktIn;
    pkt.sequence = seq_.fetch_add(1) + 1;
    const auto bytes = meterbar::encodeCaptionPacket(pkt);
    const std::lock_guard<std::mutex> lock(writeMutex_);
    const int written = socket_->write(bytes.data(), (int)bytes.size());
    if (written != (int)bytes.size()) return false;
    lastWriteMs_ = juce::Time::currentTimeMillis();
    return true;
}

void CaptionClient::sendJson(meterbar::CaptionPacketType type, const juce::var& obj,
                             uint32_t epoch) {
    meterbar::CaptionPacket pkt;
    pkt.type = type;
    pkt.epoch = epoch;
    const auto s = juce::JSON::toString(obj, true);
    pkt.payload.assign(s.toRawUTF8(), s.toRawUTF8() + s.getNumBytesAsUTF8());
    writePacket(pkt);
}

void CaptionClient::sendConfig(const juce::String& mode, const juce::String& sourceClass) {
    {
        const std::lock_guard<std::mutex> lock(configMutex_);
        pendingMode_ = mode;
        if (sourceClass.isNotEmpty()) pendingSourceClass_ = sourceClass;
    }
    if (!connected_.load()) return;
    juce::DynamicObject::Ptr cfg = new juce::DynamicObject();
    cfg->setProperty("mode", mode);
    cfg->setProperty("source_class", pendingSourceClass_);
    sendJson(meterbar::CaptionPacketType::Config, juce::var(cfg),
             tap_ ? tap_->epoch() : 0);
}

void CaptionClient::sendControl(const juce::String& event) {
    if (!connected_.load()) return;
    juce::DynamicObject::Ptr ctl = new juce::DynamicObject();
    ctl->setProperty("event", event);
    sendJson(meterbar::CaptionPacketType::Control, juce::var(ctl),
             tap_ ? tap_->epoch() : 0);
}

void CaptionClient::pushEvent(CaptionEvent e) {
    const std::lock_guard<std::mutex> lock(eventMutex_);
    if (events_.size() < 512) events_.push_back(std::move(e));
}

bool CaptionClient::pollEvent(CaptionEvent& out) {
    const std::lock_guard<std::mutex> lock(eventMutex_);
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}
