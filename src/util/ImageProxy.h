#pragma once

#include <QString>

namespace fc::util {

// Rewrites <img src="http(s)://X"> (and srcset variants) so the URLs
// route through the configured proxy pattern. {url} in the pattern is
// replaced with the percent-encoded original source. When stripPixels is
// true, tiny <img> tags (likely tracking pixels — width/height ≤ 2 px in
// either an attribute or inline style) are removed entirely.
//
// Pass-through cases (left untouched):
//   - data: URLs (already inline)
//   - cid: URLs (Gmail attachment refs we already inline as data: at
//     parse time; if any slipped through, the strict CSP will block
//     them anyway)
//   - relative URLs (no marketer to proxy)
//
// proxyPattern == "" disables rewriting (images then load directly from
// the marketer's CDN — the caller is expected to widen CSP accordingly).
//
// Idempotent on already-proxied HTML: a URL that's already prefixed by
// the proxy host is detected and left alone.
QString rewriteImagesForBrowser(const QString& html,
                                 const QString& proxyPattern,
                                 bool stripPixels);

}  // namespace fc::util
