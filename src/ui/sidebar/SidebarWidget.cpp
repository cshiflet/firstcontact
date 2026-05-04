#include "SidebarWidget.h"

#include "models/LabelTreeModel.h"

#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVBoxLayout>

namespace fc::ui {

namespace {

// Sidebar delegate that styles the synthetic "Folders" / "Labels"
// section parents distinctly from the real label rows below them: a
// theme-aware band fill + a slightly smaller, demi-bold, dimmer caption.
// The model emits these rows with TypeRole == "section"; we just add
// chrome here. Click-to-collapse is wired in SidebarWidget::onClicked.
class SidebarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
               const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        if (idx.data(fc::LabelTreeModel::TypeRole).toString()
                == QLatin1String("section")) {
            paintSection(painter, opt, idx);
            return;
        }
        QStyledItemDelegate::paint(painter, opt, idx);
    }

private:
    void paintSection(QPainter* painter,
                      const QStyleOptionViewItem& opt,
                      const QModelIndex& idx) const {
        painter->save();
        const bool dark = opt.palette.color(QPalette::Window).lightness() < 128;
        const QColor bg = dark ? QColor(0x2a, 0x2a, 0x2a)
                               : QColor(0xee, 0xef, 0xf1);
        painter->fillRect(opt.rect, bg);

        // Hover / selection highlight on top of the band, low-alpha so
        // the user sees the cursor is over a clickable region but the
        // section caption stays readable.
        if (opt.state & QStyle::State_MouseOver) {
            QColor hov = opt.palette.color(QPalette::Highlight);
            hov.setAlpha(48);
            painter->fillRect(opt.rect, hov);
        }

        QFont f = opt.font;
        f.setPointSizeF(f.pointSizeF() * 0.85);
        f.setWeight(QFont::DemiBold);
        painter->setFont(f);
        painter->setPen(opt.palette.color(QPalette::Disabled,
                                          QPalette::WindowText));

        // QTreeView paints the expand triangle into its own branch
        // gutter on the left of opt.rect; pad the caption so it doesn't
        // collide with the arrow.
        const QRect textRect = opt.rect.adjusted(4, 0, -8, 0);
        painter->drawText(textRect,
                          Qt::AlignVCenter | Qt::AlignLeft,
                          idx.data(Qt::DisplayRole).toString());
        painter->restore();
    }
};

}  // namespace

SidebarWidget::SidebarWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tree_  = new QTreeView(this);
    tree_->setObjectName(QStringLiteral("SidebarTree"));
    model_ = new fc::LabelTreeModel(this);
    tree_->setModel(model_);
    tree_->setHeaderHidden(true);
    // Show the disclosure arrows on every row that has children so users
    // can fold a parent label like "Travel" closed without losing it. The
    // previous setRootIsDecorated(false) hid the arrows on the top level —
    // the tree was technically there, just unreachable when collapsed.
    tree_->setRootIsDecorated(true);
    tree_->setIndentation(18);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setMouseTracking(true);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setExpandsOnDoubleClick(true);
    tree_->setAnimated(true);
    tree_->setItemDelegate(new SidebarDelegate(tree_));
    // Expand everything by default so users see the full label hierarchy
    // on first run; the QTreeView remembers per-node expansion state for
    // the lifetime of the model instance, so collapsing a branch sticks
    // until the next reload().
    tree_->expandAll();

    // Re-expand on reload — beginResetModel/endResetModel collapses the
    // tree back to its default state.
    connect(model_, &QAbstractItemModel::modelReset, tree_,
            [this] { tree_->expandAll(); });

    layout->addWidget(tree_);

    connect(tree_, &QAbstractItemView::clicked,
            this,  &SidebarWidget::onClicked);
    connect(tree_, &QWidget::customContextMenuRequested,
            this,  &SidebarWidget::onContextMenu);

    selectLabel(QStringLiteral("INBOX"));
}

fc::LabelTreeModel* SidebarWidget::model() const { return model_; }

void SidebarWidget::selectLabel(const QString& id) {
    // Walk the tree; depth is small.
    std::function<bool(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        const int n = model_->rowCount(parent);
        for (int i = 0; i < n; ++i) {
            const auto idx = model_->index(i, 0, parent);
            if (model_->data(idx, fc::LabelTreeModel::IdRole).toString() == id) {
                tree_->setCurrentIndex(idx);
                emit labelSelected(id);
                return true;
            }
            if (walk(idx)) return true;
        }
        return false;
    };
    walk({});
}

void SidebarWidget::onClicked(const QModelIndex& idx) {
    // Section banners ("Folders" / "Labels") collapse / expand on click,
    // anywhere in the row — not just on the small disclosure triangle.
    // Their TypeRole is "section" and they have no real label id.
    if (idx.data(fc::LabelTreeModel::TypeRole).toString()
            == QLatin1String("section")) {
        tree_->setExpanded(idx, !tree_->isExpanded(idx));
        return;
    }
    const QString id = idx.data(fc::LabelTreeModel::IdRole).toString();
    if (!id.isEmpty()) emit labelSelected(id);
}

void SidebarWidget::onContextMenu(const QPoint& p) {
    const QModelIndex idx = tree_->indexAt(p);
    QMenu menu(this);
    auto* createAct = menu.addAction(tr("New label…"));
    QAction* renameAct = nullptr;
    QAction* deleteAct = nullptr;
    QString id;
    QString type;
    if (idx.isValid()) {
        id   = idx.data(fc::LabelTreeModel::IdRole).toString();
        type = idx.data(fc::LabelTreeModel::TypeRole).toString();
        if (type == QLatin1String("user")) {
            menu.addSeparator();
            renameAct = menu.addAction(tr("Rename…"));
            deleteAct = menu.addAction(tr("Delete"));
        }
    }
    QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(p));
    if (!chosen) return;
    if (chosen == createAct) {
        emit requestCreateLabel(idx.isValid() ? id : QString());
    } else if (chosen == renameAct) {
        emit requestRenameLabel(id);
    } else if (chosen == deleteAct) {
        emit requestDeleteLabel(id);
    }
}

}  // namespace fc::ui
