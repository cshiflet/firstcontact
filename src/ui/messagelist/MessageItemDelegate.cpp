#include "MessageItemDelegate.h"

#include "models/MessageListModel.h"

#include <QApplication>
#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>

namespace fc::ui {

namespace {

constexpr int kRowHeight    = 28;
constexpr int kPadding      = 8;
constexpr int kFromColWidth = 180;
constexpr int kDateColWidth = 64;

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

    QStyleOptionViewItem o = opt;
    initStyleOption(&o, idx);
    QStyle* style = QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &o, p, o.widget);

    const QRect r = opt.rect.adjusted(kPadding, 0, -kPadding, 0);
    const bool unread     = idx.data(fc::MessageListModel::UnreadRole).toBool();
    const bool starred    = idx.data(fc::MessageListModel::StarredRole).toBool();
    const bool important  = idx.data(fc::MessageListModel::ImportantRole).toBool();
    const bool hasAttach  = idx.data(fc::MessageListModel::HasAttachmentRole).toBool();
    const QString from    = idx.data(fc::MessageListModel::FromRole).toString();
    const QString subject = idx.data(fc::MessageListModel::SubjectRole).toString();
    const QString snippet = idx.data(fc::MessageListModel::SnippetRole).toString();
    const qint64  date    = idx.data(fc::MessageListModel::DateRole).toLongLong();

    QFont font = opt.font;
    font.setBold(unread);
    p->setFont(font);

    int x = r.left();

    // Unread dot.
    if (unread) {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setBrush(QColor(0x4d, 0x90, 0xfe));
        p->setPen(Qt::NoPen);
        p->drawEllipse(QPoint(x + 4, r.center().y()), 4, 4);
        p->restore();
    }
    x += 14;

    // Star + importance markers.
    p->setPen(starred ? QColor(0xff, 0xb3, 0x00)
                      : opt.palette.color(QPalette::Disabled, QPalette::Text));
    p->drawText(QRect(x, r.top(), 18, r.height()),
                Qt::AlignVCenter, starred ? QStringLiteral("★") : QStringLiteral("☆"));
    x += 18;

    p->setPen(important ? QColor(0xff, 0xb3, 0x00)
                        : opt.palette.color(QPalette::Disabled, QPalette::Text));
    p->drawText(QRect(x, r.top(), 18, r.height()),
                Qt::AlignVCenter, important ? QStringLiteral("»") : QString());
    x += 18;

    p->setPen(opt.palette.color(QPalette::Text));

    // From column.
    QFontMetrics fm(font);
    p->drawText(QRect(x, r.top(), kFromColWidth, r.height()),
                Qt::AlignVCenter,
                fm.elidedText(from, Qt::ElideRight, kFromColWidth));
    x += kFromColWidth + kPadding;

    // Date column on the right.
    const QRect dateRect(r.right() - kDateColWidth, r.top(),
                         kDateColWidth, r.height());
    p->drawText(dateRect, Qt::AlignVCenter | Qt::AlignRight, formatDate(date));

    // Attachment icon left of date.
    int rightCursor = dateRect.left() - kPadding;
    if (hasAttach) {
        const QRect aRect(rightCursor - 14, r.top(), 14, r.height());
        p->drawText(aRect, Qt::AlignVCenter, QStringLiteral("📎"));
        rightCursor = aRect.left() - 4;
    }

    // Subject + snippet between from and date.
    const int midWidth = rightCursor - x;
    if (midWidth > 16) {
        QFont normal = font;
        normal.setBold(false);
        const QString joined = subject + (snippet.isEmpty()
            ? QString()
            : QStringLiteral("  —  ") + snippet);
        // Subject in bold-or-normal (matches unread state); snippet always
        // dim. For simplicity here we paint the whole string in one pass with
        // current font and let elision drop the snippet first.
        p->drawText(QRect(x, r.top(), midWidth, r.height()),
                    Qt::AlignVCenter,
                    QFontMetrics(font).elidedText(joined, Qt::ElideRight, midWidth));
    }

    p->restore();
}

}  // namespace fc::ui
