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
//    (family style ordinal + opacity); the group PARENT's background
//    is adopted by the others. Since BLK6 the parent is the FIRST
//    bar to turn its Link on in the group (first-clicked-wins,
//    Jackson's stated semantics 2026-11-01): every slot carries the
//    linkSeq it claimed from the shared counter when its link
//    turned on, and the parent is the live linked member with the
//    smallest linkSeq (ties — an unclaimed 0 or a counter reset —
//    break by lowest appId, so every member computes the same
//    parent from the same block). The client publishes only on
//    change, so adoption cannot ping-pong.
//
// A drag session is "live" while its stamp is fresher than
// kLeaderStaleMs; staleness — never a stuck flag — ends it.

inline constexpr unsigned kLinkMagic = 0x424C4B37; // "BLK7" — appended the appearance
    // extension (bgTheme/bgScheme/palette) + pulse fields to LinkSlot and
    // lastClaim/linkAll* to LinkShared (BLK7 link-direction fix, 2026-11-03).
    // BLK6 appended linkSeq to
    // LinkSlot and nextLinkSeq to LinkShared (the first-clicked-wins
    // join-order stamp). Same rule as every earlier bump: the layout
    // grew, so the magic moves with it — mixed BLK5/BLK6 binaries
    // never link (magic mismatch = the whole block reads empty).
    // BLK5 widened the bgEffect value
    // domain again (4 = visualizer Field) and appended audioBalance
    // to LinkSlot. Same rule as BLK4 (v0.21.5, which widened the
    // domain for 3 = visualizer Lines): the ordinal's MEANING grew,
    // so the magic moves with it — mixed old/new binaries never
    // link (magic mismatch = the whole block reads empty), the same
    // failure shape as every earlier bump. BLK3 appended bgEffect to
    // LinkSlot (the background packet carries the effect selection,
    // not just style + opacity); BLK2 appended the one-click-stack
    // request fields and MeterBar's audio broadcast.
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
    // Background effect (BLK3, v0.21.3; domain widened by BLK4,
    // v0.21.5, and BLK5, v0.25.0): the publisher's bgEffect
    // ordinal — 0 styled, 1 visualizer Bubbles, 2 glow,
    // 3 visualizer Lines, 4 visualizer Field. The visualizer
    // STYLE rides this ordinal (rather than a new field) so the
    // style propagates exactly the way the effect selection
    // already does. MeterBar has no glow and no 3-way effect: it
    // publishes 1, 3 or 4 when its visualizer is on (by its
    // style), else 0, and adopts 1/3/4 as visualizer-on with
    // that style (2 lands as visualizer-on, style untouched).
    int bgEffect = 0;
    // Stereo balance (BLK5, v0.25.0): MeterBar publishes its
    // StereoFrame balance here (positive = right-heavy) beside
    // the band energies above, so linked siblings place their
    // distributed emitters by the same stereo picture. Appended
    // last, like every field since BLK1.
    float audioBalance = 0.f;
    // Join order (BLK6): the value this app claimed from the shared
    // block's nextLinkSeq counter at the moment its Link turned on
    // (0 = not linked / never claimed). The group background parent
    // is the live linked member with the SMALLEST linkSeq — the
    // first bar that clicked Link (see bgParentApp). Appended last,
    // like every field since BLK1.
    unsigned linkSeq = 0;
    // BLK7: appearance extension — the group parent's theme, colour
    // scheme and visualizer palette ride the background packet.
    int bgTheme = -1;        // family theme ordinal (see DESIGN §2), -1 = none yet
    int bgScheme = -1;       // family colour-scheme ordinal (DESIGN §2), -1 = none yet
    float bgPalHue = 0.f, bgPalSat = 0.f, bgPalSpread = 0.f, bgPalCycle = 0.f;
    // BLK7: linked transients — MeterBar stamps a pulse on each audio
    // impact; linked siblings fire a cell impulse on the seq edge.
    unsigned pulseSeq = 0;
    int pulseColour = 0;     // 0xRRGGBB
    float pulseStrength = 0.f; // 0..1 emitter-side strength at stamp time
};
struct LinkShared {
    unsigned magic = kLinkMagic;
    LinkSlot slots[3];
    unsigned nextLinkSeq = 0; // BLK6: join-order counter; a client claims ++nextLinkSeq under the block mutex when its Link turns on
    unsigned lastClaim[3] = { 0, 0, 0 }; // BLK7: per-app last linkSeq claim,
        // survives that app's restart while any member keeps the block
        // alive; cleared only by a deliberate link OFF edge (DESIGN §4).
    // BLK7: LINK ALL request (DESIGN §6), stamped by the initiator under the
    // block mutex; consumed by every client incl. the initiator.
    unsigned linkAllSeq = 0;
    int linkAllEdge = 0;              // ReserveEdge ordinal: 0 Top, 1 Bottom only
    unsigned linkAllBaseSeq = 0;      // claims reserved: base+appId+1 (DESIGN §6)
    IntRect linkAllArea{};            // display bounds the stack docks into
};

inline bool slotLive(const LinkSlot& s, long long nowMs) {
    return s.appId >= 0 && s.heartbeatMs > 0 && nowMs - s.heartbeatMs <= kPeerStaleMs;
}
// BLK7 claim restore (DESIGN §4): the seq a (re)joining app should
// stamp. Returns the app's recorded lastClaim when it is nonzero AND
// no LIVE slot currently holds that value (the holder may be the
// app's own stale slot from its previous process — that is exactly
// the restart case); 0 = take a fresh counter claim (++nextLinkSeq).
inline unsigned claimFor(const LinkShared& sh, int appId, long long nowMs) {
    if (appId < 0 || appId > 2) return 0;
    const unsigned last = sh.lastClaim[appId];
    if (last == 0) return 0;
    for (int i = 0; i < 3; ++i)
        if (sh.slots[i].linkSeq == last && slotLive(sh.slots[i], nowMs)) return 0;
    return last;
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
inline int bgParentApp(const LinkShared& sh, int selfApp, unsigned selfLinkSeq, long long nowMs, int* parentSlotOut = nullptr); // defined with the bg sync decisions below
// BLK7 (DESIGN §5): the AUTOMATIC group-dock never moves the group
// PARENT — parentage governs geometry at exactly this one point
// (drag-follow / drag-end snap / Stack stay parent-blind: explicit
// gestures/commands move the whole stack, per Jackson's rule).
inline bool groupDockMovesSelf(const LinkShared& sh, int selfApp, unsigned selfLinkSeq, long long nowMs) {
    return bgParentApp(sh, selfApp, selfLinkSeq, nowMs) != selfApp;
}
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
//
//  4. NO PARENT (v0.25.1, the 2026-11-01 field report): the rules
//     above are SYMMETRIC — any bar's packet can be adopted by any
//     other. Combined with the client's link-join re-stamp (a bar
//     turning its link on force-republishes its own pre-link
//     background under a fresh bgSeq), two bars linking up each
//     adopted the OTHER's stale background — a swap, not an
//     inheritance — and the bar Jackson set the shared background
//     on never became the authority. His standing rule is
//     link-PARENT ownership: when bars are linked, the parent
//     applies the visual settings to the others. Rule: adoption
//     flows DOWNHILL ONLY from the group parent. A follower's own
//     local change still publishes (its packet becomes the
//     authority if it ever becomes the parent), but no follower
//     packet is ever adopted while a parent is live.
//
//  5. WRONG PARENT + NO PARENT-LINK TRIGGER (BLK6, the 2026-11-01
//     evening field test): v0.25.1 picked the parent by FIXED slot
//     order (lowest appId), so the parent was whoever owned the
//     lowest slot — not the bar Jackson linked first, which is the
//     semantics he stated live: "click the button for the link
//     first would be ideally what you would want the background to
//     change to." Adoption was also purely bgSeq-gated: nothing
//     re-triggered it when the PARENT's link event was the one
//     that completed the group. Rule: the parent is the live
//     linked member with the smallest linkSeq (first-clicked
//     wins); adoption re-fires whenever the parent's identity OR
//     its linkSeq changes (adoptedFrom / adoptedLinkSeq), not only
//     when its bgSeq advances — so a follower adopts the standing
//     packet when the parent's packet first appears in the block,
//     when parenthood hands off after the parent unlinks (the
//     earliest-linked remaining member takes over), and never
//     from a late joiner (largest linkSeq, never the parent while
//     an earlier member is live). A parent that unlinks and
//     relinks claims a NEW linkSeq and returns as a late joiner.
struct BgPacket {
    int style = 0;      // family background-style ordinal (LinkSlot::bgStyle)
    int opacityPct = 100;
    int effect = 0;     // bgEffect ordinal (LinkSlot::bgEffect) — BLK3; BLK4 adds ordinal 3 (Lines), BLK5 ordinal 4 (Field)
    // BLK7 appearance extension (DESIGN §3): family theme ordinal,
    // family colour-scheme ordinal, visualizer palette — ride the
    // same packet, gated by the same "Link background" toggle.
    int theme = -1;     // family theme ordinal (DESIGN §2), -1 = none yet
    int scheme = -1;    // family colour-scheme ordinal (DESIGN §2), -1 = none yet
    float palHue = 0.f, palSat = 0.f, palSpread = 0.f, palCycle = 0.f;
};
struct BgSyncState {
    int publishedStyle = -999, publishedPct = -999; // values this app last published OR adopted (-999 = none yet)
    int publishedEffect = -999;                     // same, for the effect ordinal
    int publishedTheme = -999, publishedScheme = -999; // BLK7: same, for theme/scheme (-999 = none yet)
    float publishedPalHue = -999.f, publishedPalSat = -999.f, publishedPalSpread = -999.f, publishedPalCycle = -999.f; // BLK7 palette markers
    unsigned seenSeq[3] = { 0, 0, 0 };              // newest bgSeq consumed per app slot
    int adoptedFrom = -1;                           // v0.25.1: appId of the parent the current values came from (-1 = own / none)
    unsigned adoptedLinkSeq = 0;                    // BLK6: linkSeq of that parent when the values were adopted (a parent relink / handoff re-fires adoption)
};
// True when (style, pct, effect) differ from what this app last
// published or adopted — the ONLY condition under which bgSeq may
// be bumped.
inline bool bgIsLocalChange(const BgSyncState& s, int style, int pct, int effect) {
    return style != s.publishedStyle || pct != s.publishedPct || effect != s.publishedEffect;
}
// BLK7: the nine-field gate — the ONLY condition under which bgSeq
// may be bumped once the appearance extension rides the packet.
inline bool bgIsLocalChange(const BgSyncState& s, const BgPacket& v) {
    return v.style != s.publishedStyle || v.opacityPct != s.publishedPct || v.effect != s.publishedEffect
        || v.theme != s.publishedTheme || v.scheme != s.publishedScheme
        || v.palHue != s.publishedPalHue || v.palSat != s.publishedPalSat
        || v.palSpread != s.publishedPalSpread || v.palCycle != s.publishedPalCycle;
}
// Live-linked opacity repair (v0.3.0, item 13): drop this client's
// published markers to the "none yet" sentinel, so the next publish
// re-stamps the CURRENT values under a fresh bgSeq and peers adopt
// them. The client calls this when its link turns on AND it is the
// group parent (or no parent packet exists yet) — the join path
// where the v0.2.1 "an opacity change only lands after re-linking"
// report lived. Since v0.25.1 a FOLLOWER joining a group whose
// parent already publishes does NOT re-stamp: it adopts the
// parent's standing packet instead (parent ownership, defect 4).
inline void bgForceRepublish(BgSyncState& s) { s.publishedStyle = -999; s.publishedPct = -999; s.publishedEffect = -999;
    s.publishedTheme = -999; s.publishedScheme = -999; // BLK7
    s.publishedPalHue = -999.f; s.publishedPalSat = -999.f; s.publishedPalSpread = -999.f; s.publishedPalCycle = -999.f; }
// The group background PARENT (BLK6 first-clicked-wins — defect 5
// above): the live, linked member with the smallest linkSeq; ties
// (linkSeq 0 = unclaimed, or a counter reset after the shared
// block was recreated) break by lowest appId, so every member
// reading the same block computes the same parent. selfApp is
// always a candidate (the caller is linked by construction);
// selfLinkSeq, when nonzero, is self's claim even if self's slot
// in the block has not been re-stamped with it yet (the link-join
// path runs before the tick that republishes self's slot).
// parentSlotOut, when given, receives the parent's slot index
// (-1 when the parent is self and self has no slot in the block).
inline int bgParentApp(const LinkShared& sh, int selfApp, unsigned selfLinkSeq, long long nowMs, int* parentSlotOut) {
    auto effSeq = [](unsigned s) { return s == 0 ? 0xFFFFFFFFu : s; };
    // Self's effective claim: the explicit claim when given, else
    // self's slot in the block. Computed BEFORE the peer scan — an
    // earlier draft re-read it mid-scan and clobbered the incumbent
    // whenever self's slot sat after the parent's in slot order.
    unsigned selfEff = effSeq(selfLinkSeq);
    if (selfLinkSeq == 0)
        for (int i = 0; i < 3; ++i)
            if (sh.slots[i].appId == selfApp && sh.slots[i].linked && slotLive(sh.slots[i], nowMs))
                selfEff = effSeq(sh.slots[i].linkSeq);
    unsigned bestSeq = selfEff;
    int parent = selfApp;
    for (int i = 0; i < 3; ++i) {
        const LinkSlot& p = sh.slots[i];
        if (p.appId < 0) continue;
        if (p.appId == selfApp) continue; // self already counted above
        if (!p.linked || !slotLive(p, nowMs)) continue;
        const unsigned e = effSeq(p.linkSeq);
        if (e < bestSeq || (e == bestSeq && p.appId < parent)) { bestSeq = e; parent = p.appId; }
    }
    if (parentSlotOut) {
        *parentSlotOut = -1;
        for (int i = 0; i < 3; ++i)
            if (sh.slots[i].appId == parent) { *parentSlotOut = i; break; }
    }
    return parent;
}
// Scan the shared block for the group PARENT's background (parent
// ownership — defects 4 and 5 above). When this app IS the parent
// there is nothing to adopt (and adoptedFrom resets, so a later
// parent is never mistaken for the source of the current values).
// Otherwise the parent's packet is adopted when it is non-zero and
// any of these moved since the last adoption: its bgSeq (a real
// background change), the parent's identity (handoff — the new
// parent's standing packet is authoritative even if its seq was
// consumed under an old regime), or the parent's linkSeq (the
// parent relinked, or its packet first appeared in the block
// after this app's earlier scans). Adopted values are recorded
// as this app's published values (the echo rule). Peers that are
// unlinked, stale, empty, or not the parent never supply values.
inline bool bgAdoption(const LinkShared& sh, int selfApp, BgSyncState& s, long long nowMs, BgPacket& out) {
    unsigned selfSeq = 0;
    for (int i = 0; i < 3; ++i)
        if (sh.slots[i].appId == selfApp) selfSeq = sh.slots[i].linkSeq;
    int parentSlot = -1;
    const int parentApp = bgParentApp(sh, selfApp, selfSeq, nowMs, &parentSlot);
    if (parentApp == selfApp) { s.adoptedFrom = -1; s.adoptedLinkSeq = 0; return false; }
    if (parentSlot < 0) return false;
    const LinkSlot& p = sh.slots[parentSlot];
    if (p.bgSeq == 0) return false;
    // BLK7 defect-6 port (DESIGN §3): the siblings' DIVERGED gate,
    // extended to all nine fields — adoption is CONTINUOUS: whenever
    // this app's published values differ from the parent's standing
    // packet, the packet is (re-)adopted, whatever the seq
    // bookkeeping says (a follower-side local change no longer
    // strands the follower away from the parent's values).
    const bool triggerMoved = p.bgSeq != s.seenSeq[parentSlot] || s.adoptedFrom != parentApp || s.adoptedLinkSeq != p.linkSeq;
    const bool diverged = s.publishedStyle != p.bgStyle || s.publishedPct != p.bgOpacityPct || s.publishedEffect != p.bgEffect
        || s.publishedTheme != p.bgTheme || s.publishedScheme != p.bgScheme
        || s.publishedPalHue != p.bgPalHue || s.publishedPalSat != p.bgPalSat
        || s.publishedPalSpread != p.bgPalSpread || s.publishedPalCycle != p.bgPalCycle;
    if (!triggerMoved && !diverged) return false;
    s.seenSeq[parentSlot] = p.bgSeq;
    s.adoptedFrom = parentApp;
    s.adoptedLinkSeq = p.linkSeq;
    out = BgPacket{};
    out.style = p.bgStyle; out.opacityPct = p.bgOpacityPct; out.effect = p.bgEffect;
    out.theme = p.bgTheme; out.scheme = p.bgScheme;
    out.palHue = p.bgPalHue; out.palSat = p.bgPalSat; out.palSpread = p.bgPalSpread; out.palCycle = p.bgPalCycle;
    s.publishedStyle = out.style; s.publishedPct = out.opacityPct; s.publishedEffect = out.effect;
    s.publishedTheme = out.theme; s.publishedScheme = out.scheme;
    s.publishedPalHue = out.palHue; s.publishedPalSat = out.palSat; s.publishedPalSpread = out.palSpread; s.publishedPalCycle = out.palCycle;
    return true;
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

// BLK7 LINK ALL PLANNER (DESIGN §6c). Participants: slots that are
// slotLive (a bar that is not running cannot be stacked; a running-
// but-unlinked bar IS included — its client links it in the same
// event; its slot rect supplies its height). Order is fixed appId
// 0,1,2 top-to-bottom for BOTH edges. Every target: x = area.x,
// w = area.w, h = that bar's own slot height. Top: cursor starts at
// area.y, each next bar below the previous. Bottom: cursor starts at
// area.bottom(); place appId 2 first upward, then 1, then 0 — visual
// top-to-bottom order stays 0,1,2 with SessionBar flush to the
// bottom edge. Non-participants keep w == 0. Returns participant count.
inline int planLinkAll(const LinkShared& sh, long long nowMs, int edge, IntRect area, IntRect targets[3]) {
    for (int i = 0; i < 3; ++i) targets[i] = IntRect{};
    if (area.w <= 0 || area.h <= 0) return 0;
    bool part[3] = { false, false, false };
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        const LinkSlot& sl = sh.slots[i];
        if (slotLive(sl, nowMs) && sl.rect.h > 0) { part[i] = true; ++count; }
    }
    if (edge == 1) { // Bottom
        int cursor = area.bottom();
        for (int i = 2; i >= 0; --i) {
            if (!part[i]) continue;
            targets[i] = IntRect{ area.x, cursor - sh.slots[i].rect.h, area.w, sh.slots[i].rect.h };
            cursor = targets[i].y;
        }
    } else { // Top (and any non-Bottom ordinal)
        int cursor = area.y;
        for (int i = 0; i < 3; ++i) {
            if (!part[i]) continue;
            targets[i] = IntRect{ area.x, cursor, area.w, sh.slots[i].rect.h };
            cursor = targets[i].bottom();
        }
    }
    return count;
}

// BLK7 linked transients (DESIGN §7): read a peer's stamped pulse.
// True when the slot is live and has stamped at least once.
inline bool pulsePacket(const LinkSlot& s, long long nowMs, unsigned& seq, int& colour, float& strength) {
    if (!slotLive(s, nowMs) || s.pulseSeq == 0) return false;
    seq = s.pulseSeq; colour = s.pulseColour; strength = s.pulseStrength;
    return true;
}

// Read a peer's audio broadcast. True when the slot is live and has
// published at least once (audioSeq != 0); freshness is slotLive's
// heartbeat rule — exactly the "MeterBar is running and linked"
// test the reactive background wants.
inline bool audioPacket(const LinkSlot& s, long long nowMs, float out[5]) {
    if (!slotLive(s, nowMs) || s.audioSeq == 0) return false;
    out[0] = s.audioLow; out[1] = s.audioMid; out[2] = s.audioHigh; out[3] = s.audioLevel;
    out[4] = s.audioBalance; // BLK5 (v0.25.0)
    return true;
}

// ------------------------------------------------------------------
// GROUP FIELD MAP (TickerBar v0.5.0 / MeterBar v0.21.0 / SessionBar
// v0.4.0) — Jackson's linked-background ask: when the bars are
// LINKED, the visualizer background is ONE field sized by the
// COMBINATION of the linked bars' screen rects (the group rect the
// client computes), not by each bar alone. The particle model
// stays in unit space; this pure map converts a unit field point
// into the bar's own render-cache pixels — the field spans the
// group and each bar draws only the slice of it that falls inside
// its own window, the same group-space trick the reactive glow
// has used since item 17, now applied to the particle layer.
// A burst anchored at field x = 0.5 sits at the centre of the
// COMBINED strip, and the ambient convergers stream toward that
// same centre across the seams. Dot RADIUS is deliberately not
// remapped: a dot keeps the physical size it has unlinked (cache
// pixels) — only its position comes from the group.
//
// When the group rect equals the bar's own rect (unlinked, linked
// but alone, or a degenerate rect) the map is the historical
// bar-local one — kx = iw, ky = ih, offsets 0, grouped = false —
// so the unlinked render is exactly the pre-link behaviour.
struct FieldMap {
    float kx = 0, ky = 0;   // cache px per unit of field span
    float ox = 0, oy = 0;   // cache px offset of the field origin
    bool grouped = false;   // true when the field spans beyond this bar
};
inline FieldMap fieldMap(IntRect own, IntRect group, int iw, int ih) {
    FieldMap m;
    m.kx = (float)iw; m.ky = (float)ih;
    if (own.w <= 0 || own.h <= 0 || group.w <= 0 || group.h <= 0 || iw <= 0 || ih <= 0)
        return m;
    if (group.x == own.x && group.y == own.y && group.w == own.w && group.h == own.h)
        return m;
    const float sx = (float)iw / (float)own.w;   // cache px per screen px, this bar
    const float sy = (float)ih / (float)own.h;
    m.kx = (float)group.w * sx;
    m.ky = (float)group.h * sy;
    m.ox = -(float)(own.x - group.x) * sx;
    m.oy = -(float)(own.y - group.y) * sy;
    m.grouped = true;
    return m;
}
// One field point -> this bar's cache pixels under a FieldMap.
inline void fieldToLocal(const FieldMap& m, float fx, float fy, float& lx, float& ly) {
    lx = fx * m.kx + m.ox;
    ly = fy * m.ky + m.oy;
}
// True when a dot of radius r (cache px) centred at (lx, ly) can
// touch the cache at all — the render skips off-slice dots instead
// of asking the rasteriser to clip them one by one. A dot that is
// even partly inside still draws, exactly as the bar-local render
// treated its own edges.
inline bool fieldDotVisible(float lx, float ly, float r, int iw, int ih) {
    return lx + r >= 0.f && ly + r >= 0.f && lx - r <= (float)iw && ly - r <= (float)ih;
}

} // namespace meterbar
