#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>

namespace fc::ui {

// In-memory map of labelId → (display name, bg / fg colours, type).
// Backs the message-list delegate's per-row label pills — looking each
// label up via SQL on every paint would be too slow (paint runs many
// times per scroll frame on hundreds of rows), and labels are small
// and bounded so a flat hash on the UI thread is fine.
//
// Lifetime: a single instance() singleton owned by the UI thread. The
// sync layer rebuilds the labels table off-thread; SyncService emits
// `labelsUpdated` once the rebuild commits, MainWindow forwards that
// to invalidate() (queued connection — runs on the UI thread).
//
// Thread safety: every call must come from the GUI thread. The QObject
// signal hop is enough to keep that invariant honest.
class LabelStyleCache : public QObject {
    Q_OBJECT
public:
    struct Style {
        QString name;     // display name (Gmail-style, "/" still embedded)
        QColor  bg;       // empty / invalid when Gmail has no colour set
        QColor  fg;
        QString type;     // "system" | "user"
    };

    static LabelStyleCache& instance();

    // Replaces the cache from a fresh LabelRepository::all(accountId)
    // read. Per-account v1: callers pass the active account; the same
    // cache is rebuilt on every account switch. v2 will add a
    // multi-account variant that prefixes ids with the account id so
    // pills paint correctly in the unified inbox view.
    void invalidate(const QString& accountId);

    // Returns Style{} (empty / invalid) for unknown ids.
    Style get(const QString& labelId) const;

signals:
    // Fired right after invalidate() finishes so views can repaint.
    void changed();

private:
    LabelStyleCache();
    QHash<QString, Style> cache_;
};

}  // namespace fc::ui
