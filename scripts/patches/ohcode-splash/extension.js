const fs = require("fs");

// Written by the local UI extension host when the VS Code workbench starts.
// The HarmonyOS shell polls this file to remove the splash overlay early.
const READY_FLAG = "/data/storage/el2/base/files/ohcode-ready.flag";
const STARTUP_STATUS_FILE = "/data/storage/el2/base/files/ohcode-startup-status.txt";

function activate() {
  try {
    fs.writeFileSync(READY_FLAG, String(Date.now()));
    fs.writeFileSync(STARTUP_STATUS_FILE, `${Date.now()} [ohcode-splash] ready flag written`);
  } catch (_) {
    // Best effort only. The ArkTS side still has a timeout fallback.
  }
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
