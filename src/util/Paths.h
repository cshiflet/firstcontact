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

}  // namespace fc::util
