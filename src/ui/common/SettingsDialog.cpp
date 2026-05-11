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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
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

    // Text scale — covers HiDPI environments where the system DPI is
    // wrong (WSL especially). Stored as a percentage and applied to
    // QApplication::font() at startup; widget metrics fully recompute
    // on next launch.
    auto* scaleBox = new QComboBox(content);
    const QList<QPair<QString, double>> scaleOpts = {
        { tr("100% (system default)"), 1.0 },
        { tr("110%"),                   1.1 },
        { tr("125%"),                   1.25 },
        { tr("150%"),                   1.5 },
        { tr("175%"),                   1.75 },
        { tr("200%"),                   2.0 },
    };
    for (const auto& [label, value] : scaleOpts) {
        scaleBox->addItem(label, value);
    }
    scaleBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    scaleBox->setMinimumWidth(280);
    {
        const double current = Preferences::uiFontScale();
        // Pick the closest option — if a user set something custom via
        // QSettings directly, we still highlight a sensible match.
        int bestIdx = 0;
        double bestDelta = 1e9;
        for (int i = 0; i < scaleBox->count(); ++i) {
            const double delta = qAbs(scaleBox->itemData(i).toDouble() - current);
            if (delta < bestDelta) { bestDelta = delta; bestIdx = i; }
        }
        scaleBox->setCurrentIndex(bestIdx);
    }
    connect(scaleBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [scaleBox](int idx) {
                Preferences::setUiFontScale(scaleBox->itemData(idx).toDouble());
                // Re-apply the active theme — Theme::loadStylesheet
                // bakes the scale into every "font-size" QSS rule at
                // load time, so re-running it picks up the new value
                // without requiring the user to restart. Widget
                // METRICS (toolbar height, list row height) only
                // recompute on the next launch though, so the hint
                // below still calls a restart out for the cleanest
                // result.
                Theme::apply(Theme::currentMode());
            });
    form->addRow(tr("Text scale:"), scaleBox);

    auto* scaleHint = new QLabel(tr(
        "Bumps the application font size on HiDPI displays where the "
        "system-reported DPI doesn't match the physical scale (notably "
        "WSL). <b>Restart FirstContact</b> for the change to fully "
        "apply — text resizes immediately, but toolbar / row heights "
        "only recompute on the next launch."),
        content);
    scaleHint->setObjectName(QStringLiteral("FormHint"));
    scaleHint->setWordWrap(true);
    scaleHint->setTextFormat(Qt::RichText);
    form->addRow(QString(), scaleHint);

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
        "loaded into FirstContact."), content);
    htmlHint->setObjectName(QStringLiteral("FormHint"));
    htmlHint->setWordWrap(true);
    htmlHint->setTextFormat(Qt::RichText);
    htmlForm->addRow(QString(), htmlHint);

    // Image proxy URL — for the "Open with images" path in the reader.
    // Editable so users can swap in another no-log image proxy if they
    // distrust wsrv.nl, leave blank to disable rewriting (then the
    // browser fetches images directly from the marketer's CDN, leaking
    // IP/UA — discouraged), or restore the default at any time.
    auto* proxyEdit = new QLineEdit(Preferences::imageProxyUrlPattern(), content);
    proxyEdit->setMinimumWidth(420);
    proxyEdit->setPlaceholderText(QStringLiteral("https://wsrv.nl/?url={url}"));
    connect(proxyEdit, &QLineEdit::editingFinished, this, [proxyEdit] {
        Preferences::setImageProxyUrlPattern(proxyEdit->text());
    });
    htmlForm->addRow(tr("Image proxy:"), proxyEdit);

    auto* proxyHint = new QLabel(tr(
        "<b>{url}</b> is replaced with the percent-encoded source URL. "
        "Default uses <b>wsrv.nl</b> (a public Cloudflare-fronted image "
        "proxy with a stated no-log policy). With a proxy configured, "
        "the marketer's CDN sees only the proxy's egress IP — never "
        "yours. Leave blank to disable proxying (images load directly, "
        "which leaks your IP and User-Agent)."), content);
    proxyHint->setObjectName(QStringLiteral("FormHint"));
    proxyHint->setWordWrap(true);
    proxyHint->setTextFormat(Qt::RichText);
    htmlForm->addRow(QString(), proxyHint);

    auto* stripPixelsBox = new QCheckBox(
        tr("Strip likely tracking pixels (1×1 / 2×2 images)"), content);
    stripPixelsBox->setChecked(Preferences::stripTrackingPixels());
    connect(stripPixelsBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setStripTrackingPixels(on);
    });
    htmlForm->addRow(QString(), stripPixelsBox);

    auto* stripHint = new QLabel(tr(
        "Tracking pixels are tiny (typically 1×1) images marketers use "
        "to detect when you open their email. With this on, those "
        "<img> tags are removed before the page reaches your browser, "
        "so even the proxy never fetches them — the 'opened' signal is "
        "fully suppressed. <b>Off by default:</b> some legitimate emails "
        "use small spacer images for layout, and stripping them can "
        "subtly distort the result. The image proxy alone already "
        "prevents IP / fingerprint correlation; this checkbox is only "
        "for users who also want to suppress the open-tracking signal."),
        content);
    stripHint->setObjectName(QStringLiteral("FormHint"));
    stripHint->setWordWrap(true);
    stripHint->setTextFormat(Qt::RichText);
    htmlForm->addRow(QString(), stripHint);

    auto* linkBox = new QComboBox(content);
    linkBox->addItem(tr("Label only — hover to see URL"),
                     int(fc::util::LinkDisplayMode::Labeled));
    linkBox->addItem(tr("Label and full URL"),
                     int(fc::util::LinkDisplayMode::FullUrl));
    linkBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    linkBox->setMinimumWidth(280);
    {
        const int idx = linkBox->findData(int(Preferences::linkDisplayMode()));
        if (idx >= 0) linkBox->setCurrentIndex(idx);
    }
    connect(linkBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [linkBox](int idx) {
                Preferences::setLinkDisplayMode(
                    static_cast<fc::util::LinkDisplayMode>(
                        linkBox->itemData(idx).toInt()));
            });
    htmlForm->addRow(tr("Link display:"), linkBox);

    auto* linkHint = new QLabel(tr(
        "When emails arrive in the <code>label [https://url]</code> shape "
        "(common for marketing / transactional mail), the label-only "
        "mode hides the URL behind a hover tooltip / status-bar preview "
        "for cleaner reading. Toggle live with <b>Shift+L</b>."),
        content);
    linkHint->setObjectName(QStringLiteral("FormHint"));
    linkHint->setWordWrap(true);
    linkHint->setTextFormat(Qt::RichText);
    htmlForm->addRow(QString(), linkHint);

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

    // Messages-per-batch: governs both the cache page reads
    // (MessageListModel::pageSize) and the Gmail messages.list page
    // size used during scroll-driven top-up. Smaller values give
    // smoother first-paint at the cost of more server round-trips
    // when the user scrolls deep; larger values fetch more eagerly
    // but pause longer per batch. Progressive load keeps chaining
    // additional batches if a single page doesn't fill the view, so
    // this knob is purely about granularity not about whether
    // back-fill works.
    auto* batchBox = new QSpinBox(content);
    batchBox->setRange(10, 500);
    batchBox->setSingleStep(10);
    batchBox->setValue(Preferences::messagePageSize());
    batchBox->setSuffix(tr(" messages"));
    connect(batchBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { Preferences::setMessagePageSize(v); });
    convForm->addRow(tr("Messages per batch:"), batchBox);
    auto* batchHint = new QLabel(tr(
        "How many messages to read from the local cache per page, and "
        "how many to fetch from Gmail in one top-up request. Range "
        "10–500. Default 50. Applies to the next label refresh."),
        content);
    batchHint->setObjectName(QStringLiteral("FormHint"));
    batchHint->setWordWrap(true);
    convForm->addRow(QString(), batchHint);
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

    // ------------------------------------------------------------- Compose
    auto* composeTitle = new QLabel(
        tr("<h3 style='margin:0'>Compose</h3>"), content);
    composeTitle->setTextFormat(Qt::RichText);
    contentLayout->addWidget(composeTitle);

    auto* composeForm = new QFormLayout;
    composeForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    composeForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Signature: multi-line plain text. Saved on focus-out so users
    // don't lose work if they close the dialog without clicking Close.
    auto* sigEdit = new QPlainTextEdit(content);
    sigEdit->setPlainText(Preferences::signatureText());
    sigEdit->setMinimumHeight(110);
    sigEdit->setPlaceholderText(tr(
        "e.g.\nJane Doe\nSenior Engineer\njane@example.com"));
    sigEdit->setTabChangesFocus(true);
    connect(sigEdit, &QPlainTextEdit::textChanged, this, [sigEdit] {
        Preferences::setSignatureText(sigEdit->toPlainText());
    });
    composeForm->addRow(tr("Signature:"), sigEdit);

    auto* sigHint = new QLabel(tr(
        "Appended to every new message after a <code>-- </code> "
        "delimiter (RFC 3676). Most clients hide signatures when "
        "quoting your reply, so chains stay tidy."),
        content);
    sigHint->setObjectName(QStringLiteral("FormHint"));
    sigHint->setWordWrap(true);
    sigHint->setTextFormat(Qt::RichText);
    composeForm->addRow(QString(), sigHint);

    auto* replyBox = new QComboBox(content);
    replyBox->addItem(tr("Above the original (Gmail / Outlook default)"), true);
    replyBox->addItem(tr("Below the original (mailing-list convention)"), false);
    replyBox->setCurrentIndex(Preferences::replyAboveOriginal() ? 0 : 1);
    connect(replyBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [replyBox](int idx) {
        Preferences::setReplyAboveOriginal(replyBox->itemData(idx).toBool());
    });
    composeForm->addRow(tr("Reply position:"), replyBox);

    auto* quoteBox = new QComboBox(content);
    quoteBox->addItem(tr("Indented blockquote (HTML reply)"),
                      int(Preferences::QuoteStyle::BlockQuote));
    quoteBox->addItem(tr("Greater-than prefix (\"> line\")"),
                      int(Preferences::QuoteStyle::GreaterPrefix));
    const int quoteIdx = quoteBox->findData(int(Preferences::quoteStyle()));
    if (quoteIdx >= 0) quoteBox->setCurrentIndex(quoteIdx);
    connect(quoteBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [quoteBox](int idx) {
        Preferences::setQuoteStyle(static_cast<Preferences::QuoteStyle>(
            quoteBox->itemData(idx).toInt()));
    });
    composeForm->addRow(tr("Quoted text:"), quoteBox);

    auto* quoteHint = new QLabel(tr(
        "<b>Indented blockquote</b> renders as a real <code>&lt;blockquote&gt;</code> "
        "in HTML replies (and as <code>&gt; </code> in plain text). "
        "<b>Greater-than prefix</b> uses <code>&gt; </code> in both — useful "
        "for mailing lists or anyone who reads in a plain-text client."),
        content);
    quoteHint->setObjectName(QStringLiteral("FormHint"));
    quoteHint->setWordWrap(true);
    quoteHint->setTextFormat(Qt::RichText);
    composeForm->addRow(QString(), quoteHint);

    contentLayout->addLayout(composeForm);

    // Storage — small section with a single button that opens the
    // cache-manager dialog. Lives near the end so it sits below the
    // common compose / appearance / html knobs the user is more
    // likely to be tweaking.
    auto* storageTitle = new QLabel(
        tr("<h3 style='margin:0'>Storage</h3>"), content);
    storageTitle->setTextFormat(Qt::RichText);
    contentLayout->addSpacing(12);
    contentLayout->addWidget(storageTitle);

    auto* storageHint = new QLabel(tr(
        "View the on-disk cache footprint per account. From there "
        "you can drop a single account's cache, drop messages older "
        "than N days, reduce a cache to a target size, or wipe "
        "orphaned data left behind by removed accounts."), content);
    storageHint->setObjectName(QStringLiteral("FormHint"));
    storageHint->setWordWrap(true);
    storageHint->setTextFormat(Qt::RichText);
    contentLayout->addWidget(storageHint);

    auto* cacheBtn = new QPushButton(tr("Manage cache…"), content);
    cacheBtn->setCursor(Qt::PointingHandCursor);
    auto* cacheRow = new QHBoxLayout;
    cacheRow->addWidget(cacheBtn);
    cacheRow->addStretch(1);
    contentLayout->addLayout(cacheRow);
    connect(cacheBtn, &QPushButton::clicked, this, [this] {
        emit cacheManagerRequested();
    });

    // Auto-prune master toggle. Per-account caps (age / count / size)
    // live in Cache Manager → Manage… → "Auto-prune: …" actions.
    // When this toggle is on, every successful sync runs the three
    // caps for the affected account; off → caps stay configured but
    // dormant.
    auto* autoPruneForm = new QFormLayout;
    autoPruneForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    autoPruneForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* autoPruneBox = new QCheckBox(
        tr("Auto-prune cache after each sync"), content);
    autoPruneBox->setChecked(Preferences::cacheAutoPrune());
    connect(autoPruneBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setCacheAutoPrune(on);
    });
    autoPruneForm->addRow(QString(), autoPruneBox);
    auto* autoPruneHint = new QLabel(tr(
        "When enabled, FirstContact applies your per-account cache "
        "limits (max age, max messages, max size) after every sync. "
        "Set limits via Manage cache… → Manage… → Auto-prune entries."),
        content);
    autoPruneHint->setObjectName(QStringLiteral("FormHint"));
    autoPruneHint->setWordWrap(true);
    autoPruneForm->addRow(QString(), autoPruneHint);
    contentLayout->addLayout(autoPruneForm);

    // Body compression: zstd-with-trained-dictionary. The toggle
    // gates the auto-train trigger (the SyncService side detects
    // 200+ bodies and emits compressionPromptDue, which MainWindow
    // pops). Off = no prompt, new writes stay plaintext, existing
    // compressed rows still decompress correctly on read. The
    // Recompress button asks MainWindow to rebuild the dictionary
    // and rewrite every body — slow but reclaims disk after a
    // significant change in the user's mailbox content.
    auto* compressionForm = new QFormLayout;
    compressionForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    compressionForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* compressBox = new QCheckBox(
        tr("Compress message bodies (zstd with trained dictionary)"),
        content);
    compressBox->setChecked(Preferences::dbCompression());
    connect(compressBox, &QCheckBox::toggled, this, [](bool on) {
        Preferences::setDbCompression(on);
    });
    compressionForm->addRow(QString(), compressBox);
    auto* compressHint = new QLabel(tr(
        "Once an account has cached enough message bodies (~200) the "
        "app trains a per-account dictionary and rewrites future "
        "writes through the codec. Typical reduction: 3-5× on HTML "
        "bodies. Decompression is microseconds per message; you "
        "won't notice it in the reader."), content);
    compressHint->setObjectName(QStringLiteral("FormHint"));
    compressHint->setWordWrap(true);
    compressionForm->addRow(QString(), compressHint);

    auto* recompressBtn = new QPushButton(
        tr("Recompress now…"), content);
    recompressBtn->setCursor(Qt::PointingHandCursor);
    connect(recompressBtn, &QPushButton::clicked, this, [this] {
        emit recompressRequested();
    });
    auto* recompressRow = new QHBoxLayout;
    recompressRow->addWidget(recompressBtn);
    recompressRow->addStretch(1);
    compressionForm->addRow(QString(), recompressRow);
    auto* recompressHint = new QLabel(tr(
        "Rebuilds the dictionary from a fresh body sample and "
        "rewrites every cached body. Useful after a major change in "
        "your mailbox (lots of new senders / templates). Memory-"
        "frugal but slow: several minutes per gigabyte of cache."),
        content);
    recompressHint->setObjectName(QStringLiteral("FormHint"));
    recompressHint->setWordWrap(true);
    compressionForm->addRow(QString(), recompressHint);

    contentLayout->addLayout(compressionForm);

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
