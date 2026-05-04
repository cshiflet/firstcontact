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
    // Wider default + a sane minimum so wrapped hint text and the longest
    // combo entries ("Inline rich preview (Qt WebEngine, +~80 MB RAM,
    // restart required)") don't get truncated at first paint. Users can
    // still resize larger; we just refuse to go absurdly small.
    resize(720, 560);
    setMinimumSize(640, 480);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* appearanceTitle = new QLabel(tr("<h3 style='margin:0'>Appearance</h3>"), this);
    appearanceTitle->setTextFormat(Qt::RichText);
    root->addWidget(appearanceTitle);

    auto* form = new QFormLayout;
    // Stretch field columns so combos / line edits fill the available
    // dialog width rather than collapsing to their content's intrinsic
    // sizeHint (which on Fusion is just "wide enough for the current
    // item").
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* themeBox = new QComboBox(this);
    themeBox->addItem(tr("Match system"), int(Theme::Mode::Auto));
    themeBox->addItem(tr("Light"),        int(Theme::Mode::Light));
    themeBox->addItem(tr("Dark"),         int(Theme::Mode::Dark));
    themeBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    themeBox->setMinimumWidth(280);
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
    htmlForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    htmlForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* htmlBox = new QComboBox(this);
    htmlBox->addItem(tr("Disabled — sanitized text only"),
                     int(Preferences::HtmlPreview::Disabled));
    htmlBox->addItem(tr("Open in external browser (recommended)"),
                     int(Preferences::HtmlPreview::ExternalBrowser));
    htmlBox->addItem(tr("Inline rich preview (Qt WebEngine, +~80 MB RAM, restart required)"),
                     int(Preferences::HtmlPreview::InlineWebEngine));
    htmlBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    htmlBox->setMinimumWidth(420);
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
    attachForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    attachForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Default download directory: read-only field + Browse… button. Wrap
    // both in a QWidget so the QFormLayout treats them as a single growing
    // field — addRow(QHBoxLayout*) was collapsing the inner widgets to
    // their sizeHint and giving the line edit no horizontal room.
    auto* dirHolder = new QWidget(this);
    auto* dirRow    = new QHBoxLayout(dirHolder);
    dirRow->setContentsMargins(0, 0, 0, 0);
    auto* dirEdit  = new QLineEdit(Preferences::attachmentDir(), dirHolder);
    dirEdit->setReadOnly(true);
    dirEdit->setMinimumWidth(320);
    dirEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* browseBtn = new QPushButton(tr("Browse…"), dirHolder);
    browseBtn->setMinimumWidth(96);
    dirRow->addWidget(dirEdit, /*stretch=*/1);
    dirRow->addWidget(browseBtn);
    attachForm->addRow(tr("Save folder:"), dirHolder);

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

    // ----------------------------------------------------- Conversation view
    auto* convTitle = new QLabel(
        tr("<h3 style='margin:0'>Inbox layout</h3>"), this);
    convTitle->setTextFormat(Qt::RichText);
    root->addWidget(convTitle);

    auto* convForm = new QFormLayout;
    convForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    convForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* convBox = new QCheckBox(
        tr("Group messages by conversation"), this);
    convBox->setChecked(Preferences::conversationView());
    connect(convBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setConversationView(on);
    });
    convForm->addRow(QString(), convBox);
    auto* convHint = new QLabel(tr(
        "When on, the message list shows one row per thread (Gmail-web "
        "default). When off, every message is its own row. Toggling takes "
        "effect on the next label / search refresh."), this);
    convHint->setObjectName(QStringLiteral("FormHint"));
    convHint->setWordWrap(true);
    convForm->addRow(QString(), convHint);
    root->addLayout(convForm);

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
