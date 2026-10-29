#pragma once
// =====================================================================
//  UPDATER CONFIG — the ONE place that says where MeterBar updates
//  come from. v0.19.
//
//  kGitHubRepo is "owner/repository" on GitHub, e.g. "someuser/meterbar".
//
//  IT IS EMPTY BY DEFAULT, ON PURPOSE: until Jackson creates the
//  GitHub repo (his account, his `gh auth login` — Milo never handles
//  his credentials), there is nowhere honest for Check For Updates to
//  point at. With the constant empty, Check For Updates shows
//  "updates not configured yet" and performs NO network request —
//  it never pretends to have checked.
//
//  To enable updates later: set this one constant to the repo slug,
//  rebuild, ship. Nothing else changes. The repo must be PUBLIC for
//  the unauthenticated releases API + asset downloads to work (a
//  private repo would require embedding a token in the exe, which
//  would be public the moment anyone inspected the binary — see
//  docs/V019_PLAN.md section 3.3).
// =====================================================================
namespace mbupdate {
inline constexpr const char* kGitHubRepo = "";
} // namespace mbupdate
