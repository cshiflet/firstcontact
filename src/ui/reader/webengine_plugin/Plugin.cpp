// Optional QtWebEngine-based HTML renderer for FirstContact.
//
// Loaded into the host process by HtmlRenderHostLoader via QLibrary on the
// first user click of "Show full HTML". Linking against Qt6::WebEngineWidgets
// here (and nowhere else in the tree) keeps Chromium out of the idle process.
//
// Defence-in-depth measures applied here:
//   - off-the-record QWebEngineProfile        — no cookies, cache, history
//   - JavaScript disabled in the profile      — no scripted XSS / fingerprint
//   - PluginsEnabled / WebGL / LocalStorage all OFF
//   - QWebEngineUrlRequestInterceptor that blocks every http(s) request unless
//     the caller opted in for "show remote images" mode
//   - HTML loaded via setHtml(..., baseUrl=about:blank) — no implicit base
//
// The plugin treats the *html* string as already-sanitized by HtmlSanitizer.
// We do not relax sanitization rules here.

#include "../IHtmlRenderHost.h"

#include <QString>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QWidget>

namespace {

class BlockingInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    explicit BlockingInterceptor(QObject* parent = nullptr)
        : QWebEngineUrlRequestInterceptor(parent) {}

    void setAllowRemote(bool allow) { allow_ = allow; }

    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        const QUrl u = info.requestUrl();
        const QString s = u.scheme();
        // about: and data: are needed for the initial setHtml; data: is only
        // populated by us with sanitized content (cid: parts -> data: URIs in
        // a future Phase 4 enhancement).
        if (s == QLatin1String("about") || s == QLatin1String("data") ||
            s == QLatin1String("file")  || s == QLatin1String("qrc")) {
            // file: / qrc: only ever appear for our own embedded resources;
            // we intentionally do not allow them today either (no resources
            // are loaded), so block.
            if (s != QLatin1String("about") && s != QLatin1String("data")) {
                info.block(true);
            }
            return;
        }
        if ((s == QLatin1String("http") || s == QLatin1String("https")) && allow_) {
            return;
        }
        info.block(true);
    }

private:
    bool allow_ = false;
};

class WebEngineRenderHost : public fc::ui::IHtmlRenderHost {
public:
    explicit WebEngineRenderHost(QWidget* parent) {
        container_ = new QWidget(parent);
        auto* layout = new QVBoxLayout(container_);
        layout->setContentsMargins(0, 0, 0, 0);

        // Off-the-record profile: no cookies, no on-disk cache.
        profile_ = new QWebEngineProfile(container_);
        profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        profile_->setHttpCacheType(QWebEngineProfile::NoCache);

        interceptor_ = new BlockingInterceptor(container_);
        profile_->setUrlRequestInterceptor(interceptor_);

        auto* settings = profile_->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled,        false);
        settings->setAttribute(QWebEngineSettings::PluginsEnabled,           false);
        settings->setAttribute(QWebEngineSettings::WebGLEnabled,             false);
        settings->setAttribute(QWebEngineSettings::LocalStorageEnabled,      false);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls,
                               false);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                               false);
        settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent,
                               false);

        view_ = new QWebEngineView(container_);
        page_ = new QWebEnginePage(profile_, view_);
        view_->setPage(page_);
        layout->addWidget(view_);
    }

    QWidget* widget() override { return container_; }

    void render(const QString& html, bool allowRemote) override {
        interceptor_->setAllowRemote(allowRemote);
        page_->setHtml(html, QUrl(QStringLiteral("about:blank")));
    }

private:
    QWidget*              container_   = nullptr;
    QWebEngineProfile*    profile_     = nullptr;
    QWebEnginePage*       page_        = nullptr;
    QWebEngineView*       view_        = nullptr;
    BlockingInterceptor*  interceptor_ = nullptr;
};

}  // namespace

extern "C" Q_DECL_EXPORT fc::ui::IHtmlRenderHost* fc_create_html_renderer(QWidget* parent) {
    return new WebEngineRenderHost(parent);
}
