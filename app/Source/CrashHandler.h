#pragma once
// CrashHandler (v0.28.3) — MeterBar's hard-crash record, per the
// family crash-log contract (Jackson, 2026-11-03): when the app
// hard-crashes it leaves a timestamped crash .txt + minidump in
// Documents\MeterBar, and the next launch offers a notice + an
// "Open Log Folder" path. Naming/notice DECISIONS live in the pure
// core (meterbar/CrashWatch.hpp, suite-pinned); this header is only
// the Windows last-resort machinery.
//
// LAST-RESORT DISCIPLINE (per the family contract): the crash path
// does the minimum — it writes ONE pre-formatted text buffer and
// asks DbgHelp for a minidump, using only Win32 calls on static
// storage. All formatting (app version, Windows build, the state
// snapshot) happens at install/update time, NEVER in the handler.
// The handler returns EXCEPTION_CONTINUE_SEARCH: WER still runs,
// the process still dies exactly as before — we only leave the
// record behind. Non-Windows builds: everything here is a no-op
// (the naming/notice logic is still suite-tested via the core).
//
// This TU discipline matches WindowsLoopback.cpp: no JUCE headers,
// the compiler's _WIN32 macro decides (never JUCE_WINDOWS).
#include <string>
#include <cstring>
#include <exception>
#if defined(_WIN32)
 #include <windows.h>
 #include <dbghelp.h>
 #include <cstdio>
#endif

namespace crashhandler {
#if defined(_WIN32)
namespace detail {
    inline char g_dir[MAX_PATH] = {};       // ...\Documents\MeterBar (narrow, for JUCE-free callers)
    inline wchar_t g_dirW[MAX_PATH] = {};   // same, wide, for the handler's file calls
    inline char g_head[1024] = {};          // pre-formatted: version + windows + when-placeholder lines
    inline char g_state[1024] = {};         // last honest state snapshot (pre-formatted by the app)
    inline volatile LONG g_inHandler = 0;   // re-entry guard: a fault INSIDE the handler must not recurse
    inline bool g_installed = false;

    inline void appendNum(char* buf, size_t cap, size_t& len, unsigned long v, int width) {
        char tmp[16]; int n = 0;
        if (v == 0) tmp[n++] = '0';
        while (v > 0 && n < 15) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
        for (int i = n; i < width && len + 1 < cap; ++i) buf[len++] = '0';
        for (int i = n - 1; i >= 0 && len + 1 < cap; --i) buf[len++] = tmp[i];
        if (len < cap) buf[len] = '\0';
    }
    inline void appendStr(char* buf, size_t cap, size_t& len, const char* s) {
        while (*s && len + 1 < cap) buf[len++] = *s++;
        if (len < cap) buf[len] = '\0';
    }
    inline void appendHex8(char* buf, size_t cap, size_t& len, unsigned long v) {
        const char* hex = "0123456789abcdef";
        appendStr(buf, cap, len, "0x");
        for (int i = 7; i >= 0 && len + 1 < cap; --i) buf[len++] = hex[(v >> (i * 4)) & 0xF];
        if (len < cap) buf[len] = '\0';
    }

    inline LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
        if (InterlockedExchange(&g_inHandler, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
        __try {
            SYSTEMTIME st; GetLocalTime(&st);
            // file stem: crash-YYYY-MM-DD_HHMMSS (matches meterbar::crashFileStem)
            char stem[40]; size_t sl = 0; stem[0] = '\0';
            appendStr(stem, sizeof stem, sl, "crash-");
            appendNum(stem, sizeof stem, sl, (unsigned long)st.wYear, 4); appendStr(stem, sizeof stem, sl, "-");
            appendNum(stem, sizeof stem, sl, (unsigned long)st.wMonth, 2); appendStr(stem, sizeof stem, sl, "-");
            appendNum(stem, sizeof stem, sl, (unsigned long)st.wDay, 2); appendStr(stem, sizeof stem, sl, "_");
            appendNum(stem, sizeof stem, sl, (unsigned long)st.wHour, 2);
            appendNum(stem, sizeof stem, sl, (unsigned long)st.wMinute, 2);
            appendNum(stem, sizeof stem, sl, (unsigned long)st.wSecond, 2);
            wchar_t pathW[MAX_PATH];
            // text record
            {
                char whenLine[64]; size_t wl = 0; whenLine[0] = '\0';
                appendStr(whenLine, sizeof whenLine, wl, stem + 6); // YYYY-MM-DD_HHMMSS portion
                char record[3072]; size_t rl = 0; record[0] = '\0';
                appendStr(record, sizeof record, rl, "MeterBar crash record\n");
                appendStr(record, sizeof record, rl, g_head); // "app version: ...\nwindows: ...\n"
                appendStr(record, sizeof record, rl, "when: ");
                appendStr(record, sizeof record, rl, whenLine);
                appendStr(record, sizeof record, rl, "\nexception: ");
                DWORD code = 0; void* addr = nullptr;
                if (ep && ep->ExceptionRecord) { code = ep->ExceptionRecord->ExceptionCode; addr = ep->ExceptionRecord->ExceptionAddress; }
                appendHex8(record, sizeof record, rl, (unsigned long)code);
                appendStr(record, sizeof record, rl, " at ");
                char addrBuf[24]; size_t al = 0; addrBuf[0] = '\0';
                appendHex8(addrBuf, sizeof addrBuf, al, (unsigned long)(ULONG_PTR)addr);
                // full-width address: appendHex8 prints 8 digits; addresses are 64-bit — print high half too when present
                if ((ULONG_PTR)addr > 0xFFFFFFFFull) {
                    char hi[24]; size_t hl = 0; hi[0] = '\0';
                    appendHex8(hi, sizeof hi, hl, (unsigned long)(((ULONG_PTR)addr) >> 32));
                    char full[40]; size_t fl = 0; full[0] = '\0';
                    appendStr(full, sizeof full, fl, hi);
                    appendStr(full, sizeof full, fl, addrBuf + 2); // low half without its 0x
                    appendStr(record, sizeof record, rl, full);
                } else {
                    appendStr(record, sizeof record, rl, addrBuf);
                }
                appendStr(record, sizeof record, rl, "\nstate at crash (last known):\n");
                appendStr(record, sizeof record, rl, g_state[0] ? g_state : "(none recorded)\n");
                appendStr(record, sizeof record, rl, "Send the .txt and the matching .dmp from this folder to Milo.\n");
                // build path: dir + stem + .txt
                size_t pl = 0; pathW[0] = L'\0';
                for (const wchar_t* p = g_dirW; *p && pl + 1 < MAX_PATH; ++p) pathW[pl++] = *p;
                if (pl + 1 < MAX_PATH) pathW[pl++] = L'\\';
                for (const char* p = stem; *p && pl + 1 < MAX_PATH; ++p) pathW[pl++] = (wchar_t)*p;
                for (const wchar_t* p = L".txt"; *p && pl + 1 < MAX_PATH; ++p) pathW[pl++] = *p;
                pathW[pl] = L'\0';
                HANDLE h = CreateFileW(pathW, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(h, record, (DWORD)rl, &written, nullptr);
                    CloseHandle(h);
                }
            }
            // minidump (sibling .dmp) — DbgHelp does the heavy lifting;
            // if it fails we still kept the text record.
            {
                size_t pl = 0; pathW[0] = L'\0';
                for (const wchar_t* p = g_dirW; *p && pl + 1 < MAX_PATH; ++p) pathW[pl++] = *p;
                if (pl + 1 < MAX_PATH) pathW[pl++] = L'\\';
                for (const char* p = stem; *p && pl + 1 < MAX_PATH; ++p) pathW[pl++] = (wchar_t)*p;
                for (const wchar_t* p = L".dmp"; *p && pl + 1 < MAX_PATH; ++p) pathW[pl++] = *p;
                pathW[pl] = L'\0';
                HANDLE h = CreateFileW(pathW, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    MINIDUMP_EXCEPTION_INFORMATION mei = {};
                    mei.ThreadId = GetCurrentThreadId();
                    mei.ExceptionPointers = ep;
                    mei.ClientPointers = FALSE;
                    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                                      (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                                      ep ? &mei : nullptr, nullptr, nullptr);
                    CloseHandle(h);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // A fault inside the handler itself: swallow it here —
            // the original crash continues to WER regardless.
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    inline void terminateSink() {
        // std::terminate / uncaught C++ exception: no exception
        // pointers to give the filter — write the record with a
        // zero code via a synthetic call, then let the runtime end
        // the process as it would have.
        EXCEPTION_RECORD er = {}; EXCEPTION_POINTERS ep = { &er, nullptr };
        er.ExceptionCode = 0xE06D7363; // C++ EH magic — "uncaught exception class"
        unhandledFilter(&ep);
    }
} // namespace detail

// Install once, as early as possible (app initialise, before the
// main window exists). appVersionUtf8 = the app's version string.
inline void install(const char* appVersionUtf8) {
    using namespace detail;
    if (g_installed) return;
    g_installed = true;
    // Resolve Documents\MeterBar (same USERPROFILE idiom as the
    // capture log) and make sure it exists.
    wchar_t home[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dirW(home, n); dirW += L"\\Documents\\MeterBar";
    // Create the two levels best-effort (the app's own settings
    // code creates Documents\MeterBar at startup anyway — handler
    // install also tries, so a first-run crash still lands).
    {
        std::wstring docs(home, n); docs += L"\\Documents";
        CreateDirectoryW(docs.c_str(), nullptr);
        CreateDirectoryW(dirW.c_str(), nullptr);
    }
    wcsncpy_s(g_dirW, MAX_PATH, dirW.c_str(), _TRUNCATE);
    {
        char narrowDir[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, narrowDir, MAX_PATH, nullptr, nullptr);
        strncpy_s(g_dir, MAX_PATH, narrowDir, _TRUNCATE);
    }
    // Pre-format the record head: version + windows build lines.
    {
        // Windows build via RtlGetVersion (true build, no manifest lie).
        char winLine[128] = "unknown";
        using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW vi = {}; vi.dwOSVersionInfoSize = sizeof(vi);
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (fn && fn(&vi) == 0) {
            char b[128]; size_t bl = 0; b[0] = '\0';
            appendNum(b, sizeof b, bl, (unsigned long)vi.dwMajorVersion, 0); appendStr(b, sizeof b, bl, ".");
            appendNum(b, sizeof b, bl, (unsigned long)vi.dwMinorVersion, 0); appendStr(b, sizeof b, bl, " build ");
            appendNum(b, sizeof b, bl, (unsigned long)vi.dwBuildNumber, 0);
            strncpy_s(winLine, sizeof winLine, b, _TRUNCATE);
        }
        size_t hl = 0; g_head[0] = '\0';
        appendStr(g_head, sizeof g_head, hl, "app version: ");
        appendStr(g_head, sizeof g_head, hl, appVersionUtf8 ? appVersionUtf8 : "?");
        appendStr(g_head, sizeof g_head, hl, "\nwindows: ");
        appendStr(g_head, sizeof g_head, hl, winLine);
        appendStr(g_head, sizeof g_head, hl, "\n");
    }
    SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(terminateSink);
}
#else
inline void install(const char*) {}
#endif

// The app keeps this snapshot current (source switches + a slow UI
// tick): pre-formatted "key: value\n" lines describing the last
// state the app can honestly know. Called from the UI thread;
// the handler only reads the finished buffer.
inline void setStateSnapshot(const char* snapshotUtf8) {
#if defined(_WIN32)
    using namespace detail;
    if (!snapshotUtf8) { g_state[0] = '\0'; return; }
    strncpy_s(g_state, sizeof g_state, snapshotUtf8, _TRUNCATE);
#else
    (void)snapshotUtf8;
#endif
}

// Documents\MeterBar as UTF-8 ("" when unresolved) — for the
// Open Log Folder actions and the next-launch notice.
inline std::string folderPathUtf8() {
#if defined(_WIN32)
    return std::string(detail::g_dir);
#else
    return {};
#endif
}
} // namespace crashhandler
