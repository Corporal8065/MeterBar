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
//    an edge stacks against the peer's free side, so top/bottom
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
// Right -> left of). The other axis is untouched.
using DockArg = ReserveEdge; // this app's groupDockRect argument type (SessionBar uses int)
inline IntRect groupDockRect(IntRect leader, IntRect follower, ReserveEdge edge) {
    IntRect out = follower;
    switch (edge) {
        case ReserveEdge::Top:    out.y = leader.bottom(); break;
        case ReserveEdge::Bottom: out.y = leader.y - follower.h; break;
        case ReserveEdge::Left:   out.x = leader.right(); break;
        case ReserveEdge::Right:  out.x = leader.x - follower.w; break;
    }
    return out;
}
} // namespace meterbar
