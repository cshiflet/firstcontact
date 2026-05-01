#pragma once

#include <QStyledItemDelegate>

namespace fc::ui {

// Custom paint of a single inbox row. Layout (Gmail-web style):
//   [unread dot] [star] [importance] [from name] [subject — snippet]  [📎] [date]
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
