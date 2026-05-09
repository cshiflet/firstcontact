# Branch lifecycle

## Current state (2026-05-09)

| Branch | Worktree | Status |
|---|---|---|
| `claude/native-gmail-client-2bqtq` | `/home/user/firstcontact` | Single-account v1. Phases 1-3 complete. Round-1..4 autonomous review applied. **Frozen pending user manual test.** |
| `claude/multi-account` | `/home/user/firstcontact-multi` | Strict superset of single-account since merge `c8b29e8`. Adds multi-account v1 (per-account everything), v2 (unified inbox + cross-account search), v3 (per-account accent palette), v4 (cache manager dialog), and the cross-account paginated source-pinned mode. **Frozen pending user manual test.** |
| `claude/phase4-prep` | (this work) | Branched from single-account HEAD. Holds the WebEngine ADR, RestClient regression tests, deferred-items re-triage, integration test scaffolding, and Phase 4 CI work. Not landed on either trunk branch yet. |

## Decision

Both trunk branches stay alive until the user finishes manual testing on each. No deletion, rebase, or force-push.

Once the user signs off:

1. **Multi-account becomes the canonical trunk.** It already contains everything in single-account plus the multi-account features. New work targets `claude/multi-account` (or whatever it is renamed to after sign-off).
2. **Single-account branch is archived, not deleted.** Tag the final commit (`git tag v0.1.0-single-account claude/native-gmail-client-2bqtq`), then optionally delete the branch. The tag preserves git-blame and "what did v1 ship as" archaeology without keeping a parallel branch alive.
3. **`claude/phase4-prep` work is rebased onto multi-account.** The ADR, tests, and CI changes are not branch-specific — they target the codebase as a whole. The rebase is mechanical because phase4-prep is a child of single-account, which is a strict subset of multi-account.

## Why not retire single-account now

- Manual testing is still in progress; the user wants both states reachable for comparison.
- Multi-account v1 reorganizes every cache table to composite-keyed; if a regression turns up there, the single-account branch is the fallback for "did the same thing work in v1?".
- Cost of keeping the branch alive while testing is zero (no concurrent commits land on either trunk during this window).

## Why not branch off multi-account for `claude/phase4-prep`

The work in `claude/phase4-prep` is ancillary (docs + tests + CI). It does not depend on any multi-account-specific code path. Branching from single-account keeps the diff minimal, makes the PR smaller, and lets the rebase onto multi-account happen post-sign-off as a single mechanical operation rather than coupling the review of phase4 work with multi-account review.
