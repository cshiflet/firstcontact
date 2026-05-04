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

// Sidebar delegate that paints a 1-pixel separator above the FIRST root-
// level user label. Mirrors the visual break Gmail web puts between
// system folders (Inbox / Starred / Sent / …) and the user-defined label
// list. The model already tags each node with TypeRole; we just check
// neighbouring siblings to decide whether this row is the boundary.
class SidebarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& opt,
                   const QModelIndex& idx) const override {
        QSize s = QStyledItemDelegate::sizeHint(opt, idx);
        if (isFirstUserLabelAtRoot(idx)) s.rheight() += 10;  // top padding
        return s;
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        QStyleOptionViewItem o = opt;
        if (isFirstUserLabelAtRoot(idx)) {
            // Draw a faint divider just above where the row would normally
            // start, then shrink the option rect so the row contents land
            // below it.
            const int y = opt.rect.top() + 5;
            painter->save();
            QColor line = opt.palette.color(QPalette::Disabled,
                                            QPalette::WindowText);
            line.setAlpha(80);
            painter->setPen(QPen(line, 1));
            painter->drawLine(opt.rect.left() + 6, y,
                              opt.rect.right() - 6, y);
            painter->restore();
            o.rect.adjust(0, 10, 0, 0);
        }
        QStyledItemDelegate::paint(painter, o, idx);
    }

private:
    static bool isFirstUserLabelAtRoot(const QModelIndex& idx) {
        if (!idx.isValid() || idx.parent().isValid()) return false;
        const auto type = idx.data(fc::LabelTreeModel::TypeRole).toString();
        if (type != QLatin1String("user")) return false;
        if (idx.row() == 0) return true;
        const auto prev = idx.sibling(idx.row() - 1, idx.column());
        return prev.data(fc::LabelTreeModel::TypeRole).toString()
                 != QLatin1String("user");
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
