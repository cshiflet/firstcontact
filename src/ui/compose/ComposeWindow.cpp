#include "ComposeWindow.h"

#include "ui/common/Preferences.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace fc::ui {

namespace {

QStringList splitAddresses(const QString& raw) {
    return raw.split(QRegularExpression(QStringLiteral("\\s*,\\s*")),
                     Qt::SkipEmptyParts);
}

// Plain-text quoting — for the rich-text path (Phase 5) the BlockQuote
// branch will become a real <blockquote>, but in plain text both
// QuoteStyle variants render identically as "> " line prefixes since
// that's the universal email convention.
QString quoteBody(const QString& body) {
    QStringList lines = body.split(QChar('\n'));
    for (auto& line : lines) line = QStringLiteral("> ") + line;
    return lines.join(QChar('\n'));
}

QString attribution(const fc::Message& parent,
                     fc::ui::ComposeWindow::Mode mode) {
    const QString from = parent.fromName.isEmpty()
        ? parent.fromAddr
        : QStringLiteral("%1 <%2>").arg(parent.fromName, parent.fromAddr);

    if (mode == fc::ui::ComposeWindow::Mode::Forward) {
        const QString date = parent.internalDate > 0
            ? QLocale::system().toString(
                  QDateTime::fromMSecsSinceEpoch(parent.internalDate),
                  QLocale::ShortFormat)
            : QString();
        return QStringLiteral(
            "---------- Forwarded message ---------\n"
            "From: %1\nDate: %2\nSubject: %3\nTo: %4\n\n")
            .arg(from, date, parent.subject,
                  parent.toAddrs.join(QStringLiteral(", ")));
    }

    // Reply / ReplyAll attribution. Mirrors Gmail web's "On <date>,
    // <person> wrote:" line.
    const QString date = parent.internalDate > 0
        ? QLocale::system().toString(
              QDateTime::fromMSecsSinceEpoch(parent.internalDate),
              QLocale::LongFormat)
        : QString();
    return QStringLiteral("On %1, %2 wrote:\n").arg(date, from);
}

QString withSignature(const QString& body, const QString& sig) {
    if (sig.isEmpty()) return body;
    // RFC 3676-style signature delimiter: "\n-- \n" (note trailing
    // space on the dashes). Plays nicely with most clients that
    // suppress the sig from quoted replies.
    return body + QStringLiteral("\n-- \n") + sig;
}

}  // namespace

ComposeWindow::ComposeWindow(const QString& fromAddr,
                             const QString& fromName,
                             QWidget* parent)
    : QWidget(parent, Qt::Window),
      fromAddr_(fromAddr), fromName_(fromName) {
    setWindowTitle(tr("New message"));
    resize(720, 580);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(10);
    auto* form = new QFormLayout;
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    toEdit_      = new QLineEdit(this);
    ccEdit_      = new QLineEdit(this);
    subjectEdit_ = new QLineEdit(this);

    form->addRow(tr("From:"),    new QLabel(QStringLiteral("%1 <%2>")
                                              .arg(fromName, fromAddr), this));
    form->addRow(tr("To:"),      toEdit_);
    form->addRow(tr("Cc:"),      ccEdit_);
    form->addRow(tr("Subject:"), subjectEdit_);

    bodyEdit_ = new QTextEdit(this);
    bodyEdit_->setAcceptRichText(false);

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color: gray;"));

    auto* sendBtn      = new QPushButton(tr("Send"),         this);
    sendBtn->setObjectName(QStringLiteral("primary"));
    sendBtn->setToolTip(tr("Send (Ctrl+Enter)"));
    auto* scheduleBtn  = new QPushButton(tr("Schedule…"),    this);
    auto* saveDraftBtn = new QPushButton(tr("Save draft"),   this);
    auto* cancelBtn    = new QPushButton(tr("Discard"),      this);
    cancelBtn->setToolTip(tr("Discard (Esc)"));
    connect(sendBtn,      &QPushButton::clicked, this, &ComposeWindow::onSend);
    connect(scheduleBtn,  &QPushButton::clicked, this, &ComposeWindow::onScheduleSend);
    connect(saveDraftBtn, &QPushButton::clicked, this, &ComposeWindow::onSaveDraft);
    connect(cancelBtn,    &QPushButton::clicked, this, [this] {
        suppressClosePrompt_ = true;
        close();
    });

    // Gmail-web shortcuts inside the compose window:
    //   Ctrl+Enter (Cmd+Enter on macOS via Qt::CTRL alias) — Send
    //   Esc                                                   — Close
    auto* sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    sendShortcut->setContext(Qt::WindowShortcut);
    connect(sendShortcut, &QShortcut::activated, this, &ComposeWindow::onSend);
    auto* sendShortcut2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), this);
    sendShortcut2->setContext(Qt::WindowShortcut);
    connect(sendShortcut2, &QShortcut::activated, this, &ComposeWindow::onSend);
    auto* escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escShortcut->setContext(Qt::WindowShortcut);
    connect(escShortcut, &QShortcut::activated, this, [this] { close(); });

    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(statusLabel_, 1);
    btnRow->addWidget(saveDraftBtn);
    btnRow->addWidget(scheduleBtn);
    btnRow->addWidget(sendBtn);
    btnRow->addWidget(cancelBtn);

    root->addLayout(form);
    root->addWidget(bodyEdit_, /*stretch=*/1);
    root->addLayout(btnRow);

    auto markDirty = [this] { dirty_ = true; };
    connect(toEdit_,      &QLineEdit::textChanged, this, markDirty);
    connect(ccEdit_,      &QLineEdit::textChanged, this, markDirty);
    connect(subjectEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(bodyEdit_,    &QTextEdit::textChanged, this, markDirty);
}

QString ComposeWindow::draftId() const { return draftId_; }

fc::util::OutgoingMessage ComposeWindow::currentMessage() const {
    fc::util::OutgoingMessage msg;
    msg.fromAddr         = fromAddr_;
    msg.fromName         = fromName_;
    msg.subject          = subjectEdit_->text();
    msg.bodyText         = bodyEdit_->toPlainText();
    msg.to               = splitAddresses(toEdit_->text());
    msg.cc               = splitAddresses(ccEdit_->text());
    msg.rfc822InReplyTo  = inReplyToHeader_;
    msg.rfc822References = referencesHeader_;
    return msg;
}

void ComposeWindow::loadFromDraft(const QString& draftId,
                                  const QString& threadId,
                                  const QString& subject,
                                  const QStringList& to,
                                  const QStringList& cc,
                                  const QString& body) {
    draftId_  = draftId;
    threadId_ = threadId;
    subjectEdit_->setText(subject);
    toEdit_->setText(to.join(QStringLiteral(", ")));
    ccEdit_->setText(cc.join(QStringLiteral(", ")));
    bodyEdit_->setPlainText(body);
    bodyEdit_->moveCursor(QTextCursor::End);
    dirty_ = false;
}

void ComposeWindow::prefillFrom(const fc::Message& parent, Mode mode) {
    threadId_ = parent.threadId;
    referencesHeader_.clear();
    inReplyToHeader_.clear();

    QString to;
    if (!parent.replyTo.isEmpty()) to = parent.replyTo;
    else if (!parent.fromAddr.isEmpty())
        to = parent.fromName.isEmpty()
                ? parent.fromAddr
                : QStringLiteral("%1 <%2>").arg(parent.fromName, parent.fromAddr);

    QString subjectPrefix;
    switch (mode) {
        case Mode::Reply:
        case Mode::ReplyAll:
            subjectPrefix = QStringLiteral("Re: ");
            toEdit_->setText(to);
            if (mode == Mode::ReplyAll) {
                QStringList cc = parent.toAddrs;
                cc.append(parent.ccAddrs);
                cc.removeAll(fromAddr_);
                ccEdit_->setText(cc.join(QStringLiteral(", ")));
            }
            break;
        case Mode::Forward:
            subjectPrefix = QStringLiteral("Fwd: ");
            break;
        case Mode::New:
            break;
    }

    QString subject = parent.subject;
    if (!subject.startsWith(subjectPrefix, Qt::CaseInsensitive)) {
        subject = subjectPrefix + subject;
    }
    subjectEdit_->setText(subject);

    // Compose-body assembly. Three pieces, ordered by preference:
    //
    //   replyAboveOriginal == true  (Gmail / Outlook default):
    //       <cursor>
    //       <signature>
    //       <attribution + quoted parent>
    //
    //   replyAboveOriginal == false (Usenet / mailing-list convention):
    //       <attribution + quoted parent>
    //       <cursor>
    //       <signature>
    //
    // For Mode::New there's no parent — the body is empty + signature
    // and the cursor sits at the start of the empty body.
    const QString sig = Preferences::signatureText();

    QString body;
    int cursorPos = 0;

    if (mode == Mode::New) {
        body = withSignature(QString(), sig);
        cursorPos = 0;
    } else {
        const QString quoted = attribution(parent, mode)
                              + quoteBody(parent.bodyText);
        if (Preferences::replyAboveOriginal()) {
            // Two leading newlines so the cursor lands on line 1, with
            // a clear gap before the signature / quoted block.
            const QString prefix = QStringLiteral("\n\n");
            body = prefix + withSignature(QString(), sig)
                 + QStringLiteral("\n\n") + quoted;
            cursorPos = 0;
        } else {
            body = quoted + QStringLiteral("\n\n");
            cursorPos = body.length();
            body = withSignature(body, sig);
        }
    }

    bodyEdit_->setPlainText(body);

    // Place the cursor where the user is expected to start typing.
    QTextCursor c = bodyEdit_->textCursor();
    c.setPosition(qMin(cursorPos, bodyEdit_->document()->characterCount() - 1));
    bodyEdit_->setTextCursor(c);

    // We just programmatically populated fields; the user hasn't typed yet.
    dirty_ = false;
}

void ComposeWindow::onSend() {
    const auto msg = currentMessage();
    if (msg.to.isEmpty()) {
        statusLabel_->setText(tr("Add at least one recipient."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
        return;
    }
    suppressClosePrompt_ = true;
    emit composeReady(msg, threadId_, /*sendAtMs=*/0);
    close();
}

void ComposeWindow::onScheduleSend() {
    const auto msg = currentMessage();
    if (msg.to.isEmpty()) {
        statusLabel_->setText(tr("Add at least one recipient."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
        return;
    }

    // Tiny scheduling dialog: a single QDateTimeEdit defaulted to one
    // hour from now. No fancier presets ("tomorrow morning", "next
    // Monday") for v1; users can drive the picker.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Schedule send"));
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr(
        "Pick a delivery time. The message stays queued in your local "
        "outbox until that moment, then sends like any other message. "
        "Sync must be running for delivery to actually fire — leave the "
        "app open or relaunch in time."), &dlg));
    auto* picker = new QDateTimeEdit(
        QDateTime::currentDateTime().addSecs(60 * 60), &dlg);
    picker->setCalendarPopup(true);
    picker->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
    picker->setDisplayFormat(QStringLiteral("ddd MMM d, yyyy  h:mm AP"));
    layout->addWidget(picker);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* okBtn     = new QPushButton(tr("Schedule"), &dlg);
    okBtn->setObjectName(QStringLiteral("primary"));
    okBtn->setDefault(true);
    auto* cancelBtn = new QPushButton(tr("Cancel"),   &dlg);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    layout->addLayout(btnRow);
    QObject::connect(okBtn,     &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const QDateTime when = picker->dateTime();
    if (when <= QDateTime::currentDateTime()) {
        statusLabel_->setText(tr("Pick a time in the future."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
        return;
    }
    suppressClosePrompt_ = true;
    emit composeReady(msg, threadId_, when.toMSecsSinceEpoch());
    close();
}

void ComposeWindow::onSaveDraft() {
    emit saveDraftRequested(currentMessage(), threadId_, draftId_);
    dirty_ = false;
    statusLabel_->setText(tr("Draft saved."));
    statusLabel_->setStyleSheet(QStringLiteral("color: gray;"));
}

void ComposeWindow::closeEvent(QCloseEvent* e) {
    if (suppressClosePrompt_ || !dirty_) {
        e->accept();
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Save draft?"),
        tr("Save this message as a draft before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    switch (answer) {
        case QMessageBox::Save:
            emit saveDraftRequested(currentMessage(), threadId_, draftId_);
            e->accept();
            break;
        case QMessageBox::Discard:
            e->accept();
            break;
        default:
            e->ignore();
            break;
    }
}

}  // namespace fc::ui
