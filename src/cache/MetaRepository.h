#pragma once

#include <QString>

namespace fc::cache {

class MetaRepository {
public:
    // Global key/value sheet (schema_version, fts_version, etc.). After
    // schema 0006, per-account state lives in account_meta and is reached
    // via the (accountId, key) overloads below. The single-arg overloads
    // here remain for genuinely global keys; they were also the API used
    // by all single-account v1 callers, who are progressively migrated
    // to the per-account form during step 3.
    static QString get(const QString& key);
    static void    set(const QString& key, const QString& value);

    // Per-account key/value sheet (account_meta table). v1 keys:
    //   - "history_id"          (sync delta cursor)
    //   - "email"                (canonical sender address)
    //   - "last_used_from"       (compose default; set on every send)
    //   - "notification_mode"    ("subject" or "envelope-only")
    //   - "color_hint"           (v3 accent palette key, v3+ only)
    static QString get(const QString& accountId, const QString& key);
    static void    set(const QString& accountId, const QString& key,
                       const QString& value);

    // Convenience for the single-account legacy zero-arg history_id
    // accessors. New code should use the per-account form.
    static QString historyId();
    static void    setHistoryId(const QString& v);

    static QString historyId(const QString& accountId);
    static void    setHistoryId(const QString& accountId, const QString& v);
};

}  // namespace fc::cache
