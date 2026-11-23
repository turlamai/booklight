// File-first architecture guard: fast, dependency-free and platform-neutral.
// It cannot replace packaged UI or Racket tests, but prevents the retired
// Books-style catalog from silently returning during routine feature work.
// Since #75 S6 the surface is the single web UI on the glaze carrier; the
// native host legs are retired, so the guard reads host/web + host/main.rkt.
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const text = (path) => readFileSync(join(root, path), "utf8");
const absent = [
  "racket/booklight/library.rkt",
  "racket/tests/library-test.rkt",
  "macos-host",
  "windows",
  "linux",
  "rivet.rktd",
];
const failures = [];
for (const path of absent) {
  if (existsSync(join(root, path))) failures.push("retired surface returned: " + path);
}
function requires(path, pattern, message) {
  if (!pattern.test(text(path))) failures.push(path + ": missing " + message);
}
function forbids(path, pattern, message) {
  if (pattern.test(text(path))) failures.push(path + ": forbidden " + message);
}
// Welcome / drop-to-open is the empty-document state (no shelf).
requires("host/web/index.html", /id="dropmask"/, "welcome drop mask");
requires("host/web/app.js", /api\/host\/scratch/, "drop stages through the carrier scratch dir");
forbids("host/web/app.js", /\b(loadLibrary|importBooks|startReadingClock|rescanShelf|refreshShelf)\s*\(/,
        "library import or reading-time service");
// Explicit save-as: page edits stay in memory until the user exports.
requires("host/web/app.js", /id="pendingbar"|\$\("#pendingbar"\)/, "pending page-edit bar");
requires("host/web/app.js", /active\.dirty/, "independent PDF dirty state");
requires("host/web/app.js", /api\/host\/write/, "explicit write/export path");
requires("host/main.rkt", /'dialog:save/, "explicit save-as capability");
requires("host/main.rkt", /'host:write/, "host write capability");
// Settings stay crash-safe and never copy the old config tree implicitly.
forbids("racket/booklight/settings.rkt", /copy-directory\/files|migrate-legacy-config!/, "implicit old-directory copy");
requires("racket/booklight/settings.rkt", /call-with-atomic-output-file/, "crash-safe settings replacement");
forbids("app/backend.rkt", /library:(?:update-shelf!|load-shelf|covers-dir-path)/, "backend library writes");
requires("racket/tests/backend-test.rkt", /retired-library-bytes/, "legacy catalog preservation test");

if (failures.length > 0) {
  for (const failure of failures) console.error("file-first contract: " + failure);
  process.exitCode = 1;
} else {
  console.log("file-first contract: static architecture guard passed");
}