#pragma once

#include <QString>

namespace fc::util {

// Controls how labeled "label [https://url]" patterns and bare URLs
// render in the linkified output.
//
//   Labeled — the default. Labeled patterns show only the label (URL
//             goes into title= for hover). Bare URLs show the URL
//             with a start/ellipsis/end truncation when long.
//   FullUrl — both label AND URL are visible for labeled patterns
//             ("label [URL]" with the URL part the click target).
//             Bare URLs render verbatim, no truncation.
//
// Mirrors baremail-terminal's linkModeLabeled / linkModeURL toggle.
enum class LinkDisplayMode { Labeled, FullUrl };

// Wraps URLs and bare email addresses in <a href="…"> tags. Input is treated
// as plain text and HTML-escaped first; the result is safe to feed to a
// QTextBrowser/QTextDocument that has setOpenExternalLinks(true).
QString linkifyPlainText(const QString& plain,
                          LinkDisplayMode mode = LinkDisplayMode::Labeled);

}  // namespace fc::util
