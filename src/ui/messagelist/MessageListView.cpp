#include "MessageListView.h"

#include "MessageItemDelegate.h"
#include "models/MessageListModel.h"

#include <QMouseEvent>

namespace fc::ui {

MessageListView::MessageListView(QWidget* parent) : QListView(parent) {
    setItemDelegate(new MessageItemDelegate(this));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMouseTracking(true);            // hover row highlight
    setAlternatingRowColors(false);    // QSS row hover/selection looks better
    setSpacing(0);
    setFrameShape(QFrame::NoFrame);

    connect(this, &QAbstractItemView::clicked,   this, &MessageListView::onActivated);
    connect(this, &QAbstractItemView::activated, this, &MessageListView::onActivated);
}

void MessageListView::mousePressEvent(QMouseEvent* e) {
    // Intercept clicks on the row's interactive glyphs (chevron, star)
    // BEFORE QListView treats them as a row activation. For all other
    // points in a row, fall through to the base implementation so
    // selection + activation behave normally.
    if (e->button() == Qt::LeftButton) {
        const QModelIndex idx = indexAt(e->pos());
        if (idx.isValid()) {
            const QRect rowRect = visualRect(idx);

            // Chevron: only meaningful on parent rows of multi-message
            // threads. Toggle the model's expansion state in place;
            // child rows then appear / disappear inline.
            const int threadCount =
                idx.data(fc::MessageListModel::ThreadCountRole).toInt();
            const bool isChild =
                idx.data(fc::MessageListModel::IsChildRole).toBool();
            if (!isChild && threadCount > 1
                && MessageItemDelegate::chevronRect(rowRect).contains(e->pos())) {
                if (auto* m = qobject_cast<fc::MessageListModel*>(model())) {
                    m->toggleThreadExpand(idx.row());
                }
                e->accept();
                return;
            }

            if (MessageItemDelegate::starRect(rowRect).contains(e->pos())) {
                const auto id = idx.data(fc::MessageListModel::IdRole).toString();
                if (!id.isEmpty()) emit starToggled(id);
                e->accept();
                return;
            }
        }
    }
    QListView::mousePressEvent(e);
}

void MessageListView::onActivated(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    const auto id = idx.data(fc::MessageListModel::IdRole).toString();
    emit messageActivated(id, idx.row());
}

}  // namespace fc::ui
