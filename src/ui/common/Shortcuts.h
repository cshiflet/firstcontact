#pragma once

#include <QKeySequence>
#include <QObject>
#include <QString>

class QMainWindow;

namespace fc::ui {

// Wires Gmail-web–compatible shortcuts to MainWindow. The full mapping
// is rendered in the help dialog (`?`); the highlights:
//   /          focus search
//   j / k      next / prev message
//   c          compose         r      reply         a / Shift+R  reply all
//   f          forward         e      archive       #            delete
//   s          star toggle     b      snooze        l            apply labels
//   v          move to label   m      mute thread   !            report spam
//   = / -      mark important / not important
//   o / Enter  open conversation
//   [ / ]      archive + prev / next
//   Shift+I    mark read       Shift+U mark unread
//   u          back to thread list (Gmail-web behavior)
//   g i / s / t / d           go to inbox / starred / sent / drafts
//   ?          show this list
class Shortcuts : public QObject {
    Q_OBJECT
public:
    explicit Shortcuts(QMainWindow* main);

signals:
    void focusSearch();
    void selectNext();              // j
    void selectPrev();               // k
    void openCurrent();              // o / Enter — open conversation
    void backToList();              // u — return focus to threadlist

    void composeNew();               // c
    void replyToCurrent();           // r
    void replyAllToCurrent();        // a / Shift+R
    void forwardCurrent();           // f

    void archiveCurrent();           // e
    void deleteCurrent();            // #
    void archiveAndPrev();           // [
    void archiveAndNext();           // ]
    void snoozeCurrent();            // b
    void muteThread();               // m
    void reportSpam();               // !

    void toggleStar();               // s
    void markRead();                 // Shift+I
    void markUnread();               // Shift+U
    void markImportant();            // =
    void markNotImportant();         // -

    void applyLabels();              // l (Phase 2)
    void moveToLabel();              // v (Phase 2)

    void goToLabel(const QString& labelId);  // gi / gs / gt / gd

    void showHelp();                 // ?
};

}  // namespace fc::ui
