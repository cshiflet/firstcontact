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
#include <QTabWidget>
#include <QVBoxLayout>

namespace fc::ui {

namespace {

// Builds the section-heading row inside a tab. Tabs handle the broad
// "Reading vs Composing" categorization; subheadings still help when
// a tab bundles two related-but-distinct groups (e.g. HTML preview +
// Attachments both belong under Reading).
QLabel* makeSubheading(QWidget* parent, const QString& text) {
    auto* h = new QLabel(QStringLiteral("<h3 style='margin:0'>%1</h3>")
                            .arg(text), parent);
    h->setTextFormat(Qt::RichText);
    return h;
}

// Hint label below a form row. Styled smaller / dimmer via the QSS
// rule on objectName="FormHint". Pass wide=true when the hint sits
// between a wide row (e.g. a checkbox) and a narrow row (e.g. a
// label+spinbox+stretch HBox): QFormLayout sizes the field column
// to the narrow row's sizeHint, collapsing the hint unless it
// declares an Expanding policy + minimum width.
QLabel* makeHint(QWidget* parent, const QString& text,
                  Qt::TextFormat fmt = Qt::PlainText,
                  bool wide = false) {
    auto* l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("FormHint"));
    l->setWordWrap(true);
    l->setTextFormat(fmt);
    if (wide) {
        l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        l->setMinimumWidth(360);
    }
    return l;
}

// Standard QFormLayout factory: expanding fields + left-aligned labels.
// Every form section in this dialog uses the same shape, so factor it.
QFormLayout* makeForm() {
    auto* f = new QFormLayout;
    f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    f->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return f;
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("FirstContact Settings"));
    resize(760, 580);
    setMinimumSize(560, 360);

    // Some window managers gate diagonal resize handles on the
    // maximize-button hint being set. Add the maximize + minimize
    // hints explicitly so the dialog is resizable under every WM,
    // and toss in a corner size grip as a belt-and-braces fallback.
    setWindowFlags(windowFlags()
                    | Qt::WindowMinimizeButtonHint
                    | Qt::WindowMaximizeButtonHint);
    setSizeGripEnabled(true);

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

    // Tabs replaced the single tall scroll area. Each tab still uses
    // its own QScrollArea so individual tabs can scroll if they grow
    // — but in practice no tab is tall enough to need it on a
    // reasonable display.
    auto* tabs = new QTabWidget(shell);
    tabs->setDocumentMode(true);
    root->addWidget(tabs, /*stretch=*/1);

    // Helper: builds a scroll-wrapped tab page and returns its content
    // widget + outer vertical layout. Widgets parented to `content`
    // inherit the right life-cycle; widgets added to `layout` flow
    // top-to-bottom inside the scroll area.
    struct TabPage { QWidget* content; QVBoxLayout* layout; };
    auto addTab = [tabs](const QString& title) -> TabPage {
        auto* scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        auto* content = new QWidget;
        scroll->setWidget(content);
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(20, 20, 20, 20);
        layout->setSpacing(14);
        tabs->addTab(scroll, title);
        return { content, layout };
    };

    // ============================================================
    // Tab: General  (Appearance + Inbox layout + Label colors)
    // ============================================================
    auto general = addTab(tr("General"));
    {
        QWidget* content     = general.content;
        QVBoxLayout* outer   = general.layout;

        outer->addWidget(makeSubheading(content, tr("Appearance")));
        auto* form = makeForm();

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
        form->addRow(QString(), makeHint(content, tr(
            "Theme changes apply immediately and are remembered next launch.")));

        // Text scale — covers HiDPI environments where the system DPI
        // is wrong (WSL especially). Stored as a percentage and applied
        // to QApplication::font() at startup; widget metrics fully
        // recompute on next launch.
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
            int bestIdx = 0;
            double bestDelta = 1e9;
            for (int i = 0; i < scaleBox->count(); ++i) {
                const double delta =
                    qAbs(scaleBox->itemData(i).toDouble() - current);
                if (delta < bestDelta) { bestDelta = delta; bestIdx = i; }
            }
            scaleBox->setCurrentIndex(bestIdx);
        }
        connect(scaleBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [scaleBox](int idx) {
                    Preferences::setUiFontScale(
                        scaleBox->itemData(idx).toDouble());
                    Theme::apply(Theme::currentMode());
                });
        form->addRow(tr("Text scale:"), scaleBox);
        form->addRow(QString(), makeHint(content, tr(
            "Bumps the application font size on HiDPI displays where "
            "the system-reported DPI doesn't match the physical scale "
            "(notably WSL). <b>Restart FirstContact</b> for the change "
            "to fully apply — text resizes immediately, but toolbar / "
            "row heights only recompute on the next launch."),
            Qt::RichText));
        outer->addLayout(form);

        outer->addSpacing(8);
        outer->addWidget(makeSubheading(content, tr("Inbox layout")));
        auto* convForm = makeForm();
        auto* convBox = new QCheckBox(
            tr("Group messages by conversation"), content);
        convBox->setChecked(Preferences::conversationView());
        connect(convBox, &QCheckBox::toggled, this, [](bool on) {
            Preferences::setConversationView(on);
        });
        convForm->addRow(QString(), convBox);
        convForm->addRow(QString(), makeHint(content, tr(
            "When on, the message list shows one row per thread "
            "(Gmail-web default). When off, every message is its own "
            "row. Toggling takes effect on the next label / search "
            "refresh.")));

        auto* toolbarBox = new QCheckBox(
            tr("Show text labels next to toolbar icons"), content);
        toolbarBox->setChecked(Preferences::toolbarShowText());
        connect(toolbarBox, &QCheckBox::toggled, this, [](bool on) {
            Preferences::setToolbarShowText(on);
        });
        convForm->addRow(QString(), toolbarBox);
        outer->addLayout(convForm);

        outer->addSpacing(8);
        outer->addWidget(makeSubheading(content, tr("Label colors")));
        auto* labelsForm = makeForm();
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

        labelsForm->addRow(QString(), makeHint(content, tr(
            "Colors are pulled from your Gmail account (Settings → "
            "Labels in Gmail web). Labels with no color set just don't "
            "get a swatch / pill — system labels (Inbox, Starred, "
            "categories) are suppressed from the message-list pills "
            "since they'd appear on every row.")));
        outer->addLayout(labelsForm);
        outer->addStretch(1);
    }

    // ============================================================
    // Tab: Reading  (HTML preview + Attachments)
    // ============================================================
    auto reading = addTab(tr("Reading"));
    {
        QWidget* content   = reading.content;
        QVBoxLayout* outer = reading.layout;

        outer->addWidget(makeSubheading(content, tr("HTML preview")));
        auto* htmlForm = makeForm();
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
        htmlForm->addRow(QString(), makeHint(content, tr(
            "<b>Disabled</b> shows messages as sanitized plain-text + "
            "safe HTML subset only — fastest, lowest memory, lowest "
            "fidelity.<br><b>External browser</b> serves the original "
            "HTML over a one-shot loopback URL and hands it to your "
            "system browser. No Chromium ever loaded into FirstContact."),
            Qt::RichText));

        auto* proxyEdit = new QLineEdit(Preferences::imageProxyUrlPattern(),
                                          content);
        proxyEdit->setMinimumWidth(420);
        proxyEdit->setPlaceholderText(
            QStringLiteral("https://wsrv.nl/?url={url}"));
        connect(proxyEdit, &QLineEdit::editingFinished, this, [proxyEdit] {
            Preferences::setImageProxyUrlPattern(proxyEdit->text());
        });
        htmlForm->addRow(tr("Image proxy:"), proxyEdit);
        htmlForm->addRow(QString(), makeHint(content, tr(
            "<b>{url}</b> is replaced with the percent-encoded source "
            "URL. Default uses <b>wsrv.nl</b> (a public Cloudflare-"
            "fronted image proxy with a stated no-log policy). With a "
            "proxy configured, the marketer's CDN sees only the "
            "proxy's egress IP — never yours. Leave blank to disable "
            "proxying (images load directly, which leaks your IP and "
            "User-Agent)."),
            Qt::RichText));

        auto* stripPixelsBox = new QCheckBox(
            tr("Strip likely tracking pixels (1×1 / 2×2 images)"), content);
        stripPixelsBox->setChecked(Preferences::stripTrackingPixels());
        connect(stripPixelsBox, &QCheckBox::toggled, this, [](bool on) {
            Preferences::setStripTrackingPixels(on);
        });
        htmlForm->addRow(QString(), stripPixelsBox);
        htmlForm->addRow(QString(), makeHint(content, tr(
            "Tracking pixels are tiny (typically 1×1) images marketers "
            "use to detect when you open their email. With this on, "
            "those <img> tags are removed before the page reaches your "
            "browser, so even the proxy never fetches them — the "
            "'opened' signal is fully suppressed. <b>Off by default:</b> "
            "some legitimate emails use small spacer images for "
            "layout, and stripping them can subtly distort the result. "
            "The image proxy alone already prevents IP / fingerprint "
            "correlation; this checkbox is only for users who also "
            "want to suppress the open-tracking signal."),
            Qt::RichText));

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
        htmlForm->addRow(QString(), makeHint(content, tr(
            "When emails arrive in the <code>label [https://url]</code> "
            "shape (common for marketing / transactional mail), the "
            "label-only mode hides the URL behind a hover tooltip / "
            "status-bar preview for cleaner reading. Toggle live with "
            "<b>Shift+L</b>."),
            Qt::RichText));
        outer->addLayout(htmlForm);

        outer->addSpacing(8);
        outer->addWidget(makeSubheading(content, tr("Attachments")));
        auto* attachForm = makeForm();

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
        attachForm->addRow(QString(), makeHint(content, tr(
            "Left-click an attachment to <b>open</b> it from a per-"
            "session temp folder (no permanent save). Right-click for "
            "<b>Save as…</b> — the picker starts in the folder above. "
            "<b>Download all</b> writes every attachment to a folder "
            "you choose."),
            Qt::RichText));
        outer->addLayout(attachForm);
        outer->addStretch(1);
    }

    // ============================================================
    // Tab: Composing  (Signature, Reply position, Quote style)
    // ============================================================
    auto composing = addTab(tr("Composing"));
    {
        QWidget* content   = composing.content;
        QVBoxLayout* outer = composing.layout;
        auto* composeForm = makeForm();

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
        composeForm->addRow(QString(), makeHint(content, tr(
            "Appended to every new message after a <code>-- </code> "
            "delimiter (RFC 3676). Most clients hide signatures when "
            "quoting your reply, so chains stay tidy."),
            Qt::RichText));

        auto* replyBox = new QComboBox(content);
        replyBox->addItem(
            tr("Above the original (Gmail / Outlook default)"), true);
        replyBox->addItem(
            tr("Below the original (mailing-list convention)"), false);
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

        composeForm->addRow(QString(), makeHint(content, tr(
            "<b>Indented blockquote</b> renders as a real "
            "<code>&lt;blockquote&gt;</code> in HTML replies (and as "
            "<code>&gt; </code> in plain text). <b>Greater-than prefix"
            "</b> uses <code>&gt; </code> in both — useful for "
            "mailing lists or anyone who reads in a plain-text client."),
            Qt::RichText));
        outer->addLayout(composeForm);
        outer->addStretch(1);
    }

    // ============================================================
    // Tab: Sync  (Low-bandwidth, Messages-per-batch, Background prefetch)
    // ============================================================
    auto sync = addTab(tr("Sync"));
    {
        QWidget* content   = sync.content;
        QVBoxLayout* outer = sync.layout;
        auto* syncForm = makeForm();

        auto* lowBwBox = new QCheckBox(
            tr("Low-bandwidth mode (metadata only; fetch bodies on open)"),
            content);
        lowBwBox->setChecked(Preferences::lowBandwidthMode());
        syncForm->addRow(QString(), lowBwBox);
        syncForm->addRow(QString(), makeHint(content, tr(
            "Sync pulls only message headers / labels. The full body "
            "is fetched the first time you open each message, with a "
            "brief \"Fetching message body…\" placeholder. Saves "
            "bandwidth at the cost of a slower first open. While "
            "enabled, background prefetch is suspended (its saved "
            "setting is preserved).")));

        // Messages-per-batch spinbox + caption. Packed into a single
        // horizontal strip so the form's label column stays empty
        // for every row — keeps the surrounding checkboxes flush-
        // left. Lives on Sync (not General) because the value
        // governs how many ids each top-up requests from Gmail.
        auto* batchBox = new QSpinBox(content);
        batchBox->setRange(10, 500);
        batchBox->setSingleStep(10);
        batchBox->setValue(Preferences::messagePageSize());
        batchBox->setSuffix(tr(" messages"));
        connect(batchBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [](int v) { Preferences::setMessagePageSize(v); });
        auto* batchRow = new QHBoxLayout;
        batchRow->setContentsMargins(0, 0, 0, 0);
        batchRow->addWidget(new QLabel(tr("Messages per batch:"), content));
        batchRow->addWidget(batchBox);
        batchRow->addStretch(1);
        syncForm->addRow(QString(), batchRow);
        syncForm->addRow(QString(), makeHint(content, tr(
            "How many messages to read from the local cache per page, "
            "and how many to fetch from Gmail in one top-up request. "
            "Range 10–500. Default 50. Applies to the next label "
            "refresh.")));

        auto* crawlBox = new QCheckBox(
            tr("Background prefetch: slowly fill the cache for every label"),
            content);
        crawlBox->setChecked(Preferences::backgroundCrawl());
        syncForm->addRow(QString(), crawlBox);

        // Visible only while low-bandwidth mode is gating prefetch
        // off. Wide hint — the tick-interval row below would collapse
        // its column otherwise.
        auto* prefetchGatedHint = makeHint(content, tr(
            "(Disable Low-bandwidth mode to use background prefetch)"),
            Qt::PlainText, /*wide=*/true);
        syncForm->addRow(QString(), prefetchGatedHint);

        // Tick interval: label + spinbox packed into a single field-
        // column row so the form's label column stays empty and the
        // surrounding checkboxes stay flush-left.
        auto* crawlIntervalLabel = new QLabel(tr("Tick interval"), content);
        auto* crawlIntervalSpin  = new QSpinBox(content);
        crawlIntervalSpin->setRange(1, 600);
        crawlIntervalSpin->setSuffix(tr(" sec"));
        crawlIntervalSpin->setValue(Preferences::backgroundCrawlIntervalSec());
        auto* crawlIntervalRow = new QHBoxLayout;
        crawlIntervalRow->setContentsMargins(0, 0, 0, 0);
        crawlIntervalRow->addWidget(crawlIntervalLabel);
        crawlIntervalRow->addWidget(crawlIntervalSpin);
        crawlIntervalRow->addStretch(1);
        syncForm->addRow(QString(), crawlIntervalRow);

        auto* crawlHint = makeHint(content, tr(
            "When enabled, one un-exhausted label advances by a "
            "single page each tick. Foreground sync and user-"
            "requested top-ups always preempt — the crawler never "
            "races with interactive work. Use Reset progress to "
            "revisit labels that have already been walked end-to-end."));
        syncForm->addRow(QString(), crawlHint);

        auto* crawlResetBtn = new QPushButton(
            tr("Reset crawl progress"), content);
        crawlResetBtn->setCursor(Qt::PointingHandCursor);
        auto* crawlResetRow = new QHBoxLayout;
        crawlResetRow->addWidget(crawlResetBtn);
        crawlResetRow->addStretch(1);
        syncForm->addRow(QString(), crawlResetRow);

        // The prefetch widget tree is enabled only when (a) the
        // user's prefetch preference is on AND (b) low-bandwidth
        // mode is off. Low-bandwidth turning the trio off must
        // NOT clobber the user's saved preference — only the live
        // enabled state. SyncService::tickBackgroundCrawl applies
        // the same gate at runtime so the timer simply no-ops
        // until low-bandwidth flips back off.
        auto applyPrefetchGate =
            [crawlBox, crawlIntervalLabel, crawlIntervalSpin, crawlResetBtn,
             crawlHint, prefetchGatedHint, lowBwBox]() {
                const bool gatedByLowBw = lowBwBox->isChecked();
                crawlBox->setEnabled(!gatedByLowBw);
                const bool live = !gatedByLowBw && crawlBox->isChecked();
                crawlIntervalLabel->setEnabled(live);
                crawlIntervalSpin->setEnabled(live);
                crawlResetBtn->setEnabled(!gatedByLowBw);
                crawlHint->setEnabled(!gatedByLowBw);
                prefetchGatedHint->setVisible(gatedByLowBw);
            };
        applyPrefetchGate();

        connect(lowBwBox, &QCheckBox::toggled, this,
            [this, applyPrefetchGate](bool on) {
                Preferences::setLowBandwidthMode(on);
                applyPrefetchGate();
                // Re-push to SyncService too: even though the saved
                // preference is unchanged, the live gate flipped and
                // any running tick should observe it next pulse.
                emit backgroundCrawlSettingsChanged();
            });
        connect(crawlBox, &QCheckBox::toggled, this,
            [this, applyPrefetchGate](bool on) {
                Preferences::setBackgroundCrawl(on);
                applyPrefetchGate();
                emit backgroundCrawlSettingsChanged();
            });
        connect(crawlIntervalSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int v) {
                Preferences::setBackgroundCrawlIntervalSec(v);
                emit backgroundCrawlSettingsChanged();
            });
        connect(crawlResetBtn, &QPushButton::clicked, this, [this] {
            emit backgroundCrawlResetRequested();
        });
        outer->addLayout(syncForm);
        outer->addStretch(1);
    }

    // ============================================================
    // Tab: Storage  (Manage cache, Auto-prune, Compression)
    // ============================================================
    auto storage = addTab(tr("Storage"));
    {
        QWidget* content   = storage.content;
        QVBoxLayout* outer = storage.layout;

        outer->addWidget(makeSubheading(content, tr("Cache")));
        outer->addWidget(makeHint(content, tr(
            "View the on-disk cache footprint per account. From there "
            "you can drop a single account's cache, drop messages "
            "older than N days, reduce a cache to a target size, or "
            "wipe orphaned data left behind by removed accounts."),
            Qt::RichText));
        auto* cacheBtn = new QPushButton(tr("Manage cache…"), content);
        cacheBtn->setCursor(Qt::PointingHandCursor);
        auto* cacheRow = new QHBoxLayout;
        cacheRow->addWidget(cacheBtn);
        cacheRow->addStretch(1);
        outer->addLayout(cacheRow);
        connect(cacheBtn, &QPushButton::clicked, this, [this] {
            emit cacheManagerRequested();
        });

        outer->addSpacing(8);
        auto* autoPruneForm = makeForm();
        auto* autoPruneBox = new QCheckBox(
            tr("Auto-prune cache after each sync"), content);
        autoPruneBox->setChecked(Preferences::cacheAutoPrune());
        connect(autoPruneBox, &QCheckBox::toggled, this, [](bool on) {
            Preferences::setCacheAutoPrune(on);
        });
        autoPruneForm->addRow(QString(), autoPruneBox);
        autoPruneForm->addRow(QString(), makeHint(content, tr(
            "When enabled, FirstContact applies your per-account "
            "cache limits (max age, max messages, max size) after "
            "every sync. Set limits via Manage cache… → Manage… → "
            "Auto-prune entries.")));
        outer->addLayout(autoPruneForm);

        outer->addSpacing(8);
        outer->addWidget(makeSubheading(content, tr("Compression")));
        auto* compressionForm = makeForm();
        auto* compressBox = new QCheckBox(
            tr("Compress message bodies (zstd with trained dictionary)"),
            content);
        compressBox->setChecked(Preferences::dbCompression());
        connect(compressBox, &QCheckBox::toggled, this, [](bool on) {
            Preferences::setDbCompression(on);
        });
        compressionForm->addRow(QString(), compressBox);
        compressionForm->addRow(QString(), makeHint(content, tr(
            "Once an account has cached enough message bodies (~200) "
            "the app trains a per-account dictionary and rewrites "
            "future writes through the codec. Typical reduction: 3-5× "
            "on HTML bodies. Decompression is microseconds per "
            "message; you won't notice it in the reader.")));

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
        compressionForm->addRow(QString(), makeHint(content, tr(
            "Rebuilds the dictionary from a fresh body sample and "
            "rewrites every cached body. Useful after a major change "
            "in your mailbox (lots of new senders / templates). "
            "Memory-frugal but slow: several minutes per gigabyte of "
            "cache.")));
        outer->addLayout(compressionForm);
        outer->addStretch(1);
    }

    // Buttons sit OUTSIDE the QTabWidget so they're always visible no
    // matter which tab is active or how deep the user has scrolled
    // within it.
    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(20, 12, 20, 16);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)
        ->setObjectName(QStringLiteral("primary"));
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
