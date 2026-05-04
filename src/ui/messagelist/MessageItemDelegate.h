#pragma once

#include <QRect>
#include <QStyledItemDelegate>

namespace fc::ui {

// Custom paint of a single inbox row. Two-line modern Gmail layout:
//
//   ┃ ☆ Sender Name                                     date │
//   ┃   Subject — preview of the message snippet…    📎     │
//
// The accent stripe (┃) appears only when the message is unread; sender and
// date are bold and accent-coloured in that state. Hover/selection are
// painted by the system style and themed via QSS so we get clean Material-
// ish row treatments without rolling our own.
class MessageItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit MessageItemDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    // Geometry of the clickable star within a row, given the full item
    // rect. The view uses this in mousePressEvent to detect star clicks
    // and emit starToggled instead of selecting / opening the row.
    static QRect starRect(const QRect& itemRect);
};

}  // namespace fc::ui
