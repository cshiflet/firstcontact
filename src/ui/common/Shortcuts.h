#pragma once

#include <QKeySequence>
#include <QObject>

class QMainWindow;

namespace fc::ui {

// Wires Gmail-web–compatible shortcuts to MainWindow. The full mapping
// is rendered in the help dialog (`?`); the highlights:
//   /          focus search
//   j / k      next / prev message
//   c          compose         r      reply         a / Shift+R  reply all
//   f          forward         e      archive       #            delete
//   s          star toggle     b      snooze        l            apply labels
//   Shift+I    mark read       Shift+U mark unread
//   u          back to thread list (Gmail-web behavior)
//   ?          show this list
class Shortcuts : public QObject {
    Q_OBJECT
public:
    explicit Shortcuts(QMainWindow* main);

signals:
    void focusSearch();
    void selectNext();
    void selectPrev();
    void composeNew();
    void replyToCurrent();
    void replyAllToCurrent();
    void forwardCurrent();
    void archiveCurrent();
    void deleteCurrent();
    void toggleStar();
    void markRead();        // Shift+I
    void markUnread();      // Shift+U
    void snoozeCurrent();   // b
    void applyLabels();     // l
    void backToList();      // u — return focus to threadlist (Gmail-web)
    void showHelp();
};

}  // namespace fc::ui
