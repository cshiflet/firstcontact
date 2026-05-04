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

    // Folder where attachments land when the user takes the default action
    // (left-click on an attachment chip with "always ask" off, or "Download
    // all"). Empty string means "use the platform default Downloads folder
    // resolved via QStandardPaths". Returned value is always non-empty —
    // unset / blank stored values are resolved to the platform default.
    static QString attachmentDir();
    static void    setAttachmentDir(const QString& dir);

    // When true, every save action — including left-clicks and "Download
    // all" — pops a file picker so the user can confirm the destination
    // and rename the file. When false, left-click goes straight to
    // attachmentDir() with collision-safe naming.
    static bool alwaysAskAttachmentLocation();
    static void setAlwaysAskAttachmentLocation(bool ask);

    // Conversation view: when true, the message list groups messages by
    // thread and shows one row per thread (Gmail-web default). When false,
    // each message is its own row regardless of thread membership.
    static bool conversationView();
    static void setConversationView(bool on);
};

}  // namespace fc::ui
