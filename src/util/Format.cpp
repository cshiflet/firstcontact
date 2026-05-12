#include "util/Format.h"

namespace fc::util {

QString humanBytes(qint64 b) {
    constexpr qint64 KB = 1024;
    constexpr qint64 MB = KB * 1024;
    constexpr qint64 GB = MB * 1024;
    if (b >= GB) return QStringLiteral("%1 GB").arg(b / double(GB), 0, 'f', 2);
    if (b >= MB) return QStringLiteral("%1 MB").arg(b / double(MB), 0, 'f', 1);
    if (b >= KB) return QStringLiteral("%1 KB").arg(b / double(KB), 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b);
}

}  // namespace fc::util
