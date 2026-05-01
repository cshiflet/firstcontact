#pragma once

#include "IHtmlRenderHost.h"

class QWidget;

namespace fc::ui {

// Lazy QLibrary wrapper around the QtWebEngine plugin. The shared library is
// resolved only on the first call to create(); subsequent calls reuse the
// open library handle.
class HtmlRenderHostLoader {
public:
    // Returns nullptr if the plugin isn't available (FC_ENABLE_WEBENGINE was
    // OFF at build time, the .so/.dll wasn't deployed, or initialization
    // failed for any reason).
    static IHtmlRenderHost* create(QWidget* parent);

    static bool available();
};

}  // namespace fc::ui
