#include "Shortcuts.h"

#include <QMainWindow>
#include <QShortcut>

namespace fc::ui {

namespace {
QShortcut* make(QMainWindow* w, const QKeySequence& seq) {
    auto* s = new QShortcut(seq, w);
    s->setContext(Qt::WindowShortcut);
    return s;
}
}  // namespace

Shortcuts::Shortcuts(QMainWindow* main) : QObject(main) {
    connect(make(main, QKeySequence(QStringLiteral("/"))),
            &QShortcut::activated, this, &Shortcuts::focusSearch);
    connect(make(main, QKeySequence(QStringLiteral("j"))),
            &QShortcut::activated, this, &Shortcuts::selectNext);
    connect(make(main, QKeySequence(QStringLiteral("k"))),
            &QShortcut::activated, this, &Shortcuts::selectPrev);
    connect(make(main, QKeySequence(QStringLiteral("c"))),
            &QShortcut::activated, this, &Shortcuts::composeNew);
    connect(make(main, QKeySequence(QStringLiteral("r"))),
            &QShortcut::activated, this, &Shortcuts::replyToCurrent);
    // Reply-all: Gmail web binds `a`. We ALSO keep Shift+R for parity with
    // the original FirstContact binding so existing muscle memory isn't
    // broken.
    connect(make(main, QKeySequence(QStringLiteral("a"))),
            &QShortcut::activated, this, &Shortcuts::replyAllToCurrent);
    connect(make(main, QKeySequence(QStringLiteral("Shift+R"))),
            &QShortcut::activated, this, &Shortcuts::replyAllToCurrent);
    connect(make(main, QKeySequence(QStringLiteral("f"))),
            &QShortcut::activated, this, &Shortcuts::forwardCurrent);
    connect(make(main, QKeySequence(QStringLiteral("e"))),
            &QShortcut::activated, this, &Shortcuts::archiveCurrent);
    connect(make(main, QKeySequence(QStringLiteral("#"))),
            &QShortcut::activated, this, &Shortcuts::deleteCurrent);
    connect(make(main, QKeySequence(QStringLiteral("s"))),
            &QShortcut::activated, this, &Shortcuts::toggleStar);
    connect(make(main, QKeySequence(QStringLiteral("Shift+I"))),
            &QShortcut::activated, this, &Shortcuts::markRead);
    connect(make(main, QKeySequence(QStringLiteral("Shift+U"))),
            &QShortcut::activated, this, &Shortcuts::markUnread);
    connect(make(main, QKeySequence(QStringLiteral("b"))),
            &QShortcut::activated, this, &Shortcuts::snoozeCurrent);
    connect(make(main, QKeySequence(QStringLiteral("l"))),
            &QShortcut::activated, this, &Shortcuts::applyLabels);
    connect(make(main, QKeySequence(QStringLiteral("u"))),
            &QShortcut::activated, this, &Shortcuts::backToList);
    connect(make(main, QKeySequence(QStringLiteral("?"))),
            &QShortcut::activated, this, &Shortcuts::showHelp);
}

}  // namespace fc::ui
