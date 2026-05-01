#include "ComposeWindow.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace fc::ui {

ComposeWindow::ComposeWindow(const QString& fromAddr,
                             const QString& fromName,
                             QWidget* parent)
    : QWidget(parent, Qt::Window),
      fromAddr_(fromAddr), fromName_(fromName) {
    setWindowTitle(tr("Compose"));
    resize(680, 540);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout;

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

    auto* sendBtn   = new QPushButton(tr("Send"),   this);
    auto* cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(sendBtn,   &QPushButton::clicked, this, &ComposeWindow::onSend);
    connect(cancelBtn, &QPushButton::clicked, this, &QWidget::close);

    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(statusLabel_, 1);
    btnRow->addWidget(sendBtn);
    btnRow->addWidget(cancelBtn);

    root->addLayout(form);
    root->addWidget(bodyEdit_, /*stretch=*/1);
    root->addLayout(btnRow);
}

void ComposeWindow::prefillFrom(const fc::Message& parent, Mode mode) {
    threadId_ = parent.threadId;
    referencesHeader_.clear();
    inReplyToHeader_.clear();

    // Pull a Message-ID from the parent if we can recover it from headers
    // (Phase 4 stores raw headers; in Phase 3 we approximate via a pseudo
    // id derived from parent.id which Gmail accepts via threadId).
    // The In-Reply-To header is best-effort; threadId carries the real
    // continuity to Gmail.

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

    if (mode != Mode::New) {
        const QString header = QStringLiteral(
            "\n\n----- Original message -----\nFrom: %1\nDate: %2\nSubject: %3\n\n%4")
            .arg(parent.fromAddr, QString(), parent.subject, parent.bodyText);
        bodyEdit_->setPlainText(header);
        bodyEdit_->moveCursor(QTextCursor::Start);
    }
}

void ComposeWindow::onSend() {
    fc::util::OutgoingMessage msg;
    msg.fromAddr = fromAddr_;
    msg.fromName = fromName_;
    msg.subject  = subjectEdit_->text();
    msg.bodyText = bodyEdit_->toPlainText();

    auto split = [](const QString& raw) {
        return raw.split(QRegularExpression(QStringLiteral("\\s*,\\s*")),
                         Qt::SkipEmptyParts);
    };
    msg.to = split(toEdit_->text());
    msg.cc = split(ccEdit_->text());

    msg.rfc822InReplyTo  = inReplyToHeader_;
    msg.rfc822References = referencesHeader_;

    if (msg.to.isEmpty()) {
        statusLabel_->setText(tr("Add at least one recipient."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
        return;
    }

    emit composeReady(msg, threadId_);
    close();
}

}  // namespace fc::ui
