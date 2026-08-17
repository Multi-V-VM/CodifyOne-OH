"use strict";

const fs = require("fs");
const path = require("path");

const projectRoot = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const targetName = process.env.OHCODE_TARGET_NAME || "default";

const originalAppFallbackSnippet =
  'if(null==c)throw process.nextTick((function(){return process.exit(1)})),new Error("Unable to find a valid app");';
const packageJsonFallbackSnippet =
  'if(null==c){l="/data/storage/el1/bundle/electron/resources/resfile/resources/app";c=i._load(l+"/package.json")};';
const forcedAppFallbackSnippet =
  'if(null==c){l="/data/storage/el1/bundle/electron/resources/resfile/resources/app";c={name:"OHcode"}};'.padEnd(
    packageJsonFallbackSnippet.length,
    " "
  );
const markedAppFallbackSnippet =
  'if(null==c){l="/data/storage/el1/bundle/electron/resources/resfile/resources/app";process.env.O="F"+l,c={}};'.padEnd(
    packageJsonFallbackSnippet.length,
    " "
  );
const defineAppFallbackSnippet =
  'if(null==c){l=process.resourcesPath+"/resources/app";Object.defineProperty(process,"O",{value:l}),c={}};'.padEnd(
    packageJsonFallbackSnippet.length,
    " "
  );
const invalidFinalEntryMarkerSnippet =
  'l?(p="ohcode-entry-probe.js",process.env.O=l,i._load(s.join(l,p),i,!0))                     :';
const finalEntryMarkerSnippet =
  'l?(process.env.O=l,i._load(s.join(l,"ohcode-entry-probe.js"),i,!0))                         :';
const directMainEntrySnippet =
  'l?(process.env.O=l,i._load(s.join(l,"out/main.js"),i,!0))                                   :';
const resetSearchPathsSnippet =
  'process.argv.splice(1,1),__webpack_require__("./lib/common/reset-search-paths.ts")';
const browserRpcServerSnippet =
  '__webpack_require__("./lib/browser/rpc-server.ts")';
const browserGuestViewManagerSnippet =
  '__webpack_require__("./lib/browser/guest-view-manager.ts")';
const nodeRunMainSnippet =
  "require('internal/modules/cjs/loader').Module.runMain(process.argv[1]);";
const nodePreloadRunMainSnippet = 'require(process.argv[2]);'.padEnd(
  nodeRunMainSnippet.length,
  " "
);
const nodeAppBarrierTailSnippet =
  "// XXX: the monkey-patchability here should probably be deprecated.\n" +
  nodeRunMainSnippet;
const nodeAppBarrierReleaseSnippet =
  (
    "process.appCodeLoaded?.();process.appCodeLoaded=()=>{};\n" +
    nodeRunMainSnippet
  ).padEnd(nodeAppBarrierTailSnippet.length, " ");
const stageAMarkerSnippet =
  'process.argv.splice(1,1),Object.defineProperty(process,"A",{value:1})'.padEnd(
    resetSearchPathsSnippet.length,
    " "
  );
const stageAHiddenMarkerSnippet =
  'process._linkedBinding("electron_common_v8_util").setHiddenValue(process,"A",1)'.padEnd(
    resetSearchPathsSnippet.length,
    " "
  );
const stageARawDebugMarkerSnippet =
  'process.argv.splice(1,1),process._rawDebug("[OHcode] A")'.padEnd(
    resetSearchPathsSnippet.length,
    " "
  );
const stageAAppCodeLoadedOnlySnippet =
  'process.argv.splice(1,1),process.appCodeLoaded?.()'.padEnd(
    resetSearchPathsSnippet.length,
    " "
  );
const stageAAppCodeLoadedSnippet =
  'process.argv.splice(1,1),process.appCodeLoaded?.(),process._rawDebug("[OH] READY")';
const stageCMarkerSnippet =
  'Object.defineProperty(process,"C",{value:1})'.padEnd(
    browserRpcServerSnippet.length,
    " "
  );
const stageDMarkerSnippet =
  'Object.defineProperty(process,"D",{value:1})'.padEnd(
    browserGuestViewManagerSnippet.length,
    " "
  );
const stageDCombinedSnippet =
  `${browserRpcServerSnippet},${browserGuestViewManagerSnippet}`;
const stageDDefineCombinedSnippet = `${stageCMarkerSnippet},${stageDMarkerSnippet}`;
const stageDHiddenMarkerSnippet =
  'process._linkedBinding("electron_common_v8_util").setHiddenValue(process,"D",1)'.padEnd(
    stageDCombinedSnippet.length,
    " "
  );
const stageDRawDebugMarkerSnippet =
  'process._rawDebug("[OHcode] D")'.padEnd(stageDCombinedSnippet.length, " ");
const rawDebugAppFallbackSnippet =
  'if(null==c){l=process.resourcesPath+"/resources/app";process._rawDebug("[OHcode] O "+l),c={}};'.padEnd(
    packageJsonFallbackSnippet.length,
    " "
  );
const unsafeAppSearchPathsSnippet =
  'let l=null,c=null;const d=process._linkedBinding("electron_common_v8_util").getHiddenValue(global,"appSearchPaths");';
const safeAppSearchPathsSnippet =
  'let l=null,c=null,d=process._linkedBinding("electron_common_v8_util").getHiddenValue(global,"appSearchPaths")||[];  ';
const binaryPatches = [
  {
    name: "disable incompatible browser Node startup snapshot",
    offset: 0x2cfbe94,
    oldBytes: Buffer.from("160040f9", "hex"),
    newBytes: Buffer.from("f6031faa", "hex")
  },
  {
    name: "disable broken eager CompileFunction path",
    oldBytes: Buffer.from("e87703d0f403072af503062af60300aa", "hex"),
    newBytes: Buffer.from("e87703d0f403072af5031f2af60300aa", "hex")
  },
  {
    name: "restore Node environment bootstrap failure handling",
    oldBytes: Buffer.from("5a0100941f2003d5683a45f9", "hex"),
    newBytes: Buffer.from("5a010094e00300b4683a45f9", "hex")
  },
  {
    name: "restore Node source execution thunk",
    offset: 0x857fbf0,
    oldBytes: Buffer.from(
      "24930d14ffc301d1fd7b04a9f65705a9f44f06a9fd030191080440f9",
      "hex"
    ),
    newBytes: Buffer.from(
      "7f2303d5ffc301d1fd7b04a9f65705a9f44f06a9fd030191080440f9",
      "hex"
    )
  }
];

const patches = [
  {
    name: "restore Node run_main",
    oldSnippet: nodePreloadRunMainSnippet,
    newSnippet: nodeRunMainSnippet
  },
  {
    name: "restore Node run_main barrier handling",
    oldSnippet: nodeAppBarrierReleaseSnippet,
    newSnippet: nodeAppBarrierTailSnippet
  },
  {
    name: "restore Electron search-path initialization",
    oldSnippets: [
      stageAMarkerSnippet,
      stageAHiddenMarkerSnippet,
      stageARawDebugMarkerSnippet,
      stageAAppCodeLoadedOnlySnippet,
      stageAAppCodeLoadedSnippet
    ],
    newSnippet: resetSearchPathsSnippet
  },
  {
    name: "restore Electron browser services",
    oldSnippets: [stageDDefineCombinedSnippet, stageDHiddenMarkerSnippet, stageDRawDebugMarkerSnippet],
    newSnippet: stageDCombinedSnippet
  },
  {
    name: "use bundled OHcode app fallback",
    oldSnippets: [
      originalAppFallbackSnippet,
      packageJsonFallbackSnippet,
      markedAppFallbackSnippet,
      defineAppFallbackSnippet,
      rawDebugAppFallbackSnippet
    ],
    newSnippet: forcedAppFallbackSnippet
  },
  {
    name: "default missing app search paths",
    oldSnippet: unsafeAppSearchPathsSnippet,
    newSnippet: safeAppSearchPathsSnippet
  },
  {
    name: "entry probe",
    oldSnippet:
      'l?(process._firstFileName=i._resolveFilename(s.join(l,p),null,!1),i._load(s.join(l,p),i,!0)):',
    newSnippet:
      'l?(p="ohcode-entry-probe.js",process._firstFileName=s.join(l,p),i._load(s.join(l,p),i,!0))  :'
  },
  {
    name: "load OHcode main directly",
    oldSnippets: [
      'l?(p="ohcode-entry-probe.js",process._firstFileName=s.join(l,p),i._load(s.join(l,p),i,!0))  :',
      invalidFinalEntryMarkerSnippet,
      finalEntryMarkerSnippet
    ],
    newSnippet: directMainEntrySnippet
  }
];

for (const patch of patches) {
  for (const oldSnippet of patch.oldSnippets || [patch.oldSnippet]) {
    if (oldSnippet.length !== patch.newSnippet.length) {
      throw new Error(
        `browser_init ${patch.name} patch must be length-preserving: old=${oldSnippet.length} new=${patch.newSnippet.length}`
      );
    }
  }
}
for (const patch of binaryPatches) {
  if (patch.oldBytes.length !== patch.newBytes.length) {
    throw new Error(
      `libelectron ${patch.name} patch must be length-preserving: old=${patch.oldBytes.length} new=${patch.newBytes.length}`
    );
  }
}
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
    "libelectron.so"
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
  let candidatePatched = 0;
  let candidateAlreadyPatched = 0;

  for (const patch of binaryPatches) {
    const oldIndex = Number.isInteger(patch.offset)
      ? data.subarray(patch.offset, patch.offset + patch.oldBytes.length).equals(patch.oldBytes)
        ? patch.offset
        : -1
      : data.indexOf(patch.oldBytes);
    const patchedIndex = Number.isInteger(patch.offset)
      ? data.subarray(patch.offset, patch.offset + patch.newBytes.length).equals(patch.newBytes)
        ? patch.offset
        : -1
      : data.indexOf(patch.newBytes);

    if (oldIndex === -1) {
      if (patchedIndex !== -1) {
        candidateAlreadyPatched++;
        continue;
      }
      throw new Error(`libelectron ${patch.name} bytes not found in ${candidate}`);
    }
    if (
      !Number.isInteger(patch.offset) &&
      data.indexOf(patch.oldBytes, oldIndex + patch.oldBytes.length) !== -1
    ) {
      throw new Error(`libelectron ${patch.name} bytes are not unique in ${candidate}`);
    }

    patch.newBytes.copy(data, oldIndex);
    candidatePatched++;
  }

  let text = data.toString("latin1");
  for (const patch of patches) {
    const oldSnippets = patch.oldSnippets || [patch.oldSnippet];
    const oldMatches = oldSnippets
      .map((oldSnippet) => ({ oldSnippet, oldIndex: text.indexOf(oldSnippet) }))
      .filter((match) => match.oldIndex !== -1);
    const patchedIndex = text.indexOf(patch.newSnippet);

    if (oldMatches.length === 0) {
      if (patchedIndex !== -1) {
        candidateAlreadyPatched++;
        continue;
      }
      if (
        patch.name === "entry probe" &&
        (text.indexOf(directMainEntrySnippet) !== -1 ||
          text.indexOf(finalEntryMarkerSnippet) !== -1 ||
          text.indexOf(invalidFinalEntryMarkerSnippet) !== -1)
      ) {
        candidateAlreadyPatched++;
        continue;
      }
      throw new Error(`browser_init ${patch.name} snippet not found in ${candidate}`);
    }
    if (oldMatches.length > 1) {
      throw new Error(`browser_init ${patch.name} found multiple old snippets in ${candidate}`);
    }

    const { oldSnippet, oldIndex } = oldMatches[0];

    if (text.indexOf(oldSnippet, oldIndex + oldSnippet.length) !== -1) {
      throw new Error(`browser_init ${patch.name} snippet is not unique in ${candidate}`);
    }

    Buffer.from(patch.newSnippet, "latin1").copy(data, oldIndex);
    text = data.toString("latin1");
    candidatePatched++;
  }

  if (candidatePatched > 0) {
    fs.writeFileSync(candidate, data);
    patched++;
    console.info(
      `[OHcode] Patched libelectron browser_init (${candidatePatched} patch(es)): ${path.relative(projectRoot, candidate)}`
    );
  } else if (candidateAlreadyPatched === patches.length + binaryPatches.length) {
    alreadyPatched++;
    console.info(`[OHcode] libelectron browser_init already patched: ${path.relative(projectRoot, candidate)}`);
  }
}

if (patched === 0 && alreadyPatched === 0) {
  throw new Error(`No libelectron.so candidates found; missing=${missing}`);
}
