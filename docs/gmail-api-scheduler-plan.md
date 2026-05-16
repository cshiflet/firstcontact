# Gmail API Scheduler Plan

## Purpose

Introduce a FirstContact-specific Gmail API scheduler that routes Gmail work
through one execution model. The scheduler should improve latency for
user-visible actions, keep background work from crowding interactive work,
preserve offline mutation order, and centralize retry, token refresh,
rate-limit, and connectivity policy.

This is a follow-on architecture plan, not a prerequisite for the near-term
review fixes in [review-execution-plan.md](review-execution-plan.md). The review
plan should land first. This scheduler should then migrate one call path at a
time without changing user-visible behavior.

## Current State

Gmail API work is currently spread across several owners:

- `MainWindow` directly triggers user-visible reads and mutations.
- `SyncService` performs initial sync, incremental history sync, top-up fetches,
  body-on-demand fetches, and background crawling.
- `OutboxWorker` drains pending sends from `OutboxRepository`.
- `PendingOpsWorker` drains label/read/star/archive-style mutations from
  `PendingOpsRepository`.
- `DraftSync` drains draft synchronization work.
- `RestClient` owns low-level HTTP retry and request execution.
- `OAuthClient` owns token refresh and persistence through `TokenStore`.

This split has worked for the current app, but it makes ordering and retry
policy difficult to reason about. The scheduler should become the single
coordination boundary above `GmailClient` / `RestClient`, while repositories
remain the durable cache/log layer.

## Core Invariant

For any two mutations that affect overlapping or related Gmail resources,
execution order must match user intent order.

The scheduler may run non-conflicting work in parallel, reorder background reads
by priority, and coalesce redundant final-state mutations. It must never reorder
conflicting mutations in a way that changes the final user-intended state.

## Non-Goals

- Do not reintroduce a broad per-account `QThread` refactor as part of the first
  scheduler step.
- Do not replace `PendingOpsRepository`, `OutboxRepository`, or
  `DraftRepository` in the first step.
- Do not coalesce sends.
- Do not change visible mailbox behavior while moving a call path into the
  scheduler.

## Work Categories

Classify every Gmail API operation before scheduling it.

- Fast read: user-visible reads where latency matters, such as opening a message
  body or thread.
- Fast mutation: user-visible mutation requested while online, such as star,
  archive, trash, mark read, or modify labels.
- Durable mutation: mutation recorded while offline or while replay order must
  be preserved.
- Slow read: background or bulk reads, such as delta sync, load more, label
  hydration, and body prefetch.
- Maintenance: cleanup, cache refreshes, body compression dictionary work, or
  other opportunistic tasks.

Sends and draft operations are special mutations because they are not safely
idempotent.

## Scheduler Model

Use two execution lanes plus the existing durable repositories.

Fast lane:

- FIFO for latency-sensitive work.
- Accepts user-visible reads and non-conflicting online mutations.
- Must not wait behind background sync or prefetch.

Slow lane:

- Priority queue, not pure FIFO.
- Handles background reads, sync, prefetch, maintenance, and durable mutation
  replay.
- Preserves FIFO within each priority.

Durable mutation logs:

- `PendingOpsRepository` remains the durable log for replayable label/read/star
  mutations during the first migration.
- `OutboxRepository` remains the durable log for sends.
- `DraftRepository` remains the durable draft state store.
- The scheduler should initially orchestrate these repositories rather than
  replacing them.

## Slow Lane Priorities

Use priority levels within the slow lane:

1. durable mutation replay from `PendingOpsRepository`;
2. due sends from `OutboxRepository`, with non-idempotent send handling;
3. draft sync work from `DraftSync` / `DraftRepository`;
4. user-requested refresh or delta sync;
5. active-view hydration, such as load-more or label details;
6. background body prefetch and background crawl;
7. maintenance and opportunistic work.

When durable replay has pending work, lower-priority slow jobs should pause,
yield, or wait behind replay. Fast reads may continue unless they conflict with
a pending mutation.

## Mutation Scope

Every mutation should declare the resources it may affect.

Example fields:

```text
account_id
message_ids
thread_ids
label_ids
draft_ids
mailbox_wide
destructive
non_idempotent
unknown_scope
```

Examples:

- Star message: `message_ids = [message_id]`
- Archive thread: `thread_ids = [thread_id]`
- Add label to message: `message_ids = [message_id]`, `label_ids = [label_id]`
- Delete label: `label_ids = [label_id]`, `destructive = true`
- Send message: `draft_ids = [draft_id]` when applicable,
  `non_idempotent = true`

Scopes are per account. Two operations on different `account_id` values do not
conflict unless they mutate shared local process state.

Unknown or broad scopes should be treated conservatively.

## Conflict Rules

Two mutations conflict when their scopes overlap or one scope may contain the
other.

Rules:

- Same account and same message id conflict.
- Same account and same thread id conflict.
- Same account and same label id conflict when either operation mutates label
  membership or label existence.
- Thread mutation conflicts with known member-message mutations.
- Destructive container operations conflict broadly with contained or
  potentially contained resources.
- Mailbox-wide or unknown-scope mutations conflict with all mutations in that
  account.
- Sends and draft sends should not be coalesced and should not bypass older
  related mutations.

If the scheduler cannot prove two mutations are independent, preserve order.

## Scheduling Rules

When online and no durable replay is active:

- fast reads go to the fast lane;
- user mutations go to the fast lane if they do not conflict with pending
  mutations;
- background reads go to the slow lane.

When offline:

- replayable mutations append to `PendingOpsRepository`;
- sends append to `OutboxRepository`;
- draft state remains in `DraftRepository`;
- reads should use cache or fail with a recoverable offline result;
- no Gmail API calls should be attempted unless explicitly allowed as
  connectivity probes.

When reconnecting:

- durable mutation replay enters the slow lane at highest priority;
- replay executes durable mutations in FIFO order;
- background slow jobs wait behind replay;
- fast reads may continue;
- new user mutations may run in the fast lane only if they do not conflict with
  pending or running replay mutations;
- conflicting new mutations append behind the durable log.

## Coalescing Rules

Coalescing is optional and should be conservative.

Safe candidates:

- star followed by unstar on the same message collapses to final state;
- mark read followed by mark unread on the same message collapses to final
  state;
- add/remove the same label on the same message collapses to final membership.

Do not coalesce:

- sends;
- draft send/update operations until product semantics are fully specified;
- trash/delete with other mutations;
- thread-level and message-level operations unless membership is known;
- operations with unknown or broad scope.

Only coalesce queued work that has not started executing.

## Job Contract

Each scheduled job should contain:

```text
id
account_id
lane
priority
kind
display_label
mutation_scope
is_mutation
is_replayable
is_idempotent
non_idempotent
created_at
timeout
run(context, token)
on_success(result)
on_failure(error)
offline_payload
```

The scheduler should own:

- token acquisition and forced refresh on 401;
- connectivity and network-error classification;
- retry and backoff policy;
- Gmail rate-limit policy;
- in-flight tracking;
- durable mutation replay orchestration;
- conflict detection;
- conservative coalescing;
- dispatching completion events back to the UI or caller.

`RestClient` should remain the low-level transport helper. `GmailClient` should
remain the typed Gmail endpoint wrapper. Scheduler jobs should call
`GmailClient`; callers outside the scheduler should gradually stop calling it
directly.

## Error Policy

Separate error classes:

- Network unavailable: queue replayable mutations, pause background reads, start
  connectivity recovery.
- Auth expired or revoked: pause account work and request re-authentication.
- HTTP 401 with refresh token available: force refresh once, then retry once.
- Rate limited: retry with backoff when safe.
- Permanent API failure: surface to the user and remove or mark the failed job.
- Ambiguous send failure: do not auto-replay; require user-visible recovery
  because the message may already have been sent.

## Migration Strategy

Each migration step should preserve existing user-facing behavior before adding
new scheduling behavior.

1. Add scheduler types and interfaces without moving existing calls.
2. Add a test-only fake `GmailClient` or scheduler job runner so scheduling
   behavior can be tested without UI widgets or real network.
3. Route background body prefetch / background crawl through the slow lane.
   These are lowest risk because they are opportunistic reads.
4. Route `SyncService::topUpLabel` and active-view hydration through the slow
   lane at active-view priority.
5. Route `SyncService::runOnce` incremental sync through the slow lane.
6. Wrap `PendingOpsWorker` replay in scheduler jobs while keeping
   `PendingOpsRepository` as the durable source of truth.
7. Wrap `OutboxWorker` send attempts in scheduler jobs while keeping
   `OutboxRepository` as the durable source of truth and preserving
   non-idempotent send safeguards.
8. Wrap `DraftSync` in scheduler jobs after draft semantics are explicitly
   tested.
9. Route user-visible reads from `MainWindow` through the fast lane.
10. Route simple online mutations through the fast lane only when conflict
    detection can prove they do not bypass older related durable mutations.
11. Add conservative coalescing for queued final-state mutations.
12. Remove direct Gmail API execution paths outside scheduler-owned jobs, except
    low-level helpers (`GmailClient`, `RestClient`, `OAuthClient`) used by those
    jobs.

## Interaction With Near-Term Review Fixes

The review fixes intentionally happen before this migration:

- The Qt 6.10 REST canary should be updated in the current `RestClient` tests.
- Forced refresh on 401 should first be fixed in `RestClient` / `OAuthClient`;
  the scheduler later becomes the owner of that policy.
- Gmail history label deltas and remote deletes should first be implemented in
  `SyncService`; moving incremental sync into the scheduler later should carry
  those semantics forward unchanged.
- Threading comments should be corrected to describe the current UI-thread
  model; the scheduler can later define a clearer dispatch boundary.

## Test Plan

Test the scheduler independently from the UI.

Required cases:

- fast read runs while slow prefetch is queued;
- slow priorities run in expected order;
- `PendingOpsRepository` replay is FIFO;
- background slow jobs wait behind replay;
- non-conflicting fast mutation can run during replay;
- conflicting fast mutation appends behind replay;
- unknown-scope mutation conflicts conservatively;
- safe queued mutations coalesce to final state;
- sends are never coalesced or auto-replayed after ambiguous network failure;
- queue persistence failure prevents a "queued" success signal;
- process restart preserves durable mutation order.

## Success Criteria

- All Gmail API work is visible to the scheduler.
- Interactive reads are not blocked by background prefetch.
- Offline mutations replay in user intent order.
- New online mutations during replay cannot invalidate older conflicting queued
  mutations.
- Scheduler behavior is testable without the UI framework.
- Direct API execution paths are limited to low-level helpers used by scheduler
  jobs.
