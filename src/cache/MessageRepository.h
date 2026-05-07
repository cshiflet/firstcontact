#pragma once

#include "models/Message.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

// CRUD over the `messages`, `message_labels`, and `attachments` tables.
// All methods open the per-thread sqlite handle internally; safe to call
// from the sync thread or the UI thread (each opens its own connection).
//
// Multi-account: every query is scoped to a single accountId. The
// per-account API takes the accountId as the leading parameter; legacy
// zero-arg overloads exist for callers still being walked through the
// step-3 fan-out (they route through Database::defaultAccountId()).
//
// TODO(v2): cross-account variants for searchFts / searchFtsThreads (and
// possibly listByLabel for an "All Inboxes" synthetic node).
class MessageRepository {
public:
    // ---------- Per-account API ----------

    // m.accountId must be non-empty; the row lands under that account.
    static qint64 upsert(const QString& accountId, const fc::Message& m);

    static std::vector<fc::Message> listByLabel(const QString& accountId,
                                                const QString& labelId,
                                                int limit, int offset);
    static std::vector<fc::Message> listThreadsByLabel(const QString& accountId,
                                                       const QString& labelId,
                                                       int limit, int offset);
    static std::vector<fc::Message> searchFts(const QString& accountId,
                                              const QString& query, int limit);
    static std::vector<fc::Message> searchFtsThreads(const QString& accountId,
                                                     const QString& query,
                                                     int limit);

    // ---------- Cross-account API (v2 unified inbox) ----------

    // Returns messages across every account that carries the given
    // label id (e.g. "INBOX"). Ordered newest-first across accounts.
    // Used by the "All Inboxes" sidebar entry.
    static std::vector<fc::Message> listByLabelAllAccounts(
        const QString& labelId, int limit, int offset);

    // Conversation-view variant. Threads are partitioned by
    // (account_id, thread_id) so a coincidence of thread ids across
    // accounts (rare; possible for shared aliases) doesn't fold.
    static std::vector<fc::Message> listThreadsByLabelAllAccounts(
        const QString& labelId, int limit, int offset);

    // Cross-account FTS5 search. The MATCH expression covers every
    // account; results carry the source accountId so the caller can
    // route a click back to the right context.
    static std::vector<fc::Message> searchFtsAllAccounts(
        const QString& query, int limit);
    static std::vector<fc::Message> searchFtsThreadsAllAccounts(
        const QString& query, int limit);
    static fc::Message              byId(const QString& accountId,
                                         const QString& id);
    static bool                     exists(const QString& accountId,
                                            const QString& id);
    static std::vector<fc::Message> byThread(const QString& accountId,
                                             const QString& threadId);
    static void                     applyLabelDiff(const QString& accountId,
                                                    const QString& messageId,
                                                    const QStringList& added,
                                                    const QStringList& removed);
    static void                     markAccessed(const QString& accountId,
                                                  const QString& id);
    static void                     setSnoozeUntil(const QString& accountId,
                                                    const QString& id,
                                                    qint64 wakeAtMs);
    static QStringList              dueSnoozeWakeups(const QString& accountId);

    // ---------- Legacy zero-arg overloads ----------

    static qint64                   upsert(const fc::Message& m);
    static std::vector<fc::Message> listByLabel(const QString& labelId,
                                                int limit, int offset);
    static std::vector<fc::Message> listThreadsByLabel(const QString& labelId,
                                                       int limit, int offset);
    static std::vector<fc::Message> searchFts(const QString& query, int limit);
    static std::vector<fc::Message> searchFtsThreads(const QString& query,
                                                      int limit);
    static fc::Message              byId(const QString& id);
    static bool                     exists(const QString& id);
    static std::vector<fc::Message> byThread(const QString& threadId);
    static void                     applyLabelDiff(const QString& messageId,
                                                    const QStringList& added,
                                                    const QStringList& removed);
    static void                     markAccessed(const QString& id);
    static void                     setSnoozeUntil(const QString& id,
                                                    qint64 wakeAtMs);
    static QStringList              dueSnoozeWakeups();
};

}  // namespace fc::cache
