# Booklight visual system

**Status:** macOS implementation landed in the file-first line; Windows and
Linux keep their first-party Fluent and Adwaita chrome and consume the same
information hierarchy. This is a design contract, not a claim that every host
already renders identically.

Booklight is a document reader, not a collection manager. The interface should
feel like a well-made reading tool: quiet at rest, precise in motion, and
honest about what is saved. The visual target is Apple's document applications
(Preview, Books, Pages) on macOS, and the platform's own conventions on
Windows and Linux — never a cross-platform skin that feels foreign everywhere.

## Principles

1. **The document is the interface.** Chrome recedes; paper, type and selection
   carry the hierarchy. No persistent decoration, gradients or glass for its
   own sake.
2. **One accent.** Terracotta marks the product, the primary action and the
   selected navigation item. Red is reserved for failure; green for recovery.
3. **Two planes, not one sheet.** The reading canvas and the navigation surface
   differ in tone. The sidebar is a material, not more page.
4. **Native structure.** Use the platform's split view, toolbar, form, list,
   sheet, menu and empty-state components. Custom drawing is limited to the
   product mark and reading typography.
5. **Status is visible, not noisy.** Annotation writes show pending, recovered
   and failed states in place. A failure never looks like success.
6. **Keyboard and accessibility are part of the look.** Every icon-only control
   has a label and help text; every list supports arrow-key traversal.

## Surfaces

| Token | Light | Dark | Use |
|---|---|---|---|
| `bg` | `#F7F4EF` | `#171412` | reading canvas, welcome paper |
| `sidebar` | `#EFEBE4` | `#211D1A` | navigation plane, inspector |
| `panel` | `#FFFDF9` | `#2A2521` | grouped rows, floating status |
| `panelAlt` | `#F2EEE7` | `#322C27` | hover, page field, secondary fill |
| `border` | separator @ 70% | separator @ 70% | hairlines only |
| `accent` | `#C4623E` | `#E8926A` | wordmark, primary action, selection |
| `danger` | system red | system red | recoverable write failure |

The canvas is warm paper rather than pure white; dark mode uses the same warm
black as the EPUB page so the page edge never flashes during load.

## Type

Use system roles, not pixel sizes, except for the product wordmark and the
reading body.

| Role | macOS | Notes |
|---|---|---|
| Wordmark | 36 pt semibold, tracking −0.7 | welcome only |
| Window title | 13 pt semibold + 10 pt secondary subtitle | titlebar |
| Section heading | `.headline` / `.caption.weight(.semibold)` | welcome, sidebar |
| Body | `.body` / 12–13 pt | controls, notes |
| Secondary | `.footnote` / `.caption` | paths, metadata |
| Reading body | New York / Iowan Old Style / Georgia, 17 pt × user scale, 1.75 line-height | EPUB |
| Reading measure | 38 rem (≈ 608 px) centred | EPUB chapter and title |

PDF pages keep their own typography; Booklight never restyles document text.

## Spacing and shape

- 4 / 8 / 12 / 16 / 24 / 32 pt scale.
- Radii: 6 (small), 10 (medium), 14 (large). Grouped rows use 10; the welcome
  mark uses 15.
- Hairlines are 0.5–1 pt and only separate planes or rows. Avoid fill + border
  + shadow on the same card.
- Motion is limited to 120–180 ms ease-out for find, selection and inspector
  changes. No bounce, no parallax.

## Window anatomy (macOS)

```
┌ traffic lights ─ sidebar toggle ─ document title ─ page/zoom/actions ┐
│ sidebar header (view switcher) │ document                            │
│ thumbnails / outline / notes   │ PDF page or reflowed EPUB           │
│ forms                         │ optional AI inspector               │
└───────────────────────────────┴──────────────────────────────────────┘
```

- One window per document, native tabs, represented file and edited dot.
- The sidebar owns its view switcher. The titlebar never hosts a four-segment
  picker that pushes the document title out of place.
- The document title and a one-line subtitle (`PDF · 12 pages`,
  `EPUB · chapter 2 / 5`) identify the content without a second toolbar.
- Empty windows show the welcome state; closing the last document returns to
  it rather than leaving a bare Dock icon.

## Welcome state

An editorial two-column composition on wide windows and a stack on narrow
windows:

- Left: product mark, wordmark, one sentence of promise, one prominent
  **Open Document…** action, one drag hint, one privacy sentence.
- Right: up to eight recent paths as a grouped list. Rows show type icon,
  file name, abbreviated folder, resume page and a hover chevron.
- Empty recents use a dashed, quiet placeholder — never a fake bookshelf.

## Sidebar and inspector

- Native `List` / `OutlineGroup` for outline and chapters, with keyboard
  selection and automatic scroll-to-current.
- Notes are rows, not cards: colour dot, page/chapter, excerpt, note, and an
  overflow menu for Translate / Delete. Hover reveals the menu; the row itself
  jumps to the source.
- Save state is a compact banner: spinner for pending, warning + Retry Save +
  Export for recovered/failed.
- AI inspector uses three native tabs, a context-scope picker, native empty
  states, and an explicit send-review sheet before any provider request.

## Settings

A native grouped `Form` with platform labels, focus rings and keyboard
traversal: provider preset, base URL, model, secure key field, connection
actions, reading preferences, language, and privacy/support links. No custom
button skins or cramped fixed-width column.

## Platform boundary

- **macOS:** SwiftUI/AppKit implementation of the system above.
- **Windows:** WinUI 3 follows Fluent hierarchy (CommandBar, NavigationView,
  Mica where available). Do not transplant macOS spacing or materials.
- **Linux:** GTK4 follows Adwaita/libadwaita hierarchy and system accent.
- Shared behavior, strings, recovery and security contracts stay in the Racket
  core. A visual change on one platform is not evidence that another platform
  is finished; each host still needs its packaged accessibility acceptance.

## Verification

Screenshots are review aids, not acceptance. A design change is complete when:

- the packaged host exposes the expected controls and text in the native
  accessibility tree;
- keyboard focus reaches every primary action;
- light/dark and increased-contrast appearances keep text ≥ 4.5:1;
- the file-first guard and Racket tests still pass;
- the platform's packaged acceptance suite is green.
