#include "Cli.h"

#include "account/AccountManager.h"
#include "cache/BodyCompressionWorker.h"
#include "cache/Database.h"
#include "cache/MessageRepository.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QLocale>
#include <QPointer>

#include <cstdio>

namespace fc::app {

namespace {

constexpr const char* kCmdDbStats     = "db-stats";
constexpr const char* kCmdClearCache  = "clear-cache";
constexpr const char* kCmdCompressDb  = "compress-db";

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
        "  help | --help | -h\n"
        "      Print this help text.\n");
}

QString humanBytes(qint64 b) {
    if (b < 1024) return QStringLiteral("%1 B").arg(b);
    double v = b / 1024.0;
    if (v < 1024.0) return QStringLiteral("%1 KB").arg(v, 0, 'f', 1);
    v /= 1024.0;
    if (v < 1024.0) return QStringLiteral("%1 MB").arg(v, 0, 'f', 1);
    v /= 1024.0;
    return QStringLiteral("%1 GB").arg(v, 0, 'f', 2);
}

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

}  // namespace

bool argsLookLikeCliSubcommand(int argc, char** argv) {
    if (argc < 2) return false;
    const QByteArray a1(argv[1]);
    return a1 == kCmdDbStats
        || a1 == kCmdClearCache
        || a1 == kCmdCompressDb
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

    if (cmd != kCmdDbStats && cmd != kCmdClearCache && cmd != kCmdCompressDb) {
        return -1;
    }

    // SQLite + migrations. Database::initialize is idempotent and
    // safe to call from the main thread of a QCoreApplication-based
    // process.
    fc::cache::Database::initialize();

    // AccountManager(basic) — no OAuthClient / TokenStore stack, so
    // we don't pull in QtKeychain async startup. accounts() is read
    // straight from sqlite via reload().
    fc::account::AccountManager accounts;

    const QString target = args.size() > 2 ? args[2] : QString();
    if (cmd == kCmdDbStats)    return runDbStats(accounts);
    if (cmd == kCmdClearCache) return runClearCache(accounts, target);
    if (cmd == kCmdCompressDb) return runCompressDb(accounts, target);
    return -1;   // unreachable
}

}  // namespace fc::app
