#include "DryRun.h"

#include <QByteArray>
#include <QLoggingCategory>

namespace fc::util {

namespace {

bool computeEnabled() {
    const QByteArray raw = qgetenv("FC_DRY_RUN").trimmed().toLower();
    if (raw.isEmpty()) return false;
    return raw == "1" || raw == "true" || raw == "yes" || raw == "on";
}

}  // namespace

bool DryRun::enabled() {
    // First call wins: cache the env-var lookup so subsequent gates are
    // a single load. The env var is intentionally not re-checked at
    // runtime — flipping debug behaviour mid-session would make it
    // impossible to reason about which operations actually hit the
    // server.
    static const bool cached = computeEnabled();
    return cached;
}

bool DryRun::block(const QString& op) {
    if (!enabled()) return false;
    qInfo("DryRun: blocked '%s' (FC_DRY_RUN is set)",
          qUtf8Printable(op));
    return true;
}

}  // namespace fc::util
