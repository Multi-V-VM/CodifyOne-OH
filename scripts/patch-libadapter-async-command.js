"use strict";

const fs = require("fs");
const path = require("path");

const projectRoot = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const targetName = process.env.OHCODE_TARGET_NAME || "default";

// BrowserAdapter::ExecuteCommand calls a condition-variable wait here even for
// asynchronous kNewWindow commands. The wait runs on the ArkUI main thread and
// is the exact frame reported by THREAD_BLOCK_6S.
const patchOffset = 0xd6254;
const oldInstruction = Buffer.from("d564ff97", "hex");
const newInstruction = Buffer.from("1f2003d5", "hex"); // AArch64 nop
const expectedBefore = Buffer.from("07000014e00313aa", "hex");
const expectedAfter = Buffer.from("08ff5fc8090500d109ff0ac8aaffff35", "hex");

const candidates = [
  path.join(projectRoot, "electron", "libs", "arm64-v8a", "libadapter.so"),
  path.join(
    projectRoot,
    "electron",
    "build",
    targetName,
    "intermediates",
    "libs",
    targetName,
    "arm64-v8a",
    "libadapter.so"
  ),
  path.join(
    projectRoot,
    "electron",
    "build",
    targetName,
    "intermediates",
    "stripped_native_libs",
    targetName,
    "arm64-v8a",
    "libadapter.so"
  )
];

let patched = 0;
let alreadyPatched = 0;
let missing = 0;

for (const candidate of candidates) {
  if (!fs.existsSync(candidate)) {
    missing++;
    continue;
  }

  const data = fs.readFileSync(candidate);
  const beforeOffset = patchOffset - expectedBefore.length;
  const afterOffset = patchOffset + oldInstruction.length;
  if (
    data.length < afterOffset + expectedAfter.length ||
    !data.subarray(beforeOffset, patchOffset).equals(expectedBefore) ||
    !data.subarray(afterOffset, afterOffset + expectedAfter.length).equals(expectedAfter)
  ) {
    throw new Error(
      `libadapter ExecuteCommand layout mismatch at 0x${patchOffset.toString(16)} in ${candidate}`
    );
  }

  const current = data.subarray(patchOffset, patchOffset + oldInstruction.length);
  if (current.equals(newInstruction)) {
    alreadyPatched++;
    console.info(`[OHcode] libadapter async command wait already disabled: ${path.relative(projectRoot, candidate)}`);
    continue;
  }
  if (!current.equals(oldInstruction)) {
    throw new Error(
      `libadapter unexpected instruction ${current.toString("hex")} at 0x${patchOffset.toString(16)} in ${candidate}`
    );
  }

  newInstruction.copy(data, patchOffset);
  fs.writeFileSync(candidate, data);
  patched++;
  console.info(`[OHcode] Disabled libadapter async command wait: ${path.relative(projectRoot, candidate)}`);
}

if (patched === 0 && alreadyPatched === 0) {
  throw new Error(`No libadapter.so candidates found; missing=${missing}`);
}
