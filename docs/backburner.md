# Backburner / Ideas

Things we've explicitly chosen NOT to do right now but want to revisit
later. Each entry has enough notes to pick up cold.

## Obsidian-style WYSIWYG markdown editor for compose

**Status:** dropped from the rich-text compose work (Phase 5 of the
2026-05 keybind / compose pass).

**Idea:** instead of a Word-style rich-text editor (B / I / U /
blockquote toolbar over a `QTextEdit`), the compose body could behave
like Obsidian's default edit view: the user types raw markdown
syntax (`*italic*`, `**bold**`, `- list`, `> quote`, `[label](url)`),
and a syntax highlighter renders the active styling inline as the
user types. No mode-switch between raw and preview — the editor IS
the preview.

**Why it's interesting:**
- More keyboard-friendly than a toolbar; no mouse trips for emphasis.
- Markdown-savvy users (the FirstContact target audience) already
  type this way in chat.
- The on-the-wire format stays sensible: we'd convert markdown →
  plain text and markdown → HTML at send time, so Gmail web shows a
  proper rich body.

**What would it cost:**
- Custom `QSyntaxHighlighter` walking the document (cheap — Qt has
  good infrastructure for this).
- Markdown→HTML converter in the send path (we could vendor cmark
  or use Qt's `QTextDocument::toMarkdown` round-trip).
- Tricky bits: tables, inline images via paste, proper handling of
  lists across cursor moves, link insertion UX.
- Not a tiny lift — probably ~600–1000 lines of editor logic plus
  testing. Worth doing only if the toolbar-based rich-text path
  feels clunky in practice.

**Trigger to revisit:** if user feedback (or our own usage) finds the
Phase-5 toolbar editor annoying, or if we want to differentiate
FirstContact more aggressively from Gmail web.

## Compose / reading view modes (`d` keybind)

**Status:** keybind reserved; implementation deferred.

**Idea:** different layouts the user can swap between:

1. **Three-pane (current default):** sidebar | threadlist | reader.
2. **Two-pane:** sidebar | threadlist; activating a message opens
   the reader as a separate window or covers the threadlist.
3. **Reading-only:** when a message is open, the threadlist hides
   to give the reader more room (Gmail web's "reading pane right /
   bottom / off" trio).
4. **Compose in dedicated window vs. inline:** a future `d` (Gmail's
   "compose in a popup") binding would always pop a new window even
   when the inline-compose mode lands.

**Why it's interesting:**
- Different display sizes / preferences want different layouts.
- Smaller laptops can't justify three columns at full splitter
  resolution.
- A two-pane "every message opens its own window" mode is a clean
  multi-tasking workflow that nothing else does well.

**What would it cost:**
- A `ViewMode` enum in `Preferences`.
- Splitter reconfiguration in `MainWindow::buildLayout()`.
- A "Reader" top-level window class that wraps the existing
  `ReaderPane` for the popup variant.
- Likely a `View` menu (already partly absorbed by the hamburger)
  plus the `d` keybind.

**Trigger to revisit:** the user has signalled this is a desired
feature direction post-Phase 5.

## Within-thread message navigation (`n` / `p` keybinds)

**Status:** considered, deferred.

**Idea:** Gmail web's `n` and `p` move the focus between messages
in an OPEN thread — the reader pane scrolls / expands the next or
previous message card. We currently render the whole thread as a
stacked scroll area (`ReaderPane::showThread`) and don't track a
"current" message within it.

**Why it's interesting:** lets keyboard-only users walk a long
thread without touching the mouse.

**What would it cost:**
- Track an active card index in `ReaderPane`.
- `setActiveMessage(int)` slot that scrolls the active card into
  view, expands it if collapsed, and visually highlights it.
- `nextWithinThread()` / `prevWithinThread()` slots fired from
  `n` / `p` shortcuts.
- Probably 100–150 lines.

**Trigger to revisit:** anytime — small, self-contained.

## Numbered-label shortcuts (`Shift+1`…`9`) and bulk-select (`Shift+8 a/r/u/s/t`)

**Status:** noted, deferred.

**Idea:** Gmail web maps `Shift+1`…`9` to user-defined numbered
labels (so power users can apply favorite labels in one keystroke).
`Shift+8 a/r/u/s/t` selects all / read / unread / starred / unstarred
in the threadlist for bulk operations.

**Why it's interesting:** big speed-up for triage-heavy users.

**What would it cost:**
- Settings UI for "label slots 1–9" with a label picker for each.
- Multi-select model in `MessageListView` (currently single-select)
  + a checkbox column rendered by `MessageItemDelegate`.
- Bulk-action plumbing through `applyLabelDiffToThread`.
- Roughly a week of UI work; not trivial.

**Trigger to revisit:** when triage-heavy use cases come up, or
once the multi-select UI is wanted for any other reason (e.g.,
"select all flagged → mark read").
