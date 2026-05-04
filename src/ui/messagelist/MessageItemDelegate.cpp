#include "MessageItemDelegate.h"

#include "models/MessageListModel.h"
#include "ui/common/IconLoader.h"
#include "ui/common/LabelStyleCache.h"
#include "ui/common/Preferences.h"

#include <QApplication>
#include <QDateTime>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>

namespace fc::ui {

namespace {

constexpr int kRowHeight    = 56;   // taller, breathable
constexpr int kPaddingX     = 14;
constexpr int kPaddingY     = 8;
constexpr int kStripeWidth  = 3;    // accent stripe on the left of unread rows
constexpr int kStarSize     = 18;
constexpr int kFromColWidth = 200;
constexpr int kDateColWidth = 80;
constexpr int kAttachmentIconSize = 14;

QString formatDate(qint64 msEpoch) {
    if (msEpoch <= 0) return {};
    const auto dt = QDateTime::fromMSecsSinceEpoch(msEpoch).toLocalTime();
    const auto now = QDateTime::currentDateTime();
    if (dt.date() == now.date()) {
        return dt.time().toString(QStringLiteral("h:mm AP"));
    }
    if (dt.date().year() == now.date().year()) {
        return dt.toString(QStringLiteral("MMM d"));
    }
    return dt.toString(QStringLiteral("M/d/yy"));
}

QColor accentColor(const QPalette& pal) {
    // Mirror the QSS theme tokens: light theme uses #1a73e8, dark uses #8ab4f8.
    return pal.color(QPalette::Highlight).lightness() > 128
        ? QColor(0x8a, 0xb4, 0xf8)
        : QColor(0x1a, 0x73, 0xe8);
}

QColor secondaryText(const QPalette& pal) {
    return pal.color(QPalette::Disabled, QPalette::Text);
}

}  // namespace

MessageItemDelegate::MessageItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

QSize MessageItemDelegate::sizeHint(const QStyleOptionViewItem&,
                                    const QModelIndex&) const {
    return {0, kRowHeight};
}

QRect MessageItemDelegate::starRect(const QRect& itemRect) {
    // Mirrors the painter's geometry exactly: itemRect → inner-padded rect
    // → kStarSize square at the inner-left, vertically centered. We also
    // grow the hit target by a few pixels on each side so the click feels
    // forgiving without overlapping the sender column.
    const QRect inner = itemRect.adjusted(kPaddingX, kPaddingY,
                                          -kPaddingX, -kPaddingY);
    const QRect drawn(inner.left(),
                      inner.top() + (inner.height() - kStarSize) / 2,
                      kStarSize, kStarSize);
    return drawn.adjusted(-4, -4, 4, 4);
}

void MessageItemDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                                const QModelIndex& idx) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    QStyleOptionViewItem o = opt;
    initStyleOption(&o, idx);

    // Background (hover / selected come from QSS via QStyle::PE_PanelItemViewItem).
    QStyle* style = QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &o, p, o.widget);

    const QRect rect = opt.rect;
    const QRect inner = rect.adjusted(kPaddingX, kPaddingY, -kPaddingX, -kPaddingY);

    const bool unread     = idx.data(fc::MessageListModel::UnreadRole).toBool();
    const bool starred    = idx.data(fc::MessageListModel::StarredRole).toBool();
    const bool important  = idx.data(fc::MessageListModel::ImportantRole).toBool();
    const bool hasAttach  = idx.data(fc::MessageListModel::HasAttachmentRole).toBool();
    const QString fromRaw = idx.data(fc::MessageListModel::FromRole).toString();
    const QString subject = idx.data(fc::MessageListModel::SubjectRole).toString();
    const QString snippet = idx.data(fc::MessageListModel::SnippetRole).toString();
    const qint64  date    = idx.data(fc::MessageListModel::DateRole).toLongLong();
    const int  threadCount = idx.data(fc::MessageListModel::ThreadCountRole).toInt();
    // Conversation rows (threadCount > 1): a small accent-coloured pill
    // before the sender, painted further down. Setting countSuffix
    // here keeps the importance-marker math below symmetric with the
    // single-message path.
    const QString countSuffix = (threadCount > 1)
        ? QStringLiteral(" (%1)").arg(threadCount)
        : QString();

    const QColor accent  = accentColor(opt.palette);
    const QColor primary = opt.palette.color(QPalette::Text);
    const QColor secondary = secondaryText(opt.palette);

    // Left accent stripe for unread rows.
    if (unread) {
        p->fillRect(QRect(rect.left(), rect.top() + 6,
                          kStripeWidth, rect.height() - 12),
                    accent);
    }

    int x = inner.left();

    // Star — clickable visual cue on the row. Filled = starred, outlined = not.
    {
        const QRect starRect(x, inner.top() + (inner.height() - kStarSize) / 2,
                              kStarSize, kStarSize);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->translate(starRect.center());
        p->scale(kStarSize / 24.0, kStarSize / 24.0);
        QPainterPath star;
        const QPointF pts[10] = {
            { 0,    -10},  { 2.9,  -3.1}, { 10.5, -3.1},
            { 4.3,   1.4}, { 6.6,   8.7}, { 0,     4.5},
            {-6.6,   8.7}, {-4.3,   1.4}, {-10.5, -3.1},
            {-2.9,  -3.1}
        };
        star.moveTo(pts[0]);
        for (int i = 1; i < 10; ++i) star.lineTo(pts[i]);
        star.closeSubpath();
        if (starred) {
            p->setBrush(QColor(0xf4, 0xb4, 0x00));
            p->setPen(QPen(QColor(0xf4, 0xb4, 0x00), 1));
        } else {
            p->setBrush(Qt::NoBrush);
            p->setPen(QPen(secondary, 1.4));
        }
        p->drawPath(star);
        p->restore();
        x = starRect.right() + 8;
    }

    // From column — bold when unread. Conversation rows (threadCount > 1)
    // get a small filled pill BEFORE the sender showing the message
    // count: Gmail-web-style "[3] Alice Smith" so the indicator is
    // impossible to miss and never gets chopped off by the elide pass.
    QFont fromFont = opt.font;
    fromFont.setWeight(unread ? QFont::DemiBold : QFont::Normal);
    QFontMetrics fromFm(fromFont);

    int senderX = x;
    if (threadCount > 1) {
        QFont badgeFont = opt.font;
        badgeFont.setWeight(QFont::DemiBold);
        badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.85);
        const QFontMetrics badgeFm(badgeFont);
        const QString badgeText = QString::number(threadCount);
        const int textW = badgeFm.horizontalAdvance(badgeText);
        const int padX  = 6;
        const int pillW = qMax(18, textW + 2 * padX);
        const int pillH = qMax(14, badgeFm.height() - 1);
        const QRect pill(x,
                         inner.top() + (inner.height() / 2 - pillH) / 2,
                         pillW, pillH);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(accent);
        p->drawRoundedRect(pill, pillH / 2.0, pillH / 2.0);
        p->setPen(Qt::white);
        p->setFont(badgeFont);
        p->drawText(pill, Qt::AlignCenter, badgeText);
        p->restore();
        senderX = pill.right() + 6;
    }

    p->setFont(fromFont);
    p->setPen(primary);

    const int senderBudget = qMax(0, kFromColWidth - (senderX - x));
    const QString senderClipped =
        fromFm.elidedText(fromRaw, Qt::ElideRight, senderBudget);
    const QRect fromRect(senderX, inner.top(),
                         senderBudget, inner.height() / 2);
    p->drawText(fromRect, Qt::AlignVCenter | Qt::AlignLeft, senderClipped);

    int afterFromX = senderX + fromFm.horizontalAdvance(senderClipped);

    // Importance marker tucks in next to the (already-rendered) badge +
    // sender, not at the column's edge.
    if (important) {
        const int markerX = afterFromX + 6;
        p->save();
        p->setPen(accent);
        QFont importantFont = fromFont;
        importantFont.setBold(true);
        p->setFont(importantFont);
        p->drawText(QRect(markerX, fromRect.top(), 16, fromRect.height()),
                    Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("»"));
        p->restore();
    }

    // Subject row (line 2) — primary text + dim snippet.
    QFont subjectFont = opt.font;
    subjectFont.setWeight(unread ? QFont::DemiBold : QFont::Normal);
    QFont snippetFont = opt.font;
    snippetFont.setWeight(QFont::Normal);

    const int rightEdge = inner.right();
    const QRect dateRect(rightEdge - kDateColWidth,
                         inner.top(),
                         kDateColWidth,
                         inner.height() / 2);

    // Date in the top-right.
    QFont dateFont = opt.font;
    dateFont.setWeight(unread ? QFont::DemiBold : QFont::Normal);
    p->setFont(dateFont);
    p->setPen(unread ? accent : secondary);
    p->drawText(dateRect, Qt::AlignVCenter | Qt::AlignRight, formatDate(date));

    // Attachment icon: bottom-right of the row, just left of where the date
    // sat. Tint to the row's secondary text colour rather than the SVG's
    // built-in stroke — without this the icon renders black in dark mode
    // because QIcon caches the SVG as a pixmap with no theme-awareness.
    int rightCursor = rightEdge;
    if (hasAttach) {
        const QIcon icon = IconLoader::tinted(
            QStringLiteral("paperclip.svg"), secondary);
        const int y = inner.bottom() - kAttachmentIconSize;
        const QRect ir(rightEdge - kAttachmentIconSize, y,
                       kAttachmentIconSize, kAttachmentIconSize);
        icon.paint(p, ir, Qt::AlignCenter, QIcon::Normal, QIcon::On);
        rightCursor = ir.left() - 6;
    }

    // Subject (left) + snippet (continuation, dim) on line 2, single line, elided.
    const QRect subjectRow(x, inner.top() + inner.height() / 2,
                            rightCursor - x, inner.height() / 2);
    int subjectStartX = subjectRow.left();

    // Label pills first — Gmail-web style: small coloured chips between
    // the row's left edge and the subject text. Cap at kMaxPills so a
    // very-tagged message doesn't squeeze the subject to nothing; if
    // the cap (or width budget) cuts us short, append a small "…"
    // overflow chip so the user knows more labels exist.
    if (Preferences::messageListLabelPills()) {
        const auto labelIds = idx.data(fc::MessageListModel::LabelIdsRole)
                                  .toStringList();
        if (!labelIds.isEmpty()) {
            constexpr int kMaxPills        = 3;
            constexpr int kPillPadX        = 6;
            constexpr int kEllipsisPadX    = 5;   // tighter so it tends to fit
            constexpr int kPillSpacing     = 4;
            constexpr int kPillBudgetRatio = 2;   // pills get at most half the row
            const int budget = subjectRow.width() / kPillBudgetRatio;

            QFont pillFont = opt.font;
            pillFont.setPointSizeF(pillFont.pointSizeF() * 0.85);
            pillFont.setWeight(QFont::DemiBold);
            QFontMetrics pillFm(pillFont);
            const int pillH = pillFm.height();

            const auto& cache = LabelStyleCache::instance();

            // Pre-filter: only user labels that have a usable bg colour
            // are eligible. We count eligibles up front so the ellipsis
            // pill below can tell whether the row is truncated by cap /
            // budget vs. just shorter than kMaxPills.
            std::vector<LabelStyleCache::Style> eligible;
            eligible.reserve(labelIds.size());
            for (const auto& lid : labelIds) {
                auto ls = cache.get(lid);
                if (ls.type != QLatin1String("user")) continue;
                if (!ls.bg.isValid())                 continue;
                eligible.push_back(std::move(ls));
            }

            int pillsDrawn = 0;
            int xCursor = subjectRow.left();
            const int xLimit = subjectRow.left() + budget;

            for (size_t i = 0;
                 i < eligible.size() && pillsDrawn < kMaxPills; ++i) {
                const auto& ls = eligible[i];
                const int textW = pillFm.horizontalAdvance(ls.name);
                const int pillW = textW + 2 * kPillPadX;
                if (xCursor + pillW > xLimit) break;

                const QRect pill(xCursor,
                                 subjectRow.top() + (subjectRow.height() - pillH) / 2,
                                 pillW, pillH);
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                p->setPen(Qt::NoPen);
                p->setBrush(ls.bg);
                p->drawRoundedRect(pill, pillH / 2.0, pillH / 2.0);
                p->setPen(ls.fg.isValid() ? ls.fg : Qt::white);
                p->setFont(pillFont);
                p->drawText(pill, Qt::AlignCenter, ls.name);
                p->restore();

                xCursor += pillW + kPillSpacing;
                ++pillsDrawn;
            }

            // Ellipsis chip — only when there were more eligible labels
            // we couldn't show. Neutral palette colour (not a label
            // colour) so it reads as a UI-side overflow marker, not a
            // label of its own.
            if (pillsDrawn < static_cast<int>(eligible.size())) {
                const QString ellip = QStringLiteral("…");
                const int textW = pillFm.horizontalAdvance(ellip);
                const int pillW = textW + 2 * kEllipsisPadX;
                if (xCursor + pillW <= xLimit) {
                    const QRect pill(xCursor,
                                     subjectRow.top() + (subjectRow.height() - pillH) / 2,
                                     pillW, pillH);
                    QColor bg = opt.palette.color(QPalette::Disabled,
                                                   QPalette::WindowText);
                    bg.setAlpha(60);
                    p->save();
                    p->setRenderHint(QPainter::Antialiasing);
                    p->setPen(Qt::NoPen);
                    p->setBrush(bg);
                    p->drawRoundedRect(pill, pillH / 2.0, pillH / 2.0);
                    p->setPen(secondary);
                    p->setFont(pillFont);
                    p->drawText(pill, Qt::AlignCenter, ellip);
                    p->restore();
                    xCursor += pillW + kPillSpacing;
                }
            }

            subjectStartX = xCursor;
        }
    }

    const QRect subjectRect(subjectStartX, subjectRow.top(),
                            subjectRow.right() - subjectStartX,
                            subjectRow.height());
    QFontMetrics subjectFm(subjectFont);
    QString subjectClipped = subjectFm.elidedText(
        subject.isEmpty() ? QStringLiteral("(no subject)") : subject,
        Qt::ElideRight, subjectRect.width());
    int subjectAdvance = subjectFm.horizontalAdvance(subjectClipped);
    p->setFont(subjectFont);
    p->setPen(subject.isEmpty() ? secondary : primary);
    p->drawText(subjectRect, Qt::AlignVCenter | Qt::AlignLeft, subjectClipped);

    if (subjectAdvance + 16 < subjectRect.width() && !snippet.isEmpty()) {
        const QRect snippetRect(subjectRect.left() + subjectAdvance + 8,
                                subjectRect.top(),
                                subjectRect.width() - subjectAdvance - 8,
                                subjectRect.height());
        QFontMetrics snippetFm(snippetFont);
        p->setFont(snippetFont);
        p->setPen(secondary);
        p->drawText(snippetRect, Qt::AlignVCenter | Qt::AlignLeft,
                    snippetFm.elidedText(QStringLiteral("— ") + snippet,
                                         Qt::ElideRight, snippetRect.width()));
    }

    p->restore();
}

}  // namespace fc::ui
