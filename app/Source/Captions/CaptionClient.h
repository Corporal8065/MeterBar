#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <meterbar/Captions.hpp>
#include "CaptionTap.h"

// LIVE CAPTIONS — TRANSPORT CLIENT (v0.23.0, spec section 2.3).
//
// Owns the Python helper process and the loopback TCP seam:
//   resolve python -> launch helper hidden with a session id ->
//   poll its ready file -> verify pid/session/protocol -> connect ->
//   HELLO -> stream tap frames (streamer thread) -> surface
//   READY / STATUS / CAPTION / ERROR events to the controller,
//   which drains them from pollEvent() on the message thread.
//
// Python resolution order (spec 3.5): METERBAR_CAPTIONS_PYTHON env
// override; the dedicated runtime venv under the captions runtime
// dir; a discovered Room Voice Link venv (bootstrap convenience).
// The helper script ships installed at {app}\Captions\; the
// METERBAR_CAPTIONS_HELPER env override points at a source tree in
// development.
struct CaptionEvent {
    enum class Kind { Ready, Status, Caption, Error, LaunchFailed, Disconnected };
    Kind kind = Kind::Status;
    juce::var json;          // parsed payload (Ready/Status/Caption/Error)
    juce::String message;    // LaunchFailed detail
};

class CaptionClient {
public:
    CaptionClient() = default;
    ~CaptionClient() { stop(); }

    // Runtime layout: %LOCALAPPDATA%\MeterBar\Captions (falls back to
    // the JUCE app-data dir when LOCALAPPDATA is unset).
    static juce::File runtimeDir();
    static juce::File helperScriptFile();
    static juce::String findPythonExe();

    void setTap(CaptionTap* tap) { tap_ = tap; }

    // Async: returns immediately; progress arrives as events.
    // mode: "transcribe" | "translate".
    void start(const juce::String& mode);
    void stop();
    bool isRunning() const { return running_.load(); }
    bool isConnected() const { return connected_.load(); }

    // Stores the latest config and sends it (now if connected, or
    // right after HELLO during start()).
    void sendConfig(const juce::String& mode, const juce::String& sourceClass);
    // "source_paused" | "source_resumed" | "source_changed" | "stop"
    void sendControl(const juce::String& event);

    bool pollEvent(CaptionEvent& out);

private:
    void launchThreadMain(juce::String mode);
    void ioThreadMain();
    void streamerThreadMain();
    void pushEvent(CaptionEvent e);
    bool writePacket(const meterbar::CaptionPacket& pkt);
    void sendJson(meterbar::CaptionPacketType type, const juce::var& obj, uint32_t epoch);

    CaptionTap* tap_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> seq_{0};
    juce::ChildProcess process_;
    std::unique_ptr<juce::StreamingSocket> socket_;
    std::thread launchThread_, ioThread_, streamerThread_;
    std::mutex writeMutex_;
    std::mutex eventMutex_;
    std::deque<CaptionEvent> events_;
    std::mutex configMutex_;
    juce::String pendingMode_, pendingSourceClass_;
    juce::String sessionId_;
    int64_t lastWriteMs_ = 0;
    JUCE_DECLARE_NON_COPYABLE(CaptionClient)
};
