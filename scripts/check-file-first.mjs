// File-first architecture guard: fast, dependency-free and platform-neutral.
// It cannot replace packaged UI or Racket tests, but prevents the retired
// Books-style catalog from silently returning during routine feature work.
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const text = (path) => readFileSync(join(root, path), "utf8");
const absent = [
  "racket/booklight/library.rkt",
  "racket/tests/library-test.rkt",
  "linux/src/shelf.cpp",
  "linux/src/shelf.hpp",
  "windows/MainWindow.Shelf.cpp",
];
const failures = [];
for (const path of absent) {
  if (existsSync(join(root, path))) failures.push("retired bookshelf returned: " + path);
}
function requires(path, pattern, message) {
  if (!pattern.test(text(path))) failures.push(path + ": missing " + message);
}
function forbids(path, pattern, message) {
  if (pattern.test(text(path))) failures.push(path + ": forbidden " + message);
}
requires("macos-host/Sources/RivetHost/LibraryView.swift", /struct FileWelcomeView:\s*View/, "native open welcome");
requires("macos-host/Sources/RivetHost/ContentView.swift", /FileWelcomeView\(\)/, "welcome as empty-document state");
forbids("macos-host/Sources/RivetHost/AppModel.swift", /\b(startReadingClock|loadLibrary|importBooks|rescanShelf)\s*\(/, "library import or reading-time service");
requires("windows/MainWindow.xaml", /x:Name="WelcomeView"/, "file-open welcome");
forbids("windows/MainWindow.xaml", /x:Name="ShelfNav"|x:Name="GoalsCard"/, "Books-style UI");
forbids("windows/MainWindow.xaml.cpp", /\b(ImportBook|ImportBooks|RefreshShelf|StartReadingClock|LoadReadingStats)\s*\(/, "import/reading-time service");
forbids("windows/MainWindow.xaml.cpp", /\bentry\.title\b/, "title on RecentEntry (not in RPC schema)");
requires("linux/src/main.cpp", /gtk_file_chooser_set_select_multiple/, "native multi-file opening");
forbids("linux/src/main.cpp", /g_host\.shelf\b|make_unique<pdfgist::Shelf>/, "shelf instance");
forbids("linux/CMakeLists.txt", /src\/shelf\.cpp/, "retired library target");
forbids("racket/booklight/settings.rkt", /copy-directory\/files|migrate-legacy-config!/, "implicit old-directory copy");
requires("racket/booklight/settings.rkt", /call-with-atomic-output-file/, "crash-safe settings replacement");
forbids("app/backend.rkt", /library:(?:update-shelf!|load-shelf|covers-dir-path)/, "backend library writes");
requires("racket/tests/backend-test.rkt", /retired-library-bytes/, "legacy catalog preservation test");
requires("linux/src/pdfview.hpp", /memory_bytes_/, "owned PDFium in-memory backing buffer");
requires("linux/src/pdfview.cpp", /doc->memory_bytes_ = std::move\(bytes\)/,
         "PDFium buffer lifetime protection");
requires("macos-host/Sources/RivetHost/ReaderModel.swift", /func exportEditedPdf\(/,
         "explicit export of modified PDF");
requires("windows/MainWindow.Tools.cpp", /void MainWindow::ToolsSavePdf_Click\(/,
         "explicit WinUI PDF save/export action");
requires("linux/src/main.cpp", /void tools_save_pdf\(/,
         "explicit GTK PDF save/export action");
requires("linux/src/pdfview.hpp", /pdf_edits_pending/, "independent PDF dirty state");
requires("linux/src/main.cpp", /confirm_close_dirty_pdf\(/, "unsaved PDF close confirmation");
requires("linux/src/main.cpp", /"close-request"/, "window close interception");
requires("linux/src/main.cpp", /std::filesystem::equivalent\(/, "source-file alias safety");

if (failures.length > 0) {
  for (const failure of failures) console.error("file-first contract: " + failure);
  process.exitCode = 1;
} else {
  console.log("file-first contract: static architecture guard passed");
}
