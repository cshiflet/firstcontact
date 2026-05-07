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
    // -------------------------------------------------------------- Navigation
    connect(make(main, QKeySequence(QStringLiteral("/"))),
            &QShortcut::activated, this, &Shortcuts::focusSearch);
    connect(make(main, QKeySequence(QStringLiteral("j"))),
            &QShortcut::activated, this, &Shortcuts::selectNext);
    connect(make(main, QKeySequence(QStringLiteral("k"))),
            &QShortcut::activated, this, &Shortcuts::selectPrev);
    connect(make(main, QKeySequence(QStringLiteral("o"))),
            &QShortcut::activated, this, &Shortcuts::openCurrent);
    // Gmail-web also uses Enter to open the conversation when the
    // threadlist holds focus. Enter on a focused QLineEdit (search,
    // address fields, …) won't bubble up because Qt::WindowShortcut
    // hands keypresses to the focused widget first; that's exactly
    // the behavior we want.
    connect(make(main, QKeySequence(Qt::Key_Return)),
            &QShortcut::activated, this, &Shortcuts::openCurrent);
    connect(make(main, QKeySequence(QStringLiteral("u"))),
            &QShortcut::activated, this, &Shortcuts::backToList);

    // ------------------------------------------------------------------ Compose
    connect(make(main, QKeySequence(QStringLiteral("c"))),
            &QShortcut::activated, this, &Shortcuts::composeNew);
    connect(make(main, QKeySequence(QStringLiteral("r"))),
            &QShortcut::activated, this, &Shortcuts::replyToCurrent);
    // Reply-all: Gmail web binds `a`. We ALSO keep Shift+R for parity
    // with the original FirstContact binding so existing muscle memory
    // isn't broken.
    connect(make(main, QKeySequence(QStringLiteral("a"))),
            &QShortcut::activated, this, &Shortcuts::replyAllToCurrent);
    connect(make(main, QKeySequence(QStringLiteral("Shift+R"))),
            &QShortcut::activated, this, &Shortcuts::replyAllToCurrent);
    connect(make(main, QKeySequence(QStringLiteral("f"))),
            &QShortcut::activated, this, &Shortcuts::forwardCurrent);

    // ------------------------------------------------------------------ Archive / delete / move
    connect(make(main, QKeySequence(QStringLiteral("e"))),
            &QShortcut::activated, this, &Shortcuts::archiveCurrent);
    connect(make(main, QKeySequence(QStringLiteral("#"))),
            &QShortcut::activated, this, &Shortcuts::deleteCurrent);
    connect(make(main, QKeySequence(QStringLiteral("["))),
            &QShortcut::activated, this, &Shortcuts::archiveAndPrev);
    connect(make(main, QKeySequence(QStringLiteral("]"))),
            &QShortcut::activated, this, &Shortcuts::archiveAndNext);
    connect(make(main, QKeySequence(QStringLiteral("b"))),
            &QShortcut::activated, this, &Shortcuts::snoozeCurrent);
    connect(make(main, QKeySequence(QStringLiteral("m"))),
            &QShortcut::activated, this, &Shortcuts::muteThread);
    connect(make(main, QKeySequence(QStringLiteral("!"))),
            &QShortcut::activated, this, &Shortcuts::reportSpam);

    // ------------------------------------------------------------------ State / labels
    connect(make(main, QKeySequence(QStringLiteral("s"))),
            &QShortcut::activated, this, &Shortcuts::toggleStar);
    connect(make(main, QKeySequence(QStringLiteral("Shift+I"))),
            &QShortcut::activated, this, &Shortcuts::markRead);
    connect(make(main, QKeySequence(QStringLiteral("Shift+U"))),
            &QShortcut::activated, this, &Shortcuts::markUnread);
    connect(make(main, QKeySequence(QStringLiteral("="))),
            &QShortcut::activated, this, &Shortcuts::markImportant);
    connect(make(main, QKeySequence(QStringLiteral("-"))),
            &QShortcut::activated, this, &Shortcuts::markNotImportant);
    connect(make(main, QKeySequence(QStringLiteral("l"))),
            &QShortcut::activated, this, &Shortcuts::applyLabels);
    connect(make(main, QKeySequence(QStringLiteral("v"))),
            &QShortcut::activated, this, &Shortcuts::moveToLabel);

    // ------------------------------------------------------------------ Two-key go-to nav
    // Qt's QKeySequence supports up to four chords separated by commas.
    // The two-step "g, x" pattern matches Gmail web exactly: pressing
    // g, then x within the standard chord timeout fires the action.
    connect(make(main, QKeySequence(QStringLiteral("g, i"))),
            &QShortcut::activated, this,
            [this]{ emit goToLabel(QStringLiteral("INBOX")); });
    connect(make(main, QKeySequence(QStringLiteral("g, s"))),
            &QShortcut::activated, this,
            [this]{ emit goToLabel(QStringLiteral("STARRED")); });
    connect(make(main, QKeySequence(QStringLiteral("g, t"))),
            &QShortcut::activated, this,
            [this]{ emit goToLabel(QStringLiteral("SENT")); });
    connect(make(main, QKeySequence(QStringLiteral("g, d"))),
            &QShortcut::activated, this,
            [this]{ emit goToLabel(QStringLiteral("DRAFT")); });

    // ------------------------------------------------------------------ Help
    connect(make(main, QKeySequence(QStringLiteral("?"))),
            &QShortcut::activated, this, &Shortcuts::showHelp);
}

}  // namespace fc::ui
