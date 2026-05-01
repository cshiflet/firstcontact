#include "SetupWizard.h"

#include "auth/ClientConfig.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

namespace fc::ui {

SetupWizard::SetupWizard(fc::auth::ClientConfig* config, QWidget* parent)
    : QDialog(parent), config_(config) {
    setWindowTitle(tr("Welcome to FirstContact"));
    setModal(true);
    resize(580, 460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);

    auto* title = new QLabel(tr("<h2 style='margin:0;'>Sign in to your Gmail account</h2>"), this);
    title->setTextFormat(Qt::RichText);
    layout->addWidget(title);

    auto* subtitle = new QLabel(tr(
        "FirstContact talks to Gmail directly using your own Google Cloud "
        "OAuth client — no third-party server is involved."), this);
    subtitle->setObjectName(QStringLiteral("FormHint"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto* steps = new QLabel(this);
    steps->setWordWrap(true);
    steps->setOpenExternalLinks(true);
    steps->setText(tr(
        "<p style='margin:6px 0;'><b>One-time setup (≈3 minutes):</b></p>"
        "<ol style='margin:0; padding-left:20px;'>"
        "<li>Open the Google Cloud Console and create a project (or pick one).</li>"
        "<li>Enable the <b>Gmail API</b> for the project.</li>"
        "<li>Configure the OAuth consent screen (External, your email as a test user).</li>"
        "<li>Create credentials → <b>OAuth client ID</b> → application type "
        "<b>Desktop app</b>. No client secret needed.</li>"
        "<li>Paste the resulting <b>Client ID</b> below.</li>"
        "</ol>"));
    layout->addWidget(steps);

    auto* openBtn = new QPushButton(tr("Open Google Cloud Console"), this);
    openBtn->setObjectName(QStringLiteral("link"));
    openBtn->setCursor(Qt::PointingHandCursor);
    connect(openBtn, &QPushButton::clicked, this, &SetupWizard::onOpenConsole);
    layout->addWidget(openBtn, 0, Qt::AlignLeft);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    idEdit_ = new QLineEdit(this);
    idEdit_->setPlaceholderText(
        QStringLiteral("123456789012-abcdefg.apps.googleusercontent.com"));
    idEdit_->setText(config_->clientId());
    form->addRow(tr("OAuth Client ID"), idEdit_);
    layout->addLayout(form);

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    layout->addStretch(1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    if (auto* save = buttons->button(QDialogButtonBox::Save)) {
        save->setObjectName(QStringLiteral("primary"));
        save->setText(tr("Continue"));
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &SetupWizard::onSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SetupWizard::onOpenConsole() {
    QDesktopServices::openUrl(QUrl(QStringLiteral(
        "https://console.cloud.google.com/apis/credentials")));
}

void SetupWizard::onSave() {
    const QString id = idEdit_->text().trimmed();
    // Real-world Google OAuth client IDs look like
    //   123456789012-1abc-2def-3ghi.apps.googleusercontent.com
    // i.e. digits, then a hyphen, then a hash made of letters / digits /
    // hyphens / underscores, then the fixed apps.googleusercontent.com suffix.
    // The previous regex disallowed hyphens in the hash and silently rejected
    // legitimate IDs.
    static const QRegularExpression re(
        QStringLiteral(R"(^[0-9]+-[A-Za-z0-9_-]+\.apps\.googleusercontent\.com$)"));
    if (!re.match(id).hasMatch()) {
        statusLabel_->setText(tr(
            "That doesn't look like a Google OAuth client ID. It should be "
            "of the form <code>123456789012-abcdef…apps.googleusercontent.com</code>."));
        return;
    }
    config_->setClientId(id);
    accept();
}

}  // namespace fc::ui
