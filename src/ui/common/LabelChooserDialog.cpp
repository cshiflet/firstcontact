#include "LabelChooserDialog.h"

#include "cache/LabelRepository.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>

namespace fc::ui {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

// Sort user labels by name, case-insensitive. Slash-separated parents
// come along for the ride — they sort naturally because '/' < any
// letter. Good enough for the picker.
std::vector<fc::cache::LabelRow> userLabelsSorted(const QString& accountId) {
    auto rows = accountId.isEmpty()
        ? std::vector<fc::cache::LabelRow>{}
        : fc::cache::LabelRepository::all(accountId);
    rows.erase(std::remove_if(rows.begin(), rows.end(),
        [](const fc::cache::LabelRow& r) { return r.type != "user"; }),
        rows.end());
    std::sort(rows.begin(), rows.end(),
        [](const fc::cache::LabelRow& a, const fc::cache::LabelRow& b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
    return rows;
}

}  // namespace

LabelChooserDialog::LabelChooserDialog(Mode mode,
                                         const QString& accountId,
                                         const QSet<QString>& currentlyApplied,
                                         QWidget* parent)
    : QDialog(parent),
      mode_(mode),
      accountId_(accountId),
      initiallyApplied_(currentlyApplied) {
    setWindowTitle(mode_ == Mode::Apply
        ? tr("Apply labels")
        : tr("Move to label"));
    setModal(true);
    resize(360, 420);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto* hint = new QLabel(mode_ == Mode::Apply
        ? tr("Toggle labels on the current conversation. "
             "Checked labels stay applied; unchecked ones are removed.")
        : tr("Pick a destination label. The conversation is removed "
             "from its current location and added to the chosen label."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    root->addWidget(hint);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(tr("Type to filter…"));
    filterEdit_->setClearButtonEnabled(true);
    root->addWidget(filterEdit_);

    list_ = new QListWidget(this);
    list_->setSelectionMode(mode_ == Mode::Apply
        ? QAbstractItemView::NoSelection
        : QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    root->addWidget(list_, /*stretch=*/1);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* okBtn = new QPushButton(mode_ == Mode::Apply
        ? tr("Apply")
        : tr("Move"), this);
    okBtn->setObjectName(QStringLiteral("primary"));
    okBtn->setDefault(true);
    auto* cancelBtn = new QPushButton(tr("Cancel"), this);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    root->addLayout(btnRow);

    connect(okBtn,     &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(filterEdit_, &QLineEdit::textChanged,
            this,         &LabelChooserDialog::applyFilter);

    // Enter inside the filter line accepts the dialog (matches Gmail web
    // — type a few chars, hit enter to apply). For MoveTo we additionally
    // require something to actually be selected; QDialog::accept on an
    // empty selection still works because chosen() will return empty
    // and the caller can decide what to do.
    connect(filterEdit_, &QLineEdit::returnPressed,
            this,         &QDialog::accept);

    buildList();

    // For MoveTo, if nothing matches the currentlyApplied set we can't
    // pre-select; otherwise pick the first match so Enter has a target.
    if (mode_ == Mode::MoveTo && list_->count() > 0) {
        for (int i = 0; i < list_->count(); ++i) {
            auto* it = list_->item(i);
            if (initiallyApplied_.contains(it->data(kIdRole).toString())) {
                list_->setCurrentItem(it);
                break;
            }
        }
    }

    filterEdit_->setFocus(Qt::ShortcutFocusReason);
}

void LabelChooserDialog::buildList() {
    list_->clear();
    const auto rows = userLabelsSorted(accountId_);
    for (const auto& r : rows) {
        auto* it = new QListWidgetItem(r.name, list_);
        it->setData(kIdRole, r.id);
        if (mode_ == Mode::Apply) {
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(initiallyApplied_.contains(r.id)
                ? Qt::Checked : Qt::Unchecked);
        }
    }
}

void LabelChooserDialog::applyFilter(const QString& needle) {
    const QString n = needle.trimmed();
    for (int i = 0; i < list_->count(); ++i) {
        auto* it = list_->item(i);
        const bool match = n.isEmpty()
            || it->text().contains(n, Qt::CaseInsensitive);
        it->setHidden(!match);
    }
}

QStringList LabelChooserDialog::added() const {
    QStringList out;
    if (mode_ == Mode::Apply) {
        for (int i = 0; i < list_->count(); ++i) {
            const auto* it = list_->item(i);
            const QString id = it->data(kIdRole).toString();
            if (it->checkState() == Qt::Checked
                && !initiallyApplied_.contains(id)) {
                out << id;
            }
        }
    } else {
        const auto* it = list_->currentItem();
        if (it) {
            const QString id = it->data(kIdRole).toString();
            if (!id.isEmpty()) out << id;
        }
    }
    return out;
}

QStringList LabelChooserDialog::removed() const {
    QStringList out;
    if (mode_ == Mode::Apply) {
        for (int i = 0; i < list_->count(); ++i) {
            const auto* it = list_->item(i);
            const QString id = it->data(kIdRole).toString();
            if (it->checkState() == Qt::Unchecked
                && initiallyApplied_.contains(id)) {
                out << id;
            }
        }
    }
    // MoveTo doesn't compute removed() here — the caller knows which
    // label the user is leaving (typically the active sidebar entry).
    return out;
}

QString LabelChooserDialog::chosen() const {
    if (mode_ != Mode::MoveTo) return {};
    const auto* it = list_->currentItem();
    return it ? it->data(kIdRole).toString() : QString();
}

}  // namespace fc::ui
