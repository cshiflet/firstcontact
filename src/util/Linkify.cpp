#include "Linkify.h"

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringView>

#include <algorithm>

namespace fc::util {

namespace {

// Conservative URL matcher: http(s)://… or www.… up to whitespace / closing
// punctuation. Trailing ).,;!? trimmed below.
const QRegularExpression& urlRe() {
    static const QRegularExpression re(
        QStringLiteral(R"((https?://[^\s<>"']+|www\.[^\s<>"']+))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// "label [https://url]" — the canonical shape Gmail's text/plain
// converter (and our HtmlSanitizer fallback) emits when an <a> has
// visible text different from its href. Mirror baremail-terminal's
// labeledRe: label is at least one non-whitespace char, up to ~80
// chars without `[` or newline, then a single space and `[URL]`.
const QRegularExpression& labeledRe() {
    static const QRegularExpression re(
        QStringLiteral(R"((\S[^\[\n]{0,79}?) \[(https?://[^\]\s]+)\])"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Markdown-style "[label](https://url)". Some senders pre-convert
// HTML to markdown for the text/plain alternative — Amazon
// transactional mail is a notable example. Without a dedicated
// pattern the bare-URL pass would only linkify the URL inside the
// parens, leaving "[label](" and ")" as visible text noise.
const QRegularExpression& markdownLinkRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(\[([^\]\n]{1,80}?)\]\((https?://[^\)\s]+)\))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// "label (https://url)" — Gmail's text/plain converter emits this
// shape for `<a href="URL">text</a>` and for `<a><img alt="text">…</a>`
// (using the img's alt as the visible label). Without a dedicated
// pattern the bare-URL pass linkifies just the URL inside the
// parens and leaves "label (" / ")" as text noise around it.
const QRegularExpression& parenLinkRe() {
    static const QRegularExpression re(
        QStringLiteral(R"((\S[^\(\n]{0,79}?) \((https?://[^\)\s]+)\))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Literal HTML anchor — `<a href="...">label</a>`. Some senders dump
// raw HTML into the text/plain alternative (UPS shipment mail is a
// recurring example). Without this pre-pass the linkifier escapes
// the angle-brackets and renders the whole anchor as text noise.
// We detect these BEFORE HTML-escaping, decode entities in the href
// + label, and rewrite to the markdown "[label](url)" shape so the
// existing markdown pass handles the rendering.
const QRegularExpression& htmlAnchorRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(<a\b[^>]*?\bhref\s*=\s*["']([^"']+)["'][^>]*>([\s\S]*?)</a>)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression& mailRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}))"));
    return re;
}

QString trimTrailingPunct(QString s) {
    while (!s.isEmpty()) {
        const QChar c = s.back();
        if (c == ')' || c == ',' || c == '.' || c == ';' ||
            c == '!' || c == '?' || c == ']' || c == '}') {
            s.chop(1);
        } else {
            break;
        }
    }
    return s;
}

// Visibly truncate a URL to roughly `maxLen` characters with an
// ellipsis in the middle, keeping ~2/3 of the budget at the start
// (which is usually the most informative — scheme + host + first
// path segment) and ~1/3 at the end (usually the leaf, which can
// help distinguish e.g. /article/12345 vs /article/67890). The
// click target stays the FULL URL — only the display is shortened.
QString truncateForDisplay(const QString& url, int maxLen) {
    if (url.size() <= maxLen) return url;
    if (maxLen <= 1) return QStringLiteral("…");
    const int avail = maxLen - 1;
    int keepStart = (avail * 2) / 3;
    int keepEnd   = avail - keepStart;
    if (keepEnd < 1) { keepEnd = 1; keepStart = avail - 1; }
    return url.left(keepStart) + QChar(0x2026) + url.right(keepEnd);
}

// Decode HTML numeric and common named entity references inside a plain-text
// body. Handles &#NNN; (decimal), &#xHH; (hex), and a small whitelist of
// named entities that show up regularly in real-world emails. Anything we
// don't recognise is left alone so genuine "&" + "#" + digits sequences in
// user prose survive.
//
// Why we do this in the linkify path: Gmail's text/plain part frequently
// contains entity references that were never decoded when the HTML→text
// conversion ran on the sender side. The user shouldn't have to look at
// "&#847;" in their inbox.
QString decodeEntitiesInline(QString s) {
    static const QRegularExpression numRe(
        QStringLiteral(R"(&#(x?)([0-9A-Fa-f]+);)"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(s.size());
    int idx = 0;
    auto it = numRe.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append(s.mid(idx, m.capturedStart() - idx));
        const bool hex = !m.captured(1).isEmpty();
        bool ok = false;
        const uint cp = m.captured(2).toUInt(&ok, hex ? 16 : 10);
        if (ok && cp != 0 && cp <= 0x10FFFF) {
            out.append(QChar(cp));
        } else {
            out.append(m.captured(0));   // leave malformed alone
        }
        idx = m.capturedEnd();
    }
    out.append(s.mid(idx));
    s = out;

    // Small named-entity table covering >99% of what shows up in plain-text
    // email artefacts. Order matters: &amp; goes LAST so we don't double
    // decode an already-encoded ampersand.
    static const QPair<QLatin1String, QChar> kNamed[] = {
        {QLatin1String("&nbsp;"),   QChar(0x00A0)},
        {QLatin1String("&lt;"),     QChar('<')},
        {QLatin1String("&gt;"),     QChar('>')},
        {QLatin1String("&quot;"),   QChar('"')},
        {QLatin1String("&apos;"),   QChar('\'')},
        {QLatin1String("&copy;"),   QChar(0x00A9)},
        {QLatin1String("&reg;"),    QChar(0x00AE)},
        {QLatin1String("&trade;"),  QChar(0x2122)},
        {QLatin1String("&mdash;"),  QChar(0x2014)},
        {QLatin1String("&ndash;"),  QChar(0x2013)},
        {QLatin1String("&hellip;"), QChar(0x2026)},
        {QLatin1String("&laquo;"),  QChar(0x00AB)},
        {QLatin1String("&raquo;"),  QChar(0x00BB)},
        {QLatin1String("&middot;"), QChar(0x00B7)},
        {QLatin1String("&bull;"),   QChar(0x2022)},
        {QLatin1String("&rsquo;"),  QChar(0x2019)},
        {QLatin1String("&lsquo;"),  QChar(0x2018)},
        {QLatin1String("&rdquo;"),  QChar(0x201D)},
        {QLatin1String("&ldquo;"),  QChar(0x201C)},
    };
    for (const auto& [k, v] : kNamed) s.replace(k, QString(v));
    s.replace(QLatin1String("&amp;"), QStringLiteral("&"));
    return s;
}

// Maximum visible URL length for the bare-URL path. URLs longer
// than this get the start…end truncation. The link target itself
// is always the full URL.
constexpr int kMaxDisplayUrlLen = 60;

}  // namespace

QString linkifyPlainText(const QString& plain, LinkDisplayMode mode) {
    // Pre-pass: some senders dump literal HTML anchors into the
    // text/plain alternative. Convert each `<a href="X">Y</a>` to
    // "[Y](X)" so the markdown pass below renders it as a clean
    // link. Entities inside href / label are decoded here so the
    // subsequent toHtmlEscaped() pass produces correctly-escaped
    // output (a single round of escaping, not double).
    QString preprocessed = plain;
    {
        // When the inner of <a>…</a> is an <img alt="X">, take X as
        // the visible label — that's what the user sees on a graphical
        // client. Otherwise strip any stray tags from the inner so a
        // marketing email with nested <span> wrapping doesn't end up
        // with raw markup as the link label.
        static const QRegularExpression imgAltRe(
            QStringLiteral(R"(<img\b[^>]*?\balt\s*=\s*["']([^"']*)["'][^>]*>)"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression strayTagRe(
            QStringLiteral(R"(<[^>]+>)"));

        QString out;
        out.reserve(preprocessed.size());
        qsizetype idx = 0;
        for (auto it = htmlAnchorRe().globalMatch(preprocessed); it.hasNext(); ) {
            const auto m = it.next();
            out.append(preprocessed.mid(idx, m.capturedStart() - idx));
            QString href = decodeEntitiesInline(m.captured(1).trimmed());
            QString rawInner = m.captured(2);
            // Extract a usable label: prefer <img alt="…">, fall
            // back to the inner with all tags stripped.
            QString label;
            if (auto am = imgAltRe.match(rawInner); am.hasMatch()) {
                label = decodeEntitiesInline(am.captured(1)).trimmed();
            }
            if (label.isEmpty()) {
                label = decodeEntitiesInline(
                    rawInner.replace(strayTagRe, QString())).simplified();
            }
            // Only http(s) anchors are user-meaningful in plain text;
            // the linkify regexes downstream require those schemes
            // anyway. Skip everything else (mailto: still works via
            // the email-address pass; tel: / ftp: / cid: rare in
            // text-as-html, leave them as-is for the bare-URL pass).
            if (!href.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) &&
                !href.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
                out.append(preprocessed.mid(m.capturedStart(),
                                            m.capturedEnd() - m.capturedStart()));
                idx = m.capturedEnd();
                continue;
            }
            if (label.isEmpty()) label = href;
            out.append(QStringLiteral("[%1](%2)").arg(label, href));
            idx = m.capturedEnd();
        }
        out.append(preprocessed.mid(idx));
        preprocessed = std::move(out);
    }

    // Decode entity references BEFORE HTML-escaping so that &#847; etc. turn
    // into the actual Unicode character instead of being shown literally.
    QString escaped = decodeEntitiesInline(preprocessed).toHtmlEscaped();

    // Three-pass linkification, modelled on baremail-terminal with
    // an extra leading pass for markdown-style links:
    //   Pass 0 finds "[label](URL)" markdown patterns. Some senders
    //          pre-convert HTML to markdown for the text/plain part
    //          — without a dedicated pass the bare-URL pass would
    //          linkify only the URL inside the parens and leave the
    //          markdown brackets as visible text noise.
    //   Pass 1 finds "label [URL]" patterns. In LinkDisplayMode::Labeled
    //          (default) the label alone is the visible click target;
    //          in LinkDisplayMode::FullUrl both the label and URL are
    //          visible with the URL part being the click target —
    //          mirrors baremail-terminal's linkModeLabeled vs
    //          linkModeURL toggle.
    //   Pass 2 finds bare URLs that DIDN'T overlap with a labeled
    //          match and renders them as standalone links. In
    //          Labeled mode the visible URL gets a start/ellipsis/end
    //          truncation when long; FullUrl mode shows the URL
    //          verbatim. Click target is always the full URL.
    struct Replacement {
        qsizetype start;
        qsizetype end;
        QString   html;
    };
    QList<Replacement> reps;

    auto overlapsExisting = [&reps](qsizetype start, qsizetype end) {
        for (const auto& r : reps) {
            if (start < r.end && end > r.start) return true;
        }
        return false;
    };

    // Pass 0: markdown-style "[label](URL)".
    for (auto it = markdownLinkRe().globalMatch(escaped); it.hasNext(); ) {
        const auto m = it.next();
        const QString label = m.captured(1).trimmed();
        if (label.isEmpty()) continue;
        if (label.contains(QStringLiteral("://"))) continue;
        const QString url = m.captured(2);
        const QString html = (mode == LinkDisplayMode::FullUrl)
            ? QStringLiteral("[%1](<a href=\"%2\" title=\"%2\">%2</a>)").arg(label, url)
            : QStringLiteral("<a href=\"%1\" title=\"%1\">%2</a>").arg(url, label);
        reps.append({m.capturedStart(), m.capturedEnd(), html});
    }

    // Pass 1: labeled "label [URL]".
    for (auto it = labeledRe().globalMatch(escaped); it.hasNext(); ) {
        const auto m = it.next();
        if (overlapsExisting(m.capturedStart(), m.capturedEnd())) continue;
        const QString label = m.captured(1).trimmed();
        if (label.isEmpty()) continue;
        // Discard if the "label" itself looks like a URL — that's the
        // shape "https://a https://b" produces and we want to leave
        // each URL standalone for the bare-URL pass.
        if (label.contains(QStringLiteral("://"))) continue;
        const QString url = m.captured(2);
        const QString html = (mode == LinkDisplayMode::FullUrl)
            ? QStringLiteral("%1 [<a href=\"%2\" title=\"%2\">%2</a>]").arg(label, url)
            : QStringLiteral("<a href=\"%1\" title=\"%1\">%2</a>").arg(url, label);
        reps.append({m.capturedStart(), m.capturedEnd(), html});
    }

    // Pass 1.5: parenthesized "label (URL)" — Gmail's text/plain
    // converter emits this shape for `<a href>text</a>` and for the
    // <a><img alt> case (alt becomes the visible label). Same render
    // logic as the labeled pass, but ONLY when the label sits at
    // the start of a line. Mid-prose parenthesized URLs ("I went to
    // (https://x.com)") would otherwise greedily attach the
    // preceding sentence as the "label" and turn casual writing into
    // an unreadable mass of hyperlinks. Visible-button text in
    // marketing emails always lands at column 0 (or right after a
    // newline) because that's where Gmail's converter puts it.
    for (auto it = parenLinkRe().globalMatch(escaped); it.hasNext(); ) {
        const auto m = it.next();
        if (overlapsExisting(m.capturedStart(), m.capturedEnd())) continue;
        const QString label = m.captured(1).trimmed();
        if (label.isEmpty()) continue;
        if (label.contains(QStringLiteral("://"))) continue;
        const auto labelStart = m.capturedStart();
        const bool lineStart = labelStart == 0
            || escaped[labelStart - 1] == QChar('\n');
        if (!lineStart) continue;
        const QString url = m.captured(2);
        const QString html = (mode == LinkDisplayMode::FullUrl)
            ? QStringLiteral("%1 (<a href=\"%2\" title=\"%2\">%2</a>)").arg(label, url)
            : QStringLiteral("<a href=\"%1\" title=\"%1\">%2</a>").arg(url, label);
        reps.append({m.capturedStart(), m.capturedEnd(), html});
    }

    // Pass 2: bare URLs not overlapping a prior match.
    for (auto it = urlRe().globalMatch(escaped); it.hasNext(); ) {
        const auto m = it.next();
        if (overlapsExisting(m.capturedStart(), m.capturedEnd())) continue;

        QString url = trimTrailingPunct(m.captured(0));
        if (url.isEmpty()) continue;
        QString href = url;
        if (href.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) {
            href.prepend(QStringLiteral("https://"));
        }
        const QString display = (mode == LinkDisplayMode::Labeled
                                  && url.size() > kMaxDisplayUrlLen)
            ? truncateForDisplay(url, kMaxDisplayUrlLen)
            : url;
        reps.append({
            m.capturedStart(),
            m.capturedStart() + url.size(),
            QStringLiteral("<a href=\"%1\" title=\"%1\">%2</a>")
                .arg(href, display),
        });
    }

    std::sort(reps.begin(), reps.end(),
        [](const Replacement& a, const Replacement& b) {
            return a.start < b.start;
        });

    QString out;
    out.reserve(escaped.size() + 128);
    qsizetype idx = 0;
    for (const auto& r : reps) {
        out.append(escaped.mid(idx, r.start - idx));
        out.append(r.html);
        idx = r.end;
    }
    out.append(escaped.mid(idx));

    // Pass 3: bare emails. Operate on the result so we don't double-wrap URLs.
    QString result;
    result.reserve(out.size() + 64);
    qsizetype mIdx = 0;
    auto mit = mailRe().globalMatch(out);
    while (mit.hasNext()) {
        const auto m = mit.next();
        // Skip matches inside an existing href (cheap heuristic).
        const qsizetype contextStart = qMax(qsizetype{0}, m.capturedStart() - 16);
        if (QStringView{out}.mid(contextStart, m.capturedStart() - contextStart)
                .contains(QStringLiteral("href="))) {
            continue;
        }
        result.append(out.mid(mIdx, m.capturedStart() - mIdx));
        result.append(QStringLiteral("<a href=\"mailto:%1\">%1</a>").arg(m.captured(0)));
        mIdx = m.capturedEnd();
    }
    result.append(out.mid(mIdx));

    result.replace(QChar('\n'), QStringLiteral("<br>"));
    return result;
}

}  // namespace fc::util
