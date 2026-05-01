#include "MessageItemDelegate.h"

#include "models/MessageListModel.h"

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
    const QString from    = idx.data(fc::MessageListModel::FromRole).toString();
    const QString subject = idx.data(fc::MessageListModel::SubjectRole).toString();
    const QString snippet = idx.data(fc::MessageListModel::SnippetRole).toString();
    const qint64  date    = idx.data(fc::MessageListModel::DateRole).toLongLong();

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

    // From column — bold when unread.
    QFont fromFont = opt.font;
    fromFont.setWeight(unread ? QFont::DemiBold : QFont::Normal);
    QFontMetrics fromFm(fromFont);
    p->setFont(fromFont);
    p->setPen(primary);
    const QRect fromRect(x, inner.top(), kFromColWidth, inner.height() / 2);
    p->drawText(fromRect, Qt::AlignVCenter | Qt::AlignLeft,
                fromFm.elidedText(from, Qt::ElideRight, kFromColWidth));

    // Importance marker tucks in next to the from name.
    if (important) {
        const int markerX = x + fromFm.horizontalAdvance(
            fromFm.elidedText(from, Qt::ElideRight, kFromColWidth)) + 6;
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

    // Attachment icon: bottom-right of the row, just left of where the date sat.
    int rightCursor = rightEdge;
    if (hasAttach) {
        QIcon icon(QStringLiteral(":/icons/symbolic/paperclip.svg"));
        const int y = inner.bottom() - kAttachmentIconSize;
        const QRect ir(rightEdge - kAttachmentIconSize, y,
                       kAttachmentIconSize, kAttachmentIconSize);
        icon.paint(p, ir, Qt::AlignCenter, QIcon::Normal, QIcon::On);
        rightCursor = ir.left() - 6;
    }

    // Subject (left) + snippet (continuation, dim) on line 2, single line, elided.
    const QRect subjectRow(x, inner.top() + inner.height() / 2,
                            rightCursor - x, inner.height() / 2);
    QFontMetrics subjectFm(subjectFont);
    QString subjectClipped = subjectFm.elidedText(
        subject.isEmpty() ? QStringLiteral("(no subject)") : subject,
        Qt::ElideRight, subjectRow.width());
    int subjectAdvance = subjectFm.horizontalAdvance(subjectClipped);
    p->setFont(subjectFont);
    p->setPen(subject.isEmpty() ? secondary : primary);
    p->drawText(subjectRow, Qt::AlignVCenter | Qt::AlignLeft, subjectClipped);

    if (subjectAdvance + 16 < subjectRow.width() && !snippet.isEmpty()) {
        const QRect snippetRect(subjectRow.left() + subjectAdvance + 8,
                                subjectRow.top(),
                                subjectRow.width() - subjectAdvance - 8,
                                subjectRow.height());
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
