#include "ImageProxy.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace fc::util {

namespace {

// Convert one source URL to the proxy-rewritten form. Returns the
// original on cases we deliberately pass through (data:, cid:,
// relative, already-proxied).
QString rewriteOne(const QString& src, const QString& proxyPattern,
                   const QString& proxyHost) {
    const QString trimmed = src.trimmed();
    if (trimmed.isEmpty())                         return src;
    if (trimmed.startsWith(QLatin1String("data:"), Qt::CaseInsensitive))
        return src;
    if (trimmed.startsWith(QLatin1String("cid:"), Qt::CaseInsensitive))
        return src;
    const bool isAbsHttp =
        trimmed.startsWith(QLatin1String("http://"),  Qt::CaseInsensitive)
        || trimmed.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
    if (!isAbsHttp) return src;       // relative — nothing to proxy
    // Already routed through the proxy host — leave alone.
    if (!proxyHost.isEmpty()
        && trimmed.contains(proxyHost, Qt::CaseInsensitive)) {
        return src;
    }
    const QString encoded = QString::fromLatin1(
        QUrl::toPercentEncoding(trimmed));
    QString out = proxyPattern;
    out.replace(QStringLiteral("{url}"), encoded);
    return out;
}

// srcset is a comma-separated list of "URL [descriptor]" entries. Walk
// each, rewrite the URL portion only, leave descriptors (1x / 480w / etc)
// intact. Whitespace handling tries to mirror what browsers parse.
QString rewriteSrcset(const QString& value, const QString& proxyPattern,
                      const QString& proxyHost) {
    QStringList parts = value.split(QLatin1Char(','));
    for (auto& p : parts) {
        // Trim BEFORE locating the URL/descriptor split so leading
        // whitespace from the comma-separated layout doesn't get
        // mistaken for the URL→descriptor boundary.
        const QString trimmed = p.trimmed();
        const int firstWs = trimmed.indexOf(
            QRegularExpression(QStringLiteral("\\s")));
        QString url        = (firstWs < 0)
            ? trimmed : trimmed.left(firstWs);
        QString descriptor = (firstWs < 0)
            ? QString() : trimmed.mid(firstWs).trimmed();
        const QString proxied = rewriteOne(url, proxyPattern, proxyHost);
        // Preserve a leading space so the joined ", " separator keeps
        // browser-tolerated shape.
        QString rebuilt = QStringLiteral(" ") + proxied;
        if (!descriptor.isEmpty()) rebuilt += QLatin1Char(' ') + descriptor;
        p = rebuilt;
    }
    return parts.join(QLatin1Char(',')).trimmed();
}

// Best-effort tracking-pixel detector. Catches the common shapes:
//   width="1" height="1"        (HTML attributes)
//   style="width:1px;height:1px" (inline CSS)
// Anything wider than 2 px in either dimension is left alone.
bool looksLikeTrackingPixel(const QString& imgTagInner) {
    static const QRegularExpression attrRe(
        QStringLiteral(
            "\\b(width|height)\\s*=\\s*['\"]?\\s*(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression styleRe(
        QStringLiteral(
            "\\b(width|height)\\s*:\\s*(\\d+)\\s*px"),
        QRegularExpression::CaseInsensitiveOption);
    int w = -1, h = -1;
    {
        auto it = attrRe.globalMatch(imgTagInner);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString axis = m.captured(1).toLower();
            const int v = m.captured(2).toInt();
            if (axis == QLatin1String("width"))  w = v;
            if (axis == QLatin1String("height")) h = v;
        }
    }
    {
        auto it = styleRe.globalMatch(imgTagInner);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString axis = m.captured(1).toLower();
            const int v = m.captured(2).toInt();
            if (axis == QLatin1String("width")  && (w < 0 || v < w)) w = v;
            if (axis == QLatin1String("height") && (h < 0 || v < h)) h = v;
        }
    }
    // Either dimension explicitly tiny → tracking pixel.
    if (w >= 0 && w <= 2) return true;
    if (h >= 0 && h <= 2) return true;
    return false;
}

}  // namespace

QString rewriteImagesForBrowser(const QString& html,
                                 const QString& proxyPattern,
                                 bool stripPixels) {
    if (html.isEmpty()) return html;
    const bool haveProxy = proxyPattern.contains(QStringLiteral("{url}"));

    // Extract the proxy host once so rewriteOne can detect already-
    // rewritten URLs and skip them. If parsing fails, host comes back
    // empty and we just don't dedupe — re-rewriting an already-proxied
    // URL works (it gets re-percent-encoded), it's just slightly wasteful.
    QString proxyHost;
    if (haveProxy) {
        QString sample = proxyPattern;
        sample.replace(QStringLiteral("{url}"), QStringLiteral("about:blank"));
        const QUrl u(sample);
        if (u.isValid()) proxyHost = u.host();
    }

    // Match <img …>. We capture the whole tag so we can decide whether
    // to drop it (tracking pixel) or rewrite its attributes. DOTALL so
    // multi-line tags match.
    static const QRegularExpression imgRe(
        QStringLiteral("<img\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);

    static const QRegularExpression srcRe(
        QStringLiteral("\\bsrc\\s*=\\s*(['\"])([^'\"]*)\\1"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression srcsetRe(
        QStringLiteral("\\bsrcset\\s*=\\s*(['\"])([^'\"]*)\\1"),
        QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(html.size() + 256);
    int idx = 0;
    auto it = imgRe.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append(html.mid(idx, m.capturedStart() - idx));
        QString tag = m.captured();

        if (stripPixels && looksLikeTrackingPixel(tag)) {
            // Replace the entire <img …> tag with nothing.
            idx = m.capturedEnd();
            continue;
        }

        if (haveProxy) {
            const auto srcMatch = srcRe.match(tag);
            if (srcMatch.hasMatch()) {
                const QString quote = srcMatch.captured(1);
                const QString origSrc = srcMatch.captured(2);
                const QString proxied = rewriteOne(origSrc, proxyPattern,
                                                    proxyHost);
                if (proxied != origSrc) {
                    tag.replace(srcMatch.capturedStart(),
                                srcMatch.capturedLength(),
                                QStringLiteral("src=") + quote
                                + proxied + quote);
                }
            }
            const auto srcsetMatch = srcsetRe.match(tag);
            if (srcsetMatch.hasMatch()) {
                const QString quote = srcsetMatch.captured(1);
                const QString origSrcset = srcsetMatch.captured(2);
                const QString proxied = rewriteSrcset(origSrcset,
                                                       proxyPattern,
                                                       proxyHost);
                if (proxied != origSrcset) {
                    tag.replace(srcsetMatch.capturedStart(),
                                srcsetMatch.capturedLength(),
                                QStringLiteral("srcset=") + quote
                                + proxied + quote);
                }
            }
        }

        out.append(tag);
        idx = m.capturedEnd();
    }
    out.append(html.mid(idx));
    return out;
}

}  // namespace fc::util
