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
    connect(make(main, QKeySequence(QStringLiteral("u"))),
            &QShortcut::activated, this, &Shortcuts::toggleRead);
    connect(make(main, QKeySequence(QStringLiteral("?"))),
            &QShortcut::activated, this, &Shortcuts::showHelp);

    // Ctrl+1 .. Ctrl+9 switch among the first nine signed-in accounts.
    // The number maps to the row order rendered in the toolbar
    // account menu — same convention as Slack / Discord workspace
    // switchers. Slots beyond the available account count are no-ops.
    for (int slot = 1; slot <= 9; ++slot) {
        const QKeySequence seq(QStringLiteral("Ctrl+%1").arg(slot));
        auto* s = make(main, seq);
        connect(s, &QShortcut::activated, this, [this, slot] {
            emit switchToAccountSlot(slot);
        });
    }
}

}  // namespace fc::ui
