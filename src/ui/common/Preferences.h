#pragma once

#include <QString>

namespace fc::ui {

// User-facing preferences not covered by Theme. Persisted via QSettings.
class Preferences {
public:
    enum class HtmlPreview {
        Disabled,         // sanitized text only — no Show full HTML button
        ExternalBrowser,  // serve sanitized HTML on a loopback socket and
                          // hand the URL to the user's system browser. No
                          // Qt WebEngine dependency, no Chromium in process.
        InlineWebEngine,  // embed the HTML inline via the WebEngine plugin.
                          // Highest fidelity, costs ~50–80 MB extra resident
                          // even when no message is opened. Requires a
                          // restart to take effect because Qt6WebEngineCore
                          // has to be pre-loaded before QApplication.
    };

    static HtmlPreview htmlPreview();
    static void        setHtmlPreview(HtmlPreview m);

    static QString     htmlPreviewToString(HtmlPreview m);
    static HtmlPreview htmlPreviewFromString(const QString& s);

    // Settings-key constant exposed so main.cpp can read it through QSettings
    // before QApplication has been constructed (organisation/application
    // names are set explicitly in that path so the lookup hits the same
    // .conf file).
    static const char* htmlPreviewKey();

    // Attachment download settings ---------------------------------------

    // Initial directory the "Save as…" file picker (and "Download all"
    // folder picker) opens against. Empty string means "use the platform
    // default Downloads folder resolved via QStandardPaths". Returned
    // value is always non-empty — unset / blank stored values are
    // resolved to the platform default.
    static QString attachmentDir();
    static void    setAttachmentDir(const QString& dir);

    // Conversation view: when true, the message list groups messages by
    // thread and shows one row per thread (Gmail-web default). When false,
    // each message is its own row regardless of thread membership.
    static bool conversationView();
    static void setConversationView(bool on);

    // Toolbar layout: when true, every action in the main toolbar shows
    // its label next to the icon (the original / discoverability
    // default). When false, only the icons are drawn — useful on narrow
    // windows or for users who recognise the glyphs and want a denser
    // bar. Tooltips stay populated either way so the labels are still
    // reachable.
    static bool toolbarShowText();
    static void setToolbarShowText(bool on);

    // When true, each user label in the sidebar gets a small filled
    // swatch in its Gmail-assigned background colour (text colour
    // ignored at this size — it's just a chip). Falls back gracefully
    // for labels with no colour set on the server.
    static bool sidebarLabelColors();
    static void setSidebarLabelColors(bool on);

    // When true, message-list rows show a small coloured pill per
    // user label they carry, painted in the label's Gmail bg / fg.
    // System labels (INBOX / STARRED / UNREAD / CATEGORY_*) are
    // suppressed because they'd appear on every row and just be
    // visual noise.
    static bool messageListLabelPills();
    static void setMessageListLabelPills(bool on);
};

}  // namespace fc::ui
