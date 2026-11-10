# Third-party notices

Booklight builds on the components below. Each entry names where the
component enters the shipped artifacts and the license it is distributed
under; consult the upstream license files for full terms. The Rivet
toolchain is a first-party TurLamAI project consumed as a pinned package —
not third-party code — so it is not listed here. Every `raco rivet
release` build also generates a `THIRD_PARTY_NOTICES.txt` and a CycloneDX
1.6 `sbom.cdx.json` in the build `dist/` from the toolchain's fail-closed
license audit (`rivet-cli/compliance.rkt`); this file is the grouped
overview of what actually ships.

## Racket CS runtime

Every platform host embeds the statically linked Racket CS runtime
([racket/racket](https://github.com/racket/racket), 9.x). Racket is
distributed under the MIT license and the Apache License 2.0, at your
option; the Racket CS runtime system additionally embeds Chez Scheme,
distributed under the Apache License 2.0 (per Racket's `LICENSE.txt`).

## crypto (Racket package)

The update channel verifies Ed25519 signatures through the Racket
[crypto](https://github.com/rmculpepper/crypto) package (compiled into
the embedded runtime image through Rivet's distribution layer),
distributed under the Apache License 2.0.

## PDFium (Windows and Linux hosts)

PDF rendering on Windows and Linux uses
[PDFium](https://pdfium.googlesource.com/pdfium/) via the pinned
[bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)
build `chromium/8100`, downloaded at build time and verified against the
SHA-256 digests recorded at pin time (`windows/fetch-pdfium.ps1`,
`linux/CMakeLists.txt`); the library rides beside the executable. PDFium
is distributed under the BSD 3-Clause license ("Copyright (c) The PDFium
Authors"); it bundles further third-party components (freetype, libjpeg,
openjpeg and friends) under their own licenses — see upstream's `LICENSE`
and the binaries' included notice files. The macOS host uses
Apple's system PDFKit instead and ships no PDF engine.

## GTK 4, libadwaita and WebKitGTK (Linux host)

The Linux host is a GTK 4 application built against libadwaita, and
renders EPUB chapters with WebKitGTK. GTK, GLib, libadwaita and
WebKitGTK are licensed under the GNU Lesser General Public License,
version 2.1 or later, and are supplied by the distribution for the deb,
rpm, tar.gz and portable zip shapes; the `.AppImage` bundles the library
closure it needs, staged by the Rivet toolchain from the build
distribution's packages (same LGPL-2.1-or-later terms).

## Windows App SDK and C++/WinRT (Windows host)

The Windows workbench builds against the NuGet packages
`Microsoft.WindowsAppSDK` and `Microsoft.Windows.CppWinRT` (both
MIT-licensed, see `windows/RivetHost.vcxproj`). The Windows App SDK
runtime is deployed on the target machine as Microsoft's framework
package under its own terms — the Booklight MSI does not republish it.

## AppImageKit runtime (`.AppImage` artifacts)

`.AppImage` release artifacts contain the type-2 AppImage runtime from
[AppImage/AppImageKit](https://github.com/AppImage/AppImageKit), licensed
MIT. The build tool is the checksum-verified official `appimagetool`
fetched by the Rivet toolchain. AppImageKit's license does not apply to
the contents of the AppImage: those are Booklight's AGPL-3.0 payload plus
the entries above.
