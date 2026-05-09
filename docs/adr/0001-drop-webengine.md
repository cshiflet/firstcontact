# 0001 — Drop inline WebEngine, render full HTML via system browser

**Status:** Accepted (commit `4024534`, 2026-05-04). Recorded retroactively.

**Context:**
The first cut of HTML rendering used `QWebEngineView` embedded in `ReaderPane` as
the rich-tier renderer (the plain-text + sanitized-rich tier in `QTextBrowser`
remained unchanged). Engaging it produced three problems we measured:

1. **Memory.** Engaging the inline view spawned a Chromium helper-process cohort
   (renderer + GPU + utility) totaling ≈300 MB resident. The plan target of
   < 200 MB active RAM was unreachable while a `QWebEngineView` was on screen.
   At idle (view never engaged) the cost was already ≈100 MB shared library
   text from `libQt6WebEngineCore` mapped into the process.
2. **Performance.** First-paint was visibly sluggish (multi-second on cold
   helper-process spawn) and the embedded view didn't reliably size to the
   reader pane on splitter resize.
3. **Maintenance.** A separately-loaded plugin (`firstcontact_html`) was added
   to keep the WebEngine cost out of the idle case (`QLibrary` load on first
   click). It worked, but doubled our build matrix and introduced a
   load-failure path with no good user-visible recovery.

**Decision:**
Remove `QWebEngineView` from the process entirely. Replace it with a one-shot
loopback HTTP handoff:

```
HtmlSanitizer
   → LocalHtmlServer (token-gated, exact-path, 5-min watchdog)
   → util::launchBrowser (xdg-open / ShellExecute / LSOpenCFURLRef)
   → user's system browser
```

The system browser pays the rendering cost in its own process tree. We pay
nothing beyond serving one request over loopback.

**Consequences:**
- Idle private RAM dropped from a target ceiling of 200 MB to a measured 39 MB
  (commit `4024534` baseline). The < 80 MB target is no longer at risk from
  the rendering path.
- We never have `QtWebEngineProcess` in our process tree. CPU and GPU contention
  during render lives in the system browser, not the mail client.
- Full-fidelity rendering still works — fidelity is the user's browser's, not
  ours. CSS / images / fonts / web fonts behave exactly as the user expects from
  any other HTML page.
- The browser does not have Gmail credentials. The handoff page is served from
  `127.0.0.1` with an opaque token in the path; CSP headers prevent the page
  from making outbound requests with our origin. No cookie or token leakage.
- "Open in browser" is the only path to full HTML. Users wanting an inline
  rich preview must use the sanitized rich tier in `QTextBrowser`. This is a
  product trade-off, not a temporary limitation.
- The sanitized rich tier handles the common case (most marketing email,
  newsletters, transactional mail) without browser handoff at all. Browser
  handoff is reserved for messages where the sanitizer's allowed subset
  visibly degrades layout (CSS-heavy or table-layout-dependent senders).

**Alternatives considered:**
- *Keep WebEngine, gate it behind a setting.* Rejected: even gated, the
  ≈100 MB shared-library text is mapped at process start, and once any user
  clicks "engage" their idle baseline jumps. We can't both ship WebEngine and
  hold the < 80 MB target.
- *`QtWebView`.* Backed by the system webview on each platform (WebKit on macOS,
  EdgeHTML/WebView2 on Windows, QtWebEngine on Linux — defeats the purpose
  on our priority platform).
- *Side-channel viewer process.* A separate FirstContact-owned helper process
  rendering HTML via WebEngine. Lower memory pressure on the main app but
  doubles packaging/signing surface and still ships Chromium. The system
  browser already exists; we don't need a second one we maintain.
- *Pixel-perfect rendering inside `QTextBrowser`.* `QTextBrowser` deliberately
  supports a subset of HTML/CSS. Closing the gap means writing a renderer.
  Out of scope.

**To resurrect:**
Revert `4024534`. The plugin scaffolding (`firstcontact_html`, `HtmlRenderHostLoader`,
`IHtmlRenderHost`) and the `FC_ENABLE_WEBENGINE` CMake gate all reappear.
Expect the RAM regression to follow.

**Do not** add `QtWebEngineWidgets` back without a successor ADR that revisits
the memory measurement and the < 80 MB target. The current model is load-bearing
for the idle baseline.
