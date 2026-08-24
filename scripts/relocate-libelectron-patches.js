"use strict";

// Relocates the hard-coded binary patch offsets for a NEW libelectron.so /
// libadapter.so built from the same tree (or a rebased one).
//
// The patch scripts anchor two kinds of edits:
//   - content-searched snippets (patch-libelectron-browser-init.js "patches"
//     list): these self-relocate, we only verify they still match;
//   - hard-coded offsets (browser-init binaryPatches #1/#4,
//     patch-libadapter-async-command.js): these drift with every rebuild.
//
// Strategy: take a context window around each known offset in the OLD .so and
// find its unique occurrence in the NEW .so — the patch site relocates to
// hit + windowRadius. Emits a report and a JSON for scripting.
//
// Usage:
//   node scripts/relocate-libelectron-patches.js --new <new-libelectron.so> \
//       [--old <current-libelectron.so>] [--adapter <new-libadapter.so>] \
//       [--root <project-root>]
// Defaults: --old = <root>/electron/libs/arm64-v8a/libelectron.so,
//           --root = repository root (two levels up from this script).

const fs = require("fs");
const path = require("path");

const args = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};

const projectRoot = path.resolve(flag("--root", path.join(__dirname, "..")));
const defaultElectron = path.join(
  projectRoot, "electron", "libs", "arm64-v8a", "libelectron.so");
const oldSoPath = flag("--old", defaultElectron);
const newSoPath = flag("--new", defaultElectron);
const newAdapterPath = flag("--adapter", null);

// (name, so, oldOffset) — mirrors the offsets hard-coded in the patch
// scripts; keep in sync when those change.
const ELECTRON_OFFSETS = [
  ["disable incompatible browser Node startup snapshot", 0x2cfbe94],
  ["restore Node source execution thunk", 0x857fbf0]
];
const ADAPTER_OFFSET = 0xd6254; // libadapter ExecuteCommand condvar wait

const WINDOW = 48; // bytes of context on each side of a patch site

function findUnique(haystack, needle, from = 0) {
  const hits = [];
  let i = haystack.indexOf(needle, from);
  while (i >= 0 && hits.length < 5) {
    hits.push(i);
    i = haystack.indexOf(needle, i + 1);
  }
  return hits;
}

function relocate(oldSo, newSo, name, oldOffset) {
  const start = Math.max(0, oldOffset - WINDOW);
  const window = oldSo.subarray(start, oldOffset + WINDOW);
  const hits = findUnique(newSo, window);
  const result = { name, oldOffset, status: "", newOffset: null };
  if (hits.length === 1) {
    result.newOffset = hits[0] + (oldOffset - start);
    result.status = "relocated";
  } else if (hits.length === 0) {
    // Fall back to a tighter window (patch bytes themselves may differ if
    // already applied or codegen changed).
    const tight = oldSo.subarray(oldOffset - 8, oldOffset + 8);
    const tightHits = findUnique(newSo, tight);
    if (tightHits.length === 1) {
      result.newOffset = tightHits[0] + 8;
      result.status = "relocated-tight";
    } else {
      result.status = `not-found(hits=${tightHits.length})`;
    }
  } else {
    result.status = `ambiguous(${hits.length})`;
  }
  return result;
}

function checkSnippets(newSo) {
  // The content-searched patches from patch-libelectron-browser-init.js that
  // must keep matching. Each entry: [name, originalSnippet, patchedSnippet].
  // A snippet that matches neither means the build drifted and the patch
  // script needs a new anchor.
  const snippets = [
    [
      "restore Node run_main",
      "RegExpPrototypeExec(/^/, '');" +
        "\n\n// Note: this loads the module through the ESM loader",
      "require('internal/modules/cjs/loader').Module.runMain(process.argv[1]);" +
        "\n\n// Note: this loads the module through the ESM loader"
    ],
    [
      "restore Electron search-path init",
      'process.argv.splice(1,1),__webpack_require__("./lib/common/reset-search-paths.ts")',
      null // patched form is identical (this snippet is the RESTORED form)
    ],
    [
      "restore Electron browser services",
      '__webpack_require__("./lib/browser/rpc-server.ts")',
      null
    ],
    [
      "entry probe",
      'l?(process._firstFileName=i._resolveFilename(s.join(l,p),null,!1),i._load(s.join(l,p),i,!0)):',
      'l?(p="ohcode-entry-probe.js",process._firstFileName=s.join(l,p),i._load(s.join(l,p),i,!0))'
    ]
  ];
  return snippets.map(([name, original, patched]) => {
    const hasOriginal = newSo.includes(Buffer.from(original, "latin1"));
    const hasPatched = patched
      ? newSo.includes(Buffer.from(patched, "latin1"))
      : hasOriginal;
    return {
      name,
      present: hasOriginal || hasPatched,
      state: hasOriginal ? "original" : hasPatched ? "already-patched" : "missing"
    };
  });
}

const report = { oldSo: oldSoPath, newSo: newSoPath, electron: [], snippets: [], adapter: null };

const oldSo = fs.readFileSync(oldSoPath);
const newSo = fs.readFileSync(newSoPath);
for (const [name, off] of ELECTRON_OFFSETS) {
  report.electron.push(relocate(oldSo, newSo, name, off));
}
report.snippets = checkSnippets(newSo);

if (newAdapterPath) {
  const oldAdapter = fs.readFileSync(path.join(
    projectRoot, "electron", "libs", "arm64-v8a", "libadapter.so"));
  const newAdapter = fs.readFileSync(newAdapterPath);
  report.adapter = relocate(oldAdapter, newAdapter, "adapter condvar wait", ADAPTER_OFFSET);
}

console.info(JSON.stringify(report, null, 2));

const bad = report.electron.filter((r) => r.status !== "relocated" && r.status !== "relocated-tight")
  .concat(report.adapter && report.adapter.status.indexOf("relocated") < 0 ? [report.adapter] : []);
const missingSnippets = report.snippets.filter((s) => !s.present);
if (bad.length || missingSnippets.length) {
  console.error(`[OHcode] relocation incomplete: ${bad.length} offsets, ${missingSnippets.length} snippets`);
  process.exitCode = 2;
} else {
  console.info("[OHcode] all anchors relocated; update the offsets in the patch scripts accordingly");
}
