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

## Per-account dict training: seed with bundled dict's content tail

**Status:** noted, deferred until after the bundled-dict pipeline
ships.

**Idea:** `BodyCompressionWorker` currently calls
`ZDICT_trainFromBuffer` with no seed. Once the bundled dict
(`:/compression/bundled-bodies-v1.zdict`) is in place, switch
the worker to `ZDICT_finalizeDictionary` and pass two things as
`dictContent`:

1. The bundled dict's raw content tail (skip past the header via
   `ZDICT_getDictHeaderSize`), so universal email patterns
   carry forward.
2. Optionally the same `seed_content.txt` we use for the
   bundled dict, so hand-curated boilerplate stays guaranteed.

Then run training over the user's mailbox samples to drive the
entropy tables.

**Why it's interesting:**
- Small / brand-new mailboxes (a few hundred messages) can't
  produce a meaningful dict from scratch; seeded training gives
  them a floor that's at least as good as the bundled dict.
- Universal patterns (HTML preamble, quoted-reply prefixes, ESP
  chrome) are guaranteed coverage regardless of what landed in
  the random sample.
- No compression-time complexity: still one dict per frame.

**What would it cost:**
- Swap `trainFromBuffer` → `finalizeDictionary` in
  `BodyCompressionWorker::doWork`.
- Load the bundled dict resource (already cached in
  `MessageRepository::dictionaryFor` fallback path), strip
  its header bytes, pass as `dictContent`.
- Pick a per-account `dictID` distinct from the bundled
  `0x46430001` — e.g., `0x46438000 | accountSerial` so future
  frame-header routing can tell them apart. (Schema already
  pins dict to account by primary key, so this is only needed
  if/when we add hybrid-cache support.)
- Maybe 50 lines plus tests.

**Trigger to revisit:** as soon as the bundled dict has shipped
and we touch `BodyCompressionWorker` again.

## Hybrid dict cache via per-frame dictID routing

**Status:** pinned as an idea, complexity not currently justified.

**Idea:** zstd writes the `dictID` into every frame header, and
`ZSTD_getDictID_fromFrame` lets you read it before decompression.
That makes it possible to maintain a pool of dicts (bundled +
per-account) in memory and route each row to the right dict at
decompress time. With per-account dicts assigned distinct IDs,
you could compress new rows with the per-account dict the moment
training finishes — without rewriting the existing bundled-
compressed rows.

**Why it's interesting:**
- Avoids the full-cache `VACUUM`+rewrite that `Mode::Recompress`
  currently does after per-account training. On a multi-GB
  cache that's the longest step in the pipeline.
- Opens the door to per-label or per-sender specialized dicts
  if we ever wanted finer-grained compression tuning.

**Why it's deferred:**
- Real benefit is "skip a one-time rewrite", and the current
  rewrite already runs in the background.
- Adds dict-pool plumbing (lookup by ID, eviction, memory
  accounting) and complicates the read path.
- Cost/benefit doesn't pencil out today; only revisit if cache
  rewrites become a UX problem.

**Trigger to revisit:** if users complain about the
`Recompress` backfill duration, or if a feature wants
per-sender / per-label dicts.
