#pragma once
// BarLink client (v0.2.0, P4 item k) — the app-layer half of the
// family link. Transport: a named shared-memory block
// ("Local\BarFamilyLinkV1", one slot per app) guarded by a named
// mutex; Windows only — on other platforms available() is false and
// every operation is a no-op (honest stub, same as ScreenSpace).
// All DECISIOmeterbar (snap / follow / dock-stack geometry, staleness) are
// the pure core's (meterbar/BarLink.hpp); this class only moves bytes and
// applies the core's answers through the host's callbacks.
//
// Behaviour, driven by a 30 Hz timer owned here:
//  - publishes own slot every tick (heartbeat, rect, linked, docked
//    edge, background packet — the background only re-stamps bgSeq
//    on an actual value change, per the core's bg sync decisions
//    (meterbar/BarLink.hpp, v0.2.1): adopted values count as the
//    adopter's published values, so adoption cannot echo/ping-pong,
//    and adoption is evaluated BEFORE the drag-follow section so a
//    peer's drag can no longer starve it);
//  - notifyLocalGeometry() (the host's window moved/resized feed):
//    when LINKED and no foreign drag is live, this app leads — a
//    fresh drag session (seq + stamp + start rect) others follow;
//  - followers translate by the leader's delta (offsets preserved);
//  - when a drag session goes stale, every participant snaps once
//    to nearby linked peers (core snapToPeers);
//  - a linked follower stacks against a reserve-docked peer
//    (core groupDockRect) so the group docks as one;
//  - a background change in any linked bar is adopted.
#include <juce_gui_basics/juce_gui_basics.h>
#include <meterbar/BarLink.hpp>
#include <functional>
#include <cmath>
#include <algorithm>
#include <tuple>
#if JUCE_WINDOWS
#include <windows.h>
#endif

class BarLinkClient : private juce::Timer {
public:
    struct Bg { int style = 0; float opacity = 1.0f; };
    // Host callbacks (all message-thread).
    std::function<juce::Rectangle<int>()> getBounds;
    std::function<void(juce::Rectangle<int>)> setBounds;   // position apply (size preserved by the rect itself)
    std::function<Bg()> getBg;
    std::function<void(const Bg&)> applyBg;
    std::function<int()> getDockedEdge;                    // protocol ordinal or -1
    std::function<bool()> isLinked;
    std::function<void(float out[4])> getAudio; // v0.3.0 item 17: band energies [low,mid,high] + level (MeterBar wires this; the others leave it empty)

    explicit BarLinkClient(int appId) : appId_(appId) {}
    ~BarLinkClient() override { stop(); }

    bool available() const { return shared_ != nullptr; }

    void start() {
        openShared();
        if (shared_) startTimerHz(30);
    }
    void stop() {
        stopTimer();
        closeShared();
    }

    // The host's moved/resized feed. Publishes immediately and, when
    // linked, leads a drag session unless a foreign one is live or
    // the move was our own remote apply (guard window).
    void notifyLocalGeometry() {
        if (!shared_ || !getBounds) return;
        // v0.20.2 COALESCING: this feed fires on EVERY OS moved/resized
        // notification — hundreds per second during a window drag — and
        // each call costs two mutex round-trips on the shared block plus
        // the host's reserve-machine sync. Peers sample our slot on
        // their own 30 Hz timers, so publishing faster than ~50 Hz is
        // pure waste; cap it. A skipped call loses nothing: the 30 Hz
        // timer republishes the true rect every tick, and a drag
        // session's liveness window is 600 ms.
        const long long now = nowMs();
        if (now - lastNotifyMs_ < 20) return;
        lastNotifyMs_ = now;
        auto r = getBounds();
        meterbar::LinkShared snap;
        if (!readShared(snap)) return;
        bool foreignLive = false;
        for (auto& s : snap.slots)
            if (s.appId >= 0 && s.appId != appId_ && s.linked && meterbar::dragLive(s, now)) foreignLive = true;
        const bool remoteApply = now < applyingUntilMs_;
        meterbar::LinkSlot mine = snap.slots[(size_t)appId_];
        mine.appId = appId_;
        mine.heartbeatMs = now;
        mine.rect = toCore(r);
        mine.linked = isLinked && isLinked() ? 1 : 0;
        if (mine.linked && !foreignLive && !remoteApply) {
            if (!meterbar::dragLive(mine, now)) { // new session: stamp the PRE-move rect
                mine.dragSeq += 1;
                mine.dragStart = lastRect_.w > 0 ? lastRect_ : mine.rect;
            }
            mine.dragStampMs = now;
        }
        writeSlot(mine);
        lastRect_ = mine.rect;
    }

    // The host's settings-changed feed: flush a real background change
    // now (the timer also polls this, so hosts may skip calling).
    // v0.2.1: no force flag — publishing is gated on an actual value
    // change by the core rule; the v0.2.0 force path bumped bgSeq on
    // EVERY settings interaction, changed background or not.
    void notifyLocalSettings() { publishBg(); }

    // v0.3.0 item 12: ask every linked bar to stack against this one.
    // The host snaps ITSELF to its dock edge first, then calls this
    // with that edge (protocol ordinal) and its post-snap rect; the
    // stamp lands in one slot write, followers react on their timers.
    void requestStackAll(int edgeOrdinal, juce::Rectangle<int> anchor) {
        if (!shared_) return;
        meterbar::LinkShared snap;
        if (!readShared(snap)) return;
        meterbar::LinkSlot mine = snap.slots[(size_t)appId_];
        mine.appId = appId_;
        mine.heartbeatMs = nowMs();
        mine.linked = 1;
        mine.stackSeq += 1;
        mine.stackEdge = edgeOrdinal;
        mine.stackAnchor = toCore(anchor);
        writeSlot(mine);
        seenStackSeq_[(size_t)appId_] = mine.stackSeq;
    }

    // v0.3.0 item 17: a peer's audio broadcast (false = not live / never published).
    bool peerAudio(int appId, float out[4]) {
        if (!shared_ || appId < 0 || appId > 2) return false;
        meterbar::LinkShared snap;
        if (!readShared(snap)) return false;
        return meterbar::audioPacket(snap.slots[(size_t)appId], nowMs(), out);
    }

    // Live + linked peer count (the host's Stack button enablement).
    int liveLinkedPeers() {
        if (!shared_) return 0;
        meterbar::LinkShared snap;
        if (!readShared(snap)) return 0;
        const long long now = nowMs();
        int n = 0;
        for (auto& sl : snap.slots)
            if (sl.appId >= 0 && sl.appId != appId_ && sl.linked && meterbar::slotLive(sl, now)) ++n;
        return n;
    }

    // The union rect of this bar + its live linked peers — the
    // reactive-glow field's group space (item 17). Unlinked or alone:
    // the bar's own rect.
    juce::Rectangle<int> groupRect() {
        auto own = getBounds ? getBounds() : juce::Rectangle<int>{};
        if (!shared_ || !(isLinked && isLinked())) return own;
        meterbar::LinkShared snap;
        if (!readShared(snap)) return own;
        const long long now = nowMs();
        auto u = own;
        for (auto& sl : snap.slots) {
            if (sl.appId < 0 || sl.appId == appId_ || !sl.linked || !meterbar::slotLive(sl, now)) continue;
            if (sl.rect.w > 0 && sl.rect.h > 0) u = u.getUnion(toJuce(sl.rect));
        }
        return u;
    }

private:
    static long long nowMs() { return (long long)juce::Time::getMillisecondCounterHiRes(); }
    static bool sameRect(meterbar::IntRect a, meterbar::IntRect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
    static meterbar::IntRect toCore(juce::Rectangle<int> r) { return { r.getX(), r.getY(), r.getWidth(), r.getHeight() }; }
    static juce::Rectangle<int> toJuce(meterbar::IntRect r) { return { r.x, r.y, r.w, r.h }; }

    void timerCallback() override {
        if (!shared_ || !getBounds) return;
        const long long now = nowMs();
        // Item 13: the tick this app's link turns ON, force a fresh
        // publish of its current background (sentinel reset — see
        // bgForceRepublish) so peers adopt it immediately instead of
        // waiting for the next change.
        const bool linkedNow = isLinked && isLinked();
        if (linkedNow && !wasLinked_) meterbar::bgForceRepublish(bgSync_);
        wasLinked_ = linkedNow;
        publishBg();
        meterbar::LinkShared snap;
        if (!readShared(snap)) return;
        auto r = getBounds();
        // heartbeat + rect + toggles
        meterbar::LinkSlot mine = snap.slots[(size_t)appId_];
        mine.appId = appId_;
        mine.heartbeatMs = now;
        mine.rect = toCore(r);
        mine.linked = isLinked && isLinked() ? 1 : 0;
        mine.dockedEdge = getDockedEdge ? getDockedEdge() : -1;
        // Item 17: audio broadcast — re-stamp only on material change.
        if (getAudio) {
            float a[4] = { 0.f, 0.f, 0.f, 0.f };
            getAudio(a);
            auto diff = [](float x, float y) { return x > y ? x - y : y - x; };
            if (diff(a[0], mine.audioLow) > 0.01f || diff(a[1], mine.audioMid) > 0.01f ||
                diff(a[2], mine.audioHigh) > 0.01f || diff(a[3], mine.audioLevel) > 0.01f) {
                mine.audioLow = a[0]; mine.audioMid = a[1]; mine.audioHigh = a[2]; mine.audioLevel = a[3];
                mine.audioSeq += 1;
            }
        }
        writeSlot(mine);
        snap.slots[(size_t)appId_] = mine;
        lastRect_ = mine.rect;
        if (!mine.linked) return;

        // --- adopt a changed background from any linked peer ---
        // (core decision; runs BEFORE the follow section — the v0.2.0
        // order let a live peer drag postpone adoption indefinitely)
        {
            meterbar::BgPacket pkt;
            if (meterbar::bgAdoption(snap, appId_, bgSync_, now, pkt) && applyBg)
                applyBg(Bg{ pkt.style, pkt.opacityPct / 100.0f });
        }
        // --- one-click stack requests (item 12): a peer's fresh
        // stackSeq moves this bar to its planned target ---
        for (auto& sl : snap.slots) {
            if (sl.appId < 0 || sl.appId == appId_ || !sl.linked || !meterbar::slotLive(sl, now)) continue;
            if (sl.stackSeq == 0 || sl.stackSeq == seenStackSeq_[(size_t)sl.appId]) continue;
            seenStackSeq_[(size_t)sl.appId] = sl.stackSeq;
            meterbar::IntRect targets[3];
            if (meterbar::planStackAll(snap, now, sl.appId, targets) > 0 && targets[(size_t)appId_].w > 0
                && !sameRect(targets[(size_t)appId_], mine.rect)) {
                applyRemote(targets[(size_t)appId_]);
                mine.rect = targets[(size_t)appId_]; // v0.20.2: later sections this tick compare against the post-apply rect, not the stale snapshot
            }
        }
        // --- follow a live foreign drag ---
        const meterbar::LinkSlot* leader = nullptr;
        for (auto& s : snap.slots)
            if (s.appId >= 0 && s.appId != appId_ && s.linked && meterbar::dragLive(s, now)) leader = &s;
        if (leader) {
            if (leader->dragSeq != followSeq_) {
                followSeq_ = leader->dragSeq;
                followOwnStart_ = mine.rect;
                followLeaderStart_ = leader->dragStart;
                settledSeq_ = 0;
            }
            auto target = meterbar::followRect(followOwnStart_, followLeaderStart_, leader->rect);
            if (!sameRect(target, mine.rect)) applyRemote(target);
            return;
        }
        // --- drag just ended: snap once, then group-dock settle ---
        unsigned endedSeq = 0;
        for (auto& s : snap.slots)
            if (s.appId >= 0 && s.dragSeq != 0 && !meterbar::dragLive(s, now)) endedSeq = std::max(endedSeq, s.dragSeq);
        if (endedSeq != 0 && endedSeq != settledSeq_) {
            settledSeq_ = endedSeq;
            auto snapped = meterbar::snapToPeers(mine.rect, snap, appId_, now);
            if (!sameRect(snapped, mine.rect)) { applyRemote(snapped); mine.rect = snapped; }
        }
        // --- group dock: stack against a reserve-docked linked peer ---
        // v0.20.2: ONLY while this bar is not itself reserve-docked
        // (core rule groupDockFollows, BarLink.hpp). With both bars
        // docked, each stacks against the other's latest rect and the
        // pair chases forever at tick rate — the 2026-10-29 freeze.
        if (meterbar::groupDockFollows(mine.dockedEdge)) {
            for (auto& s : snap.slots) {
                if (s.appId < 0 || s.appId == appId_ || !s.linked || s.dockedEdge < 0 || !meterbar::slotLive(s, now)) continue;
                auto target = meterbar::groupDockRect(s.rect, mine.rect, (meterbar::DockArg)s.dockedEdge);
                const auto key = std::make_tuple(s.appId, s.dockedEdge, s.rect.x, s.rect.y, s.rect.w, s.rect.h);
                if (key != dockKey_) {
                    dockKey_ = key;
                    if (!sameRect(target, mine.rect)) applyRemote(target);
                }
            }
        }
    }

    void applyRemote(meterbar::IntRect target) {
        applyingUntilMs_ = nowMs() + 200;
        if (setBounds) setBounds(toJuce(target));
    }

    void publishBg() {
        if (!shared_ || !getBg) return;
        auto bg = getBg();
        const int pct = (int)std::lround(bg.opacity * 100.0f);
        // Core gate: publish ONLY a value this app has not already
        // published or adopted (adopted values are recorded in
        // bgSync_ by bgAdoption, so an adoption is never re-published
        // as a local change — the v0.2.0 echo).
        if (!meterbar::bgIsLocalChange(bgSync_, bg.style, pct)) return;
        meterbar::LinkShared snap;
        if (!readShared(snap)) return;
        meterbar::LinkSlot mine = snap.slots[(size_t)appId_];
        mine.appId = appId_;
        mine.heartbeatMs = nowMs();
        mine.bgStyle = bg.style;
        mine.bgOpacityPct = pct;
        mine.bgSeq += 1;
        writeSlot(mine);
        bgSync_.publishedStyle = bg.style; bgSync_.publishedPct = pct;
    }

    // --- shared-memory plumbing ---
    void openShared() {
#if JUCE_WINDOWS
        mutex_ = CreateMutexA(nullptr, FALSE, "Local\\BarFamilyLinkMutexV1");
        mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      (DWORD)sizeof(meterbar::LinkShared), "Local\\BarFamilyLinkV1");
        if (mapping_) {
            void* v = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(meterbar::LinkShared));
            if (v) {
                shared_ = (meterbar::LinkShared*)v;
                Lock lock{ mutex_ };
                if (shared_->magic != meterbar::kLinkMagic) *shared_ = meterbar::LinkShared{};
            }
        }
#endif
    }
    void closeShared() {
#if JUCE_WINDOWS
        if (shared_) { UnmapViewOfFile(shared_); shared_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (mutex_) { CloseHandle(mutex_); mutex_ = nullptr; }
#endif
    }
    struct Lock {
#if JUCE_WINDOWS
        HANDLE h = nullptr;
        explicit Lock(HANDLE m) : h(m) { if (h) WaitForSingleObject(h, 10); }
        ~Lock() { if (h) ReleaseMutex(h); }
#else
        explicit Lock(void*) {}
#endif
    };
    bool readShared(meterbar::LinkShared& out) {
        if (!shared_) return false;
        Lock lock{ mutex_ };
        out = *shared_;
        return out.magic == meterbar::kLinkMagic;
    }
    void writeSlot(const meterbar::LinkSlot& s) {
        if (!shared_) return;
        Lock lock{ mutex_ };
        if (shared_->magic == meterbar::kLinkMagic) shared_->slots[(size_t)appId_] = s;
    }

    int appId_;
    meterbar::LinkShared* shared_ = nullptr;
#if JUCE_WINDOWS
    HANDLE mapping_ = nullptr, mutex_ = nullptr;
#else
    void* mapping_ = nullptr; void* mutex_ = nullptr;
#endif
    meterbar::IntRect lastRect_{};
    long long applyingUntilMs_ = 0;
    long long lastNotifyMs_ = 0;          // v0.20.2: notifyLocalGeometry coalescing stamp
    unsigned followSeq_ = 0, settledSeq_ = 0;
    meterbar::IntRect followOwnStart_{}, followLeaderStart_{};
    std::tuple<int, int, int, int, int, int> dockKey_{ -1, -1, 0, 0, 0, 0 };
    meterbar::BgSyncState bgSync_;
    bool wasLinked_ = false;              // item 13: link-edge detection for the forced republish
    unsigned seenStackSeq_[3] = { 0, 0, 0 }; // item 12: last stack request consumed per app // v0.2.1: publish/adopt bookkeeping (core decisions)
};
