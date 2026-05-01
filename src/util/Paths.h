#pragma once

#include <QString>

namespace fc::util {

// Returns the per-user data directory; created on first access.
// Linux:   $XDG_DATA_HOME/firstcontact
// Windows: %LOCALAPPDATA%\FirstContact
// macOS:   ~/Library/Application Support/FirstContact
QString dataDir();

// Returns the per-user config directory; created on first access.
QString configDir();

// Per-user log directory.
QString logDir();

// Cache database path: <dataDir>/cache.db
QString cacheDbPath();

// Path used for the QLockFile single-instance guard.
QString singleInstanceLockPath();

// chmod 0600 on POSIX; no-op elsewhere. Best-effort; logs a warning on failure.
// Apply to cache.db, log files, and any other on-disk state holding mail
// content, search indexes, or auth metadata.
void restrictPermissionsToOwner(const QString& path);

}  // namespace fc::util
