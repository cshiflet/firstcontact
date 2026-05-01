#pragma once

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
};

}  // namespace fc::ui
