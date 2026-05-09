#include "ComposeWindow.h"

#include "ui/common/Preferences.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextListFormat>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

namespace fc::ui {

namespace {

QStringList splitAddresses(const QString& raw) {
    return raw.split(QRegularExpression(QStringLiteral("\\s*,\\s*")),
                     Qt::SkipEmptyParts);
}

// ---------- plain-text helpers used by the HTML pipeline ----------
//
// QTextEdit's toPlainText() renders bullets / blockquotes / links as
// bare text automatically, so we only need a plain-text variant for
// the attribution line that we PREPEND to the quoted block — the
// quoted parent body itself flows through the rich-text editor and
// gets stripped by Qt for the text/plain part.

QString attributionPlain(const fc::Message& parent,
                          fc::ui::ComposeWindow::Mode mode) {
    const QString from = parent.fromName.isEmpty()
        ? parent.fromAddr
        : QStringLiteral("%1 <%2>").arg(parent.fromName, parent.fromAddr);

    if (mode == fc::ui::ComposeWindow::Mode::Forward) {
        const QString date = parent.internalDate > 0
            ? QLocale::system().toString(
                  QDateTime::fromMSecsSinceEpoch(parent.internalDate),
                  QLocale::ShortFormat)
            : QString();
        return QStringLiteral(
            "---------- Forwarded message ---------\n"
            "From: %1\nDate: %2\nSubject: %3\nTo: %4\n\n")
            .arg(from, date, parent.subject,
                  parent.toAddrs.join(QStringLiteral(", ")));
    }

    // Reply / ReplyAll attribution. Mirrors Gmail web's "On <date>,
    // <person> wrote:" line.
    const QString date = parent.internalDate > 0
        ? QLocale::system().toString(
              QDateTime::fromMSecsSinceEpoch(parent.internalDate),
              QLocale::LongFormat)
        : QString();
    return QStringLiteral("On %1, %2 wrote:\n").arg(date, from);
}

// ---------- HTML quoting (drives MimeBuilder's text/html part) ----------

// Marker we drop into the assembled HTML body to stake out where the
// cursor should land. Removed in prefillFrom AFTER setHtml() so the
// cursor ends up where we want it without leaking the literal text
// into the message.
constexpr char kCursorMarker[] = "FC_CURSOR_MARKER_8X3K";

QString htmlEscapeWithBreaks(const QString& s) {
    QString h = s.toHtmlEscaped();
    h.replace(QChar('\n'), QStringLiteral("<br>\n"));
    return h;
}

QString quoteBodyHtml(const QString& body,
                       fc::ui::Preferences::QuoteStyle style) {
    if (style == fc::ui::Preferences::QuoteStyle::BlockQuote) {
        return QStringLiteral(
            "<blockquote style='margin:0 0 0 .8ex; "
            "border-left:1px solid #ccc; padding-left:1ex; color:#666;'>"
            "%1</blockquote>").arg(htmlEscapeWithBreaks(body));
    }
    // GreaterPrefix: same "> " prefix as the plain-text path, just
    // emitted as plain HTML lines (no <blockquote> wrapper).
    QStringList lines = body.split(QChar('\n'));
    for (auto& l : lines) l = QStringLiteral("&gt; ") + l.toHtmlEscaped();
    return lines.join(QStringLiteral("<br>\n"));
}

QString attributionHtml(const fc::Message& parent,
                         fc::ui::ComposeWindow::Mode mode) {
    return htmlEscapeWithBreaks(attributionPlain(parent, mode)).trimmed();
}

QString sigBlockHtml(const QString& sig) {
    if (sig.isEmpty()) return QString();
    return QStringLiteral(
        "<div style='color:#777;'>-- <br>%1</div>")
        .arg(htmlEscapeWithBreaks(sig));
}

// Returns true if the rendered HTML carries any markup the user
// would notice — bold, italic, underline, bullets, blockquote,
// links, or non-default font sizing. Pure plain-text composes skip
// the multipart path so we don't bloat tiny messages.
bool hasInterestingMarkup(const QString& html) {
    static const QRegularExpression re(
        QStringLiteral("<(b|strong|i|em|u|ul|ol|blockquote|a\\s)|"
                       "font-weight:\\s*[5-9]00|"
                       "font-style:\\s*italic|"
                       "text-decoration:\\s*underline"),
        QRegularExpression::CaseInsensitiveOption);
    return html.contains(re);
}

// Strip Qt's <html><head><style>…</style></head><body>…</body></html>
// wrapper from QTextEdit::toHtml(). Most mail clients are happier with
// just the body inner HTML and Qt's auto-generated `<style>` block has
// document-level rules (font-family, font-size) that aren't right for
// every reader.
QString extractBodyInner(const QString& html) {
    static const QRegularExpression re(
        QStringLiteral("<body[^>]*>(.*?)</body>"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    auto m = re.match(html);
    return m.hasMatch() ? m.captured(1) : html;
}

}  // namespace

ComposeWindow::ComposeWindow(const QList<AccountChoice>& choices,
                              const QString& selectedAccountId,
                              QWidget* parent)
    : QWidget(parent, Qt::Window), choices_(choices) {
    setWindowTitle(tr("New message"));
    resize(720, 580);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(10);
    auto* form = new QFormLayout;
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    toEdit_      = new QLineEdit(this);
    ccEdit_      = new QLineEdit(this);
    subjectEdit_ = new QLineEdit(this);

    // Render the From row as a QComboBox when there are multiple
    // accounts to choose from; otherwise stay with a static label so
    // single-account users see no UI churn.
    if (choices_.size() > 1) {
        fromCombo_ = new QComboBox(this);
        for (const auto& c : choices_) {
            const QString display = c.displayName.isEmpty()
                ? c.email
                : QStringLiteral("%1 <%2>").arg(c.displayName, c.email);
            fromCombo_->addItem(display, c.id);
        }
        const int idx = fromCombo_->findData(selectedAccountId);
        if (idx >= 0) fromCombo_->setCurrentIndex(idx);
        connect(fromCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) { dirty_ = true; });
        form->addRow(tr("From:"), fromCombo_);
    } else {
        const QString text = choices_.isEmpty()
            ? tr("(no account)")
            : (choices_.front().displayName.isEmpty()
                ? choices_.front().email
                : QStringLiteral("%1 <%2>")
                      .arg(choices_.front().displayName,
                           choices_.front().email));
        fromLabel_ = new QLabel(text, this);
        form->addRow(tr("From:"), fromLabel_);
    }
    form->addRow(tr("To:"),      toEdit_);
    form->addRow(tr("Cc:"),      ccEdit_);
    form->addRow(tr("Subject:"), subjectEdit_);

    bodyEdit_ = new QTextEdit(this);
    // Phase 5: flip to rich-text editing. The toolbar built below
    // exposes the canonical email-formatting subset (B/I/U + lists +
    // blockquote + link). bodyHtml gets harvested in currentMessage()
    // and rides the multipart/alternative path through MimeBuilder.
    bodyEdit_->setAcceptRichText(true);

    formatBar_ = buildFormatToolbar();

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color: gray;"));

    auto* sendBtn      = new QPushButton(tr("Send"),         this);
    sendBtn->setObjectName(QStringLiteral("primary"));
    sendBtn->setToolTip(tr("Send (Ctrl+Enter)"));
    auto* scheduleBtn  = new QPushButton(tr("Schedule…"),    this);
    auto* saveDraftBtn = new QPushButton(tr("Save draft"),   this);
    auto* cancelBtn    = new QPushButton(tr("Discard"),      this);
    cancelBtn->setToolTip(tr("Discard (Esc)"));
    connect(sendBtn,      &QPushButton::clicked, this, &ComposeWindow::onSend);
    connect(scheduleBtn,  &QPushButton::clicked, this, &ComposeWindow::onScheduleSend);
    connect(saveDraftBtn, &QPushButton::clicked, this, &ComposeWindow::onSaveDraft);
    connect(cancelBtn,    &QPushButton::clicked, this, [this] {
        suppressClosePrompt_ = true;
        close();
    });

    // Gmail-web shortcuts inside the compose window:
    //   Ctrl+Enter (Cmd+Enter on macOS via Qt::CTRL alias) — Send
    //   Esc                                                   — Close
    auto* sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    sendShortcut->setContext(Qt::WindowShortcut);
    connect(sendShortcut, &QShortcut::activated, this, &ComposeWindow::onSend);
    auto* sendShortcut2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), this);
    sendShortcut2->setContext(Qt::WindowShortcut);
    connect(sendShortcut2, &QShortcut::activated, this, &ComposeWindow::onSend);
    auto* escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escShortcut->setContext(Qt::WindowShortcut);
    connect(escShortcut, &QShortcut::activated, this, [this] { close(); });

    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(statusLabel_, 1);
    btnRow->addWidget(saveDraftBtn);
    btnRow->addWidget(scheduleBtn);
    btnRow->addWidget(sendBtn);
    btnRow->addWidget(cancelBtn);

    root->addLayout(form);
    root->addWidget(formatBar_);
    root->addWidget(bodyEdit_, /*stretch=*/1);
    root->addLayout(btnRow);

    auto markDirty = [this] { dirty_ = true; };
    connect(toEdit_,      &QLineEdit::textChanged, this, markDirty);
    connect(ccEdit_,      &QLineEdit::textChanged, this, markDirty);
    connect(subjectEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(bodyEdit_,    &QTextEdit::textChanged, this, markDirty);
}

QString ComposeWindow::draftId() const { return draftId_; }

ComposeWindow::AccountChoice ComposeWindow::selectedChoice() const {
    if (fromCombo_) {
        const QString id = fromCombo_->currentData().toString();
        for (const auto& c : choices_) if (c.id == id) return c;
    }
    return choices_.isEmpty() ? AccountChoice{} : choices_.front();
}

QString ComposeWindow::currentAccountId() const {
    return selectedChoice().id;
}

QToolBar* ComposeWindow::buildFormatToolbar() {
    auto* bar = new QToolBar(this);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setStyleSheet(QStringLiteral(
        "QToolBar { border: none; padding: 0; spacing: 4px; }"
        "QToolButton { padding: 2px 8px; }"));

    // Inline style helpers — text-only QActions get crisp single-glyph
    // labels (B/I/U look right rendered bold/italic/underlined). The
    // QAction's `text` is rendered as plain by default; we go through
    // setFont on each button after the fact for the per-button style.
    auto add = [this, bar](const QString& label,
                            const QString& tip,
                            const QKeySequence& shortcut,
                            bool checkable,
                            std::function<void(bool)> handler) {
        auto* a = bar->addAction(label);
        a->setCheckable(checkable);
        if (!shortcut.isEmpty()) a->setShortcut(shortcut);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, handler);
        return a;
    };

    actBold_ = add(QStringLiteral("B"),
                    tr("Bold (Ctrl+B)"),
                    QKeySequence::Bold, /*checkable=*/true,
                    [this](bool on) {
                        bodyEdit_->setFontWeight(on ? QFont::Bold
                                                   : QFont::Normal);
                    });
    actItalic_ = add(QStringLiteral("I"),
                     tr("Italic (Ctrl+I)"),
                     QKeySequence::Italic, /*checkable=*/true,
                     [this](bool on) { bodyEdit_->setFontItalic(on); });
    actUnderline_ = add(QStringLiteral("U"),
                        tr("Underline (Ctrl+U)"),
                        QKeySequence::Underline, /*checkable=*/true,
                        [this](bool on) { bodyEdit_->setFontUnderline(on); });

    bar->addSeparator();

    add(QStringLiteral("•"), tr("Bullet list"), {}, false,
        [this](bool) {
            QTextCursor c = bodyEdit_->textCursor();
            QTextListFormat fmt;
            fmt.setStyle(QTextListFormat::ListDisc);
            c.createList(fmt);
        });
    add(QStringLiteral("1."), tr("Numbered list"), {}, false,
        [this](bool) {
            QTextCursor c = bodyEdit_->textCursor();
            QTextListFormat fmt;
            fmt.setStyle(QTextListFormat::ListDecimal);
            c.createList(fmt);
        });
    add(QStringLiteral("“"), tr("Block quote"), {}, false,
        [this](bool) {
            QTextCursor c = bodyEdit_->textCursor();
            QTextBlockFormat fmt = c.blockFormat();
            // Toggle: if already indented as a quote, drop it. Otherwise
            // add the indent. Indent count of 1 reads visually as the
            // standard one-level blockquote.
            const int newIndent = fmt.indent() > 0 ? 0 : 1;
            fmt.setIndent(newIndent);
            c.setBlockFormat(fmt);
        });

    bar->addSeparator();

    add(QStringLiteral("⎘"), tr("Insert link"), {}, false,
        [this](bool) {
            QTextCursor c = bodyEdit_->textCursor();
            const QString existing = c.hasSelection() ? c.selectedText()
                                                      : QString();
            bool ok = false;
            const QString url = QInputDialog::getText(
                this, tr("Insert link"), tr("URL:"),
                QLineEdit::Normal, QStringLiteral("https://"), &ok);
            if (!ok || url.trimmed().isEmpty()) return;
            const QUrl parsed(url.trimmed());
            if (!parsed.isValid()) return;
            const QString display = existing.isEmpty() ? url : existing;
            c.insertHtml(QStringLiteral("<a href=\"%1\">%2</a>")
                         .arg(parsed.toString().toHtmlEscaped(),
                              display.toHtmlEscaped()));
        });
    add(QStringLiteral("✕"), tr("Clear formatting"), {}, false,
        [this](bool) {
            QTextCursor c = bodyEdit_->textCursor();
            QTextCharFormat fmt;        // default-constructed = no format
            c.setCharFormat(fmt);
            QTextBlockFormat block = c.blockFormat();
            block.setIndent(0);
            c.setBlockFormat(block);
        });

    // Reflect the cursor's current format on the toggle buttons so the
    // user can SEE whether their cursor is inside a bold word.
    connect(bodyEdit_, &QTextEdit::currentCharFormatChanged,
            this,       [this](const QTextCharFormat&) {
                syncFormatActionsToCursor();
            });
    connect(bodyEdit_, &QTextEdit::cursorPositionChanged,
            this,       &ComposeWindow::syncFormatActionsToCursor);

    return bar;
}

void ComposeWindow::syncFormatActionsToCursor() {
    if (!actBold_ || !actItalic_ || !actUnderline_) return;
    const QTextCharFormat fmt = bodyEdit_->currentCharFormat();
    actBold_->setChecked(fmt.fontWeight() >= QFont::DemiBold);
    actItalic_->setChecked(fmt.fontItalic());
    actUnderline_->setChecked(fmt.fontUnderline());
}

fc::util::OutgoingMessage ComposeWindow::currentMessage() const {
    const auto choice = selectedChoice();
    fc::util::OutgoingMessage msg;
    msg.fromAddr         = choice.email;
    msg.fromName         = choice.displayName;
    msg.subject          = subjectEdit_->text();
    msg.bodyText         = bodyEdit_->toPlainText();

    // Populate bodyHtml only when the document carries actual rich-text
    // markup (bold / italic / underline / lists / blockquote / links).
    // Pure plain-text composes get a single text/plain part instead of
    // multipart/alternative — fewer bytes on the wire and simpler for
    // any plain-only readers in the chain.
    const QString fullHtml = bodyEdit_->toHtml();
    if (hasInterestingMarkup(fullHtml)) {
        msg.bodyHtml = extractBodyInner(fullHtml);
    }

    msg.to               = splitAddresses(toEdit_->text());
    msg.cc               = splitAddresses(ccEdit_->text());
    msg.rfc822InReplyTo  = inReplyToHeader_;
    msg.rfc822References = referencesHeader_;
    return msg;
}

void ComposeWindow::loadFromDraft(const QString& draftId,
                                  const QString& threadId,
                                  const QString& subject,
                                  const QStringList& to,
                                  const QStringList& cc,
                                  const QString& body) {
    draftId_  = draftId;
    threadId_ = threadId;
    subjectEdit_->setText(subject);
    toEdit_->setText(to.join(QStringLiteral(", ")));
    ccEdit_->setText(cc.join(QStringLiteral(", ")));
    bodyEdit_->setPlainText(body);
    bodyEdit_->moveCursor(QTextCursor::End);
    dirty_ = false;
}

void ComposeWindow::prefillFrom(const fc::Message& parent, Mode mode) {
    threadId_ = parent.threadId;
    referencesHeader_.clear();
    inReplyToHeader_.clear();

    QString to;
    if (!parent.replyTo.isEmpty()) to = parent.replyTo;
    else if (!parent.fromAddr.isEmpty())
        to = parent.fromName.isEmpty()
                ? parent.fromAddr
                : QStringLiteral("%1 <%2>").arg(parent.fromName, parent.fromAddr);

    // Reply / ReplyAll: pin the From dropdown to the message's account
    // so the user replies as the account that received the mail. This
    // matches Gmail web semantics and avoids the surprise of "I clicked
    // reply from chris@personal but Gmail sent it from work@x.com".
    if ((mode == Mode::Reply || mode == Mode::ReplyAll
         || mode == Mode::Forward)
        && fromCombo_ && !parent.accountId.isEmpty()) {
        const int idx = fromCombo_->findData(parent.accountId);
        if (idx >= 0) fromCombo_->setCurrentIndex(idx);
    }

    QString subjectPrefix;
    const QString fromAddr = selectedChoice().email;
    switch (mode) {
        case Mode::Reply:
        case Mode::ReplyAll:
            subjectPrefix = QStringLiteral("Re: ");
            toEdit_->setText(to);
            if (mode == Mode::ReplyAll) {
                QStringList cc = parent.toAddrs;
                cc.append(parent.ccAddrs);
                cc.removeAll(fromAddr);
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

    // Compose-body assembly. Three pieces, ordered by preference:
    //
    //   replyAboveOriginal == true  (Gmail / Outlook default):
    //       <cursor>
    //       <signature>
    //       <attribution + quoted parent>
    //
    //   replyAboveOriginal == false (Usenet / mailing-list convention):
    //       <attribution + quoted parent>
    //       <cursor>
    //       <signature>
    //
    // For Mode::New there's no parent — the body is empty + signature
    // and the cursor sits at the start of the empty body.
    //
    // Phase 5: rich-text editor. We assemble the body as HTML so the
    // quoted block can render as a real <blockquote> when the user
    // picked QuoteStyle::BlockQuote. A literal cursor-marker token is
    // dropped where the user is expected to start typing; we find +
    // remove it post-setHtml to leave the cursor in place.
    const QString sig    = Preferences::signatureText();
    const auto    qStyle = Preferences::quoteStyle();
    const bool    above  = Preferences::replyAboveOriginal();

    const QString cursorTag = QStringLiteral("<p>%1</p>")
                                .arg(QString::fromLatin1(kCursorMarker));
    const QString sigDiv    = sigBlockHtml(sig);

    QString html;
    if (mode == Mode::New) {
        html = cursorTag;
        if (!sigDiv.isEmpty()) html += QStringLiteral("<br>") + sigDiv;
    } else {
        const QString attr      = QStringLiteral("<p>%1</p>")
                                    .arg(attributionHtml(parent, mode));
        const QString quoted    = quoteBodyHtml(parent.bodyText, qStyle);
        const QString quotedBlk = attr + quoted;

        if (above) {
            html = cursorTag;
            if (!sigDiv.isEmpty()) html += QStringLiteral("<br>") + sigDiv;
            html += QStringLiteral("<br>") + quotedBlk;
        } else {
            html = quotedBlk + QStringLiteral("<br>") + cursorTag;
            if (!sigDiv.isEmpty()) html += QStringLiteral("<br>") + sigDiv;
        }
    }

    bodyEdit_->setHtml(html);

    // Find the marker, select it (find returns the cursor with the
    // match selected), and remove it — cursor lands exactly where it
    // was. Anchor the search at document start; the default overload
    // searches from the CURRENT cursor position, which after setHtml
    // can be anywhere — including past the marker, in which case
    // find returns null and the cursor goes to Start unnecessarily.
    QTextCursor searchStart(bodyEdit_->document());
    QTextCursor mark = bodyEdit_->document()->find(
        QString::fromLatin1(kCursorMarker), searchStart);
    if (!mark.isNull()) {
        mark.removeSelectedText();
        bodyEdit_->setTextCursor(mark);
    } else {
        bodyEdit_->moveCursor(QTextCursor::Start);
    }

    // We just programmatically populated fields; the user hasn't typed yet.
    dirty_ = false;
}

void ComposeWindow::onSend() {
    const auto msg = currentMessage();
    if (msg.to.isEmpty()) {
        statusLabel_->setText(tr("Add at least one recipient."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
        return;
    }
    suppressClosePrompt_ = true;
    emit composeReady(currentAccountId(), msg, threadId_, /*sendAtMs=*/0);
    close();
}

void ComposeWindow::onScheduleSend() {
    const auto msg = currentMessage();
    if (msg.to.isEmpty()) {
        statusLabel_->setText(tr("Add at least one recipient."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
        return;
    }

    // Tiny scheduling dialog: a single QDateTimeEdit defaulted to one
    // hour from now. No fancier presets ("tomorrow morning", "next
    // Monday") for v1; users can drive the picker.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Schedule send"));
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr(
        "Pick a delivery time. The message stays queued in your local "
        "outbox until that moment, then sends like any other message. "
        "Sync must be running for delivery to actually fire — leave the "
        "app open or relaunch in time."), &dlg));
    auto* picker = new QDateTimeEdit(
        QDateTime::currentDateTime().addSecs(60 * 60), &dlg);
    picker->setCalendarPopup(true);
    picker->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
    picker->setDisplayFormat(QStringLiteral("ddd MMM d, yyyy  h:mm AP"));
    layout->addWidget(picker);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* okBtn     = new QPushButton(tr("Schedule"), &dlg);
    okBtn->setObjectName(QStringLiteral("primary"));
    okBtn->setDefault(true);
    auto* cancelBtn = new QPushButton(tr("Cancel"),   &dlg);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    layout->addLayout(btnRow);
    QObject::connect(okBtn,     &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const QDateTime when = picker->dateTime();
    if (when <= QDateTime::currentDateTime()) {
        statusLabel_->setText(tr("Pick a time in the future."));
        statusLabel_->setStyleSheet(QStringLiteral("color: #c92a2a;"));
        return;
    }
    suppressClosePrompt_ = true;
    emit composeReady(currentAccountId(), msg, threadId_,
                      when.toMSecsSinceEpoch());
    close();
}

void ComposeWindow::onSaveDraft() {
    emit saveDraftRequested(currentAccountId(), currentMessage(),
                            threadId_, draftId_);
    dirty_ = false;
    statusLabel_->setText(tr("Draft saved."));
    statusLabel_->setStyleSheet(QStringLiteral("color: gray;"));
}

void ComposeWindow::closeEvent(QCloseEvent* e) {
    if (suppressClosePrompt_ || !dirty_) {
        e->accept();
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Save draft?"),
        tr("Save this message as a draft before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    switch (answer) {
        case QMessageBox::Save:
            emit saveDraftRequested(currentAccountId(), currentMessage(),
                                    threadId_, draftId_);
            e->accept();
            break;
        case QMessageBox::Discard:
            e->accept();
            break;
        default:
            e->ignore();
            break;
    }
}

}  // namespace fc::ui
