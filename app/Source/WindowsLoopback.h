#pragma once
#include <functional>
#include <string>
#include <vector>
// System-output (loopback) capture of a CHOSEN output device (v0.9).
// Windows: real WASAPI loopback in WindowsLoopback.cpp — the render endpoint
// identified by deviceIdUtf8 (v0.13), falling back to the friendly-name match
// on deviceNameUtf8 (empty id+name = default output device).
// Other platforms: start() returns false — UI falls back to Input / Test Tone.
class WindowsLoopback {
public:
    using Callback = std::function<void(const float* L, const float* R, int numFrames, double sampleRate)>;
    struct EndpointInfo { std::string name; std::string id; }; // friendly name + MMDevice endpoint ID (UTF-8)
    // All ACTIVE render endpoints as the OS reports them (Windows). The UI
    // picker is built from THIS list so the name a user picks is the name of
    // a real endpoint, and the ID lets capture key on identity, not text.
    static std::vector<EndpointInfo> enumerateRenderEndpoints();
    // v0.20.2: cached variant — the raw enumeration is a synchronous
    // COM/MMDevice sweep on the message thread, and it used to re-run
    // on every picker build, hamburger-menu open, device pick and
    // (every ~2 s) Debug-window refresh. enumerateRenderEndpoints()
    // refreshes this cache as a side effect; the cached call returns
    // the last list unless forceRefresh (or no list exists yet), in
    // which case it enumerates. Explicit user surfaces (Settings and
    // the hamburger menu opening) enumerate raw so a just-plugged
    // device shows up; repeat consumers use the cache.
    static std::vector<EndpointInfo> enumerateRenderEndpointsCached(bool forceRefresh = false);
    bool start(Callback cb, const std::string& deviceNameUtf8 = "", const std::string& deviceIdUtf8 = "");
    void stop();
    bool isRunning() const;
    double sampleRate() const;
    // Diagnostics (v0.10): which endpoint was actually activated + packet flow,
    // so a silent loopback can be split into "wrong device" vs "no data".
    // v0.13: wanted vs active, both with endpoint IDs, plus how the pick
    // resolved ("endpoint id" / "exact name" / "partial name" / "default").
    std::string activeDeviceName() const;
    std::string activeDeviceId() const;
    std::string wantedDeviceName() const;
    std::string wantedDeviceId() const;
    std::string pickHow() const;
    long long packetsReceived() const;
    std::string formatInfo() const;   // negotiated format as decoded, e.g. "PCM24in32 ch2 @48000" (v0.12)
    std::string statusInfo() const;   // capture-thread stage: "running" or the failing stage + HRESULT (v0.12)
    float peakSeen() const;           // max |sample| the capture thread decoded
private:
    struct Impl; Impl* impl_ = nullptr;
};
