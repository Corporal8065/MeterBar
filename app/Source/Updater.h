#pragma once
// Updater (v0.19) — Check For Updates against GitHub Releases, and the
// download -> verify -> hand-off-to-Inno -> quit sequence behind the
// Update button. Design: docs/V019_PLAN.md section 3.
//
// Shape of the code, and why:
// - The repo slug lives in UpdaterConfig.h (ONE constant, EMPTY by
//   default). Unconfigured => configured() is false, no request is
//   ever made, and the UI says so honestly.
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
#include "UpdaterConfig.h"
#include <atomic>
#include <memory>
#include <thread>

namespace mbupdate {

inline bool configured() { return juce::String(kGitHubRepo).trim().isNotEmpty(); }

struct ReleaseInfo {
    bool ok = false;              // the check itself succeeded
    juce::String error;           // when !ok: what went wrong, in plain words
    juce::String tag;             // e.g. "v0.20.0"
    juce::String notes;           // release body from the API
    juce::String assetUrl;        // browser_download_url of MeterBar-Setup.exe
    juce::int64 assetSize = 0;    // published size (0 = not published)
    juce::String assetDigest;     // "sha256:<hex>" when GitHub provides it, else empty
    bool newer = false;           // tag is a strictly newer semver than the running version
};

struct SharedState {
    std::atomic<bool> done { false };
    ReleaseInfo result; // written by the worker BEFORE done is stored (release), read after an acquire load
};

// Starts the background check. currentVersion is the compiled-in
// version (METERBAR_VERSION_STRING). The worker writes only into `st`.
inline void checkLatestAsync(std::shared_ptr<SharedState> st, const juce::String& currentVersion) {
    std::thread([st, currentVersion] {
        ReleaseInfo r;
        const juce::String repo = juce::String(kGitHubRepo).trim();
        if (repo.isEmpty()) {
            r.error = "Updates are not configured in this build.";
        } else {
            juce::URL url("https://api.github.com/repos/" + repo + "/releases/latest");
            auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                               .withExtraHeaders("User-Agent: MeterBar/" + currentVersion
                                                 + "\r\nAccept: application/vnd.github+json")
                               .withConnectionTimeoutMs(15000);
            std::unique_ptr<juce::InputStream> stream(url.createInputStream(options));
            if (stream == nullptr) {
                r.error = "Could not reach GitHub (offline, or the update feed is unavailable).";
            } else {
                const juce::var json = juce::JSON::parse(stream->readEntireStreamAsString());
                if (json.getDynamicObject() == nullptr) {
                    r.error = "GitHub returned an unreadable response.";
                } else {
                    r.tag = json.getProperty("tag_name", {}).toString();
                    r.notes = json.getProperty("body", {}).toString();
                    if (auto* assets = json.getProperty("assets", {}).getArray()) {
                        for (auto& a : *assets) {
                            if (a.getProperty("name", {}).toString() == "MeterBar-Setup.exe") {
                                r.assetUrl = a.getProperty("browser_download_url", {}).toString();
                                r.assetSize = (juce::int64) a.getProperty("size", {});
                                r.assetDigest = a.getProperty("digest", {}).toString();
                                break;
                            }
                        }
                    }
                    if (r.tag.isEmpty()) r.error = "The latest release has no version tag.";
                    else if (r.assetUrl.isEmpty()) r.error = "The latest release has no MeterBar-Setup.exe to download.";
                    else {
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
