#pragma once
// Updater (v0.19) — Check For Updates against GitHub Releases, and the
// download -> verify -> hand-off-to-Inno -> quit sequence behind the
// Update button. Design: docs/V019_PLAN.md section 3.
//
// Shape of the code, and why:
// - The repo slug lives in UpdaterConfig.h (ONE constant). An empty
//   slug would mean unconfigured => configured() is false, no request
//   is ever made, and the UI says so honestly. (Since v0.19.2 the
//   constant names the real, currently-private repo — see the config
//   header for what "private" does to the flow.)
// - The release check runs on a DETACHED std::thread that touches
//   nothing but a shared_ptr<SharedState>: it cannot dangle into UI
//   objects, and the app never blocks on the network. MainComponent
//   polls the state's atomic `done` flag from its existing UI timer
//   and presents the result on the message thread.
// - Only FILTERED facts cross back: tag, notes, and the
//   MeterBar-Setup.exe asset's URL + size + digest from the API.
// - Version comparison is meterbar::semVerIsNewer (numeric semver,
//   unit-tested) — never a string compare.
// - The installer hand-off: download to %TEMP%\MeterBar-Update, verify
//   size (and SHA-256 digest when GitHub published one), launch with
//   /SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART, then
//   the app quits itself; the installer's silent-gated [Run] entry
//   (MeterBar.iss) relaunches the new version. User presets/settings
//   live outside {app} and are untouched by the installer.
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include <meterbar/SemVer.hpp>
#include <meterbar/ReleaseParse.hpp>
#include "UpdaterConfig.h"
#include <atomic>
#include <memory>
#include <thread>

namespace mbupdate {

inline bool configured() { return juce::String(kGitHubRepo).trim().isNotEmpty(); }

// Where the "Open releases page" fallback sends the user's browser.
inline juce::String releasesPageUrl() {
    return "https://github.com/" + juce::String(kGitHubRepo).trim() + "/releases";
}

struct ReleaseInfo {
    bool ok = false;              // the check itself succeeded
    juce::String error;           // when !ok: what went wrong, in plain words
    juce::String tag;             // e.g. "v0.20.0"
    juce::String notes;           // release body from the API
    juce::String assetUrl;        // browser_download_url of MeterBar-Setup.exe
    juce::int64 assetSize = 0;    // published size (0 = not published)
    juce::String assetDigest;     // "sha256:<hex>" when GitHub provides it, else empty
    bool newer = false;           // tag is a strictly newer semver than the running version
    // v0.19.2: the private-repo state is FIRST-CLASS, not an error
    // string: the repo being private makes GitHub answer the
    // unauthenticated check with 401/403/404, and the UI presents
    // that state (plus the browser fallback) instead of a failure.
    bool authBlocked = false;     // GitHub refused for auth reasons (repo private)
    int httpStatus = 0;           // status of the check response (0 = none received)
};

struct SharedState {
    std::atomic<bool> done { false };
    ReleaseInfo result; // written by the worker BEFORE done is stored (release), read after an acquire load
};

// Starts the background check. currentVersion is the compiled-in
// version (METERBAR_VERSION_STRING). The worker writes only into `st`.
//
// v0.19.2 outcome branches (all decided from what GitHub actually
// answered — see ReleaseParse.hpp for the auth-status rule):
//   - HTTP 401/403/404  -> authBlocked (the repo is private: GitHub
//     shows nothing to an unauthenticated app, and this app carries
//     no token by design). NOT reported as a generic failure.
//   - no stream, no auth status -> offline/unreachable error.
//   - any other non-2xx -> error naming the HTTP status.
//   - 2xx -> fields extracted by meterbar::parseReleaseJson (the
//     unit-tested pure-std parser), then ok + newer via semVer.
inline void checkLatestAsync(std::shared_ptr<SharedState> st, const juce::String& currentVersion) {
    std::thread([st, currentVersion] {
        ReleaseInfo r;
        const juce::String repo = juce::String(kGitHubRepo).trim();
        if (repo.isEmpty()) {
            r.error = "Updates are not configured in this build.";
        } else {
            juce::URL url("https://api.github.com/repos/" + repo + "/releases/latest");
            // The status-code sink must outlive the stream (JUCE may
            // write through the pointer while the stream lives), so
            // it is heap-held for the whole check, like `st`.
            auto statusSink = std::make_shared<int>(0);
            auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                               .withExtraHeaders("User-Agent: MeterBar/" + currentVersion
                                                 + "\r\nAccept: application/vnd.github+json")
                               .withConnectionTimeoutMs(15000)
                               .withStatusCode(statusSink.get());
            std::unique_ptr<juce::InputStream> stream(url.createInputStream(options));
            const int statusCode = *statusSink;
            r.httpStatus = statusCode;
            if (meterbar::httpStatusBlocksAuth(statusCode)) {
                r.authBlocked = true;
                r.error = "GitHub would not show the release list (HTTP " + juce::String(statusCode)
                        + ") — the repository is private and this app is not signed in.";
            } else if (stream == nullptr) {
                r.error = "Could not reach GitHub (offline, or the update feed is unavailable).";
            } else if (statusCode != 0 && (statusCode < 200 || statusCode >= 300)) {
                r.error = "GitHub answered HTTP " + juce::String(statusCode) + " to the update check.";
            } else {
                meterbar::ReleaseFields f;
                if (!meterbar::parseReleaseJson(stream->readEntireStreamAsString().toStdString(), f)) {
                    r.error = "GitHub returned an unreadable response.";
                } else {
                    r.tag = f.tag;
                    r.notes = f.notes;
                    r.assetUrl = f.assetUrl;
                    r.assetSize = (juce::int64) f.assetSize;
                    r.assetDigest = f.assetDigest;
                    if (!f.hasAsset || r.assetUrl.isEmpty()) {
                        r.error = "The latest release has no MeterBar-Setup.exe to download.";
                    } else {
                        r.ok = true;
                        r.newer = meterbar::semVerIsNewer(r.tag.toStdString(), currentVersion.toStdString());
                    }
                }
            }
        }
        st->result = r;
        st->done.store(true, std::memory_order_release);
    }).detach();
}

// SHA-256 hex of a file, lowercase — the same form GitHub's asset
// `digest` field uses after its "sha256:" prefix.
inline juce::String sha256HexOfFile(const juce::File& f) {
    std::unique_ptr<juce::FileInputStream> in(f.createInputStream());
    if (in == nullptr) return {};
    return juce::SHA256(*in).toHexString();
}

} // namespace mbupdate
