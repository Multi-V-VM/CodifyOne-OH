"use strict";

// Extracts the js2c "electron/js2c/browser_init" webpack bundle (the browser
// process bootstrap, already carrying every patch this repo applies to
// libelectron.so) out of the patched shared library and drops it next to the
// packaged app resources, where v8_pool_hook loads it from when the port's
// node startup fails to run browser_init on its own.
//
// Layout inside libelectron.so (js2c region, discovered via the TerserPlugin
// license comments every webpack build appends):
//   ... asar_bundle source ... "asar_bundle.js.LICENSE.txt"
//   [browser chunk A: asar fs-wrapper prelude]
//   "browser_init.js.LICENSE.txt"
//   [browser chunk B: ./lib/browser/api/* + init.ts entry]
//   "isolated_bundle.js.LICENSE.txt"
//   ...
// Chunk A's trailing license comment is a legal JS comment, so everything
// between the asar license and the isolated license parses as one script.

const fs = require("fs");
const path = require("path");

const projectRoot = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const targetName = process.env.OHCODE_TARGET_NAME || "default";

const candidates = [
  path.join(projectRoot, "electron", "libs", "arm64-v8a", "libelectron.so"),
  path.join(
    projectRoot,
    "electron",
    "build",
    targetName,
    "intermediates",
    "libs",
    targetName,
    "arm64-v8a",
    "libelectron.so"
  )
];

const outPath = path.join(
  projectRoot,
  "web_engine",
  "src",
  "main",
  "resources",
  "resfile",
  "resources",
  "app",
  "ohcode-browser-init.js"
);

function licenseMarker(name) {
  return Buffer.from(
    `/*! For license information please see ${name}.LICENSE.txt */`
  );
}

const soPath = candidates.find((candidate) => fs.existsSync(candidate));
if (!soPath) {
  throw new Error(`libelectron.so not found in: ${candidates.join(", ")}`);
}

const data = fs.readFileSync(soPath);
const startMarker = licenseMarker("asar_bundle.js");
const endMarker = licenseMarker("isolated_bundle.js");

// The js2c region is the only place these markers sit adjacent; scan for the
// pair with the browser chunk signature between them.
const bundleSignature = Buffer.from('"./lib/asar/fs-wrapper.ts"');
let start = -1;
let cursor = 0;
while (true) {
  const markerAt = data.indexOf(startMarker, cursor);
  if (markerAt < 0) {
    break;
  }
  const after = markerAt + startMarker.length;
  const window = data.subarray(after, after + 64);
  if (window.indexOf(bundleSignature) >= 0) {
    start = after;
    break;
  }
  cursor = markerAt + 1;
}
if (start < 0) {
  throw new Error("browser_init asar prelude not found under asar_bundle license");
}

const end = data.indexOf(endMarker, start);
if (end < 0 || end - start > 2 * 1024 * 1024) {
  throw new Error(
    `browser_init bundle end not found (start=0x${start.toString(16)}, end=0x${end.toString(16)})`
  );
}

const source = data.subarray(start, end);
fs.mkdirSync(path.dirname(outPath), { recursive: true });
fs.writeFileSync(outPath, source);
console.info(
  `[OHcode] Extracted browser_init bundle ${source.length} bytes from ${path.relative(
    projectRoot,
    soPath
  )} to ${path.relative(projectRoot, outPath)}`
);
