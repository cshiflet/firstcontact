#include "SettingsDialog.h"

#include "Preferences.h"
#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace fc::ui {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("FirstContact Settings"));
    resize(520, 360);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* appearanceTitle = new QLabel(tr("<h3 style='margin:0'>Appearance</h3>"), this);
    appearanceTitle->setTextFormat(Qt::RichText);
    root->addWidget(appearanceTitle);

    auto* form = new QFormLayout;
    auto* themeBox = new QComboBox(this);
    themeBox->addItem(tr("Match system"), int(Theme::Mode::Auto));
    themeBox->addItem(tr("Light"),        int(Theme::Mode::Light));
    themeBox->addItem(tr("Dark"),         int(Theme::Mode::Dark));
    const int themeIdx = themeBox->findData(int(Theme::currentMode()));
    if (themeIdx >= 0) themeBox->setCurrentIndex(themeIdx);
    connect(themeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,      &SettingsDialog::onThemeChanged);
    form->addRow(tr("Theme:"), themeBox);

    auto* themeHint = new QLabel(tr(
        "Theme changes apply immediately and are remembered next launch."),
        this);
    themeHint->setObjectName(QStringLiteral("FormHint"));
    themeHint->setWordWrap(true);
    form->addRow(QString(), themeHint);
    root->addLayout(form);

    // ------------------------------------------------------------- HTML preview
    auto* htmlTitle = new QLabel(tr("<h3 style='margin:0'>HTML preview</h3>"), this);
    htmlTitle->setTextFormat(Qt::RichText);
    root->addWidget(htmlTitle);

    auto* htmlForm = new QFormLayout;
    auto* htmlBox = new QComboBox(this);
    htmlBox->addItem(tr("Disabled — sanitized text only"),
                     int(Preferences::HtmlPreview::Disabled));
    htmlBox->addItem(tr("Open in external browser (recommended)"),
                     int(Preferences::HtmlPreview::ExternalBrowser));
    htmlBox->addItem(tr("Inline rich preview (Qt WebEngine, +~80 MB RAM, restart required)"),
                     int(Preferences::HtmlPreview::InlineWebEngine));
    const int htmlIdx = htmlBox->findData(int(Preferences::htmlPreview()));
    if (htmlIdx >= 0) htmlBox->setCurrentIndex(htmlIdx);
    connect(htmlBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,     &SettingsDialog::onHtmlPreviewChanged);
    htmlForm->addRow(tr("Mode:"), htmlBox);

    auto* htmlHint = new QLabel(tr(
        "<b>Disabled</b> shows messages as sanitized plain-text + safe HTML "
        "subset only — fastest, lowest memory, lowest fidelity.<br>"
        "<b>External browser</b> serves the original HTML over a one-shot "
        "loopback URL and hands it to your system browser. No Chromium ever "
        "loaded into FirstContact.<br>"
        "<b>Inline rich preview</b> renders the HTML in-place using Qt "
        "WebEngine. Highest fidelity but pre-loads Chromium at startup; "
        "requires restarting the app to take effect."), this);
    htmlHint->setObjectName(QStringLiteral("FormHint"));
    htmlHint->setWordWrap(true);
    htmlHint->setTextFormat(Qt::RichText);
    htmlForm->addRow(QString(), htmlHint);
    root->addLayout(htmlForm);

    // ---------------------------------------------------------- Attachments
    auto* attachTitle = new QLabel(
        tr("<h3 style='margin:0'>Attachments</h3>"), this);
    attachTitle->setTextFormat(Qt::RichText);
    root->addWidget(attachTitle);

    auto* attachForm = new QFormLayout;

    // Default download directory: read-only field + Browse… button.
    auto* dirRow   = new QHBoxLayout;
    auto* dirEdit  = new QLineEdit(Preferences::attachmentDir(), this);
    dirEdit->setReadOnly(true);
    auto* browseBtn = new QPushButton(tr("Browse…"), this);
    dirRow->addWidget(dirEdit, /*stretch=*/1);
    dirRow->addWidget(browseBtn);
    attachForm->addRow(tr("Save folder:"), dirRow);

    connect(browseBtn, &QPushButton::clicked, this, [this, dirEdit] {
        const QString picked = QFileDialog::getExistingDirectory(
            this, tr("Choose default download folder"),
            Preferences::attachmentDir());
        if (picked.isEmpty()) return;
        Preferences::setAttachmentDir(picked);
        dirEdit->setText(picked);
    });

    auto* askBox = new QCheckBox(
        tr("Ask me where to save every time (overrides the folder above)"),
        this);
    askBox->setChecked(Preferences::alwaysAskAttachmentLocation());
    connect(askBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setAlwaysAskAttachmentLocation(on);
    });
    attachForm->addRow(QString(), askBox);

    auto* attachHint = new QLabel(tr(
        "Left-click an attachment chip to download to the folder above. "
        "Right-click for <b>Save as…</b> at any time. With <i>ask every "
        "time</i> turned on, every download (including <b>Download all</b>) "
        "asks for the destination first."), this);
    attachHint->setObjectName(QStringLiteral("FormHint"));
    attachHint->setWordWrap(true);
    attachHint->setTextFormat(Qt::RichText);
    attachForm->addRow(QString(), attachHint);
    root->addLayout(attachForm);

    root->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("primary"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    root->addWidget(buttons);
}

void SettingsDialog::onThemeChanged(int idx) {
    auto* box = qobject_cast<QComboBox*>(sender());
    if (!box) return;
    const auto mode = static_cast<Theme::Mode>(box->itemData(idx).toInt());
    Theme::apply(mode);
}

void SettingsDialog::onHtmlPreviewChanged(int idx) {
    auto* box = qobject_cast<QComboBox*>(sender());
    if (!box) return;
    const auto mode = static_cast<Preferences::HtmlPreview>(
        box->itemData(idx).toInt());
    Preferences::setHtmlPreview(mode);
}

}  // namespace fc::ui
