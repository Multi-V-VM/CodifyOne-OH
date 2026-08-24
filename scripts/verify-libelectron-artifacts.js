"use strict";

// Pre-flight check for a candidate libelectron.so + snapshot pair before
// swapping them into the repo. The V8 snapshot handshake requires the
// snapshot's embedded version string to match the runtime's V8 — necessary
// (not sufficient: the build fingerprint also covers flags/external
// references), but it filters the common "wrong build" case in seconds.
//
// Usage:
//   node scripts/verify-libelectron-artifacts.js \
//     --so <libelectron.so> \
//     [--context-snapshot <v8_context_snapshot.bin>] \
//     [--startup-snapshot <snapshot_blob.bin>]
// Defaults pull the current repo artifacts for comparison.

const fs = require("fs");
const path = require("path");
const args = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};

const root = path.resolve(flag("--root", path.join(__dirname, "..")));
const soPath = flag("--so",
  path.join(root, "electron", "libs", "arm64-v8a", "libelectron.so"));
const ctxPath = flag("--context-snapshot",
  path.join(root, "web_engine/src/main/resources/resfile/v8_context_snapshot.bin"));
const blobPath = flag("--startup-snapshot",
  path.join(root, "web_engine/src/main/resources/resfile/snapshot_blob.bin"));

function extractIdentity(so) {
  const idents = {};
  const patterns = {
    electron: /Electron\/[0-9.]+/,
    chrome: /Chrome\/[0-9.]+/,
    node: /node\.js\/v[0-9.]+/,
    v8: /[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+-electron\.[0-9]+/
  };
  for (const [name, re] of Object.entries(patterns)) {
    const m = re.exec(so.toString("latin1"));
    idents[name] = m ? m[0] : null;
  }
  return idents;
}

function parseContextSnapshot(buf) {
  if (buf.length < 40) return { error: "too small" };
  const version = buf.readUInt32LE(0);
  const contextCount = buf.readUInt32LE(4);
  const hash = buf.readUInt32LE(8);
  const versionString = buf.subarray(12, 44).toString("latin1").replace(/\0.*$/, "");
  return {
    bytes: buf.length,
    headerVersion: version,
    contextCount,
    hash: "0x" + hash.toString(16),
    embeddedV8Version: versionString,
    looksMinimal: buf.length < 300 * 1024
  };
}

const report = { so: soPath, snapshot: ctxPath };

const so = fs.readFileSync(soPath);
report.identity = extractIdentity(so);
report.snapshotInfo = parseContextSnapshot(fs.readFileSync(ctxPath));
if (fs.existsSync(blobPath)) {
  const blob = fs.readFileSync(blobPath);
  report.startupBlobBytes = blob.length;
}

const v8FromSo = report.identity.v8;
const v8FromSnapshot = report.snapshotInfo.embeddedV8Version;
report.versionMatch =
  v8FromSo && v8FromSnapshot
    ? (v8FromSnapshot.startsWith(v8FromSo) ||
       v8FromSo.startsWith(v8FromSnapshot.split("-electron")[0]))
    : "unknown";

console.info(JSON.stringify(report, null, 2));
const fatal =
  report.versionMatch === false ||
  (report.snapshotInfo.error != null) ||
  report.snapshotInfo.looksMinimal;
if (fatal) {
  console.error(
    report.snapshotInfo.looksMinimal
      ? "[OHcode] snapshot looks MINIMAL (<300KB) — the builtin map will still be empty"
      : "[OHcode] version mismatch — V8 will reject this snapshot");
  process.exitCode = 2;
} else {
  console.info("[OHcode] artifacts pass the pre-flight version check");
}
