#pragma once

#include <QString>

namespace fc::util {

// Wraps URLs and bare email addresses in <a href="…"> tags. Input is treated
// as plain text and HTML-escaped first; the result is safe to feed to a
// QTextBrowser/QTextDocument that has setOpenExternalLinks(true).
QString linkifyPlainText(const QString& plain);

}  // namespace fc::util
