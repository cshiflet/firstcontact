#include "MessageListView.h"

#include "MessageItemDelegate.h"
#include "models/MessageListModel.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>

namespace fc::ui {

MessageListView::MessageListView(QWidget* parent)
    : QListView(parent),
      emptyTitle_(tr("No messages")),
      emptySubtitle_(tr("Nothing here yet — try another label or refresh.")) {
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

    setAccessibleName(tr("Message list"));
    setAccessibleDescription(tr(
        "List of messages in the selected label or search. "
        "Use arrow keys or j/k to move; press Enter to open."));

    connect(this, &QAbstractItemView::clicked,   this, &MessageListView::onActivated);
    connect(this, &QAbstractItemView::activated, this, &MessageListView::onActivated);
}

void MessageListView::setEmptyText(const QString& title, const QString& subtitle) {
    emptyTitle_    = title;
    emptySubtitle_ = subtitle;
    viewport()->update();   // repaint with the new copy
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

void MessageListView::paintEvent(QPaintEvent* e) {
    QListView::paintEvent(e);

    // Fall through to the empty-state placeholder when the model has no
    // rows. QListView would otherwise leave the viewport blank — there's
    // no built-in placeholder. Painted on the viewport AFTER the base
    // paint so the rule lines (none today, but if added later) don't
    // overdraw it.
    if (!model() || model()->rowCount() > 0) return;

    QPainter p(viewport());
    p.setRenderHint(QPainter::TextAntialiasing);

    const QRect r = viewport()->rect();
    const QPalette pal = palette();

    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
    titleFont.setWeight(QFont::Medium);

    const QFontMetrics tm(titleFont);
    const QFontMetrics bm(font());
    const int gap = 8;
    const int totalH = tm.height() + (emptySubtitle_.isEmpty() ? 0 : gap + bm.height());

    int y = r.top() + (r.height() - totalH) / 2 + tm.ascent();

    p.setFont(titleFont);
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(QRect(r.left(), y - tm.ascent(), r.width(), tm.height()),
               Qt::AlignHCenter | Qt::AlignTop, emptyTitle_);

    if (!emptySubtitle_.isEmpty()) {
        y += tm.descent() + gap + bm.ascent();
        p.setFont(font());
        // Dim — match the "(no subject)" placeholder used in the delegate.
        QColor dim = pal.color(QPalette::WindowText);
        dim.setAlphaF(0.55);
        p.setPen(dim);
        p.drawText(QRect(r.left(), y - bm.ascent(), r.width(), bm.height()),
                   Qt::AlignHCenter | Qt::AlignTop, emptySubtitle_);
    }
}

}  // namespace fc::ui
