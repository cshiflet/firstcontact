#include "SidebarWidget.h"

#include "account/AccountManager.h"
#include "models/LabelTreeModel.h"
#include "ui/common/Preferences.h"

#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QScopedValueRollback>
#include <QSettings>
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
        // Sidebar swatch — painted on top of the default text. Only
        // applies to user labels with a Gmail-assigned background colour
        // and only when the pref is on. Drawing AFTER the base paint
        // keeps hover / selection chrome intact; we just lay a small
        // filled square (rounded a hair) at the right edge of the row.
        if (!Preferences::sidebarLabelColors()) return;
        if (idx.data(fc::LabelTreeModel::TypeRole).toString()
                != QLatin1String("user")) return;
        const QString bgHex = idx.data(fc::LabelTreeModel::ColorBgRole).toString();
        if (bgHex.isEmpty()) return;
        const QColor bg(bgHex);
        if (!bg.isValid()) return;

        const int sw = 10;
        const QRect r = opt.rect;
        const QRect swatch(r.right() - sw - 8,
                           r.top() + (r.height() - sw) / 2,
                           sw, sw);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(bg);
        painter->drawRoundedRect(swatch, 2, 2);
        painter->restore();
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
    tree_->setAccessibleName(tr("Labels and folders"));
    tree_->setAccessibleDescription(tr(
        "Tree of system labels (Inbox, Sent, Drafts, …) and your user labels. "
        "Use arrow keys to navigate; Enter opens the label."));
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

    // Load persisted expand state. Initial reload() in the model
    // constructor ran with the default (empty) state; restoreExpandState
    // applies the QSettings snapshot once we have it. New nodes that
    // aren't in the set stay collapsed by default, which matches the
    // user's "collapsed except for the selected folder path" intent.
    {
        QSettings s;
        const auto list = s.value(QStringLiteral("sidebar/expandedKeys"))
                              .toStringList();
        for (const auto& k : list) expanded_.insert(k);
    }

    // Re-apply the cached selection AND the persisted expand state on
    // every modelReset. Without this the selected row and any open
    // branches disappear every time the model reloads.
    connect(model_, &QAbstractItemModel::modelReset, tree_,
            [this] {
                restoreExpandState();
                if (!selectedLabelId_.isEmpty()) {
                    selectLabel(selectedAccountId_, selectedLabelId_);
                }
            });

    // Persist expand state every time the user opens or closes a node.
    // restoringExpand_ guards the writes that happen during our own
    // restoreExpandState pass.
    connect(tree_, &QTreeView::expanded, this,
            [this](const QModelIndex& idx) {
                if (restoringExpand_) return;
                expanded_.insert(expandKey(idx));
                saveExpandState();
            });
    connect(tree_, &QTreeView::collapsed, this,
            [this](const QModelIndex& idx) {
                if (restoringExpand_) return;
                expanded_.remove(expandKey(idx));
                saveExpandState();
            });

    layout->addWidget(tree_);

    connect(tree_, &QAbstractItemView::clicked,
            this,  &SidebarWidget::onClicked);
    connect(tree_, &QWidget::customContextMenuRequested,
            this,  &SidebarWidget::onContextMenu);
}

void SidebarWidget::setAccountManager(fc::account::AccountManager* accounts) {
    if (accounts_ == accounts) return;
    accounts_ = accounts;
    if (!accounts_) return;
    pushAccountsToModel();
    connect(accounts_, &fc::account::AccountManager::accountsChanged,
            this, [this] { pushAccountsToModel(); });
    connect(accounts_,
            &fc::account::AccountManager::currentAccountChanged,
            this, [this](const QString& aid) {
                if (model_) model_->setAccountId(aid);
            });
}

void SidebarWidget::pushAccountsToModel() {
    if (!accounts_ || !model_) return;
    QList<fc::LabelTreeModel::AccountDescriptor> descriptors;
    for (const auto& a : accounts_->accounts()) {
        descriptors.append({a.id, a.email, a.displayName});
    }
    model_->setAccounts(descriptors);
}

QString SidebarWidget::expandKey(const QModelIndex& idx) const {
    if (!idx.isValid() || !model_) return {};
    const QString aid = model_->data(idx, fc::LabelTreeModel::AccountIdRole)
                              .toString();
    const QString full = model_->data(idx, fc::LabelTreeModel::NameRole)
                              .toString();
    // fullName already disambiguates section nodes (they carry the
    // accountId in their path) and account/synthetic nodes. For label
    // leaves we still prepend accountId so the same Gmail id (e.g.
    // INBOX) in two accounts doesn't share a single expand entry.
    if (full.startsWith(QStringLiteral("__"))) return full;
    return aid + QLatin1Char('\t') + full;
}

void SidebarWidget::saveExpandState() {
    QSettings s;
    QStringList list;
    list.reserve(expanded_.size());
    for (const auto& k : expanded_) list.append(k);
    list.sort();   // deterministic file order, eases diffing
    s.setValue(QStringLiteral("sidebar/expandedKeys"), list);
}

void SidebarWidget::restoreExpandState() {
    if (!tree_ || !model_) return;
    QScopedValueRollback<bool> guard(restoringExpand_, true);
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        const int n = model_->rowCount(parent);
        for (int i = 0; i < n; ++i) {
            const auto idx = model_->index(i, 0, parent);
            const QString key = expandKey(idx);
            if (expanded_.contains(key)) {
                tree_->setExpanded(idx, true);
            } else {
                tree_->setExpanded(idx, false);
            }
            walk(idx);
        }
    };
    walk({});
    if (!selectedLabelId_.isEmpty()) {
        expandPathTo(selectedAccountId_, selectedLabelId_);
    }
}

void SidebarWidget::expandPathTo(const QString& accountId, const QString& labelId) {
    if (!tree_ || !model_) return;
    QScopedValueRollback<bool> guard(restoringExpand_, true);
    // DFS for the (accountId, labelId) leaf; on the way back, expand
    // every ancestor. Both the persisted set AND the live tree-view
    // state get the ancestors flipped on so the persisted snapshot
    // captures the "always expand selection path" rule.
    std::function<bool(const QModelIndex&)> walk = [&](const QModelIndex& parent) -> bool {
        const int n = model_->rowCount(parent);
        for (int i = 0; i < n; ++i) {
            const auto idx = model_->index(i, 0, parent);
            const QString aid = model_->data(idx, fc::LabelTreeModel::AccountIdRole)
                                      .toString();
            const QString id  = model_->data(idx, fc::LabelTreeModel::IdRole)
                                      .toString();
            if (id == labelId && (accountId.isEmpty() || aid == accountId)) {
                // Found the leaf — expand each ancestor on the way out.
                return true;
            }
            if (walk(idx)) {
                tree_->setExpanded(idx, true);
                expanded_.insert(expandKey(idx));
                return true;
            }
        }
        return false;
    };
    walk({});
    saveExpandState();
}

fc::LabelTreeModel* SidebarWidget::model() const { return model_; }

void SidebarWidget::refreshAppearance() {
    if (tree_) tree_->viewport()->update();
}

void SidebarWidget::selectLabel(const QString& accountId, const QString& labelId) {
    // Always remember what the caller asked for, even if the model
    // doesn't yet have the row — the modelReset handler retries once
    // the labels load.
    selectedAccountId_ = accountId;
    selectedLabelId_   = labelId;

    // Make sure the path is expanded so the leaf is visible. (No-op
    // when the parents are already expanded.)
    expandPathTo(accountId, labelId);

    std::function<bool(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        const int n = model_->rowCount(parent);
        for (int i = 0; i < n; ++i) {
            const auto idx = model_->index(i, 0, parent);
            const QString aid = model_->data(idx, fc::LabelTreeModel::AccountIdRole).toString();
            const QString id  = model_->data(idx, fc::LabelTreeModel::IdRole).toString();
            if (id == labelId && (accountId.isEmpty() || aid == accountId)) {
                tree_->setCurrentIndex(idx);
                emit labelSelected(aid, id);
                return true;
            }
            if (walk(idx)) return true;
        }
        return false;
    };
    walk({});
}

void SidebarWidget::onClicked(const QModelIndex& idx) {
    const QString type = idx.data(fc::LabelTreeModel::TypeRole).toString();
    // Section banners ("Folders" / "Labels") and account headers
    // toggle expand on row click anywhere — easier target than the
    // narrow disclosure triangle. They carry no real label id.
    if (type == QLatin1String("section") || type == QLatin1String("account")) {
        tree_->setExpanded(idx, !tree_->isExpanded(idx));
        return;
    }
    const QString id  = idx.data(fc::LabelTreeModel::IdRole).toString();
    const QString aid = idx.data(fc::LabelTreeModel::AccountIdRole).toString();
    if (!id.isEmpty()) {
        selectedAccountId_ = aid;
        selectedLabelId_   = id;
        emit labelSelected(aid, id);
    }
}

void SidebarWidget::onContextMenu(const QPoint& p) {
    const QModelIndex idx = tree_->indexAt(p);
    QMenu menu(this);
    auto* createAct = menu.addAction(tr("New label…"));
    QAction* renameAct = nullptr;
    QAction* deleteAct = nullptr;
    QString id;
    QString aid;
    QString type;
    if (idx.isValid()) {
        id   = idx.data(fc::LabelTreeModel::IdRole).toString();
        aid  = idx.data(fc::LabelTreeModel::AccountIdRole).toString();
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
        emit requestCreateLabel(aid, idx.isValid() ? id : QString());
    } else if (chosen == renameAct) {
        emit requestRenameLabel(aid, id);
    } else if (chosen == deleteAct) {
        emit requestDeleteLabel(aid, id);
    }
}

}  // namespace fc::ui
