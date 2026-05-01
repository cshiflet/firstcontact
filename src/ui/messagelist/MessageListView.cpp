#include "MessageListView.h"

#include "MessageItemDelegate.h"
#include "models/MessageListModel.h"

namespace fc::ui {

MessageListView::MessageListView(QWidget* parent) : QListView(parent) {
    setItemDelegate(new MessageItemDelegate(this));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setEditTriggers(QAbstractItemView::NoEditTriggers);

    connect(this, &QAbstractItemView::clicked,   this, &MessageListView::onActivated);
    connect(this, &QAbstractItemView::activated, this, &MessageListView::onActivated);
}

void MessageListView::onActivated(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    const auto id = idx.data(fc::MessageListModel::IdRole).toString();
    emit messageActivated(id, idx.row());
}

}  // namespace fc::ui
