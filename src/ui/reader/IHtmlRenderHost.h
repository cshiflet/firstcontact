#pragma once

class QWidget;
class QString;

namespace fc::ui {

// Pure abstract interface implemented by the optional QtWebEngine plugin
// (firstcontact_html_renderer). Loaded via QLibrary at runtime so the
// Chromium dependency stays out of the idle process.
//
// The plugin guarantees:
//   - the underlying engine runs off-the-record (no cookies, no cache),
//   - JavaScript is disabled,
//   - all http(s) resource requests are blocked unless allowRemote=true.
class IHtmlRenderHost {
public:
    virtual ~IHtmlRenderHost() = default;

    // Returns the embeddable widget. Caller takes ownership when reparenting.
    virtual QWidget* widget() = 0;

    // Renders sanitized HTML. Pass allowRemote=true once the user opts into
    // loading remote images for this message.
    virtual void render(const QString& html, bool allowRemote) = 0;
};

}  // namespace fc::ui

// C-ABI factory exported by the plugin shared library.
using fc_html_renderer_create_fn = fc::ui::IHtmlRenderHost* (*)(QWidget* parent);
extern "C" fc::ui::IHtmlRenderHost* fc_create_html_renderer(QWidget* parent);
