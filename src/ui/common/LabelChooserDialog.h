#pragma once

#include <QDialog>
#include <QSet>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QVBoxLayout;

namespace fc::ui {

// Label-picking dialog used by two Gmail-web shortcuts:
//
//   l (Apply labels…)  Mode::Apply  — multi-select; checkboxes pre-checked
//                                     for any label currently on the thread.
//                                     Caller computes (added, removed) and
//                                     pushes the diff through
//                                     applyLabelDiffToThread.
//   v (Move to label…) Mode::MoveTo — single-select; the chosen label is
//                                     added and the current view's label
//                                     (e.g. INBOX) is removed in one move.
//
// The dialog exposes only user labels (LabelRow::type == "user"). System
// labels (INBOX, SENT, STARRED, …) get their own one-key shortcuts and
// shouldn't appear here — that matches Gmail web's own picker.
class LabelChooserDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Apply, MoveTo };

    // currentlyApplied: the set of label ids already on the thread.
    //                   In Apply mode every match is pre-checked; in
    //                   MoveTo mode, the single radio defaults to the
    //                   first user-label that's already on the thread
    //                   (or to no selection at all when there isn't one).
    LabelChooserDialog(Mode mode,
                        const QString& accountId,
                        const QSet<QString>& currentlyApplied,
                        QWidget* parent = nullptr);

    // Result accessors. After exec() == Accepted:
    //  - Apply mode:  added = newly checked, removed = newly unchecked,
    //                 each relative to currentlyApplied.
    //  - MoveTo mode: added = [chosen],     removed = {} — the caller
    //                 decides whether to also drop the current view's
    //                 label (typically yes, matching Gmail web `v`).
    QStringList added()   const;
    QStringList removed() const;

    // For MoveTo callers that want the raw pick (before they compose
    // it with the current view's label removal).
    QString chosen() const;

private:
    void buildList();
    void applyFilter(const QString& needle);

    Mode                  mode_;
    QString               accountId_;
    QSet<QString>         initiallyApplied_;
    QListWidget*          list_         = nullptr;
    QLineEdit*            filterEdit_   = nullptr;
    QString               chosen_;
};

}  // namespace fc::ui
