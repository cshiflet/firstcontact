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
#include <QScrollArea>
#include <QVBoxLayout>

namespace fc::ui {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("FirstContact Settings"));
    // Reasonable default + a sane minimum. Content lives inside a
    // QScrollArea, so even on small screens (or when the user shrinks
    // the dialog) every field stays reachable via vertical scrolling.
    resize(760, 580);
    setMinimumSize(560, 360);

    // The dialog itself uses zero margins; an inner styled QFrame
    // ("DialogShell") fills the entire dialog and paints the visible
    // border. QSS borders directly on QDialog don't reliably paint on
    // top-level windows under Fusion + the WM frame, so the inner
    // QFrame gives us a guaranteed-visible edge between dialog
    // content and whatever's behind it on screen.
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);
    auto* shell = new QFrame(this);
    shell->setObjectName(QStringLiteral("DialogShell"));
    shell->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(shell);

    auto* root = new QVBoxLayout(shell);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* scroll = new QScrollArea(shell);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    root->addWidget(scroll, /*stretch=*/1);

    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(20, 20, 20, 20);
    contentLayout->setSpacing(14);

    auto* appearanceTitle = new QLabel(tr("<h3 style='margin:0'>Appearance</h3>"), content);
    appearanceTitle->setTextFormat(Qt::RichText);
    contentLayout->addWidget(appearanceTitle);

    auto* form = new QFormLayout;
    // Stretch field columns so combos / line edits fill the available
    // dialog width rather than collapsing to their content's intrinsic
    // sizeHint (which on Fusion is just "wide enough for the current
    // item").
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* themeBox = new QComboBox(content);
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
        content);
    themeHint->setObjectName(QStringLiteral("FormHint"));
    themeHint->setWordWrap(true);
    form->addRow(QString(), themeHint);
    contentLayout->addLayout(form);

    // ------------------------------------------------------------- HTML preview
    auto* htmlTitle = new QLabel(tr("<h3 style='margin:0'>HTML preview</h3>"), content);
    htmlTitle->setTextFormat(Qt::RichText);
    contentLayout->addWidget(htmlTitle);

    auto* htmlForm = new QFormLayout;
    htmlForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    htmlForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* htmlBox = new QComboBox(content);
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
        "requires restarting the app to take effect."), content);
    htmlHint->setObjectName(QStringLiteral("FormHint"));
    htmlHint->setWordWrap(true);
    htmlHint->setTextFormat(Qt::RichText);
    htmlForm->addRow(QString(), htmlHint);
    contentLayout->addLayout(htmlForm);

    // ---------------------------------------------------------- Attachments
    auto* attachTitle = new QLabel(
        tr("<h3 style='margin:0'>Attachments</h3>"), content);
    attachTitle->setTextFormat(Qt::RichText);
    contentLayout->addWidget(attachTitle);

    auto* attachForm = new QFormLayout;
    attachForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    attachForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Default download directory: read-only field + Browse… button. Wrap
    // both in a QWidget so the QFormLayout treats them as a single growing
    // field — addRow(QHBoxLayout*) was collapsing the inner widgets to
    // their sizeHint and giving the line edit no horizontal room.
    auto* dirHolder = new QWidget(content);
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

    auto* attachHint = new QLabel(tr(
        "Left-click an attachment to <b>open</b> it from a per-session "
        "temp folder (no permanent save). Right-click for <b>Save as…</b> "
        "— the picker starts in the folder above. <b>Download all</b> "
        "writes every attachment to a folder you choose."), content);
    attachHint->setObjectName(QStringLiteral("FormHint"));
    attachHint->setWordWrap(true);
    attachHint->setTextFormat(Qt::RichText);
    attachForm->addRow(QString(), attachHint);
    contentLayout->addLayout(attachForm);

    // ----------------------------------------------------- Conversation view
    auto* convTitle = new QLabel(
        tr("<h3 style='margin:0'>Inbox layout</h3>"), content);
    convTitle->setTextFormat(Qt::RichText);
    contentLayout->addWidget(convTitle);

    auto* convForm = new QFormLayout;
    convForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    convForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* convBox = new QCheckBox(
        tr("Group messages by conversation"), content);
    convBox->setChecked(Preferences::conversationView());
    connect(convBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setConversationView(on);
    });
    convForm->addRow(QString(), convBox);
    auto* convHint = new QLabel(tr(
        "When on, the message list shows one row per thread (Gmail-web "
        "default). When off, every message is its own row. Toggling takes "
        "effect on the next label / search refresh."), content);
    convHint->setObjectName(QStringLiteral("FormHint"));
    convHint->setWordWrap(true);
    convForm->addRow(QString(), convHint);

    // Toolbar layout: text-beside-icon vs. icon-only. Tooltips stay
    // populated either way, so the labels are still discoverable in
    // icon-only mode.
    auto* toolbarBox = new QCheckBox(
        tr("Show text labels next to toolbar icons"), content);
    toolbarBox->setChecked(Preferences::toolbarShowText());
    connect(toolbarBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setToolbarShowText(on);
    });
    convForm->addRow(QString(), toolbarBox);
    contentLayout->addLayout(convForm);

    // ------------------------------------------------------------- Labels
    auto* labelsTitle = new QLabel(
        tr("<h3 style='margin:0'>Label colors</h3>"), content);
    labelsTitle->setTextFormat(Qt::RichText);
    contentLayout->addWidget(labelsTitle);

    auto* labelsForm = new QFormLayout;
    labelsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    labelsForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* sidebarColorsBox = new QCheckBox(
        tr("Show colored swatches next to sidebar labels"), content);
    sidebarColorsBox->setChecked(Preferences::sidebarLabelColors());
    connect(sidebarColorsBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setSidebarLabelColors(on);
    });
    labelsForm->addRow(QString(), sidebarColorsBox);

    auto* listPillsBox = new QCheckBox(
        tr("Show colored label pills on messages in the list"), content);
    listPillsBox->setChecked(Preferences::messageListLabelPills());
    connect(listPillsBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setMessageListLabelPills(on);
    });
    labelsForm->addRow(QString(), listPillsBox);

    auto* labelsHint = new QLabel(tr(
        "Colors are pulled from your Gmail account (Settings → Labels in "
        "Gmail web). Labels with no color set just don't get a swatch / "
        "pill — system labels (Inbox, Starred, categories) are suppressed "
        "from the message-list pills since they'd appear on every row."),
        content);
    labelsHint->setObjectName(QStringLiteral("FormHint"));
    labelsHint->setWordWrap(true);
    labelsHint->setTextFormat(Qt::RichText);
    labelsForm->addRow(QString(), labelsHint);
    contentLayout->addLayout(labelsForm);

    contentLayout->addStretch(1);

    // Buttons live OUTSIDE the scroll area so they're always visible at
    // the bottom regardless of how far the content has been scrolled.
    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(20, 12, 20, 16);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("primary"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    buttonRow->addStretch(1);
    buttonRow->addWidget(buttons);
    root->addLayout(buttonRow);
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
