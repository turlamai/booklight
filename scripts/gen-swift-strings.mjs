#!/usr/bin/env node
// Generate macos-host/Sources/RivetHost/GeneratedStrings.swift from the
// shared/i18n single source (zh.json / en.json). Run from the repo root:
//   node scripts/gen-swift-strings.mjs
import { readFileSync, writeFileSync } from "node:fs";

const zh = JSON.parse(readFileSync("shared/i18n/zh.json", "utf8"));
const en = JSON.parse(readFileSync("shared/i18n/en.json", "utf8"));

const esc = (s) =>
  String(s)
    .replace(/\\/g, "\\\\")
    .replace(/"/g, '\\"')
    .replace(/\n/g, "\\n")
    .replace(/\r/g, "\\r")
    .replace(/\t/g, "\\t");
const table = (obj) =>
  Object.entries(obj)
    .map(([k, v]) => `    "${esc(k)}": "${esc(String(v))}",`)
    .join("\n");

const out = `// Generated from shared/i18n/{zh,en}.json — the single i18n source.
// Do not edit; run: node scripts/gen-swift-strings.mjs

enum L10n {
    nonisolated(unsafe) static var language: String = "zh"

    private static let zh: [String: String] = [
${table(zh)}
    ]

    private static let en: [String: String] = [
${table(en)}
    ]

    /// Look up a key in the active language, falling back to zh then the key
    /// itself. "{0}"-style placeholders are substituted from the args.
    static func t(_ key: String, _ args: String...) -> String {
        let table = language == "en" ? en : zh
        var s = table[key] ?? zh[key] ?? key
        for (i, arg) in args.enumerated() {
            s = s.replacingOccurrences(of: "{\\(i)}", with: arg)
        }
        return s
    }
}
`;

const target = "macos-host/Sources/RivetHost/GeneratedStrings.swift";
if (process.argv.includes("--check")) {
  let actual;
  try {
    actual = readFileSync(target, "utf8");
  } catch {
    console.error(`${target}: missing generated file; run node scripts/gen-swift-strings.mjs`);
    process.exitCode = 1;
  }
  if (actual !== undefined && actual !== out) {
    console.error(`${target}: generated strings are stale; run node scripts/gen-swift-strings.mjs`);
    process.exitCode = 1;
  }
  if (process.exitCode !== 1) console.log(`GeneratedStrings.swift verified (${Object.keys(zh).length} zh / ${Object.keys(en).length} en keys)`);
} else {
  writeFileSync(target, out);
  console.log(`GeneratedStrings.swift: ${Object.keys(zh).length} zh / ${Object.keys(en).length} en keys`);
}
