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
};

}  // namespace fc::ui
