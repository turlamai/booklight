# Booklight user guide

**English** · [简体中文](USER-GUIDE.zh-CN.md)

Read PDF and EPUB files, annotate them and optionally ask your own AI provider about a document.

## Availability

Existing reading, annotation, light editing and single-document BYOK features remain free. No research Pro checkout is open.

## Platforms and installation

Desktop previews for macOS, Windows and Linux. Check the release for the architecture you need; mobile is not included.

Use [official Releases](https://github.com/turlamai/booklight/releases), select an explicitly listed OS/architecture asset, read its release notes and check supplied checksums. An absent asset/platform is not a delivery promise. Preview builds may lack publisher signing or notarization; do not disable system-wide security protections to install them.

## Your first successful session

1. Open a copy of a small PDF or EPUB. Import references the file in place rather than making a second book copy.
2. Add an annotation, close and reopen the book, and confirm it persists before working on valuable material.
3. Try page editing or export only on a duplicate. Relink moved books instead of assuming the library copied them.

## Data, backup and recovery

Back up original books and Booklight settings/annotation sidecars together. A library backup alone does not contain the referenced books. PDF edits can change document bytes; preserve an untouched original and reopen exported files in an independent reader.

The Notes pane reports annotation saves that are still in progress or failed. If a save fails, Booklight keeps an app-owned recovery copy: use **Retry save** after fixing the destination, or **Export** to preserve the current notes elsewhere. On the next open, **Recovered — save pending** means the recovery copy was loaded and still needs to reach the selected central or sidecar location. A successful retry clears that state. This recovery never changes the original PDF or EPUB.

## Network, keys and privacy

Reading is local. AI requests send selected text and conversation context to your chosen provider. Review the send scope and provider policy before processing confidential files; BYOK usage is billed by that provider.

Before every translation, summary or chat request, Booklight shows a native review dialog with the action, the selected scope and exact Unicode-character counts for document content, conversation history, your question and their total. Only **Continue** starts the provider request; **Cancel** leaves the typed question and conversation unchanged. The displayed total covers your content, not Booklight's additional provider instructions.

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
