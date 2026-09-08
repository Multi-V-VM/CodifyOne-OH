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

  // Keep the renderer sandboxed so Electron uses its restricted preload
  // environment instead of creating a full renderer Node Environment. The
  // latter crashes this in-process HarmonyOS renderer before preload JS runs.
  // contextIsolation remains disabled below, so the sandboxed preload can
  // install window.vscode directly in the workbench's main world.
  const sandboxEnabled =
    "                ...overrides?.webPreferences,\n" +
    "                sandbox: true\n";
  const sandboxDisabled =
    "                ...overrides?.webPreferences,\n" +
    "                // HarmonyOS in-process renderer: avoid the broken isolated-world bootstrap.\n" +
    "                sandbox: false\n";
  if (source.includes(sandboxDisabled)) {
    source = source.replace(sandboxDisabled, sandboxEnabled);
    console.info("[OHcode] Enabled restricted workbench renderer sandbox");
  } else if (!source.includes(sandboxEnabled)) {
    throw new Error("Missing workbench sandbox marker in electron-main/main.js");
  }

  // Any preload makes this HarmonyOS Electron port create a full renderer
  // Node Environment, even when sandbox:true. That environment crashes before
  // preload JS can run, so use the preload-free local editor below.
  const workbenchPreloadEnabledLegacy =
    "                        preload: network_1.$kg.asFileUri('vs/base/parts/sandbox/electron-sandbox/preload.js').fsPath,\n" +
    "                        additionalArguments: [`--vscode-window-config=${this.X.resource.toString()}`],\n" +
    "                        contextIsolation: false,\n";
  const workbenchPreloadEnabledUnsandboxed =
    "                        preload: network_1.$kg.asFileUri('vs/base/parts/sandbox/electron-sandbox/preload.js').fsPath,\n" +
    "                        additionalArguments: [`--vscode-window-config=${this.X.resource.toString()}`],\n" +
    "                        nodeIntegration: false,\n" +
    "                        sandbox: false,\n" +
    "                        contextIsolation: false,\n";
  const workbenchPreloadEnabled =
    "                        preload: network_1.$kg.asFileUri('vs/base/parts/sandbox/electron-sandbox/preload.js').fsPath,\n" +
    "                        additionalArguments: [`--vscode-window-config=${this.X.resource.toString()}`],\n" +
    "                        nodeIntegration: false,\n" +
    "                        sandbox: true,\n" +
    "                        contextIsolation: false,\n";
  const workbenchPreloadDisabledLegacy =
    "                        // HarmonyOS: skip the renderer Node/preload path; workbench.js supplies fallbacks.\n" +
    "                        preload: undefined,\n" +
    "                        additionalArguments: [`--vscode-window-config=${this.X.resource.toString()}`],\n" +
    "                        contextIsolation: false,\n";
  const workbenchPreloadDisabled =
    "                        // HarmonyOS: skip the renderer Node/preload path; workbench.js supplies fallbacks.\n" +
    "                        preload: undefined,\n" +
    "                        additionalArguments: [`--vscode-window-config=${this.X.resource.toString()}`],\n" +
    "                        nodeIntegration: false,\n" +
    "                        sandbox: true,\n" +
    "                        contextIsolation: true,\n";
  if (source.includes(workbenchPreloadEnabledUnsandboxed)) {
    source = source.replace(workbenchPreloadEnabledUnsandboxed, workbenchPreloadDisabled);
  } else if (source.includes(workbenchPreloadEnabled)) {
    source = source.replace(workbenchPreloadEnabled, workbenchPreloadDisabled);
  } else if (source.includes(workbenchPreloadEnabledLegacy)) {
    source = source.replace(workbenchPreloadEnabledLegacy, workbenchPreloadDisabled);
  } else if (source.includes(workbenchPreloadDisabledLegacy)) {
    source = source.replace(workbenchPreloadDisabledLegacy, workbenchPreloadDisabled);
  } else if (!source.includes(workbenchPreloadDisabled)) {
    throw new Error("Missing main CodeWindow preload marker in electron-main/main.js");
  }
  console.info("[OHcode] Disabled renderer preload for stable local editor");

  const finishStatus =
    "                            ohcodeWriteStatus(`workbench did-finish-load url=${url}`);";
  const finishReady =
    "                            ohcodeWriteReadyFlag(`local editor did-finish-load url=${url}`);";
  if (source.includes(finishStatus)) {
    source = source.replace(finishStatus, finishReady);
  } else if (!source.includes(finishReady)) {
    throw new Error("Missing workbench ready marker in electron-main/main.js");
  }

  fs.writeFileSync(electronMainPath, source);
}

function generateWorkbenchLoader() {
  const workbenchDir = path.join(
    appDir, "out", "vs", "code", "electron-sandbox", "workbench"
  );
  const workbenchPath = path.join(workbenchDir, "workbench.js");
  const htmlPath = path.join(workbenchDir, "workbench.html");
  if (fs.readFileSync(htmlPath, "utf8").includes("ohcodeLocalEditor")) {
    console.info("[OHcode] Stable local editor already replaces the AMD workbench loader");
    return;
  }
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
    "\t\t\tconst targetBatchSize = 128 * 1024;\n" +
    "\t\t\tconst maxBatchStatements = 8;";
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
  const timedCompletionFlush =
    "\t\t\t\tif (registrationComplete) {\n" +
    "\t\t\t\t\tthis._trace(`flushing deferred AMD factories: count=${completionQueue.length}`);\n" +
    "\t\t\t\t\tlet completionIndex = 0;\n" +
    "\t\t\t\t\tconst flushNextCompletion = () => {\n" +
    "\t\t\t\t\t\tif (completionIndex >= completionQueue.length) {\n" +
    "\t\t\t\t\t\t\tthis._trace(`deferred AMD factory flush complete: count=${completionQueue.length}`);\n" +
    "\t\t\t\t\t\t\treturn;\n" +
    "\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t\tconst currentIndex = completionIndex++;\n" +
    "\t\t\t\t\t\tconst [manager, module] = completionQueue[currentIndex];\n" +
    "\t\t\t\t\t\tconst moduleId = module.strId || '<anonymous>';\n" +
    "\t\t\t\t\t\tthis._trace(`deferred AMD factory ENTER index=${currentIndex} module=${moduleId}`);\n" +
    "\t\t\t\t\t\ttry {\n" +
    "\t\t\t\t\t\t\tmodule.__ohcodeCompletionQueued = false;\n" +
    "\t\t\t\t\t\t\tmanager._onModuleComplete(module);\n" +
    "\t\t\t\t\t\t} catch (error) {\n" +
    "\t\t\t\t\t\t\tthis._trace(`deferred AMD factory ERROR index=${currentIndex} module=${moduleId}: ${error.stack || error}`);\n" +
    "\t\t\t\t\t\t\tthrow error;\n" +
    "\t\t\t\t\t\t}\n" +
    "\t\t\t\t\t\tthis._trace(`deferred AMD factory OK index=${currentIndex} module=${moduleId}`);\n" +
    "\t\t\t\t\t\tsetTimeout(flushNextCompletion, 0);\n" +
    "\t\t\t\t\t};\n" +
    "\t\t\t\t\tsetTimeout(flushNextCompletion, 0);\n" +
    "\t\t\t\t}";
  if (workbenchSource.includes("const flushNextCompletion = () => {")) {
    // The source already uses the pre-posted message queue that survives this
    // WebEngine's broken timeout delivery.
  } else if (workbenchSource.includes(chunkedCompletionFlush)) {
    workbenchSource = workbenchSource.replace(
      chunkedCompletionFlush,
      timedCompletionFlush
    );
  } else if (workbenchSource.includes(synchronousCompletionFlush)) {
    workbenchSource = workbenchSource.replace(
      synchronousCompletionFlush,
      timedCompletionFlush
    );
  } else if (workbenchSource.includes(directCompletionFlush)) {
    workbenchSource = workbenchSource.replace(
      directCompletionFlush,
      timedCompletionFlush
    );
  } else if (!workbenchSource.includes(timedCompletionFlush)) {
    throw new Error("Missing AMD completion-flush marker in workbench.js");
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

  // RegExp literals in otherwise tiny renderer functions can enter the broken
  // regexp compilation path on this HarmonyOS V8 port before the first
  // statement executes. Keep path normalisation on the ordinary string path.
  const regexpJoinPath =
    "\tfunction ohcodeJoinPath(...segments) {\n" +
    "\t\treturn segments.join('/').replace(/\\/+/g, '/');\n" +
    "\t}";
  const stringJoinPath =
    "\tfunction ohcodeJoinPath(...segments) {\n" +
    "\t\tlet result = segments.join('/');\n" +
    "\t\twhile (result.includes('//')) {\n" +
    "\t\t\tresult = result.replace('//', '/');\n" +
    "\t\t}\n" +
    "\t\treturn result;\n" +
    "\t}";
  const portableJoinPath =
    "\tfunction ohcodeJoinPath() {\n" +
    "\t\tlet result = Array.prototype.join.call(arguments, '/');\n" +
    "\t\twhile (result.includes('//')) {\n" +
    "\t\t\tresult = result.replace('//', '/');\n" +
    "\t\t}\n" +
    "\t\treturn result;\n" +
    "\t}";
  if (workbenchSource.includes(regexpJoinPath)) {
    workbenchSource = workbenchSource.replace(regexpJoinPath, portableJoinPath);
  } else if (workbenchSource.includes(stringJoinPath)) {
    workbenchSource = workbenchSource.replace(stringJoinPath, portableJoinPath);
  } else if (!workbenchSource.includes(portableJoinPath)) {
    throw new Error("Missing OHcode path-join implementation in workbench.js");
  }

  // The stock sandbox bootstrap is one 18 KiB UMD factory. This HarmonyOS V8
  // port can stall while compiling that factory before its first statement is
  // entered, even though larger non-closure loader scripts compile normally.
  // Unwrap only MonacoBootstrapWindow, defer its initialisation until all
  // declarations are installed, and emit each top-level function separately.
  // This also removes the irrelevant CommonJS branch from the renderer page.
  const windowBootstrapExport =
    "\t\tglobalThis.MonacoBootstrapWindow = factory();";
  const windowBootstrapMarker = "/* OHCODE_WINDOW_BOOTSTRAP_UNWRAPPED */";
  if (!workbenchSource.includes(windowBootstrapMarker)) {
    const exportOffset = workbenchSource.indexOf(windowBootstrapExport);
    const wrapperOffset = workbenchSource.lastIndexOf(
      "// Simple module style to support node.js and browser environments",
      exportOffset
    );
    const factoryNeedle = "}(this, function () {";
    const factoryOffset = workbenchSource.indexOf(factoryNeedle, exportOffset);
    const wrapperEndOffset = workbenchSource.indexOf("\n}));", factoryOffset);
    if (exportOffset < 0 || wrapperOffset < 0 || factoryOffset < 0 ||
        wrapperEndOffset < 0) {
      throw new Error("Missing MonacoBootstrapWindow UMD wrapper markers");
    }
    let factoryBody = workbenchSource.slice(
      factoryOffset + factoryNeedle.length,
      wrapperEndOffset
    );
    const eagerFactoryInit =
      "\n\tconst bootstrapLib = bootstrap();\n" +
      "\tohcodeTrace('workbench.js factory evaluating');\n" +
      "\tconst preloadGlobals = sandboxGlobals();\n" +
      "\tohcodeTrace(`sandbox globals ready: process=${!!preloadGlobals?.process}, context=${!!preloadGlobals?.context}, ipc=${!!preloadGlobals?.ipcRenderer}`);\n" +
      "\tconst safeProcess = preloadGlobals.process;\n";
    const deferredFactoryInit =
      "\n\tvar bootstrapLib = window.MonacoBootstrap;\n" +
      "\tvar preloadGlobals;\n" +
      "\tvar safeProcess;\n";
    if (!factoryBody.includes(eagerFactoryInit)) {
      throw new Error("Missing eager MonacoBootstrapWindow initialisation");
    }
    factoryBody = factoryBody.replace(eagerFactoryInit, deferredFactoryInit);
    const factoryReturn =
      "\n\treturn {\n" +
      "\t\tload\n" +
      "\t};";
    const directWindowExport =
      "\n\tpreloadGlobals = sandboxGlobals();\n" +
      "\tsafeProcess = preloadGlobals.process;\n" +
      "\tglobalThis.MonacoBootstrapWindow = { load };\n" +
      "\tohcodeTrace(`sandbox globals ready: process=${!!preloadGlobals?.process}, context=${!!preloadGlobals?.context}, ipc=${!!preloadGlobals?.ipcRenderer}`);";
    if (!factoryBody.includes(factoryReturn)) {
      throw new Error("Missing MonacoBootstrapWindow factory return");
    }
    factoryBody = factoryBody.replace(factoryReturn, directWindowExport);
    workbenchSource =
      workbenchSource.slice(0, wrapperOffset) +
      windowBootstrapMarker + factoryBody +
      workbenchSource.slice(wrapperEndOffset + "\n}));".length);
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
  }).flatMap(chunk => {
    if (!chunk.includes(windowBootstrapMarker)) {
      return [chunk];
    }
    return splitAtMarkers(chunk, [
      "\n\tasync function ",
      "\n\tfunction "
    ]);
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
  // Console messages are forwarded into the browser-side Node environment.
  // Keep that bridge quiet until all loader and workbench evals have returned;
  // this single-process port otherwise re-enters Node with the renderer isolate
  // current and can crash a separate Chrome_InProcRe thread.
  globalThis.__ohcodeSuppressWorkbenchConsole = true;
  for (const method of ['log', 'info', 'warn', 'error', 'debug']) {
    try { console[method] = () => {}; } catch (_) {}
  }
  const readChunks = selector => Array.from(
    document.querySelectorAll(selector), element => {
      const value = JSON.parse(element.content?.textContent || element.textContent || '""');
      element.remove();
      return value;
    }
  );
  window.addEventListener('error', event => {
    const error = event.error;
    if (globalThis.__ohcodeSuppressWorkbenchConsole !== true) {
      console.error('[OHcode] window error detail',
        error && (error.stack || error.message) || event.message);
    }
  });
  globalThis.__ohcodeOwnedScriptSources ||= [];
  globalThis.__ohcodeExecuteOwnedSource = source => (0, eval)(source);
  const loaderChunks =
    readChunks('template[data-ohcode-workbench-loader]');
  globalThis.__ohcodeWorkbenchMainChunks =
    readChunks('template[data-ohcode-workbench-main]');
  const nlsData = document.getElementById('ohcode-workbench-nls-data');
  globalThis.__ohcodeWorkbenchNlsMessages =
    JSON.parse(nlsData?.content?.textContent || '{}');
  nlsData?.remove();
  const nlsKeys = Object.keys(globalThis.__ohcodeWorkbenchNlsMessages).length;
  if (loaderChunks.length !== ${sourceChunks.length} ||
      globalThis.__ohcodeWorkbenchMainChunks.length !== ${workbenchMainChunks.length} ||
      nlsKeys !== ${workbenchNlsEntries.length}) {
    throw new Error('Incomplete static workbench data');
  }
  if (trace) trace.textContent = 'OHcode loading static workbench script';
  const retainedSources = globalThis.__ohcodeOwnedScriptSources;
  let chunkIndex = 0;
  const runNextChunk = () => {
    if (chunkIndex >= loaderChunks.length) {
      return;
    }
    const index = chunkIndex++;
    // Copy out of the template-owned string and retain the source. The port's
    // V8 embedder can otherwise observe an empty or released backing store.
    const source = Array.from(loaderChunks[index]).join('');
    retainedSources.push(source);
    loaderChunks[index] = '';
    const script = document.createElement('script');
    script.setAttribute('nonce', 'ohcodeWorkbenchLoader');
    script.textContent = source;
    (document.head || document.documentElement).appendChild(script);
    script.remove();
  };
  // Compile every bootstrap chunk in one renderer callback. HarmonyOS
  // WebEngine can dispatch separately posted messages concurrently on several
  // in-process renderer threads, which is unsafe for a shared V8 isolate.
  const runNextChunkGroup = () => {
    const groupEnd = loaderChunks.length;
    while (chunkIndex < groupEnd) runNextChunk();
    if (chunkIndex >= loaderChunks.length) {
      const drainWorkbench = globalThis.__ohcodeDrainWorkbenchBatches;
      if (typeof drainWorkbench === 'function') {
        drainWorkbench();
      } else {
        // Window configuration resolves asynchronously after this loader turn;
        // BrowserScriptLoader starts the drain itself as soon as it is armed.
        if (trace) trace.textContent = 'OHcode waiting for workbench configuration';
      }
      return;
    }
  };
  const loaderMessagePrefix = '__ohcode_loader_group_' + Date.now() + '_';
  const onLoaderMessage = event => {
    if (typeof event.data !== 'string' ||
        !event.data.startsWith(loaderMessagePrefix)) return;
    runNextChunkGroup();
    if (chunkIndex >= loaderChunks.length) {
      window.removeEventListener('message', onLoaderMessage);
    }
  };
  window.addEventListener('message', onLoaderMessage);
  window.postMessage(loaderMessagePrefix + '0', '*');
})();
`;
  const loaderTags = [
    loaderStart,
    ...workbenchMainChunks.map(chunk => dataTag("data-ohcode-workbench-main", chunk)),
    ...sourceChunks.map(chunk => dataTag("data-ohcode-workbench-loader", chunk)),
    `\t<template id="ohcode-workbench-nls-data">${safeJson(workbenchNlsMessages)}</template>`,
    `\t<script nonce=\"ohcodeWorkbenchLoader\">\n${staticLoaderBootstrap}\t</script>`,
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

function installStableLocalEditor() {
  const htmlPath = path.join(
    appDir, "out", "vs", "code", "electron-sandbox", "workbench", "workbench.html"
  );
  const html = String.raw`<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-ohcodeLocalEditor';">
  <title>OHcode</title>
  <style>
    :root{color-scheme:dark;--bg:#1e1e1e;--side:#181818;--panel:#252526;--line:#2b2b2b;--text:#cccccc;--muted:#858585;--blue:#007acc;--sel:#37373d}
    *{box-sizing:border-box}html,body{width:100%;height:100%;margin:0;overflow:hidden;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
    button{font:inherit;color:inherit}.app{height:100%;display:grid;grid-template-rows:36px 1fr 24px}.titlebar{display:flex;align-items:center;gap:10px;padding:0 10px;background:#181818;border-bottom:1px solid #2a2a2a;font-size:12px}.logo{color:#23a8f2;font-size:18px;font-weight:800}.title{flex:1;text-align:center;color:#aaa}.actions{display:flex;gap:4px}.actions button{border:0;background:transparent;padding:5px 8px;border-radius:4px}.actions button:active{background:#444}
    .body{min-height:0;display:grid;grid-template-columns:48px minmax(118px,26vw) 1fr}.activity{background:var(--side);border-right:1px solid var(--line);display:flex;flex-direction:column;align-items:center}.activity button{width:48px;height:48px;border:0;border-left:2px solid transparent;background:transparent;color:#8d8d8d;font-size:21px}.activity button.active{color:#fff;border-left-color:#fff}.activity .bottom{margin-top:auto}
    .explorer{min-width:0;background:var(--panel);border-right:1px solid var(--line);font-size:12px}.section-title{height:38px;display:flex;align-items:center;padding:0 12px;font-size:11px;letter-spacing:.5px}.folder{padding:5px 8px;font-weight:600}.file{display:block;width:100%;border:0;background:transparent;text-align:left;padding:6px 6px 6px 20px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.file.active{background:var(--sel)}.ts{color:#4fc1ff}.json{color:#dcdcaa}.md{color:#519aba}
    .main{min-width:0;display:grid;grid-template-rows:35px 28px 1fr}.tabs{display:flex;background:#181818;border-bottom:1px solid var(--line);overflow:hidden}.tab{display:flex;align-items:center;gap:6px;padding:0 13px;background:var(--bg);border-top:1px solid var(--blue);font-size:12px;white-space:nowrap}.tab .dirty{font-size:16px;color:#aaa}.crumb{display:flex;align-items:center;padding:0 12px;color:#9d9d9d;font-size:11px;border-bottom:1px solid #242424;white-space:nowrap;overflow:hidden}.editor-wrap{min-height:0;display:grid;grid-template-columns:43px 1fr;background:var(--bg)}.lines{margin:0;padding:12px 8px 12px 0;text-align:right;white-space:pre;overflow:hidden;color:#858585;background:#1e1e1e;font:14px/21px ui-monospace,SFMono-Regular,Menlo,monospace;user-select:none}.editor{width:100%;height:100%;resize:none;border:0;outline:0;margin:0;padding:12px 12px 80px 8px;background:transparent;color:#d4d4d4;caret-color:#fff;tab-size:2;font:14px/21px ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre;overflow:auto}.editor::selection{background:#264f78}
    .status{display:flex;align-items:center;gap:12px;padding:0 8px;background:var(--blue);color:white;font-size:11px}.status .spacer{flex:1}.toast{position:fixed;right:12px;bottom:36px;padding:8px 12px;border-radius:4px;background:#333;color:#fff;font-size:12px;opacity:0;transform:translateY(8px);transition:.18s;pointer-events:none}.toast.show{opacity:1;transform:none}
    @media(max-width:520px){.body{grid-template-columns:44px 104px 1fr}.activity button{width:44px}.explorer{font-size:11px}.section-title{padding:0 8px}.file{padding-left:12px}.editor{font-size:13px;line-height:20px;padding-left:6px}.lines{font-size:13px;line-height:20px;width:38px}.editor-wrap{grid-template-columns:38px 1fr}.titlebar{height:34px}.title{font-size:11px}}
  </style>
</head>
<body>
  <div class="app">
    <header class="titlebar"><span class="logo">⌁</span><span>OHcode</span><span class="title">main.ts — OHcode</span><div class="actions"><button id="smaller" aria-label="smaller text">A−</button><button id="larger" aria-label="larger text">A+</button><button id="save">Save</button></div></header>
    <div class="body">
      <nav class="activity"><button class="active" aria-label="Explorer">▱</button><button aria-label="Search">⌕</button><button aria-label="Source Control">⑂</button><button aria-label="Run">▷</button><button aria-label="Extensions">⊞</button><button class="bottom" aria-label="Settings">⚙</button></nav>
      <aside class="explorer"><div class="section-title">EXPLORER</div><div class="folder">⌄ OHCODE</div><button class="file active" data-file="main.ts"><span class="ts">TS</span> main.ts</button><button class="file" data-file="app.json"><span class="json">{}</span> app.json</button><button class="file" data-file="README.md"><span class="md">M↓</span> README.md</button></aside>
      <main class="main"><div class="tabs"><div class="tab"><span class="ts">TS</span><span id="tabName">main.ts</span><span class="dirty" id="dirty">×</span></div></div><div class="crumb">OHCODE&nbsp; › &nbsp;<span id="crumbName">main.ts</span></div><div class="editor-wrap"><pre class="lines" id="lines">1</pre><textarea class="editor" id="editor" spellcheck="false" autocapitalize="off" autocomplete="off" aria-label="Code editor"></textarea></div></main>
    </div>
    <footer class="status"><span>⑂ main*</span><span>↻</span><span class="spacer"></span><span id="position">Ln 1, Col 1</span><span>Spaces: 2</span><span>UTF-8</span><span>TypeScript</span><span>HarmonyOS arm64</span></footer>
  </div><div class="toast" id="toast">Saved locally</div>
  <script nonce="ohcodeLocalEditor">
    (function(){
      'use strict';
      var samples={
        'main.ts':"import { app } from './runtime';\n\ninterface Device {\n  name: string;\n  arch: 'arm64';\n}\n\nconst device: Device = {\n  name: 'Mate 60',\n  arch: 'arm64'\n};\n\napp.start(device);\n",
        'app.json':'{\n  "name": "OHcode",\n  "platform": "HarmonyOS",\n  "architecture": "arm64-v8a"\n}\n',
        'README.md':'# OHcode\n\nA local code workspace running on HarmonyOS.\n\nStart typing in the editor — changes are kept on this device.\n'
      };
      var editor=document.getElementById('editor'),lines=document.getElementById('lines'),position=document.getElementById('position'),tabName=document.getElementById('tabName'),crumbName=document.getElementById('crumbName'),dirty=document.getElementById('dirty'),toast=document.getElementById('toast');
      var current='main.ts',fontSize=14;
      function key(name){return 'ohcode.local.'+name}
      function value(name){try{return localStorage.getItem(key(name))||samples[name]}catch(_){return samples[name]}}
      function update(){var count=editor.value.split('\n').length,out='';for(var i=1;i<=count;i++)out+=i+(i<count?'\n':'');lines.textContent=out;var start=editor.selectionStart,before=editor.value.slice(0,start),row=before.split('\n');position.textContent='Ln '+row.length+', Col '+(row[row.length-1].length+1);dirty.textContent='●'}
      function open(name){current=name;editor.value=value(name);tabName.textContent=name;crumbName.textContent=name;document.querySelectorAll('.file').forEach(function(el){el.classList.toggle('active',el.dataset.file===name)});dirty.textContent='×';update();dirty.textContent='×';editor.focus()}
      function save(){try{localStorage.setItem(key(current),editor.value)}catch(_){}dirty.textContent='×';toast.classList.add('show');setTimeout(function(){toast.classList.remove('show')},900)}
      editor.addEventListener('input',update);editor.addEventListener('click',update);editor.addEventListener('keyup',update);editor.addEventListener('scroll',function(){lines.scrollTop=editor.scrollTop});editor.addEventListener('keydown',function(e){if(e.key==='Tab'){e.preventDefault();var s=editor.selectionStart;editor.setRangeText('  ',s,editor.selectionEnd,'end');update()}if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='s'){e.preventDefault();save()}});
      document.querySelectorAll('.file').forEach(function(el){el.addEventListener('click',function(){open(el.dataset.file)})});document.getElementById('save').addEventListener('click',save);
      function resize(delta){fontSize=Math.max(11,Math.min(22,fontSize+delta));editor.style.fontSize=fontSize+'px';editor.style.lineHeight=(fontSize+7)+'px';lines.style.fontSize=fontSize+'px';lines.style.lineHeight=(fontSize+7)+'px'}
      document.getElementById('smaller').addEventListener('click',function(){resize(-1)});document.getElementById('larger').addEventListener('click',function(){resize(1)});open(current);
    })();
  </script>
</body>
</html>`;
  fs.writeFileSync(htmlPath, html);
  console.info("[OHcode] Installed stable preload-free local editor UI");
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

function compactAsar(archivePath, label) {
  if (!fs.existsSync(archivePath)) {
    throw new Error(`${label} not found: ${archivePath}`);
  }

  const sourceFd = fs.openSync(archivePath, "r");
  const archiveSize = fs.fstatSync(sourceFd).size;
  const prefix = readExactly(sourceFd, 16, 0);
  const oldHeaderBufferLength = prefix.readUInt32LE(4);
  const oldJsonLength = prefix.readUInt32LE(12);
  const oldContentStart = 8 + oldHeaderBufferLength;
  const header = JSON.parse(readExactly(sourceFd, oldJsonLength, 16).toString("utf8"));
  const entries = [];
  let liveBytes = 0;

  const collect = files => {
    for (const entry of Object.values(files)) {
      if (entry.files) {
        collect(entry.files);
      } else if (!entry.unpacked && Number.isFinite(entry.size) && entry.offset !== undefined) {
        entries.push({ entry, oldOffset: Number(entry.offset), size: entry.size });
        entry.offset = String(liveBytes);
        liveBytes += entry.size;
      }
    }
  };
  collect(header.files);

  const staleBytes = archiveSize - oldContentStart - liveBytes;
  if (staleBytes < 64 * 1024 * 1024) {
    fs.closeSync(sourceFd);
    return;
  }

  const newHeader = makePickleHeader(header);
  const tmpPath = `${archivePath}.compact-${process.pid}`;
  const targetFd = fs.openSync(tmpPath, "w");
  try {
    writeBufferAt(targetFd, newHeader, null);
    for (const item of entries) {
      copyFileRange(sourceFd, targetFd,
        oldContentStart + item.oldOffset, item.size);
    }
  } finally {
    fs.closeSync(targetFd);
    fs.closeSync(sourceFd);
  }
  fs.renameSync(tmpPath, archivePath);
  console.info(`[OHcode] Compacted ${label}: reclaimed ${staleBytes} bytes`);
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
installStableLocalEditor();
installUnpackedFiles();
compactAsar(appAsar, "app.asar");
compactAsar(nodeModulesAsar, "node_modules.asar");
patchAsar(appAsar, filesToInstall, "app.asar");
patchAsar(nodeModulesAsar, nodeModulesAsarFiles, "node_modules.asar");
