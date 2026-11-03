# Booklight user guide

**English** · [简体中文](USER-GUIDE.zh-CN.md)

Read PDF and EPUB files, annotate them and optionally ask your own AI provider about a document.

## Availability

Existing reading, annotation, light editing and single-document BYOK features remain free. No research Pro checkout is open.

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
The native unsaved-change close prompt and automated export/reopen verification
are still release-qualification work, not a promise of shipped behavior.

## Network, keys and privacy

Reading is local. AI requests send selected text and conversation context to your chosen provider. Review the send scope and provider policy before processing confidential files; BYOK usage is billed by that provider.

## Known limits

Damaged-book recovery and AI cancellation/secret storage remain release qualification work (#20). Annotation persistence recovery is implemented for the next release but remains subject to native-platform acceptance before release. A new multi-document research workbench is not delivered.

## Updates and removal

Back up data and read the destination release notes first. An in-app update check does not necessarily install an update for your channel; follow the manual for portable, installer and source paths. Never bypass a failed signature check or replace the program with an untrusted artifact. On failure, preserve data and the error, then recover with a trusted release. Removing the program does not necessarily erase its separate data directory; verify your backup before deleting personal data.

## Get help

Report problems in [GitHub Issues](https://github.com/turlamai/booklight/issues), in English or Chinese. Include the version, OS/architecture, installation channel, steps, expected result and actual result. Reproduce with synthetic input first; do not attach keys, credentials, databases, private source documents or complete logs. Public support has no guaranteed response time.

## Further reading

- [Detailed reading manual](../docs/guide.html)
- [Project overview and development](../README.md)
- [Current licence](../LICENSE)
