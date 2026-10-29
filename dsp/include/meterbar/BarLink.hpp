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

inline constexpr unsigned kLinkMagic = 0x424C4B32; // "BLK2" — v0.3.0/v0.20.1 appended the one-click-stack
    // request fields and MeterBar's audio broadcast to LinkSlot. Mixed old/new
    // binaries never link (magic mismatch = the whole block reads empty), the
    // same failure shape as BLK1 vs the pre-link world.
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
    // One-click stack (v0.3.0, item 12): the initiator stamps stackSeq
    // (bumped per request), the dock edge it snapped to, and its own
    // rect AFTER snapping (the anchor). Followers whose seen stackSeq
    // differs run planStackAll and move to their computed target.
    unsigned stackSeq = 0;
    int stackEdge = -1;            // ReserveEdge ordinal of the anchor edge
    IntRect stackAnchor{};
    // Audio broadcast (v0.3.0, item 17): MeterBar publishes its band
    // energies + overall level here on its link tick; audioSeq bumps
    // when the packet changes materially. Linked siblings drive
    // their reactive backgrounds from it (bands + onset pump).
    float audioLow = 0.f, audioMid = 0.f, audioHigh = 0.f, audioLevel = 0.f;
    unsigned audioSeq = 0;
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
// v0.20.2 GROUP-DOCK PARTICIPATION RULE (freeze fix): a bar group-dock-
// follows a docked peer ONLY while it is not itself reserve-docked.
// groupDockRect is a pure translation to "flush against the peer", so
// when BOTH linked bars are docked, each one's target is computed from
// the other's LATEST rect: A stacks against B, B stacks against A's new
// rect, A stacks against B's new rect... The pair of equations has no
// fixed point (same-edge: A.y = B.y - A.h with B.y = A.y - B.h demands
// A.h + B.h == 0), so two docked+linked bars chase each other across
// the screen at the link tick rate, flooding both message threads with
// setBounds/moved/resized traffic until their buttons starve — the
// field shape of "UI stopped responding after linking" (2026-10-29,
// MeterBar<->TickerBar). A docked bar's position is owned by its own
// reserve machine; the link must not move it. Floating bar + docked
// peer still stacks and settles in one step (the target depends only
// on the peer's rect, which does not move in response).
inline bool groupDockFollows(int selfDockedEdge) { return selfDockedEdge < 0; }
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
// Live-linked opacity repair (v0.3.0, item 13): drop this client's
// published markers to the "none yet" sentinel, so the next publish
// re-stamps the CURRENT values under a fresh bgSeq and peers adopt
// them. The client calls this when its link turns on — the join path
// where the v0.2.1 "an opacity change only lands after re-linking"
// report lived.
inline void bgForceRepublish(BgSyncState& s) { s.publishedStyle = -999; s.publishedPct = -999; }
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
// ONE-CLICK STACK PLANNER (v0.3.0, item 12). Given the shared state
// and the initiator (whose slot carries stackAnchor + stackEdge),
// compute where every live, LINKED bar goes: the initiator at the
// anchor; the remaining bars in appId order after the initiator
// (wrapping), each stacked flush against the previous bar along the
// anchor edge — Top: downward (x aligned to the anchor), Bottom:
// upward, Left: rightward (y aligned), Right: leftward — in
// canonical family order (Meter, Ticker, Session) after the anchor. targets[] is
// indexed by appId; non-participants keep w == 0. Returns the
// participant count. Pure: the clients do the window moves.
inline int planStackAll(const LinkShared& sh, long long nowMs, int initiatorApp, IntRect targets[3]) {
    for (int i = 0; i < 3; ++i) targets[i] = IntRect{};
    if (initiatorApp < 0 || initiatorApp > 2) return 0;
    const LinkSlot& init = sh.slots[initiatorApp];
    if (!slotLive(init, nowMs) || !init.linked || init.stackEdge < 0) return 0;
    IntRect cursor = init.stackAnchor.w > 0 ? init.stackAnchor : init.rect;
    if (cursor.w <= 0 || cursor.h <= 0) return 0;
    targets[initiatorApp] = cursor;
    int count = 1;
    for (int step = 0; step < 3; ++step) {
        const int app = step; // canonical family order (Meter, Ticker, Session), initiator skipped below
        if (app == initiatorApp) continue;
        const LinkSlot& sl = sh.slots[app];
        if (!slotLive(sl, nowMs) || !sl.linked || sl.rect.w <= 0 || sl.rect.h <= 0) continue;
        IntRect t{ 0, 0, sl.rect.w, sl.rect.h };
        switch (init.stackEdge) {
            case 0: t.x = cursor.x; t.y = cursor.bottom(); break;   // Top: stack down
            case 1: t.x = cursor.x; t.y = cursor.y - t.h; break;    // Bottom: stack up
            case 2: t.x = cursor.right(); t.y = cursor.y; break;    // Left: stack right
            case 3: t.x = cursor.x - t.w; t.y = cursor.y; break;    // Right: stack left
            default: return count;
        }
        targets[app] = t;
        cursor = t;
        ++count;
    }
    return count;
}

// Read a peer's audio broadcast. True when the slot is live and has
// published at least once (audioSeq != 0); freshness is slotLive's
// heartbeat rule — exactly the "MeterBar is running and linked"
// test the reactive background wants.
inline bool audioPacket(const LinkSlot& s, long long nowMs, float out[4]) {
    if (!slotLive(s, nowMs) || s.audioSeq == 0) return false;
    out[0] = s.audioLow; out[1] = s.audioMid; out[2] = s.audioHigh; out[3] = s.audioLevel;
    return true;
}

} // namespace meterbar
