#pragma once

namespace fc::util {

// Installs a qInstallMessageHandler that writes to a rotating log file
// in <dataDir>/logs/firstcontact.log and mirrors to stderr in debug builds.
// Idempotent.
void installLogger();

}  // namespace fc::util
