#include "HtmlSanitizer.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace fc::util {

namespace {

// Tags that we keep structurally, but flatten to `<div>` on output.
// Marketing emails almost universally use tables for layout (single-
// column, no border, deeply nested). QTextBrowser's table layout
// engine is roughly O(n²) in nesting depth — a 6-deep nested-table
// email pegs the UI thread for several seconds during the post-
// setHtml layout pass, even though setHtml itself returns in <5ms.
// Rewriting to <div> preserves block flow and content order while
// avoiding the table-layout cost; the cost is losing visual columns
// for the rare data table, which the user can recover by clicking
// "Open in browser" for the original HTML.
const QSet<QString>& flattenToDiv() {
    static const QSet<QString> s{
        QStringLiteral("table"), QStringLiteral("thead"),
        QStringLiteral("tbody"), QStringLiteral("tfoot"),
        QStringLiteral("tr"),    QStringLiteral("td"),
        QStringLiteral("th"),    QStringLiteral("caption"),
        QStringLiteral("colgroup"),
    };
    return s;
}

const QSet<QString>& tagAllow() {
    static const QSet<QString> s{
        QStringLiteral("html"), QStringLiteral("body"), QStringLiteral("head"),
        QStringLiteral("title"), QStringLiteral("p"), QStringLiteral("br"),
        QStringLiteral("hr"), QStringLiteral("div"), QStringLiteral("span"),
        QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("strong"),
        QStringLiteral("i"), QStringLiteral("em"), QStringLiteral("u"),
        QStringLiteral("s"), QStringLiteral("strike"), QStringLiteral("sub"),
        QStringLiteral("sup"), QStringLiteral("code"), QStringLiteral("pre"),
        QStringLiteral("blockquote"),
        QStringLiteral("ul"), QStringLiteral("ol"), QStringLiteral("li"),
        QStringLiteral("dl"), QStringLiteral("dt"), QStringLiteral("dd"),
        QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"),
        QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("h6"),
        QStringLiteral("table"), QStringLiteral("thead"), QStringLiteral("tbody"),
        QStringLiteral("tfoot"), QStringLiteral("tr"), QStringLiteral("td"),
        QStringLiteral("th"), QStringLiteral("caption"), QStringLiteral("colgroup"),
        QStringLiteral("col"),
        QStringLiteral("img"),
        QStringLiteral("font"), QStringLiteral("center"),
    };
    return s;
}

// Tags whose entire subtree (incl. content) must be removed.
const QSet<QString>& tagDrop() {
    static const QSet<QString> s{
        QStringLiteral("script"), QStringLiteral("style"),
        QStringLiteral("iframe"), QStringLiteral("frame"),
        QStringLiteral("frameset"), QStringLiteral("object"),
        QStringLiteral("embed"),   QStringLiteral("applet"),
        QStringLiteral("form"),    QStringLiteral("input"),
        QStringLiteral("button"),  QStringLiteral("textarea"),
        QStringLiteral("select"),  QStringLiteral("option"),
        QStringLiteral("link"),    QStringLiteral("meta"),
        QStringLiteral("base"),    QStringLiteral("svg"),
        QStringLiteral("math"),    QStringLiteral("noscript"),
    };
    return s;
}

const QSet<QString>& voidTags() {
    // HTML5 spec void elements — never have a closing tag, never wrap
    // content. The list MUST include every void tag that may appear in
    // tagDrop (meta, link, base, input, embed) — otherwise the drop-
    // subtree code at line ~246 sets dropDepth=1 on the void open and
    // waits forever for a closing tag that never comes, suppressing
    // every subsequent tag. That bug rendered marketing emails blank:
    // their <head> contains <meta charset="utf-8">, the dropDepth never
    // reset, the <body> and everything inside it got eaten silently.
    static const QSet<QString> s{
        QStringLiteral("area"),  QStringLiteral("base"),
        QStringLiteral("br"),    QStringLiteral("col"),
        QStringLiteral("embed"), QStringLiteral("hr"),
        QStringLiteral("img"),   QStringLiteral("input"),
        QStringLiteral("link"),  QStringLiteral("meta"),
        QStringLiteral("source"),QStringLiteral("track"),
        QStringLiteral("wbr"),
    };
    return s;
}

const QSet<QString>& attrAllow() {
    static const QSet<QString> s{
        QStringLiteral("href"), QStringLiteral("src"),
        QStringLiteral("alt"),  QStringLiteral("title"),
        QStringLiteral("width"),QStringLiteral("height"),
        QStringLiteral("colspan"), QStringLiteral("rowspan"),
        QStringLiteral("align"), QStringLiteral("valign"),
        QStringLiteral("border"), QStringLiteral("cellspacing"),
        QStringLiteral("cellpadding"),
    };
    return s;
}

bool isSafeUrl(QString url) {
    url = url.trimmed();
    if (url.isEmpty()) return true;
    static const QRegularExpression dangerous(
        QStringLiteral("^\\s*(javascript|vbscript|file|data|about):"),
        QRegularExpression::CaseInsensitiveOption);
    return !dangerous.match(url).hasMatch();
}

bool isRemoteHttpUrl(const QString& url) {
    return url.startsWith(QLatin1String("http://"),  Qt::CaseInsensitive) ||
           url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

QString attrEscape(const QString& s) {
    QString out = s;
    out.replace(QChar('&'),  QStringLiteral("&amp;"));
    out.replace(QChar('"'),  QStringLiteral("&quot;"));
    out.replace(QChar('<'),  QStringLiteral("&lt;"));
    out.replace(QChar('>'),  QStringLiteral("&gt;"));
    return out;
}

// Parse the inside of a tag (after `<` and before `>`) into name + attrs.
struct ParsedTag {
    bool       isClose = false;
    bool       selfClose = false;
    QString    name;
    QList<QPair<QString, QString>> attrs;
};

ParsedTag parseTag(QStringView s) {
    ParsedTag t;
    int i = 0;
    if (i < s.size() && s[i] == QLatin1Char('/')) { t.isClose = true; ++i; }
    while (i < s.size() && (s[i].isLetter() || s[i].isDigit())) {
        t.name.append(s[i].toLower());
        ++i;
    }

    auto skipWs = [&] {
        while (i < s.size() && s[i].isSpace()) ++i;
    };

    while (true) {
        skipWs();
        if (i >= s.size()) break;
        if (s[i] == QLatin1Char('/')) { t.selfClose = true; ++i; continue; }

        QString aname;
        while (i < s.size() && !s[i].isSpace() && s[i] != QLatin1Char('=') &&
               s[i] != QLatin1Char('>') && s[i] != QLatin1Char('/')) {
            aname.append(s[i].toLower());
            ++i;
        }
        if (aname.isEmpty()) break;

        QString avalue;
        skipWs();
        if (i < s.size() && s[i] == QLatin1Char('=')) {
            ++i;
            skipWs();
            if (i < s.size() && (s[i] == QLatin1Char('"') || s[i] == QLatin1Char('\''))) {
                const QChar quote = s[i];
                ++i;
                while (i < s.size() && s[i] != quote) { avalue.append(s[i]); ++i; }
                if (i < s.size()) ++i;
            } else {
                while (i < s.size() && !s[i].isSpace() &&
                       s[i] != QLatin1Char('>') && s[i] != QLatin1Char('/')) {
                    avalue.append(s[i]);
                    ++i;
                }
            }
        }
        t.attrs.append({aname, avalue});
    }
    return t;
}

}  // namespace

SanitizeResult sanitizeHtml(const QString& dirty, const SanitizeOptions& opts) {
    SanitizeResult res;
    if (dirty.isEmpty()) return res;

    QString out;
    out.reserve(dirty.size());

    int dropDepth = 0;       // we're inside the subtree of a dropped tag
    QString dropName;        // tag whose close ends drop mode

    int i = 0;
    while (i < dirty.size()) {
        // Strip HTML comments.
        if (dirty.mid(i, 4) == QLatin1String("<!--")) {
            const int end = dirty.indexOf(QStringLiteral("-->"), i + 4);
            i = (end < 0) ? dirty.size() : end + 3;
            continue;
        }

        // Strip <!DOCTYPE …> / <?xml … ?>
        if (dirty[i] == QLatin1Char('<') && i + 1 < dirty.size() &&
            (dirty[i + 1] == QLatin1Char('!') || dirty[i + 1] == QLatin1Char('?'))) {
            const int end = dirty.indexOf(QChar('>'), i);
            i = (end < 0) ? dirty.size() : end + 1;
            continue;
        }

        if (dirty[i] != QLatin1Char('<')) {
            if (dropDepth == 0) {
                out.append(dirty[i]);
            }
            ++i;
            continue;
        }

        // Tag. Scan to the matching `>`, but skip over any `>` that
        // sits inside a quoted attribute value — naive indexOf('>')
        // would terminate parsing early on inputs like
        //   <a title="a>b" href="javascript:…">
        // and leak the tail back into the output as escaped text.
        // The result was rendering noise (script never executed —
        // tail still got escaped) but ugly. Walk the input looking
        // for the close while toggling an `inQuote` flag on
        // unescaped `'`/`"`.
        //
        // Cap the scan at kMaxTagSpan bytes so a malformed input with
        // an unclosed quote (e.g. `<a title="...`) can't swallow the
        // rest of the document into one phantom tag and discard
        // everything after it. Real HTML attributes are well under
        // 64KiB; on overflow we treat the `<` as a literal text char
        // and resume sanitising at i+1.
        constexpr int kMaxTagSpan = 64 * 1024;
        const int scanLimit = qMin<int>(dirty.size(), i + 1 + kMaxTagSpan);
        int gt = -1;
        QChar quote;
        for (int j = i + 1; j < scanLimit; ++j) {
            const QChar c = dirty[j];
            if (!quote.isNull()) {
                if (c == quote) quote = QChar();
                continue;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                quote = c;
                continue;
            }
            if (c == QLatin1Char('>')) { gt = j; break; }
        }
        if (gt < 0) {
            // Unterminated `<` (or the scan ran off the cap). Treat
            // as literal text (escaped) so the document tail still
            // gets sanitised correctly.
            if (dropDepth == 0) out.append(QStringLiteral("&lt;"));
            ++i;
            continue;
        }
        const QStringView inner = QStringView{dirty}.mid(i + 1, gt - i - 1);
        const ParsedTag t = parseTag(inner);
        i = gt + 1;
        if (t.name.isEmpty()) continue;

        if (dropDepth > 0) {
            if (t.isClose && t.name == dropName) {
                --dropDepth;
                if (dropDepth == 0) dropName.clear();
            } else if (!t.isClose && t.name == dropName) {
                ++dropDepth;
            }
            continue;
        }

        if (tagDrop().contains(t.name)) {
            // Void elements (meta, link, base, input, embed) never have
            // a closing tag, so a drop-subtree increment would never
            // unwind. Treat them as a one-tag drop and move on.
            if (!t.isClose && !t.selfClose && !voidTags().contains(t.name)) {
                dropDepth = 1;
                dropName = t.name;
            }
            continue;
        }

        if (!tagAllow().contains(t.name)) {
            // Unknown tag — drop the tag itself but keep nested text.
            continue;
        }

        if (t.isClose) {
            const QString outName = flattenToDiv().contains(t.name)
                ? QStringLiteral("div") : t.name;
            out.append(QStringLiteral("</")).append(outName).append(QChar('>'));
            continue;
        }

        // Pre-scan for blocked remote <img>: when the src would be
        // dropped (remote and !allowRemoteImages), skip the entire
        // tag rather than emit a srcless <img width="640" height="200">.
        // QTextBrowser renders that as an UPSCALED broken-image
        // placeholder filling the box — uglier than no element at all,
        // and we already show a "Remote images blocked." banner above
        // the body so the user still knows there's hidden content.
        bool skipBlockedImg = false;
        if (t.name == QLatin1String("img") && !opts.allowRemoteImages) {
            for (const auto& [aname, avalue] : t.attrs) {
                if (aname == QLatin1String("src") && isRemoteHttpUrl(avalue)) {
                    res.remoteImagesBlocked = true;
                    skipBlockedImg = true;
                    break;
                }
            }
        }
        if (skipBlockedImg) continue;

        // Open tag with attributes.
        QString attrsOut;
        for (const auto& [aname, avalue] : t.attrs) {
            if (aname.startsWith(QStringLiteral("on"))) continue;          // event handler
            if (aname == QLatin1String("style")) continue;                 // CSS exploits
            if (!attrAllow().contains(aname))         continue;
            QString value = avalue;
            if (aname == QLatin1String("href") || aname == QLatin1String("src")) {
                if (!isSafeUrl(value)) continue;
                if (t.name == QLatin1String("img") && !opts.allowRemoteImages
                    && isRemoteHttpUrl(value)) {
                    // Defensive: pre-scan above should have caught this
                    // and skipped the whole tag; if we reach here for any
                    // reason (e.g. a non-src attribute also named "src"
                    // — vanishingly rare), drop the src and continue so
                    // we don't emit a remote URL.
                    res.remoteImagesBlocked = true;
                    continue;
                }
            }
            attrsOut.append(QChar(' '))
                    .append(aname)
                    .append(QStringLiteral("=\""))
                    .append(attrEscape(value))
                    .append(QChar('"'));
        }

        // Flatten table-family tags to <div> on emit so QTextBrowser
        // doesn't run its slow table-layout pass on deeply nested
        // marketing-email layouts. We keep the attribute filter on
        // the original tag name above so colspan/rowspan/etc. are
        // dropped (they're meaningless on a div anyway), and emit
        // the rewritten name here.
        const QString outName = flattenToDiv().contains(t.name)
            ? QStringLiteral("div") : t.name;
        out.append(QChar('<')).append(outName).append(attrsOut);
        // HTML5 only treats VOID elements as self-closing when written
        // `<tag />`. For non-void tags an XHTML-style `<div/>` parses
        // as an open `<div>` (with no matching close), which leaves
        // the document tree imbalanced. Honour selfClose only when
        // the element is in our void-tag list.
        if (voidTags().contains(t.name)) {
            out.append(QStringLiteral(" />"));
        } else {
            out.append(QChar('>'));
        }
    }

    res.html = out;
    return res;
}

}  // namespace fc::util
