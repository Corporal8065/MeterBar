#pragma once
#include <array>
#include <cstdlib>
#include "ReserveState.hpp" // IntRect + ReserveEdge
namespace meterbar {
// FAMILY LINK PROTOCOL (v0.2.0, P4 item k) — one design shared by all
// three bars (MeterBar / TickerBar / SessionBar), copied per repo with
// only the namespace changed. The TRANSPORT is a small named
// shared-memory block (app/Source/BarLink.h, Windows; honest stub
// elsewhere). Everything DECIDED is pure and lives here:
//
//  - SNAP: when a linked drag ends, a bar whose edge sits within
//    kSnapPx of a linked peer's edge aligns to it exactly.
//  - FOLLOW: while a linked peer is being dragged, followers move by
//    the leader's delta from the drag-session start (offsets and
//    sizes preserved — groupDockRect handles the docked case).
//  - GROUP DOCK: a follower whose linked peer is reserve-docked at
//    an edge stacks against the peer's free side AND aligns to the
//    peer on the cross axis (left edges aligned when stacked
//    vertically, top edges when stacked horizontally — v0.2.1: the
//    v0.2.0 geometry moved the follower along the dock axis only, so
//    a group stacked under a peer that had been moved sideways kept
//    its own x and no longer sat under the peer), so top/bottom
//    docking treats the linked group as one bar.
//  - SHARED BACKGROUND: each slot carries a bgSeq-stamped background
//    (family style ordinal + opacity); a change in any linked bar is
//    adopted by the others (the client publishes only on change, so
//    adoption cannot ping-pong).
//
// A drag session is "live" while its stamp is fresher than
// kLeaderStaleMs; staleness — never a stuck flag — ends it.

inline constexpr unsigned kLinkMagic = 0x424C4B31; // "BLK1"
inline constexpr int kLinkAppMeter = 0, kLinkAppTicker = 1, kLinkAppSession = 2;
inline constexpr int kSnapPx = 10;
inline constexpr long long kLeaderStaleMs = 600;
inline constexpr long long kPeerStaleMs = 3000;

struct LinkSlot {
    int appId = -1;            // kLinkApp* ; -1 = empty slot
    long long heartbeatMs = 0; // last publish from that app
    IntRect rect{};            // current screen rect
    int linked = 0;            // LINK toggle on
    int dockedEdge = -1;       // ReserveEdge ordinal while reserve-registered, else -1
    unsigned bgSeq = 0;        // increments when THAT app changes its background
    int bgStyle = 0;           // family ordinal: 0 CleanDark, 1 MidnightKids, 2 Flat (3 = MeterBar Pastel; followers clamp)
    int bgOpacityPct = 100;
    unsigned dragSeq = 0;      // increments at each new drag session of that app
    IntRect dragStart{};       // leader rect when the session began
    long long dragStampMs = 0; // last leader movement stamp
};
struct LinkShared {
    unsigned magic = kLinkMagic;
    LinkSlot slots[3];
};

inline bool slotLive(const LinkSlot& s, long long nowMs) {
    return s.appId >= 0 && s.heartbeatMs > 0 && nowMs - s.heartbeatMs <= kPeerStaleMs;
}
inline bool dragLive(const LinkSlot& s, long long nowMs) {
    return s.dragSeq != 0 && s.dragStampMs > 0 && nowMs - s.dragStampMs <= kLeaderStaleMs;
}

// Snap `moving` against the edges of live, linked peers (never self).
// Each axis aligns at most once (the closest candidate within kSnapPx).
inline IntRect snapToPeers(IntRect moving, const LinkShared& sh, int selfApp, long long nowMs) {
    if (moving.w <= 0 || moving.h <= 0) return moving;
    IntRect out = moving;
    int bestDx = kSnapPx + 1, bestDy = kSnapPx + 1;
    for (auto& s : sh.slots) {
        if (s.appId < 0 || s.appId == selfApp || !s.linked || !slotLive(s, nowMs)) continue;
        const IntRect& q = s.rect;
        if (q.w <= 0 || q.h <= 0) continue;
        const int candX[4] = { q.x - out.x, q.right() - out.x, q.x - out.right(), q.right() - out.right() };
        for (int c : candX) if (std::abs(c) < std::abs(bestDx)) bestDx = c;
        const int candY[4] = { q.y - out.y, q.bottom() - out.y, q.y - out.bottom(), q.bottom() - out.bottom() };
        for (int c : candY) if (std::abs(c) < std::abs(bestDy)) bestDy = c;
    }
    if (std::abs(bestDx) <= kSnapPx) out.x += bestDx;
    if (std::abs(bestDy) <= kSnapPx) out.y += bestDy;
    return out;
}

// A follower's rect during a group drag: its own session-start rect
// translated by the leader's delta. Size never changes.
inline IntRect followRect(IntRect ownStart, IntRect leaderStart, IntRect leaderNow) {
    return IntRect{ ownStart.x + (leaderNow.x - leaderStart.x),
                    ownStart.y + (leaderNow.y - leaderStart.y),
                    ownStart.w, ownStart.h };
}

// A follower's rect when stacking against a docked leader: directly
// against the leader's free side along the dock axis (Top dock ->
// follower below the leader; Bottom -> above; Left -> right of;
// Right -> left of), and ALIGNED to the leader on the cross axis:
// left edges aligned for a vertical stack, top edges for a
// horizontal one (v0.2.1 field fix — see the header note).
using DockArg = ReserveEdge; // this app's groupDockRect argument type (SessionBar uses int)
inline IntRect groupDockRect(IntRect leader, IntRect follower, ReserveEdge edge) {
    IntRect out = follower;
    switch (edge) {
        case ReserveEdge::Top:    out.y = leader.bottom(); out.x = leader.x; break;
        case ReserveEdge::Bottom: out.y = leader.y - follower.h; out.x = leader.x; break;
        case ReserveEdge::Left:   out.x = leader.right(); out.y = leader.y; break;
        case ReserveEdge::Right:  out.x = leader.x - follower.w; out.y = leader.y; break;
    }
    return out;
}

// ------------------------------------------------------------------
// BACKGROUND SYNC DECISIONS (v0.2.1) — the publish/adopt rules for the
// shared background, as pure functions over a per-client state block.
// They live here (not in the JUCE client) so the whole propagation
// policy is unit-tested; the client only moves bytes and applies the
// answers. Field background for this extraction: the v0.2.0 client
// kept these rules inline and untested, and the field test showed
// background changes not propagating. The rules below fix the three
// defects that review of the v0.2.0 client proved from the code:
//
//  1. ECHO: a follower that adopted a peer's background then
//     re-published those same values as its OWN change (its publish
//     gate compared against its pre-adoption values), bumping its
//     bgSeq and making the originator adopt its own values back.
//     Rule: adopted values ARE the adopter's published values
//     (bgAdoption records them in `published*`), so the publish gate
//     stays shut for them.
//  2. SEQ INFLATION: the settings feed force-published on EVERY
//     settings interaction, bumping bgSeq even when the background
//     had not changed. Rule: publishing is gated on an actual value
//     change (bgIsLocalChange) — nothing else may bump bgSeq.
//  3. LAST-WRITER bookkeeping across several peers changing at once:
//     every unseen live+linked peer seq is marked seen in one scan,
//     and the LAST such peer in slot order supplies the values (the
//     v0.2.0 loop's effective outcome, now pinned by tests).
struct BgPacket {
    int style = 0;      // family background-style ordinal (LinkSlot::bgStyle)
    int opacityPct = 100;
};
struct BgSyncState {
    int publishedStyle = -999, publishedPct = -999; // values this app last published OR adopted (-999 = none yet)
    unsigned seenSeq[3] = { 0, 0, 0 };              // newest bgSeq consumed per app slot
};
// True when (style, pct) differ from what this app last published or
// adopted — the ONLY condition under which bgSeq may be bumped.
inline bool bgIsLocalChange(const BgSyncState& s, int style, int pct) {
    return style != s.publishedStyle || pct != s.publishedPct;
}
// Scan the shared block for a background this app has not consumed.
// Every live, linked peer with an unseen non-zero bgSeq is marked
// seen; when at least one qualifies, the LAST one in slot order wins:
// returns true and fills `out`, and records the adopted values as
// this app's published values (the echo rule). Peers that are
// unlinked, stale, empty, or self never qualify.
inline bool bgAdoption(const LinkShared& sh, int selfApp, BgSyncState& s, long long nowMs, BgPacket& out) {
    bool found = false;
    for (int i = 0; i < 3; ++i) {
        const LinkSlot& p = sh.slots[i];
        if (p.appId < 0 || p.appId == selfApp || !p.linked || !slotLive(p, nowMs)) continue;
        if (p.bgSeq == 0 || p.bgSeq == s.seenSeq[i]) continue;
        s.seenSeq[i] = p.bgSeq;
        out = BgPacket{ p.bgStyle, p.bgOpacityPct };
        found = true;
    }
    if (found) { s.publishedStyle = out.style; s.publishedPct = out.opacityPct; }
    return found;
}
} // namespace meterbar
