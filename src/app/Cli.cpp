#include "Cli.h"

#include "account/AccountManager.h"
#include "cache/BodyCompressionWorker.h"
#include "cache/Database.h"
#include "cache/MessageRepository.h"
#include "cache/Migrations.h"   // databaseHandle()
#include "util/BodyCodec.h"
#include "util/Format.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QLocale>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>

#include <cstdio>

namespace fc::app {

namespace {

constexpr const char* kCmdDbStats          = "db-stats";
constexpr const char* kCmdClearCache       = "clear-cache";
constexpr const char* kCmdCompressDb       = "compress-db";
constexpr const char* kCmdCompressionStats = "compression-stats";
constexpr const char* kCmdResetDb          = "reset-db";

void printHelp() {
    std::printf(
        "Usage: firstcontact [command] [args]\n"
        "\n"
        "Without a command, launches the GUI (default).\n"
        "\n"
        "Commands:\n"
        "  db-stats\n"
        "      Print per-account cache footprint to stdout. Read-only;\n"
        "      safe while the GUI is open.\n"
        "\n"
        "  clear-cache [email-or-account-id]\n"
        "      Drop cached messages / threads / labels / attachments /\n"
        "      drafts / outbox / pending-ops / account_meta for the\n"
        "      target account, leaving the accounts row + auth tokens\n"
        "      intact so the next launch resyncs from scratch.\n"
        "      No argument = every signed-in account.\n"
        "\n"
        "  compress-db [email-or-account-id]\n"
        "      Train a fresh zstd dictionary from a random sample of\n"
        "      cached bodies, then recompress every body row with the\n"
        "      new dict. Slow, memory-frugal, single-threaded.\n"
        "      No argument = every signed-in account, sequentially.\n"
        "\n"
        "  compression-stats [email-or-account-id]\n"
        "      Walk every cached message body, decompress it, and\n"
        "      report the compressed-vs-plaintext byte counts and\n"
        "      resulting ratio. Read-only; takes a second or two per\n"
        "      thousand messages. No argument = every signed-in\n"
        "      account.\n"
        "\n"
        "  reset-db\n"
        "      Delete the cache database file (and its -wal / -shm\n"
        "      sidecars), recreate the schema fresh, and rehydrate the\n"
        "      accounts table from the QSettings mirror at\n"
        "      ~/.config/FirstContact/FirstContact.conf. Account UUIDs\n"
        "      survive verbatim, so QtKeychain tokens keyed by them\n"
        "      stay reachable — accounts remain signed in and re-sync\n"
        "      on the next launch. Use this after a schema-version\n"
        "      mismatch or to recover from a corrupted cache.\n"
        "\n"
        "  help | --help | -h\n"
        "      Print this help text.\n");
}

using fc::util::humanBytes;

// Resolve `email-or-id` to an accounts row, or return an empty AccountInfo
// when the target isn't found. Empty input → first signed-in account
// (NOT used today; only the resolveAll variant below).
fc::account::AccountInfo resolveAccount(
        const fc::account::AccountManager& accounts,
        const QString& token) {
    if (token.isEmpty()) return {};
    // Try exact id first (UUID), then email match (case-insensitive).
    for (const auto& a : accounts.accounts()) {
        if (a.id == token) return a;
    }
    for (const auto& a : accounts.accounts()) {
        if (a.email.compare(token, Qt::CaseInsensitive) == 0) return a;
    }
    return {};
}

// Resolve a single token to a list of (id, email) pairs to operate
// on. Empty token = every account. Returns empty list (with the
// caller printing an error) when a non-empty token doesn't match
// anything.
QList<fc::account::AccountInfo> resolveTargets(
        const fc::account::AccountManager& accounts,
        const QString& token) {
    if (token.isEmpty()) return accounts.accounts();
    const auto a = resolveAccount(accounts, token);
    if (a.id.isEmpty()) return {};
    return { a };
}

// db-stats — read-only. Prints per-account stats + an "Orphaned"
// row for any account_id present in cache tables but missing from
// the accounts table.
int runDbStats(const fc::account::AccountManager& accounts) {
    const auto rows = accounts.accounts();
    if (rows.isEmpty()) {
        std::printf("No signed-in accounts.\n");
        return 0;
    }
    qint64 totalBytes = 0;
    int totalMsgs = 0;
    for (const auto& a : rows) {
        const auto s = accounts.statsFor(a.id);
        const QString display = a.email.isEmpty()
            ? a.id : a.email;
        std::printf("Account: %s\n", qUtf8Printable(display));
        std::printf("  id:           %s\n", qUtf8Printable(a.id));
        std::printf("  size:         %s\n",
                     qUtf8Printable(humanBytes(s.sizeBytes)));
        std::printf("  messages:     %s\n",
                     qUtf8Printable(QLocale::system().toString(s.messageCount)));
        std::printf("  threads:      %s\n",
                     qUtf8Printable(QLocale::system().toString(s.threadCount)));
        std::printf("  labels:       %d\n", s.labelCount);
        std::printf("  attachments:  %s\n",
                     qUtf8Printable(QLocale::system().toString(s.attachmentCount)));
        std::printf("  drafts:       %d\n", s.draftCount);
        std::printf("  outbox:       %d\n", s.outboxCount);
        std::printf("  pending_ops:  %d\n", s.pendingOpsCount);
        std::printf("\n");
        totalBytes += s.sizeBytes;
        totalMsgs  += s.messageCount;
    }
    const auto orphans =
        const_cast<fc::account::AccountManager&>(accounts).orphanedAccountIds();
    if (!orphans.isEmpty()) {
        std::printf("Orphaned account ids (cache rows with no matching "
                     "accounts row):\n");
        for (const auto& id : orphans) {
            const auto s = accounts.statsFor(id);
            std::printf("  %s — %s, %d message(s)\n",
                         qUtf8Printable(id),
                         qUtf8Printable(humanBytes(s.sizeBytes)),
                         s.messageCount);
        }
        std::printf("  (clean up via: firstcontact clear-cache <id> for "
                     "each, or use the Cache Manager GUI dialog)\n\n");
    }
    std::printf("Total across signed-in accounts: %s, %s message(s)\n",
                 qUtf8Printable(humanBytes(totalBytes)),
                 qUtf8Printable(QLocale::system().toString(totalMsgs)));
    return 0;
}

// clear-cache — destructive but reversible (just resyncs from
// Gmail on next launch). Leaves the accounts table + QtKeychain
// tokens untouched.
int runClearCache(fc::account::AccountManager& accounts,
                    const QString& token) {
    const auto targets = resolveTargets(accounts, token);
    if (targets.isEmpty()) {
        if (token.isEmpty()) {
            std::printf("No signed-in accounts to clear.\n");
            return 0;
        }
        std::fprintf(stderr,
            "clear-cache: no account matched '%s'. Try a full email "
            "address or the account id (see `firstcontact db-stats`).\n",
            qUtf8Printable(token));
        return 2;
    }
    int failed = 0;
    for (const auto& a : targets) {
        const QString display = a.email.isEmpty() ? a.id : a.email;
        const auto before = accounts.statsFor(a.id);
        std::printf("Clearing cache for %s (%s, %d message(s))…\n",
                     qUtf8Printable(display),
                     qUtf8Printable(humanBytes(before.sizeBytes)),
                     before.messageCount);
        if (!accounts.dropCache(a.id)) {
            std::fprintf(stderr, "  dropCache failed.\n");
            ++failed;
            continue;
        }
        std::printf("  done.\n");
    }
    return failed == 0 ? 0 : 1;
}

// compress-db — runs the BodyCompressionWorker once per target
// account, blocking on each via a nested event loop. Sequential
// rather than parallel: the worker is deliberately memory-frugal
// and there's no benefit to racing two of them on the same DB.
int runCompressDb(fc::account::AccountManager& accounts,
                    const QString& token) {
    const auto targets = resolveTargets(accounts, token);
    if (targets.isEmpty()) {
        if (token.isEmpty()) {
            std::printf("No signed-in accounts to compress.\n");
            return 0;
        }
        std::fprintf(stderr,
            "compress-db: no account matched '%s'.\n",
            qUtf8Printable(token));
        return 2;
    }
    int exitCode = 0;
    for (const auto& a : targets) {
        const QString display = a.email.isEmpty() ? a.id : a.email;
        std::printf("Compressing %s …\n", qUtf8Printable(display));

        QEventLoop loop;
        auto* w = new fc::cache::BodyCompressionWorker(
            a.id, fc::cache::BodyCompressionWorker::Mode::Recompress);

        // The worker emits signals from its own thread; QEventLoop's
        // quit slot is invokable across threads via QueuedConnection
        // (Qt's default for cross-thread).
        QObject::connect(w, &fc::cache::BodyCompressionWorker::progress,
            &loop, [](const QString&, int done, int total) {
                std::printf("  %d / %d\r", done, total);
                std::fflush(stdout);
            });
        QObject::connect(w, &fc::cache::BodyCompressionWorker::finished,
            &loop, [&loop](const QString&, int n, qint64 saved) {
                std::printf("\n  done: %d row(s) rewritten, %s reclaimed.\n",
                             n, qUtf8Printable(humanBytes(saved)));
                loop.quit();
            });
        QObject::connect(w, &fc::cache::BodyCompressionWorker::failed,
            &loop, [&loop, &exitCode](const QString&, const QString& reason) {
                std::fprintf(stderr, "\n  failed: %s\n",
                              qUtf8Printable(reason));
                exitCode = 1;
                loop.quit();
            });

        w->start();
        loop.exec();   // blocks until finished/failed.
        if (exitCode != 0) break;
    }
    return exitCode;
}

// compression-stats — walk every cached body and tally compressed-vs-
// plaintext bytes per account. Reports the resulting ratio. Read-only.
//
// For each row we get the on-disk byte count for free (length(body_*));
// the plaintext byte count requires BodyCodec::decompress on rows with
// body_compression=1 (rows with body_compression=0 are stored plaintext
// so on-disk == plaintext for them).
int runCompressionStats(fc::account::AccountManager& accounts,
                         const QString& token) {
    const auto targets = resolveTargets(accounts, token);
    if (targets.isEmpty()) {
        std::printf("compression-stats: no matching account for '%s'.\n",
                     qUtf8Printable(token));
        return 1;
    }

    qint64 grandPlain      = 0;
    qint64 grandCompressed = 0;
    int    grandRows       = 0;

    for (const auto& acc : targets) {
        const QString display = acc.email.isEmpty() ? acc.id : acc.email;
        std::printf("Account: %s\n", qUtf8Printable(display));

        const QByteArray dict =
            fc::cache::MessageRepository::dictionaryFor(acc.id);

        auto db = fc::cache::databaseHandle();
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT body_compression, body_text, body_html "
            "FROM messages WHERE account_id = :a "
            "  AND ( (body_text IS NOT NULL AND length(body_text) > 0) "
            "     OR (body_html IS NOT NULL AND length(body_html) > 0) )"));
        q.bindValue(QStringLiteral(":a"), acc.id);
        if (!q.exec()) {
            std::printf("  query failed: %s\n",
                         qUtf8Printable(q.lastError().text()));
            continue;
        }

        qint64 plainBytes      = 0;
        qint64 compressedBytes = 0;
        qint64 compressedPlain = 0;   // plaintext-equivalent of the compressed slice
        int    rowsCompressed  = 0;
        int    rowsPlaintext   = 0;
        int    failedDecompress = 0;

        while (q.next()) {
            const int compFlag = q.value(0).toInt();
            const QByteArray bt = q.value(1).toByteArray();
            const QByteArray bh = q.value(2).toByteArray();

            const qint64 onDisk = bt.size() + bh.size();

            if (compFlag == 0) {
                // Stored plaintext.
                plainBytes      += onDisk;
                compressedBytes += onDisk;
                ++rowsPlaintext;
                continue;
            }

            // Compressed row. Decompress each field with the account dict.
            auto restoreSize = [&](const QByteArray& field) -> qint64 {
                if (field.isEmpty()) return 0;
                if (!fc::util::BodyCodec::isCompressed(field)) {
                    return field.size();
                }
                const QByteArray d =
                    fc::util::BodyCodec::decompress(field, dict);
                if (d.isEmpty()) { ++failedDecompress; return 0; }
                return d.size();
            };
            const qint64 plain = restoreSize(bt) + restoreSize(bh);

            plainBytes      += plain;
            compressedBytes += onDisk;
            compressedPlain += plain;
            ++rowsCompressed;
        }

        const qint64 rows = rowsCompressed + rowsPlaintext;
        std::printf("  rows with body:    %s "
                     "(%s compressed, %s plaintext)\n",
                     qUtf8Printable(QLocale::system().toString(rows)),
                     qUtf8Printable(QLocale::system().toString(rowsCompressed)),
                     qUtf8Printable(QLocale::system().toString(rowsPlaintext)));
        std::printf("  on-disk total:     %s\n",
                     qUtf8Printable(humanBytes(compressedBytes)));
        std::printf("  plaintext total:   %s\n",
                     qUtf8Printable(humanBytes(plainBytes)));
        if (compressedBytes > 0) {
            const double ratio = double(plainBytes) / double(compressedBytes);
            std::printf("  overall ratio:     %.2fx "
                         "(%.1f%% saved)\n",
                         ratio,
                         (1.0 - 1.0 / ratio) * 100.0);
        }
        if (rowsCompressed > 0) {
            const qint64 compressedOnDisk = compressedBytes
                                              - (plainBytes - compressedPlain);
            // compressedOnDisk = the on-disk bytes contributed by
            // compressed rows. (plainBytes - compressedPlain) is the
            // bytes contributed by plaintext rows, which equals their
            // on-disk size; subtracting leaves the compressed slice.
            if (compressedOnDisk > 0 && compressedPlain > 0) {
                const double r =
                    double(compressedPlain) / double(compressedOnDisk);
                std::printf("  compressed-only:   %.2fx "
                             "(%s plaintext → %s on disk)\n",
                             r,
                             qUtf8Printable(humanBytes(compressedPlain)),
                             qUtf8Printable(humanBytes(compressedOnDisk)));
            }
        }
        if (failedDecompress > 0) {
            std::printf("  WARN: %d field(s) failed to decompress "
                         "(dict mismatch?); excluded from plaintext "
                         "total.\n",
                         failedDecompress);
        }
        std::printf("\n");

        grandPlain      += plainBytes;
        grandCompressed += compressedBytes;
        grandRows       += rows;
    }

    if (targets.size() > 1 && grandCompressed > 0) {
        const double ratio = double(grandPlain) / double(grandCompressed);
        std::printf("Total across %d account(s): %s rows, "
                     "%s on disk → %s plaintext, ratio %.2fx "
                     "(%.1f%% saved)\n",
                     int(targets.size()),
                     qUtf8Printable(QLocale::system().toString(grandRows)),
                     qUtf8Printable(humanBytes(grandCompressed)),
                     qUtf8Printable(humanBytes(grandPlain)),
                     ratio,
                     (1.0 - 1.0 / ratio) * 100.0);
    }
    return 0;
}

// reset-db — wipe cached message/thread/label data while preserving
// the accounts list (and therefore the QtKeychain tokens keyed by
// account_id). The accounts mirror in QSettings (~/.config/...) is
// the source of truth across wipes; AccountManager rehydrates from
// it automatically the next time the DB is opened empty.
int runResetDb() {
    const QString dbPath = fc::cache::Database::filePath();
    const QStringList paths = {
        dbPath,
        dbPath + QStringLiteral("-wal"),
        dbPath + QStringLiteral("-shm"),
    };
    bool anyExisted = false;
    for (const QString& p : paths) {
        QFile f(p);
        if (!f.exists()) continue;
        anyExisted = true;
        if (!f.remove()) {
            std::printf("reset-db: failed to remove %s: %s\n",
                         qUtf8Printable(p),
                         qUtf8Printable(f.errorString()));
            return 1;
        }
        std::printf("reset-db: removed %s\n", qUtf8Printable(p));
    }
    if (!anyExisted) {
        std::printf("reset-db: nothing to remove (cache.db not present).\n");
    }

    // Initialize a fresh schema and let AccountManager's reload()
    // rehydrate accounts from the QSettings mirror. Constructing the
    // AccountManager is enough — its constructor calls reload(), and
    // reload() handles the rehydration + re-mirror cycle.
    fc::account::AccountManager accounts;
    std::printf("reset-db: done. %d account(s) preserved; tokens in "
                "QtKeychain remain reachable. Next launch re-syncs "
                "from scratch.\n",
                int(accounts.accounts().size()));
    return 0;
}

}  // namespace

bool argsLookLikeCliSubcommand(int argc, char** argv) {
    if (argc < 2) return false;
    const QByteArray a1(argv[1]);
    return a1 == kCmdDbStats
        || a1 == kCmdClearCache
        || a1 == kCmdCompressDb
        || a1 == kCmdCompressionStats
        || a1 == kCmdResetDb
        || a1 == "help" || a1 == "--help" || a1 == "-h";
}

int tryRunCli(int argc, char** argv, const QStringList& args) {
    Q_UNUSED(argc);
    if (args.size() < 2) return -1;
    const QString cmd = args[1];

    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        printHelp();
        return 0;
    }

    if (cmd != kCmdDbStats && cmd != kCmdClearCache
        && cmd != kCmdCompressDb && cmd != kCmdCompressionStats
        && cmd != kCmdResetDb) {
        return -1;
    }

    // reset-db deletes the DB file, so it must run BEFORE
    // Database::initialize() (which would qFatal on a stale
    // schema_version before we get a chance to clear anything).
    if (cmd == kCmdResetDb) return runResetDb();

    // SQLite + schema init. Database::initialize is idempotent and
    // safe to call from the main thread of a QCoreApplication-based
    // process.
    fc::cache::Database::initialize();

    // AccountManager(basic) — no OAuthClient / TokenStore stack, so
    // we don't pull in QtKeychain async startup. accounts() is read
    // straight from sqlite via reload().
    fc::account::AccountManager accounts;

    const QString target = args.size() > 2 ? args[2] : QString();
    if (cmd == kCmdDbStats)          return runDbStats(accounts);
    if (cmd == kCmdClearCache)       return runClearCache(accounts, target);
    if (cmd == kCmdCompressDb)       return runCompressDb(accounts, target);
    if (cmd == kCmdCompressionStats) return runCompressionStats(accounts, target);
    return -1;   // unreachable
}

}  // namespace fc::app
