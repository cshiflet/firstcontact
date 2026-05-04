#pragma once

#include <QString>

namespace fc::util {

// Read-only kill switch for destructive operations. When enabled, every
// gated handler short-circuits before touching the local cache or any
// server endpoint.
//
// Enabled via the FC_DRY_RUN environment variable, read once at first
// query and cached for the lifetime of the process. Truthy values are
// "1", "true", "yes", "on" (case-insensitive); anything else (including
// unset) leaves dry-run off.
//
// Gated operations:
//   - move-to-trash (delete message)
//   - archive (remove INBOX)
//   - toggle star (label add/remove)
//   - apply label diff via right-click / context menu
//   - rename / delete a label
//   - pending-ops worker flush against Gmail (catches anything that was
//     enqueued before the flag was raised — clearing the queue requires
//     turning dry-run off again)
//
// Non-destructive surfaces are intentionally left running (sign-in / out,
// sync, send mail, draft sync, attachment download, label create) so the
// app stays usable for read-only browsing while changes are blocked.
class DryRun {
public:
    static bool enabled();

    // Convenience for handler bodies:
    //   if (DryRun::block("delete-message")) return;
    // Returns true and logs the op when dry-run is on; returns false
    // otherwise.
    static bool block(const QString& op);
};

}  // namespace fc::util
