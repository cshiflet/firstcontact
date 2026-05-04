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

// Replaces numeric (&#39;, &#x27;) and a small set of named (&amp;, &lt;,
// &nbsp;, …) HTML entities with their literal characters. Idempotent — safe
// to apply to text that has already been decoded. Used to normalize subject
// lines, snippets, and display names that some senders emit pre-encoded
// (most commonly marketing tools that build the entire RFC 5322 body in HTML
// and accidentally encode the headers too).
QString decodeHtmlEntities(const QString& s);

}  // namespace fc::util
