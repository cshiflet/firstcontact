#pragma once

#include <QStringList>

namespace fc::app {

// Command-line subcommands that don't launch the GUI:
//
//   firstcontact db-stats
//       Print per-account cache footprint to stdout (size, message
//       count, label count, etc.). Read-only — safe to run while
//       the GUI is open.
//
//   firstcontact clear-cache [email-or-account-id]
//       Drop per-account cache rows (messages, threads, labels,
//       attachments, drafts, outbox, pending_ops, account_meta).
//       Leaves the accounts row + QtKeychain auth tokens intact, so
//       the next launch just resyncs from scratch. No argument =
//       every signed-in account.
//
//   firstcontact compress-db [email-or-account-id]
//       Train a fresh zstd dictionary from a random sample of
//       bodies, then recompress every cached body row with the new
//       dictionary. Single-threaded, memory-frugal, slow — same
//       worker the Settings "Recompress now…" button drives. No
//       argument = every signed-in account, sequentially.
//
//   firstcontact help | --help | -h
//       Print the help text + exit 0.
//
// Returns the CLI exit code (0 = success) when a CLI subcommand
// was recognized and executed, or a negative sentinel when no CLI
// subcommand is present and the caller should fall through to the
// GUI launch.
int tryRunCli(int argc, char** argv, const QStringList& args);

// True if argv[1] is one of the recognized CLI subcommand tokens.
// Lets main.cpp decide between QCoreApplication (CLI) and QApplication
// (GUI) before constructing either.
bool argsLookLikeCliSubcommand(int argc, char** argv);

}  // namespace fc::app
