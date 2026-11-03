#include "WindowsLoopback.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
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
 #include <audioclientactivationparams.h> // v0.27.0 (item 8): AUDIOCLIENT_ACTIVATION_PARAMS / ActivateAudioInterfaceAsync
 #include <tlhelp32.h>                     // v0.27.0 (item 8): process snapshot for the per-app PID lookup
 #include <functiondiscoverykeys_devpkey.h>
 #include <ksmedia.h>
 #include <cstdio>
 // Completion handler for ActivateAudioInterfaceAsync, structured
 // after Microsoft's ApplicationLoopback sample. It answers the
 // IAgileObject IID in QueryInterface (the activation API requires
 // an agile handler) without inheriting a declared IAgileObject
 // type — that interface adds no methods beyond IUnknown, so
 // returning this object's handler pointer for its IID is exact.
 //
 // v0.27.1 OWNERSHIP CONTRACT (the v0.27.0 accounting balanced only
 // if the activation API happened to AddRef the handler the way the
 // code assumed, and its 5 s timeout path read handler->result /
 // handler->device and Released the handler while the async
 // operation could still deliver ActivateCompleted into the freed
 // object — the field crash on selecting Spotify, 2026-11-02):
 //  - refs_ starts at 1: the CREATOR reference, owned by
 //    loopProcessGuarded, released by it exactly once.
 //  - Before ActivateAudioInterfaceAsync, the creator takes a
 //    second, RESERVED reference for the operation. It is released
 //    exactly once: by ActivateCompleted itself, as its last act,
 //    after SetEvent — or by the creator if the activation call
 //    fails synchronously (the callback can then never fire).
 //  - The reserved reference keeps the object alive until the
 //    callback has fully run in EVERY interleaving, including a
 //    callback that arrives after the creator timed out and left:
 //    it then self-deletes inside that final Release, and the
 //    activated device (if the late activation succeeded) is
 //    released by the destructor. Nobody touches the handler after
 //    releasing the creator reference. If the callback somehow
 //    never fires, the handler leaks — deliberately, the same
 //    leak-don't-free-under-a-live-thread pattern as Impl::stop().
 //  - GetActivateResult's device reference is owned by the handler
 //    and balanced exactly once, by the destructor.
 namespace {
 class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler {
 public:
     HANDLE done = nullptr; HRESULT result = E_FAIL; IUnknown* device = nullptr;
     ActivateHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
     ~ActivateHandler() { if (done) CloseHandle(done); if (device) device->Release(); }
     STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
         if (!ppv) return E_POINTER;
         static const GUID kAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
         if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) || riid == kAgileObject) {
             *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
         } else { *ppv = nullptr; return E_NOINTERFACE; }
         AddRef(); return S_OK;
     }
     STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
     STDMETHODIMP_(ULONG) Release() override { ULONG r = --refs_; if (r == 0) delete this; return r; }
     STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
         if (op) op->GetActivateResult(&result, &device);
         if (done) SetEvent(done);
         Release(); // the RESERVED reference (see contract above) —
                    // last member access in this callback; the
                    // object may delete itself here.
         return S_OK;
     }
 private:
     std::atomic<ULONG> refs_{ 1 };
 };
#if defined(_MSC_VER)
 // v0.27.1: SEH guard as a FREE function — MSVC forbids __try in a
 // function that requires C++ object unwinding (C2712, the first
 // CI build of this hotfix), and a member function calling member
 // state from its handler qualifies. This frame holds nothing but
 // raw pointers, so it never requires unwinding; the guarded body
 // runs in its own function (Impl::loopProcessGuarded).
 inline void sehGuard(void (*fn)(void*), void* ctx, bool& faulted) {
     faulted = false;
     __try { fn(ctx); }
     __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }
 }
#endif
 } // namespace
 struct WindowsLoopback::Impl {
     meterbar::BoundedThread th; std::atomic<bool> run{false}; std::atomic<double> sr{48000}; Callback cb;
     std::string wantName; // v0.9: capture THIS output device (empty = default)
     std::string wantId;   // v0.13: capture THIS endpoint ID (wins over the name when set)
     std::string wantExe;  // v0.27.0 (item 8): capture THIS application's process tree only (processMode)
     std::string wantLabel; // v0.27.2: display label override (e.g. "Spotify (Spicetify)"); empty = derive from wantExe
     bool processMode = false;
     // ---- v0.27.2: persistent capture log --------------------------
     // The v0.27.1 field crash (2026-11-02) produced a WER dump whose
     // delivered copy was truncated — every thread stack sat beyond
     // EOF — so the failure left almost no evidence. This log is the
     // durable substitute: the last ~50 timestamped capture
     // status/stage lines, written next to the app's settings
     // (Documents\MeterBar\capture-log.txt, the folder above
     // Presets). Appended on status transitions only — never in the
     // packet loop — so the file IO cost is a few writes per source
     // switch. Best-effort throughout: any failure is silent.
     static std::wstring captureLogPathW() {
         wchar_t home[MAX_PATH] = {};
         const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
         if (n == 0 || n >= MAX_PATH) return {};
         return std::wstring(home, n) + L"\\Documents\\MeterBar\\capture-log.txt";
     }
     static std::string captureLogPathStr() { return narrow(captureLogPathW()); }
     static void appendCaptureLog(const std::string& line) {
         static std::mutex logLock;
         std::lock_guard<std::mutex> g(logLock);
         const std::wstring wpath = captureLogPathW();
         if (wpath.empty()) return;
         CreateDirectoryW(wpath.substr(0, wpath.find_last_of(L'\\')).c_str(), nullptr); // no-op when it exists
         SYSTEMTIME st; GetLocalTime(&st);
         char ts[32];
         std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d ",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
         std::string all;
         if (FILE* f = _wfopen(wpath.c_str(), L"rb")) {
             char chunk[4096]; size_t r;
             while ((r = std::fread(chunk, 1, sizeof chunk, f)) > 0) all.append(chunk, r);
             std::fclose(f);
         }
         std::vector<std::string> lines;
         for (size_t pos = 0; pos < all.size();) {
             const size_t nl = all.find('\n', pos);
             std::string one = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
             if (!one.empty() && one.back() == '\r') one.pop_back();
             if (!one.empty()) lines.push_back(one);
             if (nl == std::string::npos) break;
             pos = nl + 1;
         }
         lines.push_back(std::string(ts) + line);
         while (lines.size() > 50) lines.erase(lines.begin());
         if (FILE* f = _wfopen(wpath.c_str(), L"wb")) {
             for (auto& l : lines) { std::fwrite(l.data(), 1, l.size(), f); std::fputc('\n', f); }
             std::fclose(f);
         }
     }
     // ---- v0.27.2: per-app process resolution ----------------------
     // v0.27.0/v0.27.1 activated against the FIRST process in the
     // Toolhelp snapshot with a matching exe name. Under Spicetify
     // (and Chromium-style app layouts generally) the app runs as a
     // FAMILY of same-named processes — bootstrap, GPU, storage,
     // renderer, audio service — and the first snapshot entry can
     // be a transient child whose process tree does NOT contain
     // the audio-rendering processes, or which exits mid-
     // activation; the 2026-11-02 field crash was a heap
     // corruption (0xC0000374) detected on this path. The resolver
     // enumerates ALL matches with full paths + parent PIDs,
     // prefers the candidate group's TREE ROOT (the audio tree
     // hangs off it — INCLUDE_TARGET_PROCESS_TREE from the root
     // covers every renderer), tie-breaks by owning a visible
     // top-level window, and reports the candidate set + choice
     // for the capture log.
     struct ProcInfo { DWORD pid = 0; DWORD parentPid = 0; std::wstring path; };
     static std::vector<ProcInfo> enumerateProcesses(const std::wstring& exeLower) {
         std::vector<ProcInfo> out;
         HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
         if (snap == INVALID_HANDLE_VALUE) return out;
         PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
         if (Process32FirstW(snap, &pe)) {
             do {
                 if (lower(std::wstring(pe.szExeFile)) != exeLower) continue;
                 ProcInfo info; info.pid = pe.th32ProcessID; info.parentPid = pe.th32ParentProcessID;
                 if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.pid)) {
                     wchar_t buf[1024]; DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
                     if (QueryFullProcessImageNameW(h, 0, buf, &n)) info.path.assign(buf, n);
                     CloseHandle(h);
                 }
                 out.push_back(info);
             } while (Process32NextW(snap, &pe));
         }
         CloseHandle(snap);
         return out;
     }
     static BOOL CALLBACK collectWindowPid(HWND hwnd, LPARAM lp) {
         auto* pids = reinterpret_cast<std::set<DWORD>*>(lp);
         if (!IsWindowVisible(hwnd)) return TRUE;
         if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
         DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
         if (pid) pids->insert(pid);
         return TRUE;
     }
     // Returns the chosen process (pid 0 when none) and fills
     // `describe` with a one-line account of the candidate set and
     // the choice, for the capture log.
     static ProcInfo resolveTargetProcess(const std::wstring& exeLower, std::string& describe) {
         const auto cands = enumerateProcesses(exeLower);
         if (cands.empty()) { describe = "no matching process"; return {}; }
         std::set<DWORD> candPids;
         for (auto& c : cands) candPids.insert(c.pid);
         std::vector<ProcInfo> roots;
         for (auto& c : cands) if (!candPids.count(c.parentPid)) roots.push_back(c);
         const auto& pool = roots.empty() ? cands : roots;
         std::set<DWORD> windowPids;
         EnumWindows(collectWindowPid, reinterpret_cast<LPARAM>(&windowPids));
         const ProcInfo* chosen = nullptr;
         for (auto& c : pool) if (windowPids.count(c.pid) && (!chosen || c.pid < chosen->pid)) chosen = &c;
         const bool byWindow = chosen != nullptr;
         if (!chosen) { chosen = &pool[0]; for (auto& c : pool) if (c.pid < chosen->pid) chosen = &c; }
         std::string pidList;
         for (auto& c : cands) {
             char b[24]; std::snprintf(b, sizeof b, "%lu", (unsigned long)c.pid);
             if (!pidList.empty()) pidList += ", ";
             pidList += b;
         }
         char buf[1024];
         std::snprintf(buf, sizeof buf, "%zu candidate(s) [pid %s]; chose pid %lu (%s)%s, path %s",
                       cands.size(), pidList.c_str(), (unsigned long)chosen->pid,
                       roots.empty() ? "lowest pid, no tree root found" : "tree root",
                       byWindow ? ", owns a window" : "",
                       chosen->path.empty() ? "?" : narrow(chosen->path).c_str());
         describe = buf;
         return *chosen;
     }
     static std::string exeLabel(const std::string& exe) { // "Spotify.exe" -> "Spotify" for status lines
         std::string s = exe;
         if (s.size() > 4) {
             std::string tail = s.substr(s.size() - 4);
             for (auto& c : tail) c = (char)tolower((unsigned char)c);
             if (tail == ".exe") s = s.substr(0, s.size() - 4);
         }
         return s;
     }
     std::atomic<long long> packets{0};
     std::atomic<float> peakSeen{0.f};
     // Diagnostics are written by the capture thread and read by the UI
     // thread — v0.12: guard them with a mutex (the v0.11 code raced plain
     // std::strings across the two threads).
     std::mutex diagLock; std::string activeName, activeId, pickHow, fmtLabel, status{"not started"};
     std::string targetDesc; // v0.27.2: "pid <n>, <full exe path>" of the resolved per-app target
     enum class Fmt { F32, I16, I24, I32 }; Fmt fmtKind = Fmt::F32; int bytesPerSample = 4;
     // v0.27.2: every status transition is also appended to the
     // persistent capture log (see appendCaptureLog above).
     void setStatus(const std::string& s) { std::lock_guard<std::mutex> g(diagLock); status = s; appendCaptureLog(s); }
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
     // Process loopback (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_
     // LOOPBACK) exists only on Windows 11 / Windows Server 2022 —
     // build 20348+. RtlGetVersion reports the TRUE build (the
     // version APIs lie to unmanifested callers). v0.27.1: gate on
     // it BEFORE any activation machinery is touched; an unproven
     // build counts as unsupported.
     static bool processLoopbackSupported() {
         using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
         OSVERSIONINFOW vi = {}; vi.dwOSVersionInfoSize = sizeof(vi);
         auto fn = reinterpret_cast<RtlGetVersionFn>(
             GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
         if (!fn || fn(&vi) != 0) return false;
         if (vi.dwMajorVersion != 10) return vi.dwMajorVersion > 10;
         return vi.dwBuildNumber >= 20348;
     }
     // v0.27.0 (item 8): PER-APP capture (Microsoft's
     // ApplicationLoopback sample structure). Find the app's PID,
     // ActivateAudioInterfaceAsync on the process-loopback virtual
     // device with INCLUDE_TARGET_PROCESS_TREE, then Initialize +
     // the capture-client path. ANY failure leaves the meters
     // silent with the reason in status — never a fallback to
     // system audio, never a crash.
     //
     // v0.27.1 (hotfix — crashes on selecting Spotify in the field):
     //  - FORMAT: the canonical sample does NOT GetMixFormat the
     //    process-loopback virtual device. It requests a FIXED
     //    format (IEEE float, 48 kHz, stereo, 32-bit) and lets
     //    AUTOCONVERTPCM resample the target tree's mix into it.
     //    v0.27.0 Initialized with the virtual device's own mix
     //    format and LOOPBACK alone — replaced by the canonical
     //    form below; sr/fmt state comes from the fixed format.
     //  - HANDLER: ActivateHandler's ownership contract (above)
     //    now keeps the handler alive until the completion callback
     //    has fully run, in every interleaving; the 5 s timeout
     //    path below releases only the creator reference and never
     //    reads handler state again.
     //  - OS GATE: processLoopbackSupported() runs first.
     //  - Every exit path sets status before returning.
     //
     // v0.27.2 (hotfix — v0.27.1 crashed again in the field; the
     // WER dump, see DUMP_ANALYSIS in the round folder, proved a
     // heap corruption 0xC0000374, not an access violation):
     //  - RESOLUTION: the target PID comes from
     //    resolveTargetProcess (tree root of the exe's process
     //    family), not the first snapshot match — see the resolver
     //    comment. The candidate set + choice go to the capture
     //    log; the chosen pid + path ride the status surfaces.
     //  - The ingest side of the heap-corruption surface is closed
     //    in AudioEngine::pushStereo (serialised producers).
     void loopProcessGuarded() {
         CoInitializeEx(nullptr, COINIT_MULTITHREADED);
         const std::string label = wantLabel.empty() ? exeLabel(wantExe) : wantLabel;
         if (!processLoopbackSupported()) {
             setStatus(label + " — per-app capture needs Windows 11");
             CoUninitialize(); return;
         }
         // v0.27.2: resolve the target process (resolver above) and
         // leave the whole decision in the capture log; the chosen
         // pid + path then ride the status surfaces, so any failure
         // from here on is diagnosable from the status alone.
         std::string resolveWhy;
         const ProcInfo target = resolveTargetProcess(lower(widen(wantExe)), resolveWhy);
         appendCaptureLog(label + " resolve: " + resolveWhy);
         if (target.pid == 0) { setStatus(label + " — not running"); CoUninitialize(); return; }
         const DWORD pid = target.pid;
         std::string targetText;
         { char tb[64]; std::snprintf(tb, sizeof tb, "pid %lu, ", (unsigned long)pid);
           targetText = std::string(tb) + (target.path.empty() ? std::string("path unavailable") : narrow(target.path)); }
         { std::lock_guard<std::mutex> g(diagLock); targetDesc = targetText; }
         setStatus(label + " — target " + targetText);
         AUDIOCLIENT_ACTIVATION_PARAMS params = {};
         params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
         params.ProcessLoopbackParams.TargetProcessId = pid;
         params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
         PROPVARIANT prop; PropVariantInit(&prop);
         prop.vt = VT_BLOB;
         prop.blob.cbSize = sizeof(params);
         prop.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
         auto* handler = new ActivateHandler();
         handler->AddRef(); // the RESERVED reference (contract above):
                            // released by ActivateCompleted itself,
                            // or by us if the call below fails.
         IActivateAudioInterfaceAsyncOperation* op = nullptr;
         HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                  __uuidof(IAudioClient), &prop, handler, &op);
         PropVariantClear(&prop);
         if (FAILED(hr) || !op) {
             setStatus(hrText("ActivateAudioInterfaceAsync", hr));
             handler->Release(); // reserved (the callback never fires)
             handler->Release(); // creator
             CoUninitialize(); return;
         }
         const DWORD waitRc = WaitForSingleObject(handler->done, 5000);
         op->Release();
         if (waitRc != WAIT_OBJECT_0) {
             // Timeout (or wait failure): the callback may STILL
             // arrive. Release ONLY the creator reference and never
             // touch the handler, its result, or its device again —
             // the reserved reference keeps the handler alive until
             // the callback's own final Release, and ~ActivateHandler
             // balances any late-arriving device. Deliberate leak
             // rather than use-after-free.
             setStatus(label + " — activation timed out");
             handler->Release(); // creator reference only
             CoUninitialize(); return;
         }
         // The callback has run to its SetEvent; result/device are
         // final. (The callback's reserved Release may land before
         // or after our reads — the creator reference keeps the
         // object alive until we release it below.)
         IAudioClient* client = nullptr;
         if (SUCCEEDED(handler->result) && handler->device)
             handler->device->QueryInterface(__uuidof(IAudioClient), (void**)&client);
         const HRESULT actHr = handler->result;
         handler->Release(); // creator reference — last touch
         if (!client) {
             setStatus(actHr != E_FAIL ? hrText("ActivateCompleted", actHr)
                                       : label + " — activation failed");
             CoUninitialize(); return;
         }
         { std::lock_guard<std::mutex> g(diagLock); activeName = label + " (process loopback, " + targetText + ")"; pickHow = "process loopback"; }
         // Canonical fixed format (see header note): float 48 kHz
         // stereo, with the engine converting the tree's mix.
         WAVEFORMATEX fmt = {};
         fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
         fmt.nChannels = 2;
         fmt.nSamplesPerSec = 48000;
         fmt.wBitsPerSample = 32;
         fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
         fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
         fmt.cbSize = 0;
         sr.store((double)fmt.nSamplesPerSec); detectFormat(&fmt);
         hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                     | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 10000000, 0, &fmt, nullptr);
         if (FAILED(hr)) { setStatus(hrText("Initialize", hr)); client->Release(); CoUninitialize(); return; }
         const int ch = fmt.nChannels; const int bps = bytesPerSample;
         const double rate = sr.load();
         // Stride contract: the decode loop walks frames of
         // ch * bytesPerSample bytes; that MUST equal the format's
         // nBlockAlign, or every frame pointer after the first
         // reads across frame boundaries. Bail to status, no Start.
         if (bps <= 0 || (int)fmt.nBlockAlign != ch * bps) {
             char buf[128];
             std::snprintf(buf, sizeof buf, "capture stride mismatch (nBlockAlign %lu, channels %d, bytes/sample %d)",
                           (unsigned long)fmt.nBlockAlign, ch, bps);
             setStatus(buf); client->Release(); CoUninitialize(); return;
         }
         IAudioCaptureClient* cap = nullptr;
         hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
         if (FAILED(hr) || !cap) { setStatus(hrText("GetService", hr)); client->Release(); CoUninitialize(); return; }
         hr = client->Start();
         if (FAILED(hr)) { setStatus(hrText("Start", hr)); cap->Release(); client->Release(); CoUninitialize(); return; }
         setStatus("running");
         const UINT32 maxFrames = (UINT32)(rate * 2); // 2 s in one packet is not a packet — a broken client, bail
         bool done = false;
         while (run.load() && !done) {
             UINT32 packet = 0;
             hr = cap->GetNextPacketSize(&packet);
             if (FAILED(hr)) { setStatus(hrText("GetNextPacketSize", hr)); break; }
             if (packet == 0) { Sleep(5); continue; }
             while (packet > 0) {
                 BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                 hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                 if (FAILED(hr)) { setStatus(hrText("GetBuffer", hr)); done = true; break; }
                 if (frames > maxFrames) { setStatus("capture packet size invalid"); done = true; break; }
                 if (frames > 0 && cb) {
                     packets.fetch_add(1);
                     std::vector<float> L(frames), R(frames);
                     const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                     float localPeak = 0.f;
                     for (UINT32 i = 0; i < frames; ++i) {
                         if (silent) { L[i] = 0; R[i] = 0; continue; }
                         BYTE* framePtr = data + (size_t)i * fmt.nBlockAlign;
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
         // status keeps whatever the loop last recorded (same
         // convention as the endpoint loop).
         cap->Release(); client->Release();
         CoUninitialize();
     }
     // v0.27.1 belt and braces: NOTHING inside the per-app path —
     // including an access violation inside a WASAPI / driver call —
     // may take the process down. A fault lands here as a status
     // line and silent meters instead. (MSVC: SEH, which also
     // catches the AV class; other compilers: C++ exceptions.)
     void loopProcess() {
 #if defined(_MSC_VER)
         bool faulted = false;
         sehGuard([](void* p) { static_cast<Impl*>(p)->loopProcessGuarded(); }, this, faulted);
         if (faulted) setStatus("per-app capture stopped (internal fault)");
 #else
         try { loopProcessGuarded(); }
         catch (...) { setStatus("per-app capture stopped (internal fault)"); }
 #endif
     }
     void loopEntry() { if (processMode) loopProcess(); else loop(); }
 };
 bool WindowsLoopback::start(Callback cb, const std::string& deviceNameUtf8, const std::string& deviceIdUtf8) { stop(); impl_ = new Impl(); impl_->cb = std::move(cb); impl_->wantName = deviceNameUtf8; impl_->wantId = deviceIdUtf8; impl_->run = true;
     // v0.20.2: the thread lambda captures the Impl POINTER, not `this`.
     // With the bounded stop below, a timed-out (leaked) capture thread
     // can outlive a stop()->start() cycle; dereferencing impl_ through
     // `this` could then run the NEW Impl's loop on the old thread.
     Impl* im = impl_;
     im->th.start([im]{ im->loop(); }); return true; }
 bool WindowsLoopback::startProcessLoopback(Callback cb, const std::string& exeNameUtf8, const std::string& labelUtf8) { stop(); impl_ = new Impl(); impl_->cb = std::move(cb); impl_->wantExe = exeNameUtf8; impl_->wantLabel = labelUtf8; impl_->processMode = true; impl_->run = true;
     Impl* im = impl_;
     im->th.start([im]{ im->loopEntry(); }); return true; }
 bool WindowsLoopback::isProcessRunning(const std::string& exeNameUtf8) {
     return !Impl::enumerateProcesses(Impl::lower(Impl::widen(exeNameUtf8))).empty();
 }
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
 std::string WindowsLoopback::targetInfo() const { if (!impl_) return {}; std::lock_guard<std::mutex> g(impl_->diagLock); return impl_->targetDesc; }
 std::string WindowsLoopback::captureLogPath() { return Impl::captureLogPathStr(); }
 float WindowsLoopback::peakSeen() const { return impl_ ? impl_->peakSeen.load() : 0.f; }
#else
 struct WindowsLoopback::Impl {};
 std::vector<WindowsLoopback::EndpointInfo> WindowsLoopback::enumerateRenderEndpoints() { return {}; }
 std::vector<WindowsLoopback::EndpointInfo> WindowsLoopback::enumerateRenderEndpointsCached(bool) { return {}; }
 bool WindowsLoopback::start(Callback, const std::string&, const std::string&) { return false; }
 bool WindowsLoopback::startProcessLoopback(Callback, const std::string&, const std::string&) { return false; }
 bool WindowsLoopback::isProcessRunning(const std::string&) { return false; }
 std::string WindowsLoopback::targetInfo() const { return {}; }
 std::string WindowsLoopback::captureLogPath() { return {}; }
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
