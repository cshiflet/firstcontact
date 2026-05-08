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

QString linkifyPlainText(const QString& plain) {
    // Decode entity references BEFORE HTML-escaping so that &#847; etc. turn
    // into the actual Unicode character instead of being shown literally.
    QString escaped = decodeEntitiesInline(plain).toHtmlEscaped();

    // Two-pass linkification, modelled on baremail-terminal:
    //   Pass 1 finds "label [URL]" patterns and renders them with the
    //          label as the visible click target — the URL goes into
    //          title= (so a hover or long-press surfaces it) but stays
    //          out of the visible text. Most marketing / transactional
    //          emails route through Gmail's HTML→text converter and
    //          come out in this shape, so the user sees clean prose
    //          instead of bracketed URL noise.
    //   Pass 2 finds bare URLs that DIDN'T overlap with a labeled
    //          match and renders them as standalone links. URLs
    //          longer than kMaxDisplayUrlLen are visibly truncated
    //          to keep the message readable; the click target stays
    //          the full URL.
    struct Replacement {
        qsizetype start;
        qsizetype end;
        QString   html;
    };
    QList<Replacement> reps;

    // Pass 1: labeled.
    for (auto it = labeledRe().globalMatch(escaped); it.hasNext(); ) {
        const auto m = it.next();
        const QString label = m.captured(1).trimmed();
        if (label.isEmpty()) continue;
        // Discard if the "label" itself looks like a URL — that's the
        // shape "https://a https://b" produces and we want to leave
        // each URL standalone for Pass 2.
        if (label.contains(QStringLiteral("://"))) continue;
        const QString url = m.captured(2);
        reps.append({
            m.capturedStart(),
            m.capturedEnd(),
            QStringLiteral("<a href=\"%1\" title=\"%1\">%2</a>")
                .arg(url, label),
        });
    }

    // Pass 2: bare URLs not overlapping a Pass 1 match.
    for (auto it = urlRe().globalMatch(escaped); it.hasNext(); ) {
        const auto m = it.next();
        bool overlap = false;
        for (const auto& r : reps) {
            if (m.capturedStart() < r.end && m.capturedEnd() > r.start) {
                overlap = true; break;
            }
        }
        if (overlap) continue;

        QString url = trimTrailingPunct(m.captured(0));
        if (url.isEmpty()) continue;
        QString href = url;
        if (href.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) {
            href.prepend(QStringLiteral("https://"));
        }
        const QString display = url.size() > kMaxDisplayUrlLen
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
    QString final;
    final.reserve(out.size() + 64);
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
        final.append(out.mid(mIdx, m.capturedStart() - mIdx));
        final.append(QStringLiteral("<a href=\"mailto:%1\">%1</a>").arg(m.captured(0)));
        mIdx = m.capturedEnd();
    }
    final.append(out.mid(mIdx));

    final.replace(QChar('\n'), QStringLiteral("<br>"));
    return final;
}

}  // namespace fc::util
