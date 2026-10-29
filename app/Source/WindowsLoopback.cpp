#include "WindowsLoopback.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <meterbar/BoundedThread.hpp> // v0.20.2: bounded capture-thread stop (freeze fix)
// v0.14 ROOT-CAUSE FIX — read this before ever touching the guard again.
// This translation unit deliberately includes NO JUCE header (std-only),
// and JUCE_WINDOWS is a macro that exists ONLY inside JUCE's own headers
// (juce_TargetPlatform.h) or as a -D on the command line; the app build
// defines neither for this TU. So `#if JUCE_WINDOWS` was 0 in EVERY build —
// including MSVC on Windows — and the stub branch below is what shipped in
// every kit since the guard was introduced: start() returned false and
// stored nothing, enumerateRenderEndpoints() returned {}, every diagnostic
// read empty. That is the whole "loopback silent on the chosen device"
// field history (v0.10–v0.13): the WASAPI implementation was never in the
// binary. v0.13's MinGW check passed -DJUCE_WINDOWS=1 on the command line,
// type-checking code the real build never compiled — a false verification.
// The fix: test the COMPILER's predefined target macro instead. MSVC and
// MinGW both predefine _WIN32 for Windows targets, no header required.
// (The other `#if JUCE_WINDOWS` sites — Settings.h, MainComponent.cpp —
// are safe: those TUs include JUCE headers before the guard.)
#if defined(_WIN32)
 #define METERBAR_LOOPBACK_WINDOWS 1
#else
 #define METERBAR_LOOPBACK_WINDOWS 0
#endif
#if METERBAR_LOOPBACK_WINDOWS
 #include <windows.h>
 #include <mmdeviceapi.h>
 #include <audioclient.h>
 #include <functiondiscoverykeys_devpkey.h>
 #include <ksmedia.h>
 #include <cstdio>
 struct WindowsLoopback::Impl {
     meterbar::BoundedThread th; std::atomic<bool> run{false}; std::atomic<double> sr{48000}; Callback cb;
     std::string wantName; // v0.9: capture THIS output device (empty = default)
     std::string wantId;   // v0.13: capture THIS endpoint ID (wins over the name when set)
     std::atomic<long long> packets{0};
     std::atomic<float> peakSeen{0.f};
     // Diagnostics are written by the capture thread and read by the UI
     // thread — v0.12: guard them with a mutex (the v0.11 code raced plain
     // std::strings across the two threads).
     std::mutex diagLock; std::string activeName, activeId, pickHow, fmtLabel, status{"not started"};
     enum class Fmt { F32, I16, I24, I32 }; Fmt fmtKind = Fmt::F32; int bytesPerSample = 4;
     void setStatus(const std::string& s) { std::lock_guard<std::mutex> g(diagLock); status = s; }
     static std::string hrText(const char* stage, HRESULT hr) {
         char buf[96]; std::snprintf(buf, sizeof buf, "%s failed (0x%08lx)", stage, (unsigned long)hr);
         return buf;
     }
     // Decode one sample of the mix format at byte pointer p (v0.10: the old code
     // treated every 32-bit extensible format as float — Realtek endpoints report
     // PCM in a 32-bit container, which decoded to near-silence).
     float readSample(const BYTE* p) const {
         switch (fmtKind) {
         case Fmt::F32: { float v; std::memcpy(&v, p, 4); return v; }
         case Fmt::I16: { int16_t v; std::memcpy(&v, p, 2); return v / 32768.f; }
         case Fmt::I24: { int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
                         if (v & 0x800000) v |= ~0xFFFFFF; return v / 8388608.f; }
         case Fmt::I32: { int32_t v; std::memcpy(&v, p, 4); return v / 2147483648.f; }
         }
         return 0.f;
     }
     void detectFormat(WAVEFORMATEX* fmt) {
         // v0.12: the frame stride comes from nBlockAlign (authoritative),
         // NOT wBitsPerSample/8 — for WAVE_FORMAT_EXTENSIBLE, wBitsPerSample
         // is the container size while Samples.wValidBitsPerSample is the
         // real precision, and a few drivers report wBitsPerSample loosely.
         // Container bytes decide the decode stride; per the extensible spec
         // valid bits are LEFT-justified in the container, so decoding the
         // full container width stays correct for 16/20/24-in-32.
         const int ch = std::max(1, (int)fmt->nChannels);
         int containerBytes = (fmt->nBlockAlign > 0) ? (int)fmt->nBlockAlign / ch : 0;
         if (containerBytes <= 0) containerBytes = std::max(1, (int)fmt->wBitsPerSample / 8);
         bytesPerSample = containerBytes;
         bool isFloatFmt = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
         int validBits = (int)fmt->wBitsPerSample;
         if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
             auto* ext = (WAVEFORMATEXTENSIBLE*)fmt;
             isFloatFmt = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
             if (ext->Samples.wValidBitsPerSample > 0) validBits = (int)ext->Samples.wValidBitsPerSample;
         }
         std::string base;
         if (isFloatFmt) { fmtKind = Fmt::F32; base = "F32"; }
         else switch (containerBytes) {
             case 2: fmtKind = Fmt::I16; base = "PCM16"; break;
             case 3: fmtKind = Fmt::I24; base = "PCM24"; break;
             default: fmtKind = Fmt::I32; base = "PCM32"; break;
         }
         char buf[96];
         if (!isFloatFmt && validBits > 0 && validBits != containerBytes * 8)
             std::snprintf(buf, sizeof buf, "PCM%din%d ch%d @%lu", validBits, containerBytes * 8, ch, (unsigned long)fmt->nSamplesPerSec);
         else
             std::snprintf(buf, sizeof buf, "%s ch%d @%lu", base.c_str(), ch, (unsigned long)fmt->nSamplesPerSec);
         std::lock_guard<std::mutex> g(diagLock); fmtLabel = buf;
     }
     static std::wstring widen(const std::string& s) {
         if (s.empty()) return {};
         int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
         std::wstring w((size_t)n, L'\0');
         MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w;
     }
     static std::string narrow(const std::wstring& w) {
         if (w.empty()) return {};
         int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
         std::string s((size_t)std::max(0, n - 1), '\0');
         if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
         return s;
     }
     static std::wstring lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
     static std::wstring friendlyName(IMMDevice* d) {
         std::wstring fname;
         IPropertyStore* props = nullptr;
         if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) && props) {
             PROPVARIANT v; PropVariantInit(&v);
             if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.pwszVal) fname = v.pwszVal;
             PropVariantClear(&v); props->Release();
         }
         return fname;
     }
     // Find the render endpoint whose friendly name matches wantName;
     // fall back to the default endpoint. v0.12: an EXACT match is tried
     // over the whole list before any substring match — the v0.9 first-hit
     // substring scan could pick the wrong device when two endpoints share
     // a prefix (e.g. two different "Speakers (...)" render devices).
     IMMDevice* pickDevice(IMMDeviceEnumerator* en) {
         auto setHow = [this](const char* h) { std::lock_guard<std::mutex> g(diagLock); pickHow = h; };
         IMMDevice* def = nullptr; en->GetDefaultAudioEndpoint(eRender, eConsole, &def);
         if (wantName.empty()) { setHow("default (no device chosen)"); return def; }
         std::wstring want = lower(widen(wantName));
         IMMDeviceCollection* coll = nullptr;
         if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || !coll) { setHow("default (endpoint list failed)"); return def; }
         UINT count = 0; coll->GetCount(&count);
         std::vector<IMMDevice*> devs; std::vector<std::wstring> names;
         for (UINT i = 0; i < count; ++i) {
             IMMDevice* d = nullptr; coll->Item(i, &d); if (!d) continue;
             devs.push_back(d); names.push_back(lower(friendlyName(d)));
         }
         coll->Release();
         IMMDevice* best = nullptr;
         for (int pass = 0; pass < 2 && !best; ++pass) {
             for (size_t i = 0; i < devs.size(); ++i) {
                 if (names[i].empty()) continue;
                 bool hit = (pass == 0) ? (names[i] == want)
                                        : (names[i].find(want) != std::wstring::npos || want.find(names[i]) != std::wstring::npos);
                 if (hit) { best = devs[i]; setHow(pass == 0 ? "exact name" : "partial name"); break; }
             }
         }
         for (auto* d : devs) if (d != best) d->Release();
         if (best) { if (def) def->Release(); return best; }
         // v0.13: this fallback is the historical headphone bug — a chosen
         // device whose name matches NO endpoint silently captured the
         // default speakers instead. It stays as a last resort, but the
         // Debug window now names it: pickHow = "default (NO MATCH ...)".
         setHow("default (NO MATCH for chosen name)");
         return def;
     }
     void loop() {
         CoInitializeEx(nullptr, COINIT_MULTITHREADED);
         IMMDeviceEnumerator* en = nullptr;
         HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
         if (FAILED(hr) || !en) { setStatus(hrText("CoCreateInstance", hr)); CoUninitialize(); return; }
         // v0.13: an endpoint ID (stored when the user picked the device in
         // the endpoint-backed list) identifies the device EXACTLY — no name
         // matching involved. Only when no ID is known do we fall back to
         // the v0.9/v0.12 friendly-name pick (and its default fallback).
         IMMDevice* dev = nullptr;
         if (!wantId.empty() && SUCCEEDED(en->GetDevice(widen(wantId).c_str(), &dev)) && dev) {
             std::lock_guard<std::mutex> g(diagLock); pickHow = "endpoint id";
         } else {
             dev = pickDevice(en); // sets pickHow itself
         }
         if (!dev) { setStatus("no render endpoint found"); en->Release(); CoUninitialize(); return; }
         { // record the activated endpoint's friendly name + ID for the Debug window
             std::string idStr;
             LPWSTR idW = nullptr;
             if (SUCCEEDED(dev->GetId(&idW)) && idW) { idStr = narrow(std::wstring(idW)); CoTaskMemFree(idW); }
             std::lock_guard<std::mutex> g(diagLock); activeName = narrow(friendlyName(dev)); activeId = idStr;
         }
         // v0.12: every stage's HRESULT is checked. The v0.11 code called
         // GetMixFormat on a possibly-null client (failed Activate) and
         // ignored Initialize/GetService/Start failures — a failed Start
         // left the packet loop spinning forever on a dead client, which
         // presents exactly as "loopback silent, packets counted or not".
         IAudioClient* client = nullptr;
         hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
         if (FAILED(hr) || !client) { setStatus(hrText("Activate", hr)); dev->Release(); en->Release(); CoUninitialize(); return; }
         WAVEFORMATEX* fmt = nullptr;
         hr = client->GetMixFormat(&fmt);
         if (FAILED(hr) || !fmt) { setStatus(hrText("GetMixFormat", hr)); client->Release(); dev->Release(); en->Release(); CoUninitialize(); return; }
         sr.store(fmt->nSamplesPerSec); detectFormat(fmt);
         hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, fmt, nullptr);
         if (FAILED(hr)) { setStatus(hrText("Initialize", hr)); CoTaskMemFree(fmt); client->Release(); dev->Release(); en->Release(); CoUninitialize(); return; }
         IAudioCaptureClient* cap = nullptr;
         hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
         if (FAILED(hr) || !cap) { setStatus(hrText("GetService", hr)); CoTaskMemFree(fmt); client->Release(); dev->Release(); en->Release(); CoUninitialize(); return; }
         hr = client->Start();
         if (FAILED(hr)) { setStatus(hrText("Start", hr)); cap->Release(); CoTaskMemFree(fmt); client->Release(); dev->Release(); en->Release(); CoUninitialize(); return; }
         setStatus("running");
         const int ch = fmt->nChannels; const int bps = bytesPerSample;
         const double rate = sr.load();
         bool done = false;
         while (run.load() && !done) {
             UINT32 packet = 0;
             hr = cap->GetNextPacketSize(&packet);
             if (FAILED(hr)) { setStatus(hrText("GetNextPacketSize", hr)); break; }
             if (packet == 0) { Sleep(5); continue; }
             while (packet > 0) {
                 BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                 hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                 if (FAILED(hr)) {
                     // ReleaseBuffer may only follow a SUCCESSFUL GetBuffer;
                     // the v0.11 code released unconditionally. A device
                     // invalidation here must end the loop, not spin on it.
                     setStatus(hrText(hr == AUDCLNT_E_DEVICE_INVALIDATED ? "device lost (GetBuffer)" : "GetBuffer", hr));
                     done = true; break;
                 }
                 if (frames > 0 && cb) {
                     packets.fetch_add(1);
                     // First stereo pair. (Channel-pair selection for multichannel
                     // formats beyond the first pair is a known follow-up.)
                     std::vector<float> L(frames), R(frames);
                     const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                     float localPeak = 0.f;
                     for (UINT32 i = 0; i < frames; ++i) {
                         if (silent) { L[i] = 0; R[i] = 0; continue; }
                         BYTE* framePtr = data + (size_t)i * ch * bps;
                         L[i] = readSample(framePtr);
                         R[i] = readSample(framePtr + (ch > 1 ? bps : 0));
                         localPeak = std::max({localPeak, std::abs(L[i]), std::abs(R[i])});
                     }
                     float prev = peakSeen.load();
                     while (localPeak > prev && !peakSeen.compare_exchange_weak(prev, localPeak)) {}
                     cb(L.data(), R.data(), (int)frames, rate);
                 }
                 cap->ReleaseBuffer(frames);
                 hr = cap->GetNextPacketSize(&packet);
                 if (FAILED(hr)) { setStatus(hrText("GetNextPacketSize", hr)); done = true; break; }
             }
         }
         client->Stop();
         // status keeps whatever the loop last recorded: "running" on a clean
         // stop(), or the specific failing stage if the loop bailed on error.
         cap->Release(); client->Release(); CoTaskMemFree(fmt);
         dev->Release(); en->Release(); CoUninitialize();
     }
 };
 bool WindowsLoopback::start(Callback cb, const std::string& deviceNameUtf8, const std::string& deviceIdUtf8) { stop(); impl_ = new Impl(); impl_->cb = std::move(cb); impl_->wantName = deviceNameUtf8; impl_->wantId = deviceIdUtf8; impl_->run = true;
     // v0.20.2: the thread lambda captures the Impl POINTER, not `this`.
     // With the bounded stop below, a timed-out (leaked) capture thread
     // can outlive a stop()->start() cycle; dereferencing impl_ through
     // `this` could then run the NEW Impl's loop on the old thread.
     Impl* im = impl_;
     im->th.start([im]{ im->loop(); }); return true; }
 void WindowsLoopback::stop() { if (impl_) { impl_->run = false;
     // v0.20.2 BOUNDED STOP (freeze fix, audit suspect 1): the capture
     // thread only observes run=false between WASAPI calls; a call
     // blocked inside a driver (device-invalidation race, virtual or
     // USB endpoint) used to hang this join — and with it the message
     // thread that reaches stop() via AudioEngine::applySource — for
     // good, leaving every button dead. Wait 500 ms; on timeout the
     // thread is detached and the Impl deliberately LEAKED (it keeps
     // sole ownership of everything the thread touches; the same
     // leak-don't-free-under-a-live-thread pattern as
     // AudioEngine::stopRecording, v0.12). The leaked thread exits on
     // its own when the blocked call returns and loop() sees run=false.
     if (impl_->th.stopBounded(500)) delete impl_;
     impl_ = nullptr; } }
 bool WindowsLoopback::isRunning() const { return impl_ != nullptr; }
 double WindowsLoopback::sampleRate() const { return impl_ ? impl_->sr.load() : 48000; }
 // v0.20.2: last-enumeration cache (see enumerateRenderEndpointsCached).
 namespace {
     std::mutex g_epsCacheLock;
     std::vector<WindowsLoopback::EndpointInfo> g_epsCache;
     bool g_epsCacheValid = false;
 }
 std::vector<WindowsLoopback::EndpointInfo> WindowsLoopback::enumerateRenderEndpoints() {
     // Called from the UI thread (picker/menu construction). COM is
     // initialised for this call only if this thread hasn't got an apartment
     // yet — RPC_E_CHANGED_MODE means the thread already has one (JUCE's
     // message thread does), in which case we must NOT CoUninitialize.
     std::vector<EndpointInfo> out;
     HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
     // v0.14: only an S_OK means THIS call initialised COM and owes a
     // CoUninitialize. S_FALSE = the thread already had an apartment (the
     // JUCE message thread does) — the old code counted it as didInit and
     // unbalanced the message thread's COM count on every picker build.
     bool didInit = (hrInit == S_OK);
     IMMDeviceEnumerator* en = nullptr;
     HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
     if (SUCCEEDED(hr) && en) {
         IMMDeviceCollection* coll = nullptr;
         if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
             UINT count = 0; coll->GetCount(&count);
             for (UINT i = 0; i < count; ++i) {
                 IMMDevice* d = nullptr; coll->Item(i, &d); if (!d) continue;
                 EndpointInfo info; info.name = Impl::narrow(Impl::friendlyName(d));
                 LPWSTR idW = nullptr;
                 if (SUCCEEDED(d->GetId(&idW)) && idW) { info.id = Impl::narrow(std::wstring(idW)); CoTaskMemFree(idW); }
                 if (!info.name.empty()) out.push_back(info);
                 d->Release();
             }
             coll->Release();
         }
         en->Release();
     }
     if (didInit) CoUninitialize();
     { std::lock_guard<std::mutex> g(g_epsCacheLock); g_epsCache = out; g_epsCacheValid = true; }
     return out;
 }
 std::vector<WindowsLoopback::EndpointInfo> WindowsLoopback::enumerateRenderEndpointsCached(bool forceRefresh) {
     if (!forceRefresh) {
         std::lock_guard<std::mutex> g(g_epsCacheLock);
         if (g_epsCacheValid) return g_epsCache;
     }
     return enumerateRenderEndpoints(); // refreshes the cache as a side effect
 }
 std::string WindowsLoopback::activeDeviceName() const { if (!impl_) return {}; std::lock_guard<std::mutex> g(impl_->diagLock); return impl_->activeName; }
 std::string WindowsLoopback::activeDeviceId() const { if (!impl_) return {}; std::lock_guard<std::mutex> g(impl_->diagLock); return impl_->activeId; }
 std::string WindowsLoopback::wantedDeviceName() const { return impl_ ? impl_->wantName : std::string{}; }
 std::string WindowsLoopback::wantedDeviceId() const { return impl_ ? impl_->wantId : std::string{}; }
 std::string WindowsLoopback::pickHow() const { if (!impl_) return {}; std::lock_guard<std::mutex> g(impl_->diagLock); return impl_->pickHow; }
 long long WindowsLoopback::packetsReceived() const { return impl_ ? impl_->packets.load() : 0; }
 std::string WindowsLoopback::formatInfo() const { if (!impl_) return {}; std::lock_guard<std::mutex> g(impl_->diagLock); return impl_->fmtLabel; }
 std::string WindowsLoopback::statusInfo() const { if (!impl_) return "not started"; std::lock_guard<std::mutex> g(impl_->diagLock); return impl_->status; }
 float WindowsLoopback::peakSeen() const { return impl_ ? impl_->peakSeen.load() : 0.f; }
#else
 struct WindowsLoopback::Impl {};
 std::vector<WindowsLoopback::EndpointInfo> WindowsLoopback::enumerateRenderEndpoints() { return {}; }
 std::vector<WindowsLoopback::EndpointInfo> WindowsLoopback::enumerateRenderEndpointsCached(bool) { return {}; }
 bool WindowsLoopback::start(Callback, const std::string&, const std::string&) { return false; }
 void WindowsLoopback::stop() {}
 bool WindowsLoopback::isRunning() const { return false; }
 double WindowsLoopback::sampleRate() const { return 48000; }
 std::string WindowsLoopback::activeDeviceName() const { return {}; }
 std::string WindowsLoopback::activeDeviceId() const { return {}; }
 std::string WindowsLoopback::wantedDeviceName() const { return {}; }
 std::string WindowsLoopback::wantedDeviceId() const { return {}; }
 std::string WindowsLoopback::pickHow() const { return {}; }
 long long WindowsLoopback::packetsReceived() const { return 0; }
 std::string WindowsLoopback::formatInfo() const { return {}; }
 std::string WindowsLoopback::statusInfo() const { return "loopback unavailable (non-Windows build)"; }
 float WindowsLoopback::peakSeen() const { return 0.f; }
#endif
