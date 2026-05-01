#include "SettingsDialog.h"

#include "Preferences.h"
#include "Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
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
