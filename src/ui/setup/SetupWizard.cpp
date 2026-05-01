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
    setWindowTitle(tr("FirstContact — First-Run Setup"));
    setModal(true);
    resize(560, 360);

    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("<h2>Sign in to your Gmail account</h2>"), this);
    layout->addWidget(title);

    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setOpenExternalLinks(true);
    intro->setText(tr(
        "FirstContact talks to Gmail directly using your own Google Cloud "
        "OAuth client — no third-party server is involved.<br><br>"
        "<b>One-time setup (≈3 minutes):</b>"
        "<ol>"
        "<li>Open the Google Cloud Console and create a project (or pick one).</li>"
        "<li>Enable the <b>Gmail API</b> for the project.</li>"
        "<li>Configure the OAuth consent screen (External, your email as a test user).</li>"
        "<li>Create credentials → <b>OAuth client ID</b> → application type "
        "<b>Desktop app</b>. No client secret needed.</li>"
        "<li>Paste the resulting <b>Client ID</b> below.</li>"
        "</ol>"));
    layout->addWidget(intro);

    auto* openBtn = new QPushButton(tr("Open Google Cloud Console"), this);
    connect(openBtn, &QPushButton::clicked, this, &SetupWizard::onOpenConsole);
    layout->addWidget(openBtn);

    auto* form = new QFormLayout;
    idEdit_ = new QLineEdit(this);
    idEdit_->setPlaceholderText(QStringLiteral("123456789012-abcdefg.apps.googleusercontent.com"));
    idEdit_->setText(config_->clientId());
    form->addRow(tr("OAuth Client ID:"), idEdit_);
    layout->addLayout(form);

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
    layout->addWidget(statusLabel_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
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
