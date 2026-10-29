#pragma once
// =====================================================================
//  UPDATER CONFIG — the ONE place that says where MeterBar updates
//  come from. v0.19 skeleton; wired to the real repo in v0.19.2.
//
//  kGitHubRepo is "owner/repository" on GitHub.
//
//  THE REPO IS CURRENTLY PRIVATE, and that shapes what the updater
//  can honestly do: GitHub does not serve a private repo's releases
//  to unauthenticated callers at all — the API answers 404 — and
//  this app carries NO token or credential (embedding one in a
//  shipped binary would publish it). So while the repo is private,
//  the check lands in the updater's private-repo state: it says so
//  plainly and offers an "Open releases page" button (the user is
//  signed in to GitHub in their browser, so the page works there).
//  If the repo is ever made public, this SAME code performs the
//  full flow — check -> release notes -> download -> verify ->
//  install -> relaunch — with no changes: the fallback is driven
//  purely by the HTTP status GitHub answers, never by a flag here.
// =====================================================================
namespace mbupdate {
inline constexpr const char* kGitHubRepo = "Corporal8065/meterbar";
} // namespace mbupdate
