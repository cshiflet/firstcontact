#pragma once

#include <QString>

namespace fc::util {

struct SanitizeOptions {
    bool allowRemoteImages = false;   // user opt-in flips this on per-message
};

struct SanitizeResult {
    QString  html;                    // safe HTML for QTextBrowser/QTextDocument
    bool     remoteImagesBlocked = false;  // true if any <img src="http…"> got dropped
};

// Whitelist-based sanitizer suitable for the rich-but-safe HTML reader tier.
// Removes: <script>, <style>, <iframe>, <object>, <embed>, <form>, <link>,
// <meta>, <base>, every event-handler attribute (onload=, onclick=, …), and
// any javascript:/data:/vbscript:/file: URLs in href/src.
// Blocks remote http(s) images by default; allowRemoteImages flips them back on.
SanitizeResult sanitizeHtml(const QString& dirty,
                            const SanitizeOptions& opts = {});

}  // namespace fc::util
