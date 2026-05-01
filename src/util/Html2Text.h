#pragma once

#include <QString>

namespace fc::util {

// Strip HTML markup down to a plain-text approximation, preserving paragraph
// boundaries and basic list structure. Used to:
//   - generate the snippet column when Gmail's snippet is missing,
//   - feed the FTS5 body column from HTML-only messages,
//   - render an HTML message body when the user has not opted into rich view.
//
// This is intentionally lossy. For rich rendering use HtmlSanitizer (rich tier)
// or HtmlRenderHost (webview tier).
QString html2text(const QString& html);

}  // namespace fc::util
