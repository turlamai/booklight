# Booklight user guide

**English** · [简体中文](USER-GUIDE.zh-CN.md)

Read PDF and EPUB files, annotate them and optionally ask your own AI provider about a document.

## Availability

Booklight is free and non-commercial. Reading, annotation, light editing, single-document BYOK AI and the multi-document research workbench are all included; there is no paid tier or checkout.

## Platforms and installation

Desktop previews for macOS, Windows and Linux. Check the release for the architecture you need; mobile is not included.

Use [official Releases](https://github.com/turlamai/booklight/releases), select an explicitly listed OS/architecture asset, read its release notes and check supplied checksums. An absent asset/platform is not a delivery promise. Preview builds may lack publisher signing or notarization; do not disable system-wide security protections to install them.

## Your first successful session

1. Open a small PDF or EPUB directly from its existing folder (File > Open or drag and drop). For initial tests, choose a disposable sample. There is no library import.
2. Add an annotation, close and reopen the book, and confirm it persists before working on valuable material.
3. Try page editing and export on test files. Use an explicit destination and reopen the exported document; moving a source file outside the app changes its path, not a Booklight catalog.

## Files, backup and recovery

New installations prefer `<document>.booklight.json` annotations beside the source; existing profiles keep their configured storage location. Recent paths and resume page live in a small app settings file. Old `library.json` and cover data are left untouched but are no longer used. Reading does not duplicate the source file.

Back up original PDF/EPUB files and Booklight settings/annotation sidecars together. There is no managed book library, reading goal or reading-time database. PDF edits can change document bytes; preserve an untouched original and reopen exported files in an independent reader.

The Notes pane reports annotation saves that are still in progress or failed. If a save fails, Booklight keeps an app-owned recovery copy: use **Retry save** after fixing the destination, or **Export** to preserve the current notes elsewhere. On the next open, **Recovered — save pending** means the recovery copy was loaded and still needs to reach the selected central or sidecar location. A successful retry clears that state. This recovery never changes the original PDF or EPUB.

## Previous installations

Booklight does not clone an old PDFGist configuration directory. If only
`site.jrtx.pdfgist` exists, it is reused directly; if the Booklight
profile already exists, that profile takes priority. Keep both folders
until you have inspected your settings and opened important annotations.
Changing the annotation storage preference does not automatically move
older notes between central and sidecar locations.

## Save an edited PDF

Page rotation, insertion, deletion, merge and watermark operations may first
modify the document **in memory only**. The source PDF is not silently
replaced. To preserve that edited document, use **File > Save PDF…**
(Command-Shift-S) on macOS, or **Tools > Save PDF…** on Windows/Linux.
The suggested `-edited.pdf` filename is only a suggestion; confirm the
destination yourself. macOS writes exports atomically. Do not close an edited
document until you have saved it and reopened the output to verify the result.
All three hosts show a native unsaved-change confirmation before closing a
document with unsaved PDF edits (shipped in 0.9.0).

## Research across documents

The research workbench searches the text of your open and recent documents
locally — no account, no upload. Open it from **Tools > Research Workbench…**
(Option-Command-R on macOS; Ctrl+Alt+R on Windows and Linux). Results show the
matching passage and its source: jump to the exact page or chapter, copy a
formatted citation, or pin passages to a citation deck. Pinned citations are
stored as ordinary `cite` annotations on the source document, so they follow
the same save/recovery contract as other notes.

## Network, keys and privacy

Reading is local. AI requests send selected text and conversation context to your chosen provider. Review the send scope and provider policy before processing confidential files; BYOK usage is billed by that provider.

Before every translation, summary or chat request, Booklight shows a native review dialog with the action, the selected scope and exact Unicode-character counts for document content, conversation history, your question and their total. Only **Continue** starts the provider request; **Cancel** leaves the typed question and conversation unchanged. The displayed total covers your content, not Booklight's additional provider instructions.

Your API key is not written to `settings.json`. Booklight stores it in the operating system credential store (macOS Keychain, Linux Secret Service, or Windows DPAPI) and falls back to an owner-only file only when no desktop store is available. Provider error text is redacted before it can appear in a dialog or support log; `settings.json` keeps an empty `api_key` field for compatibility with older releases.

## Known limits

Damaged or hostile PDF/EPUB inputs are bounded by explicit resource budgets and fail with a clear open error instead of exhausting memory; the limits are documented in [the file-first design](FILE-FIRST-DESIGN.md#hostile-and-damaged-input). The same welcome, reader, sidebar, annotation, AI and settings information architecture is implemented on macOS, Windows and Linux, with each host using its own native chrome. Each host still needs its packaged accessibility acceptance before a design change is claimed as shipped.

## Updates and removal

Back up data and read the destination release notes first. An in-app update check does not necessarily install an update for your channel; follow the manual for portable, installer and source paths. Never bypass a failed signature check or replace the program with an untrusted artifact. On failure, preserve data and the error, then recover with a trusted release. Removing the program does not necessarily erase its separate data directory; verify your backup before deleting personal data.

## Get help

Report problems in [GitHub Issues](https://github.com/turlamai/booklight/issues), in English or Chinese. Include the version, OS/architecture, installation channel, steps, expected result and actual result. Reproduce with synthetic input first; do not attach keys, credentials, databases, private source documents or complete logs. Public support has no guaranteed response time.

## Further reading

- [Detailed reading manual](../site/guide.html)
- [Project overview and development](../README.md)
- [Current licence](../LICENSE)
