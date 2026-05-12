#include "ui/common/CompressionProgressDialog.h"

#include "account/AccountManager.h"
#include "cache/BodyCompressionWorker.h"
#include "util/Format.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace fc::ui {

CompressionProgressDialog::CompressionProgressDialog(
        fc::account::AccountManager* accounts,
        const QString& accountId,
        QWidget* parent)
    : QDialog(parent), accounts_(accounts), accountId_(accountId) {
    setWindowTitle(tr("Compressing message database"));
    setModal(true);
    setSizeGripEnabled(true);
    setMinimumSize(560, 360);
    resize(620, 420);
    setWindowFlags(windowFlags()
                    | Qt::WindowMaximizeButtonHint
                    | Qt::WindowMinimizeButtonHint);

    if (accounts_) {
        beforeBytes_ = accounts_->cacheSizeFor(accountId_);
    }

    QString email;
    if (accounts_) email = accounts_->accountById(accountId_).email;
    if (email.isEmpty()) email = accountId_;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(10);

    headerLabel_ = new QLabel(email, this);
    {
        QFont f = headerLabel_->font();
        f.setBold(true);
        headerLabel_->setFont(f);
    }
    root->addWidget(headerLabel_);

    beforeLabel_ = new QLabel(
        tr("Before: %1").arg(fc::util::humanBytes(beforeBytes_)), this);
    afterLabel_  = new QLabel(tr("After: —"), this);
    root->addWidget(beforeLabel_);
    root->addWidget(afterLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 0);   // indeterminate until first progress signal
    progressBar_->setTextVisible(true);
    root->addWidget(progressBar_);

    statusLabel_ = new QLabel(tr("Starting compression…"), this);
    statusLabel_->setWordWrap(true);
    root->addWidget(statusLabel_);

    errorText_ = new QPlainTextEdit(this);
    errorText_->setReadOnly(true);
    errorText_->setVisible(false);
    errorText_->setMaximumBlockCount(200);
    errorText_->setMinimumHeight(140);
    errorText_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    errorText_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { color: palette(text); "
        "background: rgba(220, 53, 69, 0.10); "
        "border: 1px solid rgba(220, 53, 69, 0.40); "
        "padding: 6px; }"));
    root->addWidget(errorText_, 1);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeBtn_  = btns->button(QDialogButtonBox::Close);
    closeBtn_->setEnabled(false);   // user can't dismiss until done/failed
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(btns);
}

void CompressionProgressDialog::attachWorker(
        fc::cache::BodyCompressionWorker* worker) {
    worker_ = worker;
    if (!worker_) return;
    connect(worker_, &fc::cache::BodyCompressionWorker::progress,
            this,    &CompressionProgressDialog::onProgress);
    connect(worker_, &fc::cache::BodyCompressionWorker::finished,
            this,    &CompressionProgressDialog::onFinished);
    connect(worker_, &fc::cache::BodyCompressionWorker::failed,
            this,    &CompressionProgressDialog::onFailed);
}

void CompressionProgressDialog::onProgress(const QString& aid,
                                              int done, int total) {
    if (aid != accountId_ || finished_) return;
    if (total > 0) {
        if (progressBar_->maximum() != total) {
            progressBar_->setRange(0, total);
        }
        progressBar_->setValue(done);
        statusLabel_->setText(tr("Rewriting message %1 of %2…")
                                .arg(QLocale().toString(done),
                                      QLocale().toString(total)));
    } else {
        // Prep phase (sampling / training / VACUUM) — no quantified total.
        progressBar_->setRange(0, 0);
        statusLabel_->setText(tr("Preparing…"));
    }
}

void CompressionProgressDialog::onFinished(const QString& aid,
                                              int rewroteCount,
                                              qint64 savedBytes) {
    if (aid != accountId_) return;

    // Re-query for the post-VACUUM size rather than trusting the
    // worker's running delta — VACUUM can release extra pages.
    qint64 afterBytes = accounts_ ? accounts_->cacheSizeFor(accountId_)
                                   : beforeBytes_ - savedBytes;
    afterLabel_->setText(tr("After: %1 (−%2)")
                            .arg(fc::util::humanBytes(afterBytes),
                                  fc::util::humanBytes(qMax<qint64>(0, savedBytes))));

    if (progressBar_->maximum() <= 0) progressBar_->setRange(0, 1);
    progressBar_->setValue(progressBar_->maximum());

    if (rewroteCount == 0) {
        statusLabel_->setText(tr("Nothing to do — no message bodies "
                                   "needed recompression."));
    } else {
        statusLabel_->setText(tr("Done: %n message(s) rewritten.",
                                   "", rewroteCount));
    }
    markFinished();
}

void CompressionProgressDialog::onFailed(const QString& aid,
                                            const QString& reason) {
    if (aid != accountId_) return;
    // Make sure the user can actually see the error text — the
    // dialog opens compact for the happy path, but a multi-line
    // failure message needs room to wrap.
    if (height() < 360) resize(width(), 360);
    if (width()  < 560) resize(560, height());
    progressBar_->setRange(0, 1);
    progressBar_->setValue(0);
    statusLabel_->setText(tr("Compression failed."));
    errorText_->setPlainText(reason);
    errorText_->setVisible(true);
    markFinished();
}

void CompressionProgressDialog::markFinished() {
    finished_ = true;
    if (closeBtn_) {
        closeBtn_->setEnabled(true);
        closeBtn_->setDefault(true);
        closeBtn_->setFocus();
    }
}

}  // namespace fc::ui
