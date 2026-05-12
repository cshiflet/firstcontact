#pragma once

#include <QString>

namespace fc::util {

// Format a byte count as a short human-readable string ("12.3 MB").
// Output format: B for < 1024, KB / MB with one decimal, GB with two.
// Non-localized: keeps the dot as the decimal separator so CLI output
// and parseable logs stay stable.
QString humanBytes(qint64 b);

}  // namespace fc::util
