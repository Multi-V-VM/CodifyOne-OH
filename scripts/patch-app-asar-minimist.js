"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const projectRoot = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const resourcesRoot = path.join(projectRoot, "web_engine", "src", "main", "resources", "resfile", "resources");
const appDir = path.join(resourcesRoot, "app");
const appAsar = path.join(resourcesRoot, "app.asar");
const patchRoot = path.join(projectRoot, "scripts", "patches");
const minimistPatchDir = path.join(patchRoot, "minimist");
const gracefulFsPatchDir = path.join(patchRoot, "graceful-fs");
const nativeKeymapPatchDir = path.join(patchRoot, "native-keymap");
const vscodeSqlite3PatchDir = path.join(patchRoot, "vscode-sqlite3");
const moduleDir = path.join(appDir, "node_modules", "minimist");
const filesToInstall = [
  ["out/main.js", path.join(appDir, "out", "main.js")],
  ["node_modules/minimist/index.js", path.join(minimistPatchDir, "index.js")],
  ["node_modules/minimist/package.json", path.join(minimistPatchDir, "package.json")],
  ["node_modules/graceful-fs/index.js", path.join(gracefulFsPatchDir, "index.js")],
  ["node_modules/graceful-fs/package.json", path.join(gracefulFsPatchDir, "package.json")],
  ["node_modules/native-keymap/index.js", path.join(nativeKeymapPatchDir, "index.js")],
  ["node_modules/native-keymap/package.json", path.join(nativeKeymapPatchDir, "package.json")],
  ["out/node_modules/@vscode/sqlite3/index.js", path.join(vscodeSqlite3PatchDir, "index.js")],
  ["out/node_modules/@vscode/sqlite3/package.json", path.join(vscodeSqlite3PatchDir, "package.json")],
  ["node_modules/@vscode/sqlite3/index.js", path.join(vscodeSqlite3PatchDir, "index.js")],
  ["node_modules/@vscode/sqlite3/package.json", path.join(vscodeSqlite3PatchDir, "package.json")],
  [
    "extensions/ohcode-splash/package.json",
    path.join(patchRoot, "ohcode-splash", "package.json")
  ]
];

function sha256(buffer) {
  return crypto.createHash("sha256").update(buffer).digest("hex");
}

function integrity(buffer) {
  const blockSize = 4194304;
  const blocks = [];
  for (let offset = 0; offset < buffer.length; offset += blockSize) {
    blocks.push(sha256(buffer.subarray(offset, Math.min(offset + blockSize, buffer.length))));
  }
  if (blocks.length === 0) {
    blocks.push(sha256(Buffer.alloc(0)));
  }
  return {
    algorithm: "SHA256",
    hash: sha256(buffer),
    blockSize,
    blocks
  };
}

function getDir(files, parts) {
  let node = files;
  for (const part of parts) {
    if (!node[part]) {
      node[part] = { files: {} };
    }
    if (!node[part].files) {
      throw new Error(`${parts.join("/")} collides with an existing ASAR file entry`);
    }
    node = node[part].files;
  }
  return node;
}

function getEntry(files, archiveName) {
  const parts = archiveName.split("/");
  let node = files;
  for (let index = 0; index < parts.length; index++) {
    node = node[parts[index]];
    if (!node) {
      return undefined;
    }
    if (index < parts.length - 1) {
      node = node.files;
      if (!node) {
        return undefined;
      }
    }
  }
  return node;
}

function readEntry(content, entry) {
  const offset = Number(entry.offset);
  return content.subarray(offset, offset + entry.size);
}

function makePickleHeader(header) {
  const json = Buffer.from(JSON.stringify(header), "utf8");
  const payloadLength = 4 + json.length;
  const padding = (4 - (payloadLength % 4)) % 4;
  const headerPayloadLength = payloadLength + padding;
  const headerBufferLength = 4 + headerPayloadLength;
  const out = Buffer.alloc(8 + headerBufferLength);
  out.writeUInt32LE(4, 0);
  out.writeUInt32LE(headerBufferLength, 4);
  out.writeUInt32LE(headerPayloadLength, 8);
  out.writeUInt32LE(json.length, 12);
  json.copy(out, 16);
  return out;
}

function installUnpackedFiles() {
  for (const [archiveName, source] of filesToInstall) {
    const target = path.join(appDir, ...archiveName.split("/"));
    fs.mkdirSync(path.dirname(target), { recursive: true });
    const data = fs.readFileSync(source);
    if (!fs.existsSync(target) || !fs.readFileSync(target).equals(data)) {
      fs.writeFileSync(target, data);
      console.info(`[OHcode] Installed ${path.relative(projectRoot, target)}`);
    }
  }
}

function patchAsar() {
  if (!fs.existsSync(appAsar)) {
    throw new Error(`app.asar not found: ${appAsar}`);
  }

  const archive = fs.readFileSync(appAsar);
  const oldHeaderBufferLength = archive.readUInt32LE(4);
  const oldJsonLength = archive.readUInt32LE(12);
  const oldContentStart = 8 + oldHeaderBufferLength;
  const oldContent = archive.subarray(oldContentStart);
  const header = JSON.parse(archive.subarray(16, 16 + oldJsonLength).toString("utf8"));

  let appendOffset = oldContent.length;
  const appended = [];
  let changed = false;

  for (const [archiveName, source] of filesToInstall) {
    const data = fs.readFileSync(source);
    const existing = getEntry(header.files, archiveName);
    if (existing && existing.size === data.length && readEntry(oldContent, existing).equals(data)) {
      continue;
    }

    const parts = archiveName.split("/");
    const fileName = parts.pop();
    const dir = getDir(header.files, parts);
    dir[fileName] = {
      size: data.length,
      offset: String(appendOffset),
      integrity: integrity(data)
    };
    appended.push(data);
    appendOffset += data.length;
    changed = true;
  }

  if (!changed) {
    console.info("[OHcode] app.asar runtime patches are already applied");
    return;
  }

  const newHeader = makePickleHeader(header);
  const tmpPath = `${appAsar}.tmp-${process.pid}`;
  fs.writeFileSync(tmpPath, Buffer.concat([newHeader, oldContent, ...appended]));
  fs.renameSync(tmpPath, appAsar);
  console.info("[OHcode] Patched app.asar runtime files");
}

installUnpackedFiles();
patchAsar();
