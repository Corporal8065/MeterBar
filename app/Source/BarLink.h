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
        auto r = getBounds();
        const long long now = nowMs();
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

private:
    static long long nowMs() { return (long long)juce::Time::getMillisecondCounterHiRes(); }
    static bool sameRect(meterbar::IntRect a, meterbar::IntRect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
    static meterbar::IntRect toCore(juce::Rectangle<int> r) { return { r.getX(), r.getY(), r.getWidth(), r.getHeight() }; }
    static juce::Rectangle<int> toJuce(meterbar::IntRect r) { return { r.x, r.y, r.w, r.h }; }

    void timerCallback() override {
        if (!shared_ || !getBounds) return;
        const long long now = nowMs();
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
    unsigned followSeq_ = 0, settledSeq_ = 0;
    meterbar::IntRect followOwnStart_{}, followLeaderStart_{};
    std::tuple<int, int, int, int, int, int> dockKey_{ -1, -1, 0, 0, 0, 0 };
    meterbar::BgSyncState bgSync_; // v0.2.1: publish/adopt bookkeeping (core decisions)
};
