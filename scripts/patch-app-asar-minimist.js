"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const projectRoot = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const resourcesRoot = path.join(projectRoot, "web_engine", "src", "main", "resources", "resfile", "resources");
const appDir = path.join(resourcesRoot, "app");
const appAsar = path.join(resourcesRoot, "app.asar");
const nodeModulesAsar = path.join(appDir, "node_modules.asar");
const patchRoot = path.join(projectRoot, "scripts", "patches");
const minimistPatchDir = path.join(patchRoot, "minimist");
const gracefulFsPatchDir = path.join(patchRoot, "graceful-fs");
const nativeKeymapPatchDir = path.join(patchRoot, "native-keymap");
const vscodeSqlite3PatchDir = path.join(patchRoot, "vscode-sqlite3");
const moduleDir = path.join(appDir, "node_modules", "minimist");
const filesToInstall = [
  ["package.json", path.join(appDir, "package.json")],
  ["ohcode-entry-probe.js", path.join(appDir, "ohcode-entry-probe.js")],
  ["out/main.js", path.join(appDir, "out", "main.js")],
  [
    "out/vs/base/parts/sandbox/electron-sandbox/preload.js",
    path.join(appDir, "out", "vs", "base", "parts", "sandbox", "electron-sandbox", "preload.js")
  ],
  [
    "out/vs/base/parts/sandbox/electron-sandbox/preload-aux.js",
    path.join(appDir, "out", "vs", "base", "parts", "sandbox", "electron-sandbox", "preload-aux.js")
  ],
  [
    "out/vs/code/electron-main/main.js",
    path.join(appDir, "out", "vs", "code", "electron-main", "main.js")
  ],
  [
    "out/vs/code/electron-sandbox/workbench/workbench.html",
    path.join(appDir, "out", "vs", "code", "electron-sandbox", "workbench", "workbench.html")
  ],
  [
    "out/vs/code/electron-sandbox/workbench/workbench.js",
    path.join(appDir, "out", "vs", "code", "electron-sandbox", "workbench", "workbench.js")
  ],
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
  ],
  [
    "extensions/ohcode-splash/extension.js",
    path.join(patchRoot, "ohcode-splash", "extension.js")
  ]
];
const nodeModulesAsarFiles = [
  ["native-keymap/index.js", path.join(nativeKeymapPatchDir, "index.js")],
  ["native-keymap/package.json", path.join(nativeKeymapPatchDir, "package.json")]
];

function patchSandboxPreloadFallback() {
  const preloadPath = path.join(
    appDir, "out", "vs", "base", "parts", "sandbox", "electron-sandbox", "preload.js"
  );
  const product = JSON.parse(fs.readFileSync(path.join(appDir, "product.json"), "utf8"));
  const pkg = JSON.parse(fs.readFileSync(path.join(appDir, "package.json"), "utf8"));
  product.version ||= pkg.version;
  product.quality ||= "stable";

  const blockStart = "\t\t\t// OHCODE_FALLBACK_PRODUCT_START\n";
  const blockEnd = "\t\t\t// OHCODE_FALLBACK_PRODUCT_END\n";
  const productBlock =
    blockStart +
    `\t\t\tproduct: ${JSON.stringify(product)},\n` +
    blockEnd;
  let preloadSource = fs.readFileSync(preloadPath, "utf8");
  const existingStart = preloadSource.indexOf(blockStart);
  const existingEnd = preloadSource.indexOf(blockEnd, existingStart + blockStart.length);
  if (existingStart >= 0 && existingEnd >= existingStart) {
    preloadSource =
      preloadSource.slice(0, existingStart) +
      productBlock +
      preloadSource.slice(existingEnd + blockEnd.length);
  } else {
    const fallbackReturnNeedle =
      "\t\treturn {\n\t\t\tappRoot,\n";
    if (!preloadSource.includes(fallbackReturnNeedle)) {
      throw new Error("Missing sandbox fallback-configuration marker in preload.js");
    }
    preloadSource = preloadSource.replace(
      fallbackReturnNeedle,
      "\t\treturn {\n" + productBlock + "\t\t\tappRoot,\n"
    );
  }
  fs.writeFileSync(preloadPath, preloadSource);
}

function patchElectronMainStartupWindow() {
  const electronMainPath = path.join(
    appDir, "out", "vs", "code", "electron-main", "main.js"
  );
  let source = fs.readFileSync(electronMainPath, "utf8");
  const original =
    "                cli: args,\n" +
    "                forceNewWindow: args['new-window'] || (!hasCliArgs && args['unity-launch']),\n";
  const replacement =
    "                // HarmonyOS forwards its Chromium bootstrap argv through the\n" +
    "                // Electron bridge. VS Code otherwise treats those values as\n" +
    "                // paths and can finish startup without creating a CodeWindow.\n" +
    "                cli: { ...args, _: [] },\n" +
    "                forceNewWindow: true,\n" +
    "                forceEmpty: true,\n";
  if (source.includes(original)) {
    source = source.replace(original, replacement);
    console.info("[OHcode] Patched HarmonyOS startup window selection");
  } else if (!source.includes(replacement)) {
    throw new Error("Missing HarmonyOS startup-window marker in electron-main/main.js");
  }

  // The HarmonyOS port runs Chromium's renderer in-process. Electron's
  // sandbox option forces context isolation back on even when the CodeWindow
  // override requests contextIsolation:false. Creating that isolated world
  // executes ElectronRenderFrameObserver's `void 0` bootstrap outside a V8
  // HandleScope on this port and aborts the renderer. The preload already has
  // a non-isolated `window.vscode = globals` path, so keep the main workbench
  // in the main world here.
  const sandboxEnabled =
    "                ...overrides?.webPreferences,\n" +
    "                sandbox: true\n";
  const sandboxDisabled =
    "                ...overrides?.webPreferences,\n" +
    "                // HarmonyOS in-process renderer: avoid the broken isolated-world bootstrap.\n" +
    "                sandbox: false\n";
  if (source.includes(sandboxEnabled)) {
    source = source.replace(sandboxEnabled, sandboxDisabled);
    console.info("[OHcode] Disabled workbench renderer sandbox/context isolation bootstrap");
  } else if (!source.includes(sandboxDisabled)) {
    throw new Error("Missing workbench sandbox marker in electron-main/main.js");
  }

  fs.writeFileSync(electronMainPath, source);
}

function generateWorkbenchLoader() {
  const workbenchDir = path.join(
    appDir, "out", "vs", "code", "electron-sandbox", "workbench"
  );
  const workbenchPath = path.join(workbenchDir, "workbench.js");
  const htmlPath = path.join(workbenchDir, "workbench.html");
  let workbenchSource = fs.readFileSync(workbenchPath, "utf8");
  const ownedSourceNeedle =
    "\t\t\tconst ownedSource = Array.from(sourceWithCompletion).join('');\n";
  if (!workbenchSource.includes(
    "globalThis.__ohcodeOwnedScriptSources.push(ownedSource)"
  )) {
    if (!workbenchSource.includes(ownedSourceNeedle)) {
      throw new Error("Missing owned AMD source-copy marker in workbench.js");
    }
    workbenchSource = workbenchSource.replace(
      ownedSourceNeedle,
      ownedSourceNeedle +
      "\t\t\tglobalThis.__ohcodeOwnedScriptSources ||= [];\n" +
      "\t\t\tglobalThis.__ohcodeOwnedScriptSources.push(ownedSource);\n"
    );
  }
  const dynamicOwnedScriptExecution =
    "\t\t\tconst script = document.createElement('script');\n" +
    "\t\t\tscript.setAttribute('type', 'text/javascript');\n" +
    "\t\t\tconst { cspNonce } = moduleManager.getConfig().getOptionsLiteral();\n" +
    "\t\t\tscript.setAttribute('nonce', cspNonce || 'ohcodeWorkbenchLoader');\n" +
    "\t\t\t// The amdLoader TrustedScript wrapper is unreliable in this WebEngine and\n" +
    "\t\t\t// can surface as an empty V8 value in the script.text binding. The nonce is\n" +
    "\t\t\t// sufficient for this document, so pass the owned primitive string directly.\n" +
    "\t\t\tscript.text = ownedSource;\n" +
    "\t\t\tdocument.documentElement.appendChild(script);";
  const globalEvalExecution =
    "\t\t\t// Dynamic script elements use Chromium ScriptOrigin host metadata that is\n" +
    "\t\t\t// corrupt on this Harmony WebEngine build. Indirect eval keeps AMD globals\n" +
    "\t\t\t// while avoiding the embedder-only dynamic-script compilation path.\n" +
    "\t\t\tglobalThis.__ohcodeExecuteOwnedSource(ownedSource);";
  if (workbenchSource.includes(dynamicOwnedScriptExecution)) {
    workbenchSource = workbenchSource.replace(
      dynamicOwnedScriptExecution,
      globalEvalExecution
    );
  } else if (!workbenchSource.includes(globalEvalExecution)) {
    throw new Error("Missing owned AMD script-execution marker in workbench.js");
  }
  const oldBatchLayout =
    "\t\t\tconst targetBatchSize = 64 * 1024;\n" +
    "\t\t\tconst maxBatchStatements = 4;";
  const newBatchLayout =
    "\t\t\tconst targetBatchSize = 256 * 1024;\n" +
    "\t\t\tconst maxBatchStatements = 32;";
  if (workbenchSource.includes(oldBatchLayout)) {
    workbenchSource = workbenchSource.replace(oldBatchLayout, newBatchLayout);
  } else if (!workbenchSource.includes(newBatchLayout)) {
    throw new Error("Missing optimized AMD batch-layout marker in workbench.js");
  }
  const appendPolicyNeedle =
    "\t\t\tconst canAppendStatement = statement => !batchSize || (\n" +
    "\t\t\t\tbatchSize + statement.length <= targetBatchSize && batchParts.length < maxBatchStatements\n" +
    "\t\t\t);";
  const isolatedAppendPolicy =
    "\t\t\tconst mustIsolateStatement = statement => statement.includes('partialCommandDetectionCapability*/]');\n" +
    "\t\t\tconst canAppendStatement = statement => !batchSize || (\n" +
    "\t\t\t\t!mustIsolateStatement(statement) && batchSize + statement.length <= targetBatchSize && batchParts.length < maxBatchStatements\n" +
    "\t\t\t);";
  if (workbenchSource.includes(appendPolicyNeedle)) {
    workbenchSource = workbenchSource.replace(appendPolicyNeedle, isolatedAppendPolicy);
  } else if (!workbenchSource.includes(isolatedAppendPolicy)) {
    throw new Error("Missing optimized AMD append-policy marker in workbench.js");
  }
  const flushPolicyNeedle =
    "\t\t\t\t\tif (batchSize >= targetBatchSize || batchParts.length >= maxBatchStatements) {";
  const isolatedFlushPolicy =
    "\t\t\t\t\tif (mustIsolateStatement(statement) || batchSize >= targetBatchSize || batchParts.length >= maxBatchStatements) {";
  if (workbenchSource.includes(flushPolicyNeedle)) {
    workbenchSource = workbenchSource.replace(flushPolicyNeedle, isolatedFlushPolicy);
  } else if (!workbenchSource.includes(isolatedFlushPolicy)) {
    throw new Error("Missing optimized AMD flush-policy marker in workbench.js");
  }
  const cssLoaderNeedle =
    "    function createLinkTag(name, cssUrl, callback, errorback) {\n" +
    "        const linkNode = document.createElement('link');\n" +
    "        linkNode.setAttribute('rel', 'stylesheet');\n" +
    "        linkNode.setAttribute('type', 'text/css');\n" +
    "        linkNode.setAttribute('data-name', name);\n" +
    "        attachListeners(name, linkNode, callback, errorback);\n" +
    "        linkNode.setAttribute('href', cssUrl);\n" +
    "        // eslint-disable-next-line no-restricted-globals\n" +
    "        const head = window.document.head || window.document.getElementsByTagName('head')[0];\n" +
    "        head.appendChild(linkNode);\n" +
    "    }";
  const scheduledCssCompletion =
    "        globalThis.__ohcodeScheduleTask(complete);";
  const directCssCompletion =
    "        complete();".padEnd(scheduledCssCompletion.length, " ");
  const compatibleCssLoader =
    "    function createLinkTag(name, cssUrl, callback, errorback) {\n" +
    "        const linkNode = document.createElement('link');\n" +
    "        linkNode.setAttribute('rel', 'stylesheet');\n" +
    "        linkNode.setAttribute('type', 'text/css');\n" +
    "        linkNode.setAttribute('data-name', name);\n" +
    "        let completed = false;\n" +
    "        const complete = () => {\n" +
    "            if (completed) return;\n" +
    "            completed = true;\n" +
    "            callback();\n" +
    "        };\n" +
    "        const fail = error => {\n" +
    "            if (completed) return;\n" +
    "            completed = true;\n" +
    "            errorback(error);\n" +
    "        };\n" +
    "        attachListeners(name, linkNode, complete, fail);\n" +
    "        linkNode.setAttribute('href', cssUrl);\n" +
    "        // eslint-disable-next-line no-restricted-globals\n" +
    "        const head = window.document.head || window.document.getElementsByTagName('head')[0];\n" +
    "        head.appendChild(linkNode);\n" +
    "        // HarmonyOS WebEngine can apply a vscode-file stylesheet without\n" +
    "        // dispatching the link load event. Do not leave the AMD graph\n" +
    "        // permanently pending while the stylesheet loads asynchronously.\n" +
    "        // Complete on the next task: synchronous completion can re-enter\n" +
    "        // AMD registration, while this WebEngine's V8 can crash at the\n" +
    "        // equivalent microtask boundary.\n" +
    directCssCompletion + "\n" +
    "    }";
  if (workbenchSource.includes(cssLoaderNeedle)) {
    workbenchSource = workbenchSource.replace(cssLoaderNeedle, compatibleCssLoader);
  } else if (!workbenchSource.includes(compatibleCssLoader)) {
    throw new Error("Missing HarmonyOS CSS-loader compatibility marker in workbench.js");
  }
  const synchronousCompletionFlush =
    "\t\t\t\tif (registrationComplete) {\n" +
    "\t\t\t\t\tthis._trace(`flushing deferred AMD factories: count=${completionQueue.length}`);\n" +
    "\t\t\t\t\tfor (const [manager, module] of completionQueue) {\n" +
    "\t\t\t\t\t\tmodule.__ohcodeCompletionQueued = false;\n" +
    "\t\t\t\t\t\tmanager._onModuleComplete(module);\n" +
    "\t\t\t\t\t}\n" +
    "\t\t\t\t}";
  const chunkedCompletionFlush =
    "\t\t\t\tif (registrationComplete) {\n" +
    "\t\t\t\t\tthis._trace(`flushing deferred AMD factories: count=${completionQueue.length}`);\n" +
    "\t\t\t\t\tlet completionIndex = 0;\n" +
    "\t\t\t\t\tconst flushCompletionChunk = () => {\n" +
    "\t\t\t\t\t\ttry {\n" +
    "\t\t\t\t\t\t\tconst end = Math.min(completionIndex + 1, completionQueue.length);\n" +
    "\t\t\t\t\t\t\tfor (; completionIndex < end; completionIndex++) {\n" +
    "\t\t\t\t\t\t\t\tconst [manager, module] = completionQueue[completionIndex];\n" +
    "\t\t\t\t\t\t\t\tmodule.__ohcodeCompletionQueued = false;\n" +
    "\t\t\t\t\t\t\t\tmanager._onModuleComplete(module);\n" +
    "\t\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t\t} catch (error) {\n" +
    "\t\t\t\t\t\t\tthis._trace(`deferred AMD factory flush ERROR index=${completionIndex}: ${error.stack || error}`);\n" +
    "\t\t\t\t\t\t\tthrow error;\n" +
    "\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t\tif (completionIndex === 1 || completionIndex % 16 === 0) {\n" +
    "\t\t\t\t\t\t\tthis._trace(`deferred AMD factory flush progress: ${completionIndex}/${completionQueue.length}`);\n" +
    "\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t\tif (completionIndex < completionQueue.length) {\n" +
    "\t\t\t\t\t\t\tglobalThis.__ohcodeScheduleTask(flushCompletionChunk);\n" +
    "\t\t\t\t\t\t} else {\n" +
    "\t\t\t\t\t\t\tthis._trace(`deferred AMD factory flush complete: count=${completionQueue.length}`);\n" +
    "\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t};\n" +
    "\t\t\t\t\tflushCompletionChunk();\n" +
    "\t\t\t\t}";
  const directCompletionFlush =
    "\t\t\t\tif (registrationComplete) {\n" +
    "\t\t\t\t\tthis._trace(`flushing deferred AMD factories: count=${completionQueue.length}`);\n" +
    "\t\t\t\t\tfor (let completionIndex = 0; completionIndex < completionQueue.length; completionIndex++) {\n" +
    "\t\t\t\t\t\ttry {\n" +
    "\t\t\t\t\t\t\tconst [manager, module] = completionQueue[completionIndex];\n" +
    "\t\t\t\t\t\t\tmodule.__ohcodeCompletionQueued = false;\n" +
    "\t\t\t\t\t\t\tmanager._onModuleComplete(module);\n" +
    "\t\t\t\t\t\t} catch (error) {\n" +
    "\t\t\t\t\t\t\tthis._trace(`deferred AMD factory flush ERROR index=${completionIndex}: ${error.stack || error}`);\n" +
    "\t\t\t\t\t\t\tthrow error;\n" +
    "\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t\tif (completionIndex === 0 || (completionIndex + 1) % 16 === 0) {\n" +
    "\t\t\t\t\t\t\tthis._trace(`deferred AMD factory flush progress: ${completionIndex + 1}/${completionQueue.length}`);\n" +
    "\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t}\n" +
    "\t\t\t\t\tthis._trace(`deferred AMD factory flush complete: count=${completionQueue.length}`);\n" +
    "\t\t\t\t}";
  if (workbenchSource.includes(chunkedCompletionFlush)) {
    workbenchSource = workbenchSource.replace(
      chunkedCompletionFlush,
      directCompletionFlush
    );
  } else if (workbenchSource.includes(synchronousCompletionFlush)) {
    workbenchSource = workbenchSource.replace(
      synchronousCompletionFlush,
      directCompletionFlush
    );
  } else if (!workbenchSource.includes(directCompletionFlush)) {
    throw new Error("Missing direct AMD completion-flush marker in workbench.js");
  }
  const completeModuleNeedle =
    "\t\t\tmodule.complete(recorder, this._config, dependenciesValues, inversedependenciesProvider);";
  const diagnosedModuleCompletion =
    completeModuleNeedle + "\n" +
    "\t\t\tif (module.error) {\n" +
    "\t\t\t\tconsole.error(`[OHcode] AMD factory error module=${module.strId}: ${module.error.stack || module.error}`);\n" +
    "\t\t\t}";
  if (workbenchSource.includes(completeModuleNeedle) &&
      !workbenchSource.includes("[OHcode] AMD factory error module=")) {
    workbenchSource = workbenchSource.replace(
      completeModuleNeedle,
      diagnosedModuleCompletion
    );
  } else if (!workbenchSource.includes(diagnosedModuleCompletion)) {
    throw new Error("Missing AMD factory-error diagnostic marker in workbench.js");
  }
  const workbenchMainSource = fs.readFileSync(
    path.join(appDir, "out", "vs", "workbench", "workbench.desktop.main.js"),
    "utf8"
  );
  const workbenchNlsSource = fs.readFileSync(
    path.join(appDir, "out", "vs", "workbench", "workbench.desktop.main.nls.js"),
    "utf8"
  );
  const nlsObjectStart = workbenchNlsSource.indexOf(
    "{",
    workbenchNlsSource.indexOf("define(")
  );
  const nlsObjectEnd = workbenchNlsSource.lastIndexOf("});");
  if (nlsObjectStart < 0 || nlsObjectEnd < nlsObjectStart) {
    throw new Error("Unexpected workbench NLS bundle layout");
  }
  const workbenchNlsMessages = JSON.parse(
    workbenchNlsSource.slice(nlsObjectStart, nlsObjectEnd + 1)
  );
  const workbenchNlsEntries = Object.entries(workbenchNlsMessages);
  const splitSource = (source, chunkSize) => {
    const chunks = [];
    for (let offset = 0; offset < source.length; offset += chunkSize) {
      chunks.push(source.slice(offset, offset + chunkSize));
    }
    return chunks;
  };
  const splitAtMarkers = (source, markers) => {
    const offsets = [0];
    for (const marker of markers) {
      let offset = source.indexOf(marker);
      while (offset >= 0) {
        offsets.push(offset + 1);
        offset = source.indexOf(marker, offset + marker.length);
      }
    }
    offsets.push(source.length);
    offsets.sort((left, right) => left - right);
    return offsets.slice(0, -1).map((offset, index) =>
      source.slice(offset, offsets[index + 1])
    );
  };
  const sourceChunks = splitAtMarkers(workbenchSource, [
    "\nvar AMDLoader;",
    "\n/// <reference path=\"typings/require.d.ts\" />",
    "\n/// <reference path=\"../../../../typings/require.d.ts\" />"
  ]).flatMap(chunk => {
    if (!chunk.includes("class OnlyOnceScriptLoader")) {
      return [chunk];
    }
    const opening = "var AMDLoader;\n(function (AMDLoader) {\n";
    const closing = "})(AMDLoader || (AMDLoader = {}));";
    const closingOffset = chunk.lastIndexOf(closing);
    if (!chunk.startsWith(opening) || closingOffset < opening.length) {
      throw new Error("Unexpected AMD script-loader namespace layout");
    }
    const body = chunk.slice(opening.length, closingOffset);
    const markers = [
      "\n\tclass BrowserScriptLoader",
      "\n\tclass WorkerScriptLoader",
      "\n\tclass NodeScriptLoader"
    ];
    const offsets = [0, ...markers.map(marker => {
      const offset = body.indexOf(marker);
      if (offset < 0) {
        throw new Error(`Missing script-loader class marker: ${marker}`);
      }
      return offset + 1;
    }), body.length];
    return offsets.slice(0, -1).map((offset, index) => {
      const part = body.slice(offset, offsets[index + 1]);
      return index === 0 ? `var AMDLoader = AMDLoader || {};\n${part}` : part;
    });
  });
  // Keep the large payload inert while Chromium parses the document. Thousands
  // of executable push/assignment scripts make the in-process renderer race and
  // crash before DOMContentLoaded. JSON data blocks are not sent through V8;
  // the loader parses them only after the document is complete.
  const workbenchMainChunks = splitSource(workbenchMainSource, 512 * 1024);
  const loader = `/* Generated by scripts/patch-app-asar-minimist.js. */
'use strict';
(function loadWorkbenchFromOwnedString() {
  const trace = document.getElementById('ohcode-boot-trace');
  const fail = error => {
    const detail = error && (error.stack || error.message) || String(error);
    console.error('[OHcode] workbench owned-string loader failed', detail);
    if (trace) trace.textContent = 'OHcode workbench loader failed\\n' + detail;
  };
  const taskQueue = [];
  const taskMessagePrefix = '__ohcode_owned_task_' + Date.now() + '_';
  let taskMessageSerial = 0;
  let taskMessagePending = false;
  let drainingTasks = false;
  const postTaskMessage = () => {
    if (drainingTasks || taskMessagePending || !taskQueue.length) return;
    taskMessagePending = true;
    window.postMessage(taskMessagePrefix + (++taskMessageSerial), '*');
  };
  window.addEventListener('message', event => {
    if (typeof event.data !== 'string' || !event.data.startsWith(taskMessagePrefix)) return;
    taskMessagePending = false;
    drainingTasks = true;
    try {
      // HarmonyOS WebEngine delivers the first window message but can discard
      // messages posted by that message's own callback. Drain successors in
      // this browser-event turn; tasks scheduled later still get a fresh event.
      while (taskQueue.length) {
        taskQueue.shift()();
      }
    } catch (error) {
      fail(error);
      throw error;
    } finally {
      drainingTasks = false;
      postTaskMessage();
    }
  });
  globalThis.__ohcodeScheduleTask = task => {
    if (typeof task !== 'function') {
      throw new TypeError('OHcode task must be a function, got ' + typeof task);
    }
    taskQueue.push(task);
    postTaskMessage();
  };
  window.addEventListener('error', event => {
    const error = event.error;
    console.error('[OHcode] window error detail', error && (error.stack || error.message) || event.message);
  });
  globalThis.__ohcodeExecuteOwnedSource = source => (0, eval)(source);
  const runWorkbench = () => {
    const chunks = globalThis.__ohcodeWorkbenchChunks;
    const retainedSources = globalThis.__ohcodeOwnedScriptSources ||
      (globalThis.__ohcodeOwnedScriptSources = []);
    globalThis.__ohcodeWorkbenchChunks = undefined;
    // The AMD loader pieces share lazy parser state. Evaluating them as
    // separate global scripts can leave this WebEngine blocked while parsing
    // the second class declaration, so restore one complete lexical unit.
    const source = ';\\n' + chunks.map(chunk => Array.from(chunk).join('')).join('\\n');
    retainedSources.push(source);
    chunks.fill('');
    console.error('[OHcode] workbench loader combined ENTER chunks=' +
      chunks.length + ' chars=' + source.length);
    globalThis.__ohcodeExecuteOwnedSource(source);
    console.error('[OHcode] workbench loader combined OK chunks=' + chunks.length);
  };
  try {
    console.error('[OHcode] workbench inert data loader started');
    if (trace) trace.textContent = 'OHcode loading workbench data';
    window.addEventListener('DOMContentLoaded', () => {
      const readChunks = selector => Array.from(
        document.querySelectorAll(selector), element => {
          const value = JSON.parse(element.content?.textContent || element.textContent || '""');
          element.remove();
          return value;
        }
      );
      globalThis.__ohcodeWorkbenchChunks = readChunks('template[data-ohcode-workbench-loader]');
      globalThis.__ohcodeWorkbenchMainChunks = readChunks('template[data-ohcode-workbench-main]');
      const nlsData = document.getElementById('ohcode-workbench-nls-data');
      globalThis.__ohcodeWorkbenchNlsMessages = JSON.parse(nlsData?.content?.textContent || '{}');
      nlsData?.remove();
      console.error('[OHcode] workbench inert data ready loader=' +
        globalThis.__ohcodeWorkbenchChunks.length + ', main=' +
        globalThis.__ohcodeWorkbenchMainChunks.length + ', nlsKeys=' +
        Object.keys(globalThis.__ohcodeWorkbenchNlsMessages).length);
      if (globalThis.__ohcodeWorkbenchChunks.length !== ${sourceChunks.length}) {
        fail(new Error('Expected ${sourceChunks.length} workbench chunks, got ' + globalThis.__ohcodeWorkbenchChunks.length));
        return;
      }
      if (globalThis.__ohcodeWorkbenchMainChunks.length !== ${workbenchMainChunks.length} ||
          Object.keys(globalThis.__ohcodeWorkbenchNlsMessages).length !== ${workbenchNlsEntries.length}) {
        fail(new Error('Incomplete preloaded AMD data: main=' +
          globalThis.__ohcodeWorkbenchMainChunks.length + '/${workbenchMainChunks.length}, nlsKeys=' +
          Object.keys(globalThis.__ohcodeWorkbenchNlsMessages).length + '/${workbenchNlsEntries.length}'));
        return;
      }
      try {
${"        console.error('[OHcode] owned task scheduler probe disabled');".padEnd(103, " ")}
        runWorkbench();
      } catch (error) {
        fail(error);
      }
    }, { once: true });
  } catch (error) {
    fail(error);
  }
})();
`;
  const loaderStart = "\t<!-- OHCODE_WORKBENCH_LOADER_START -->";
  const loaderEnd = "\t<!-- OHCODE_WORKBENCH_LOADER_END -->";
  const safeJson = value => JSON.stringify(value)
      .replaceAll("<", "\\u003c")
      .replaceAll("&", "\\u0026")
      .replaceAll("\u2028", "\\u2028")
      .replaceAll("\u2029", "\\u2029");
  const dataTag = (attribute, chunk) =>
    `\t<template ${attribute}>${safeJson(chunk)}</template>`;
  const staticLoaderBootstrap = `/* Generated by scripts/patch-app-asar-minimist.js. */
'use strict';
(function prepareStaticWorkbenchLoader() {
  const trace = document.getElementById('ohcode-boot-trace');
  const readChunks = selector => Array.from(
    document.querySelectorAll(selector), element => {
      const value = JSON.parse(element.content?.textContent || element.textContent || '""');
      element.remove();
      return value;
    }
  );
  window.addEventListener('error', event => {
    const error = event.error;
    console.error('[OHcode] window error detail',
      error && (error.stack || error.message) || event.message);
  });
  globalThis.__ohcodeOwnedScriptSources ||= [];
  globalThis.__ohcodeExecuteOwnedSource = source => (0, eval)(source);
  globalThis.__ohcodeWorkbenchMainChunks =
    readChunks('template[data-ohcode-workbench-main]');
  const nlsData = document.getElementById('ohcode-workbench-nls-data');
  globalThis.__ohcodeWorkbenchNlsMessages =
    JSON.parse(nlsData?.content?.textContent || '{}');
  nlsData?.remove();
  const nlsKeys = Object.keys(globalThis.__ohcodeWorkbenchNlsMessages).length;
  console.error('[OHcode] static workbench data ready main=' +
    globalThis.__ohcodeWorkbenchMainChunks.length + ', nlsKeys=' + nlsKeys);
  if (globalThis.__ohcodeWorkbenchMainChunks.length !== ${workbenchMainChunks.length} ||
      nlsKeys !== ${workbenchNlsEntries.length}) {
    throw new Error('Incomplete static workbench data');
  }
  if (trace) trace.textContent = 'OHcode loading static workbench script';
  console.error('[OHcode] static workbench script ENTER');
})();
`;
  const loaderTags = [
    loaderStart,
    ...workbenchMainChunks.map(chunk => dataTag("data-ohcode-workbench-main", chunk)),
    `\t<template id="ohcode-workbench-nls-data">${safeJson(workbenchNlsMessages)}</template>`,
    `\t<script nonce=\"ohcodeWorkbenchLoader\">\n${staticLoaderBootstrap}\t</script>`,
    `\t<script nonce=\"ohcodeWorkbenchLoader\" src=\"./workbench.js\"></script>`,
    `\t<script nonce=\"ohcodeWorkbenchLoader\">console.error('[OHcode] static workbench script OK');</script>`,
    loaderEnd
  ].join("\n");
  const oldHtml = fs.readFileSync(htmlPath, "utf8");
  // This WebEngine accepts the TrustedScript used by the initial inline loader,
  // but can turn an equivalent wrapper into an empty V8 value when later AMD
  // batches assign HTMLScriptElement.text. Keep the nonce-based script policy
  // and disable only mandatory Trusted Types for this bundled local document so
  // those batches can use their deliberately copied primitive strings.
  const baseHtml = oldHtml.replace(
    /\n\s*require-trusted-types-for\s*\n\s*'script'\s*\n\s*;/,
    ""
  );
  const loaderStartIndex = baseHtml.indexOf(loaderStart);
  const loaderEndIndex = baseHtml.lastIndexOf(loaderEnd);
  if (loaderStartIndex < 0 || loaderEndIndex < loaderStartIndex) {
    throw new Error("OHcode workbench loader markers are missing from workbench.html");
  }
  const newHtml =
    baseHtml.slice(0, loaderStartIndex) +
    loaderTags +
    baseHtml.slice(loaderEndIndex + loaderEnd.length);
  if (newHtml !== oldHtml) {
    fs.writeFileSync(htmlPath, newHtml);
    console.info(`[OHcode] Updated ${path.relative(projectRoot, htmlPath)} chunk tags`);
  }
}

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

function readExactly(fd, length, position) {
  const out = Buffer.allocUnsafe(length);
  let offset = 0;
  while (offset < length) {
    const count = fs.readSync(fd, out, offset, length - offset, position + offset);
    if (count === 0) {
      throw new Error(`Unexpected EOF at ${position + offset}`);
    }
    offset += count;
  }
  return out;
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

const maxFileChunkSize = 64 * 1024 * 1024;

function writeBufferAt(fd, buffer, position) {
  let offset = 0;
  while (offset < buffer.length) {
    const length = Math.min(maxFileChunkSize, buffer.length - offset);
    let written = 0;
    while (written < length) {
      written += fs.writeSync(
        fd,
        buffer,
        offset + written,
        length - written,
        position === null ? null : position + offset + written
      );
    }
    offset += length;
  }
}

function fileSliceEquals(fd, position, expected) {
  const scratch = Buffer.allocUnsafe(Math.min(maxFileChunkSize, expected.length));
  let offset = 0;
  while (offset < expected.length) {
    const length = Math.min(scratch.length, expected.length - offset);
    const count = fs.readSync(fd, scratch, 0, length, position + offset);
    if (count !== length || !scratch.subarray(0, length).equals(expected.subarray(offset, offset + length))) {
      return false;
    }
    offset += length;
  }
  return true;
}

function copyFileRange(sourceFd, targetFd, sourcePosition, length) {
  const scratch = Buffer.allocUnsafe(Math.min(maxFileChunkSize, length));
  let copied = 0;
  while (copied < length) {
    const wanted = Math.min(scratch.length, length - copied);
    const count = fs.readSync(sourceFd, scratch, 0, wanted, sourcePosition + copied);
    if (count === 0) {
      throw new Error(`Unexpected EOF while copying at ${sourcePosition + copied}`);
    }
    writeBufferAt(targetFd, scratch.subarray(0, count), null);
    copied += count;
  }
}

function patchAsar(archivePath, patches, label) {
  if (!fs.existsSync(archivePath)) {
    throw new Error(`${label} not found: ${archivePath}`);
  }

  const sourceFd = fs.openSync(archivePath, "r");
  const archiveSize = fs.fstatSync(sourceFd).size;
  const prefix = readExactly(sourceFd, 16, 0);
  const oldHeaderBufferLength = prefix.readUInt32LE(4);
  const oldJsonLength = prefix.readUInt32LE(12);
  const oldContentStart = 8 + oldHeaderBufferLength;
  const oldContentLength = archiveSize - oldContentStart;
  const header = JSON.parse(readExactly(sourceFd, oldJsonLength, 16).toString("utf8"));

  let appendOffset = oldContentLength;
  const appended = [];
  const replacements = [];
  let changed = false;

  for (const [archiveName, source] of patches) {
    const data = fs.readFileSync(source);
    const existing = getEntry(header.files, archiveName);
    if (existing && existing.size === data.length) {
      const entryPosition = oldContentStart + Number(existing.offset);
      if (fileSliceEquals(sourceFd, entryPosition, data)) {
        continue;
      }
      existing.integrity = integrity(data);
      replacements.push({ offset: Number(existing.offset), data });
      changed = true;
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
    fs.closeSync(sourceFd);
    console.info(`[OHcode] ${label} runtime patches are already applied`);
    return;
  }

  const newHeader = makePickleHeader(header);
  if (appended.length === 0 && newHeader.length === oldContentStart) {
    fs.closeSync(sourceFd);
    const updateFd = fs.openSync(archivePath, "r+");
    try {
      for (const replacement of replacements) {
        writeBufferAt(updateFd, replacement.data, oldContentStart + replacement.offset);
      }
      writeBufferAt(updateFd, newHeader, 0);
    } finally {
      fs.closeSync(updateFd);
    }
    console.info(`[OHcode] Patched ${label} runtime files in place`);
    return;
  }

  const tmpPath = `${archivePath}.tmp-${process.pid}`;
  const targetFd = fs.openSync(tmpPath, "w");
  try {
    writeBufferAt(targetFd, newHeader, null);
    copyFileRange(sourceFd, targetFd, oldContentStart, oldContentLength);
    for (const replacement of replacements) {
      writeBufferAt(targetFd, replacement.data, newHeader.length + replacement.offset);
    }
    for (const data of appended) {
      writeBufferAt(targetFd, data, null);
    }
  } finally {
    fs.closeSync(targetFd);
    fs.closeSync(sourceFd);
  }
  fs.renameSync(tmpPath, archivePath);
  console.info(`[OHcode] Patched ${label} runtime files`);
}

patchSandboxPreloadFallback();
patchElectronMainStartupWindow();
generateWorkbenchLoader();
installUnpackedFiles();
patchAsar(appAsar, filesToInstall, "app.asar");
patchAsar(nodeModulesAsar, nodeModulesAsarFiles, "node_modules.asar");
