#pragma once

#include "util/Linkify.h"

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
        // (InlineWebEngine was removed — see the "Drop inline WebEngine
        // HTML preview" commit. The plugin / loader / interface are
        // gone too. To resurrect, revert that commit.)
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

    // URL pattern used to proxy remote images on the "Open with images"
    // path. {url} is the placeholder replaced by the percent-encoded
    // source URL. Default uses wsrv.nl (a no-log public image proxy
    // fronted by Cloudflare): the user's browser only ever connects
    // to the proxy, the marketer's CDN sees the proxy's egress IP,
    // not the user's. Set to empty to skip rewriting (dangerous —
    // images load directly from the marketer's CDN).
    static QString imageProxyUrlPattern();
    static void    setImageProxyUrlPattern(const QString& pattern);

    // When true, strip 1×1 / 2×2 (likely tracking-pixel) <img> tags
    // before serving HTML to the browser. Off by default — some
    // legitimate emails use small spacer images and stripping them
    // can subtly break layout. The proxy already eliminates IP / UA
    // fingerprint correlation; pixel stripping additionally suppresses
    // the "this email was opened" signal at the cost of layout
    // robustness.
    static bool stripTrackingPixels();
    static void setStripTrackingPixels(bool on);

    // ----------------------------------------------------------------
    // Compose preferences

    // Plain-text signature appended to outgoing messages. Empty string
    // means no signature. Inserted as-is after a leading "\n--\n"
    // separator, matching the de-facto RFC 3676 sig delimiter.
    static QString signatureText();
    static void    setSignatureText(const QString& text);

    // True: the user's reply body sits ABOVE the quoted original
    // (Gmail / Outlook web default — top-posting). False: body sits
    // BELOW the quoted original (Usenet / mailing-list convention —
    // bottom-posting). The cursor is placed appropriately on
    // prefill so typing lands in the right spot.
    static bool replyAboveOriginal();
    static void setReplyAboveOriginal(bool on);

    // How the original message gets quoted in a reply / forward body:
    //   BlockQuote  — RFC-1849 style: each line prefixed with "> ".
    //                 Renders as a real blockquote in HTML.
    //   GreaterPrefix — same prefix; identical text format. Difference
    //                   only matters for the rich-text / HTML reply
    //                   path (Phase 5) where BlockQuote becomes a
    //                   real <blockquote> element and GreaterPrefix
    //                   stays as plain ">"-prefixed lines.
    enum class QuoteStyle { BlockQuote, GreaterPrefix };
    static QuoteStyle quoteStyle();
    static void       setQuoteStyle(QuoteStyle s);
    static QString    quoteStyleToString(QuoteStyle s);
    static QuoteStyle quoteStyleFromString(const QString& s);

    // ----------------------------------------------------------------
    // Filter state — persisted across sessions so the user's last
    // toggle survives a restart (matches Gmail web's behavior).

    // When true, every label's message list is filtered down to
    // messages currently carrying UNREAD (or in conversation view,
    // threads with at least one unread message). Off by default.
    static bool unreadOnly();
    static void setUnreadOnly(bool on);

    // Multiplier applied to the application's default font point size
    // at startup. Useful on HiDPI displays where the system's reported
    // DPI doesn't match the physical scale (notably WSL, where the
    // host's HiDPI scale doesn't always propagate through X). Values
    // outside [0.75, 3.0] are clamped. 1.0 is the platform default.
    //
    // Read-once-at-startup contract: changing this lives via a UI
    // control but most widget metrics only fully recompute on the
    // next launch, so the Settings dialog hints at a restart for
    // best results.
    static double uiFontScale();
    static void   setUiFontScale(double scale);

    // How URLs render in the linkified plain-text body (see
    // util::LinkDisplayMode). Toggleable via Shift+L; the dropdown
    // in Settings → Reader mirrors the same pref.
    static fc::util::LinkDisplayMode linkDisplayMode();
    static void                       setLinkDisplayMode(fc::util::LinkDisplayMode m);

    // Messages-per-batch: page size used both for cache reads
    // (MessageListModel::pageSize) and for server top-up fetches
    // (SyncService's messages.list maxResults). Default 50,
    // clamped to [10, 500]. The underlying storage lives in
    // util::PageSizePref so the sync layer can read the same key
    // without depending on this header.
    static int  messagePageSize();
    static void setMessagePageSize(int n);

    // ----------------------------------------------------------------
    // Low-bandwidth mode (meta-first sync)
    //
    // When true, SyncService fetches metadata only during initial sync
    // and label top-up — bodies arrive on demand the first time the
    // user opens each message. Default off; users who hate the "first
    // open is slow" feel keep the eager-body behaviour. Read by both
    // SyncService (to choose between batched metadata fetch and the
    // legacy serial-getMessage path) and the reader (which only
    // triggers the on-demand fetch when this is on).
    static bool lowBandwidthMode();
    static void setLowBandwidthMode(bool on);

    // ----------------------------------------------------------------
    // Background crawler — opt-in slow walk of every label
    //
    // backgroundCrawl: master toggle. Default off. When on, the sync
    // service runs a periodic "fill the cache slowly" tick: each
    // tick top-ups one un-exhausted label by exactly one page. Walks
    // are gated on the existing busy / user-triggered cacheLabelTarget
    // state so they never race with foreground sync.
    //
    // backgroundCrawlIntervalSec: spacing between ticks. 5s default,
    // clamped to >= 1s. Higher values trade fill speed for less
    // network/CPU activity. Reset crawl progress (settings button)
    // clears the per-label exhausted flags so the next round revisits
    // labels that have already been walked end-to-end.
    static bool backgroundCrawl();
    static void setBackgroundCrawl(bool on);
    static int  backgroundCrawlIntervalSec();
    static void setBackgroundCrawlIntervalSec(int seconds);

    // Last sidebar selection at quit time. Restored on launch so the
    // app reopens to the same folder/label the user was reading.
    // crossAccountView=true pins to the synthetic "All Inboxes" view;
    // accountId is empty in that case. accountId/labelId both empty
    // → never set (fresh install / migration), caller picks a default.
    struct LastView {
        QString accountId;
        QString labelId;
        bool    crossAccountView = false;
    };
    static LastView lastViewedSelection();
    static void     setLastViewedSelection(const LastView& v);

    // ----------------------------------------------------------------
    // Database compression preferences
    //
    // dbCompression: master toggle. Default on. Writes/reads still
    // route through BodyCodec regardless (the codec passes through
    // when no dictionary is loaded); the toggle gates the auto-train
    // workflow and the first-time prompt.
    //
    // dbCompressionPromptShown(accountId): set true once the user
    // has dismissed the "200 bodies, want to compress?" dialog for
    // a given account so we don't re-prompt every restart.
    static bool dbCompression();
    static void setDbCompression(bool on);
    static bool dbCompressionPromptShown(const QString& accountId);
    static void setDbCompressionPromptShown(const QString& accountId,
                                              bool shown);

    // ----------------------------------------------------------------
    // Per-account cache bounds. Each defaults to 0 = unlimited.
    //
    // These are upper limits enforced via auto-prune after every
    // successful sync (when cacheAutoPrune is true). Future work
    // (task #14) gates the fetch path too, so out-of-bounds bodies
    // never get pulled — for now the bounds run as cleanup, so they
    // do save disk but not bandwidth.
    //
    // maxAgeDays: drop messages whose internal_date is older than
    //   the cutoff. Threads / labels survive — only message rows go.
    // maxMessages: cap total cached message count per account.
    //   Oldest-first eviction.
    // maxCacheMb: cap on-disk cache size per account. Oldest-first
    //   eviction summed via bytes_cached.
    static bool cacheAutoPrune();
    static void setCacheAutoPrune(bool on);
    static int  cacheMaxAgeDays(const QString& accountId);
    static void setCacheMaxAgeDays(const QString& accountId, int days);
    static int  cacheMaxMessages(const QString& accountId);
    static void setCacheMaxMessages(const QString& accountId, int n);
    static int  cacheMaxCacheMb(const QString& accountId);
    static void setCacheMaxCacheMb(const QString& accountId, int mb);
};

}  // namespace fc::ui
