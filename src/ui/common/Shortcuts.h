#pragma once

#include <QKeySequence>
#include <QObject>

class QMainWindow;

namespace fc::ui {

// Wires Gmail-style shortcuts to MainWindow:
//   /  focus search    j/k  next/prev message
//   c  compose         r    reply         e    archive
//   #  delete          s    star toggle   ?    show shortcuts help
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
    void showHelp();
};

}  // namespace fc::ui
