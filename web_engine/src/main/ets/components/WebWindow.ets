// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { NativeContext, WindowBound } from '../interface/CommonInterface';
import { AbilityManager } from '../common/AbilityManager';
import lazy { DragDropAdapter } from '../adapter/DragDropAdapter';
import LogUtil from '../utils/LogUtil';
import { ConfigData, ContextType, PanAction } from '../common/Constants';
import Inject from '../common/InjectModule';
import JsBindingUtils from '../utils/JsBindingUtils';
import { getV8PoolHook } from '../common/V8PoolHook';
import { MultiInputAdapter } from '../adapter/MultiInputAdapter';
import { DragParamManager } from '../common/DragParamManager';
import { window } from '@kit.ArkUI';
import { fileIo } from '@kit.CoreFileKit';

import { CommandType } from '../common/Constants';
import { IMessageBoxParams } from '../adapter/DialogAdapter';
import { MessageBox } from '../components/MessageBox';
import { typeNode as TypedNode, NodeController, FrameNode } from '@ohos.arkui.node';

const TAG: string = 'WindowXComponentController';
const STARTUP_STATUS_FILE: string = '/data/storage/el2/base/files/ohcode-startup-status.txt';
const STARTUP_MAIN_TRACE_FILES: string[] = [
  '/data/storage/el2/base/files/ohcode-main-trace.log',
  '/data/storage/el1/base/files/ohcode-main-trace.log',
  '/tmp/ohcode-main-trace.log'
];
const STARTUP_BUILD_STAMP: string = 'diag-20260626-tracefanout1';
const ELECTRON_APP_PATH: string = '--app-path=../../electron/resources/resfile/app';
const DISABLED_PRELOAD_FEATURES: string =
  'EnableNetworkPreload,' +
  'PreloadingConfig,' +
  'PreloadingHoldback,' +
  'PrefetchFollowsNormalCacheSemantics,' +
  'PrefetchNoVarySearch,' +
  'PrefetchPrivacyChanges,' +
  'PrefetchRedirects,' +
  'PrefetchUseContentRefactor,' +
  'Prerender2,' +
  'Prerender2Holdback,' +
  'Prerender2InNewTab,' +
  'Prerender2MainFrameNavigation,' +
  'Prerender2MemoryControls,' +
  'Prerender2SequentialPrerendering,' +
  'SpeculationRulesPrefetch,' +
  'SpeculationRulesPrefetchFuture,' +
  'SpeculationRulesPrefetchProxy,' +
  'SpeculationRulesPrefetchWithSubresources';
const CHROMIUM_PROCESS_FLAGS: string[] = [
  '--no-sandbox',
  '--disable-gpu-sandbox',
  '--in-process-gpu',
  '--single-process',
  '--force-disable-user-env',
  '--disable-dev-shm-usage',
  '--disable-http-cache',
  '--disable-background-networking',
  '--dns-prefetch-disable',
  '--disable-features=' + DISABLED_PRELOAD_FEATURES,
  '--disable-crash-reporter',
  '--disable-breakpad',
  '--disable-gpu-watchdog',
  '--ignore-gpu-blocklist',
  '--enable-logging=stderr',
  '--log-level=0',
  '--ohcode-startup-build=' + STARTUP_BUILD_STAMP
];
const BLOCKED_RELAUNCH_SWITCH_PREFIXES: string[] = [
  '--use-angle',
  '--use-gl',
  '--enable-features',
  '--disable-features',
  '--enable-unsafe-swiftshader',
  '--ozone-platform',
  '--ozone-dump-file',
  '--zygote',
  '--disable-gpu',
  '--disable-vulkan',
  '--disable-gpu-compositing',
  '--disable-gpu-rasterization',
  '--disable-gpu-memory-buffer-compositor-resources',
  '--disable-gpu-memory-buffer-video-frames',
  '--disable-accelerated-2d-canvas',
  '--disable-accelerated-video-decode',
  '--disable-zero-copy',
  '--disable-software-rasterizer'
];

interface ComponentConfig {
  moduleName: string,
  xComponentId: string,
  uri: string,
  nativeContext: NativeContext,
  window?: window.Window,
  electronRelaunchArgs: string[]
}

class WindowXComponentController extends XComponentController {
  onSurfaceCreated(surfaceId: string): void {
    LogUtil.info(TAG, `onSurfaceCreated surfaceId: ${surfaceId}`);
  }
  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    LogUtil.info(TAG, `onSurfaceChanged surfaceId: ${surfaceId}, rect: ${JSON.stringify(rect)}`);
  }
  onSurfaceDestroyed(surfaceId: string): void {
    LogUtil.info(TAG, `onSurfaceDestroyed surfaceId: ${surfaceId}`);
  }
}

class WindowNodeController extends NodeController {
  private xComponentController: XComponentController = new WindowXComponentController();
  private xComponent: TypedNode.XComponent | undefined = undefined;
  private config: ComponentConfig;

  constructor(config: ComponentConfig) {
    super();
    this.config = config;
  }

  private writeStartupStatus(status: string): void {
    try {
      const file = fileIo.openSync(STARTUP_STATUS_FILE,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
      fileIo.writeSync(file.fd, `${Date.now()} [${STARTUP_BUILD_STAMP}] ${status}`);
      fileIo.closeSync(file);
    } catch (err) {
      LogUtil.error(TAG, 'write startup status failed: ' + JSON.stringify(err));
    }
    LogUtil.info(TAG, 'startup status: [' + STARTUP_BUILD_STAMP + '] ' + status);
  }

  private readStartupMainTraceTail(maxChars: number): string {
    const parts: string[] = [];
    for (let i = 0; i < STARTUP_MAIN_TRACE_FILES.length; i++) {
      const traceFile = STARTUP_MAIN_TRACE_FILES[i];
      try {
        if (!fileIo.accessSync(traceFile)) {
          parts.push(traceFile + '=<missing>');
          continue;
        }
        const traceText: string = fileIo.readTextSync(traceFile);
        const tail = traceText.length <= maxChars
          ? traceText
          : traceText.substring(traceText.length - maxChars);
        parts.push(traceFile + '=' + tail.replace(/\n/g, ' | '));
      } catch (err) {
        parts.push(traceFile + '=<read failed ' + JSON.stringify(err) + '>');
      }
    }
    return parts.join(' ; ');
  }

  makeNode(uiContext: UIContext): FrameNode | null {
    this.writeStartupStatus('makeNode');
    let node = new FrameNode(uiContext);
    this.xComponent = TypedNode.createNode(uiContext, "XComponent", {
      type: XComponentType.SURFACE,
      controller: this.xComponentController
    })
    this.xComponent.initialize({ id: this.config.moduleName, type: XComponentType.SURFACE, controller: this.xComponentController, libraryname: "adapter"});
    this.xComponent.attribute
      .defaultFocus(true)
      .focusable(true)
      .focusOnTouch(true)
      .backgroundColor(Color.Transparent)
      .renderFit(RenderFit.TOP_LEFT)
      .onLoad(() => {
        this.writeStartupStatus('xcomponent onLoad');
        this.setDefaultBounds();
        let vec_args = this.buildArgs();
        const envResult = getV8PoolHook().sanitizeNodeEnvironment();
        this.writeStartupStatus('node environment sanitized=' + JSON.stringify(envResult));
        const hookResult = getV8PoolHook().installElectronPltHooks();
        this.writeStartupStatus('electron PLT hook=' + JSON.stringify(hookResult));
        this.writeStartupStatus('calling runBrowser args=' + JSON.stringify(vec_args));
        try {
          this.config.nativeContext.runBrowser(vec_args);
          this.writeStartupStatus('runBrowser returned');
          const hookAfterRun = getV8PoolHook().getElectronPltHookStats();
          this.writeStartupStatus('electron PLT hook after runBrowser=' + JSON.stringify(hookAfterRun));
          this.schedulePostRunHookProbe();
          setTimeout(() => {
            const delayedHook = getV8PoolHook().getElectronPltHookStats();
            this.writeStartupStatus('electron PLT hook delayed=' + JSON.stringify(delayedHook));
          }, 500);
          this.scheduleNativeStartupWatch();
        } catch (err) {
          this.writeStartupStatus('runBrowser threw ' + JSON.stringify(err));
          return;
        }
        this.openInitialWindowWithRetry(0);
      })
    node.appendChild(this.xComponent);
    return node;
  }

  private schedulePostRunHookProbe(): void {
    let probeDone: boolean = false;
    for (let i = 1; i <= 20; i++) {
      const delayMs: number = i * 5;
      setTimeout(() => {
        if (probeDone) {
          return;
        }
        const probe = getV8PoolHook().getElectronPltHookStats();
        if (probe.installed || i === 20) {
          probeDone = true;
          this.writeStartupStatus('electron PLT hook probe=' + JSON.stringify(probe));
        }
      }, delayMs);
    }
  }

  private scheduleNativeStartupWatch(): void {
    for (let i = 1; i <= 30; i++) {
      setTimeout(() => {
        let destroyed: boolean | undefined = undefined;
        try {
          destroyed = this.config.nativeContext.BrowserDestroyed();
        } catch (err) {
          this.writeStartupStatus('native startup watch ' + i + ' BrowserDestroyed threw ' + JSON.stringify(err));
          return;
        }
        const hookStats = getV8PoolHook().getElectronPltHookStats();
        if (i === 3 || i === 7 || i === 15 || i === 30) {
          this.writeStartupStatus('main trace tail watch ' + i + ' ' + this.readStartupMainTraceTail(1800));
        }
        this.writeStartupStatus('native startup watch ' + i + ' destroyed=' + destroyed +
          ' v8InitializeCalls=' + hookStats.v8InitializeCalls +
          ' v8InitializeFailures=' + hookStats.v8InitializeFailures +
          ' v8StartupFlagsApplied=' + hookStats.v8StartupFlagsApplied +
          ' v8StartupFlagsApplyAttempts=' + hookStats.v8StartupFlagsApplyAttempts +
          ' v8StartupFlagsLength=' + hookStats.v8StartupFlagsLength +
          ' v8StartupFlagsSymbolResolved=' + hookStats.v8StartupFlagsSymbolResolved +
          ' v8StartupFlagsEnvEmpty=' + hookStats.v8StartupFlagsEnvEmpty +
          ' v8StartupFlagsUsingDefault=' + hookStats.v8StartupFlagsUsingDefault +
          ' v8SetSnapshotDataBlobCalls=' + hookStats.v8SetSnapshotDataBlobCalls +
          ' lastV8SnapshotDataBlobRawSize=' + hookStats.lastV8SnapshotDataBlobRawSize +
          ' lastEffectiveV8SnapshotDataBlobRawSize=' + hookStats.lastEffectiveV8SnapshotDataBlobRawSize +
          ' replaceContextSnapshotWithStartup=' + hookStats.replaceContextSnapshotWithStartup +
          ' skipV8SnapshotDataBlob=' + hookStats.skipV8SnapshotDataBlob +
          ' snapshotBlobSkips=' + hookStats.snapshotBlobSkips +
          ' snapshotBlobReplacements=' + hookStats.snapshotBlobReplacements +
          ' snapshotBlobReplacementFailures=' + hookStats.snapshotBlobReplacementFailures +
          ' baseStartupBlobRawSize=' + hookStats.baseStartupBlobRawSize +
          ' v8InitializeExternalStartupDataCalls=' + hookStats.v8InitializeExternalStartupDataCalls +
          ' v8InitializeExternalStartupDataFromFileCalls=' + hookStats.v8InitializeExternalStartupDataFromFileCalls +
          ' lastV8ExternalStartupDataPath=' + hookStats.lastV8ExternalStartupDataPath +
          ' v8InitWithSnapshotSlots=' + hookStats.v8InitWithSnapshotSlots +
          ' v8InitWithSnapshotCalls=' + hookStats.v8InitWithSnapshotCalls +
          ' v8InitWithSnapshotFailures=' + hookStats.v8InitWithSnapshotFailures +
          ' lastV8InitWithSnapshotCallerOffset=' + hookStats.lastV8InitWithSnapshotCallerOffset +
          ' lastV8InitWithSnapshotReadOnlyLength=' + hookStats.lastV8InitWithSnapshotReadOnlyLength +
          ' lastV8InitWithSnapshotSharedLength=' + hookStats.lastV8InitWithSnapshotSharedLength +
          ' lastV8InitWithSnapshotStartupLength=' + hookStats.lastV8InitWithSnapshotStartupLength +
          ' lastV8InitWithSnapshotCanRehash=' + hookStats.lastV8InitWithSnapshotCanRehash +
          ' lastV8InitWithSnapshotResult=' + hookStats.lastV8InitWithSnapshotResult +
          ' nodeNewContextSlots=' + hookStats.nodeNewContextSlots +
          ' nodeNewContextCalls=' + hookStats.nodeNewContextCalls +
          ' nodeNewContextNulls=' + hookStats.nodeNewContextNulls +
          ' lastNodeNewContextResult=' + hookStats.lastNodeNewContextResult +
          ' nodeInitializeContextCalls=' + hookStats.nodeInitializeContextCalls +
          ' nodeCreateEnvironmentSlots=' + hookStats.nodeCreateEnvironmentSlots +
          ' nodeCreateEnvironmentCalls=' + hookStats.nodeCreateEnvironmentCalls +
          ' nodeCreateEnvironmentNulls=' + hookStats.nodeCreateEnvironmentNulls +
          ' lastNodeCreateEnvironmentResult=' + hookStats.lastNodeCreateEnvironmentResult +
          ' nodeLoadEnvironmentStringCalls=' + hookStats.nodeLoadEnvironmentStringCalls +
          ' nodeLoadEnvironmentCallbackCalls=' + hookStats.nodeLoadEnvironmentCallbackCalls +
          ' nodeLoadEnvironmentNulls=' + hookStats.nodeLoadEnvironmentNulls +
          ' nodePostLoadTraceAttempts=' + hookStats.nodePostLoadTraceAttempts +
          ' nodePostLoadTraceSuccesses=' + hookStats.nodePostLoadTraceSuccesses +
          ' nodePostLoadTraceFailures=' + hookStats.nodePostLoadTraceFailures +
          ' lastNodeLoadEnvironmentPreload=' + hookStats.lastNodeLoadEnvironmentPreload +
          ' lastNodeLoadEnvironmentResult=' + hookStats.lastNodeLoadEnvironmentResult +
          ' nodePlatformForIsolateCalls=' + hookStats.nodePlatformForIsolateCalls +
          ' nodePlatformLookupHits=' + hookStats.nodePlatformLookupHits +
          ' nodePlatformLookupMisses=' + hookStats.nodePlatformLookupMisses +
          ' nodePlatformLookupFakeFallbacks=' + hookStats.nodePlatformLookupFakeFallbacks +
          ' snapshotBlobClears=' + hookStats.snapshotBlobClears +
          ' lastSnapshotBlobAddress=' + hookStats.lastSnapshotBlobAddress +
          ' snapshotDecompressCalls=' + hookStats.snapshotDecompressCalls +
          ' lastSnapshotCompressedSize=' + hookStats.lastSnapshotCompressedSize +
          ' lastSnapshotDecompressedSize=' + hookStats.lastSnapshotDecompressedSize +
          ' snapshotMmapFallbacks=' + hookStats.snapshotMmapFallbacks +
          ' adapterChildProcessCalls=' + hookStats.adapterChildProcessCalls +
          ' adapterChildProcessPassThrough=' + hookStats.adapterChildProcessPassThrough +
          ' adapterCrashpadBlocks=' + hookStats.adapterCrashpadBlocks);
      }, i * 1000);
    }
  }

  private openInitialWindowWithRetry(attemptIndex: number): void {
    const delays: number[] = [0, 750, 2000, 5000, 8000];
    if (attemptIndex >= delays.length) {
      return;
    }
    setTimeout(() => {
      this.openInitialWindow('async' + attemptIndex);
      this.openInitialWindowWithRetry(attemptIndex + 1);
    }, delays[attemptIndex]);
  }

  private openInitialWindow(attempt: string): void {
    try {
      this.writeStartupStatus('dispatching initial window ' + attempt + ' uri=' +
        (this.config.uri.length > 0 ? this.config.uri : '<empty>'));
      this.config.nativeContext.ExecuteCommand(CommandType.kNewWindow, {
        url: this.config.uri,
        is_sync: false
      });
      this.writeStartupStatus('initial kNewWindow ' + attempt + ' dispatched async');
    } catch (err) {
      this.writeStartupStatus('initial kNewWindow ' + attempt + ' threw ' + JSON.stringify(err));
    }
  }

  private setDefaultBounds() {
    if (this.config.xComponentId !== ConfigData.DEFAULT_WINDOW_ID)
      return;
    let prop = this.config.window?.getWindowProperties();
    if (prop) {
      let windowRect: WindowBound = {
        left: prop.windowRect.left,
        top: prop.windowRect.top,
        width: prop.windowRect.width,
        height: prop.windowRect.height
      }

      let drawableRect: WindowBound = {
        left: prop.drawableRect.left,
        top: prop.drawableRect.top,
        width: prop.drawableRect.width,
        height: prop.drawableRect.height
      }

      this.config.nativeContext.OnWindowInitSize(windowRect, drawableRect);
    }

    let state = this.config.window?.getWindowStatus();
    if (state) {
      this.config.nativeContext.OnWindowInitState(state);
    }
  }

  private buildArgs(): string[] {
    let resDir = '--bundle-installation-dir=' + getContext().resourceDir;
    let vec_args = [resDir, ELECTRON_APP_PATH];
    const jsFlags = '--js-flags=--max-old-space-size=512 --max-semi-space-size=16';
    let hasJsFlags: boolean = false;
    // 过滤掉残留的 --folder-uri，确保"是否连远程"只由标记文件决定（防上次启动参数残留）。
    this.config.electronRelaunchArgs.forEach((arg: string) => {
      if (!this.shouldKeepRelaunchArg(arg)) {
        LogUtil.warn(TAG, 'filtered stale relaunch arg: ' + arg);
        return;
      }
      if (arg.indexOf('--js-flags') === 0) {
        hasJsFlags = true;
      }
      if (arg.indexOf('--folder-uri') !== 0 && arg.indexOf('--file-uri') !== 0) {
        vec_args.push(arg);
      }
    });
    if (!hasJsFlags) {
      vec_args.push(jsFlags);
    }
    // [OHcode Remote] 启动时读持久标记决定是否连远程：连接写标记 + 重启 → 注入 --folder-uri；
    // 断开删标记 + 重启 → 读不到 → 本地。
    const REMOTE_MARKER: string = '/data/storage/el2/base/files/ohcode-remote-target.txt';
    let hasMarker: boolean = false;
    let folderUri: string = '';
    try {
      if (fileIo.accessSync(REMOTE_MARKER)) {
        hasMarker = true;
        folderUri = fileIo.readTextSync(REMOTE_MARKER).trim();
        // 一次性消费：读完即删。断开无需在远程窗口删本地文件——重启时标记已被消费 → 自动回本地。
        fileIo.unlinkSync(REMOTE_MARKER);
        if (folderUri.length > 0) {
          vec_args.push('--folder-uri=' + folderUri);
        }
      }
    } catch (err) {
      LogUtil.error(TAG, 'read remote marker failed: ' + JSON.stringify(err));
    }
    LogUtil.info(TAG, `buildArgs remote: marker=${hasMarker} uri=${folderUri} relaunch=${JSON.stringify(this.config.electronRelaunchArgs)}`);

    // [OHcode 打开方式] 文件管理器"用 OHcode 打开"文件/文件夹：EntryAbility 写标记 → 注入 --file-uri / --folder-uri（一次性消费）。
    const OPEN_MARKER: string = '/data/storage/el2/base/files/ohcode-open-target.txt';
    try {
      if (fileIo.accessSync(OPEN_MARKER)) {
        const raw: string = fileIo.readTextSync(OPEN_MARKER).trim();
        fileIo.unlinkSync(OPEN_MARKER);
        if (raw.length > 0) {
          const sep: number = raw.indexOf('|');
          const kind: string = sep > 0 ? raw.substring(0, sep) : 'file';
          const openUri: string = sep > 0 ? raw.substring(sep + 1) : raw;
          if (openUri.length > 0) {
            vec_args.push((kind === 'folder' ? '--folder-uri=' : '--file-uri=') + openUri);
            LogUtil.info(TAG, `buildArgs open: kind=${kind} uri=${openUri}`);
          }
        }
      }
    } catch (err) {
      LogUtil.error(TAG, 'read open marker failed: ' + JSON.stringify(err));
    }
    this.appendMissingSwitches(vec_args, CHROMIUM_PROCESS_FLAGS);
    return vec_args;
  }

  private appendMissingSwitches(vec_args: string[], switches: string[]): void {
    switches.forEach((switchName: string) => {
      if (!this.hasSwitch(vec_args, switchName)) {
        vec_args.push(switchName);
      }
    });
  }

  private shouldKeepRelaunchArg(arg: string): boolean {
    for (let i = 0; i < BLOCKED_RELAUNCH_SWITCH_PREFIXES.length; i++) {
      const prefix = BLOCKED_RELAUNCH_SWITCH_PREFIXES[i];
      if (arg === prefix || arg.indexOf(prefix + '=') === 0) {
        return false;
      }
    }
    return true;
  }

  private hasSwitch(vec_args: string[], switchName: string): boolean {
    for (let i = 0; i < vec_args.length; i++) {
      const arg = vec_args[i];
      if (arg === switchName || arg.indexOf(switchName + '=') === 0) {
        return true;
      }
    }
    return false;
  }
}

@Component
export struct WebWindow {
  private static TAG: string = 'WebWindow';
  @LocalStorageLink('xcomponentId') xComponentId: string = '';
  @LocalStorageLink('startUri') startUri: string = '';
  @LocalStorageLink('window') window: window.Window | undefined = undefined;
  @LocalStorageLink('electronRelaunchArgs') electronRelaunchArgs: string[] = [];
  private XCOMPONENT_NAME: string = 'xcomponent';
  private MODULE_NAME: string = this.xComponentId + ':' + this.XCOMPONENT_NAME;
  private dragDropAdapter: DragDropAdapter = Inject.get(DragDropAdapter);
  private multiInputAdapter: MultiInputAdapter = Inject.get(MultiInputAdapter);
  private abilityManager = Inject.get(AbilityManager);
  private dragParamManager = Inject.get(DragParamManager);
  private nativeContext: NativeContext = JsBindingUtils.getNativeContext(ContextType.kMainProcess);

  @State private messageBoxParams: IMessageBoxParams = {
    messageBoxSettings: undefined,
    icon: undefined,
    callback: () => {}
  };
  private windowNodeController: WindowNodeController = new WindowNodeController({
    moduleName: this.MODULE_NAME,
    xComponentId: this.xComponentId,
    uri: this.startUri,
    nativeContext: this.nativeContext,
    window: this.window,
    electronRelaunchArgs: this.electronRelaunchArgs
  });

  private dialogController: CustomDialogController = new CustomDialogController({
    builder: MessageBox({
      title: this.messageBoxParams.messageBoxSettings?.title,
      message: this.messageBoxParams.messageBoxSettings?.message,
      detail: this.messageBoxParams.messageBoxSettings?.detail,
      buttons: this.messageBoxParams.messageBoxSettings?.buttons,
      checkboxLabel: this.messageBoxParams.messageBoxSettings?.checkbox_label,
      checkboxChecked: this.messageBoxParams.messageBoxSettings?.checkbox_checked,
      icon: this.messageBoxParams?.icon,
      callback: this.messageBoxParams.callback
    }),
    cancel: () => {},
    autoCancel: false,
    alignment: DialogAlignment.Center,
    customStyle: false,
  })

  setMessageBoxParams() {
    const that = this;
    return (messageBoxParams: IMessageBoxParams) => {
      that.messageBoxParams = messageBoxParams;
    }
  }

  private effectiveXComponentId(): string {
    return this.xComponentId.length > 0 ? this.xComponentId : ConfigData.DEFAULT_WINDOW_ID;
  }

  aboutToAppear(): void {
    const effectiveId = this.effectiveXComponentId();
    this.dragParamManager.addDragContextData({
      id: effectiveId,
      uiContext: this.getUIContext(),
    });
    this.abilityManager.addMessageBox({
      id: effectiveId,
      setMessageBoxParams: this.setMessageBoxParams(),
      dialogController: this.dialogController
    });
  }

  aboutToDisappear(): void {
    this.dragParamManager.removeDragContextData(this.effectiveXComponentId());
  }

  build() {
    NodeContainer(this.windowNodeController)
      .gesture(
        PanGesture({ direction: PanDirection.Vertical | PanDirection.Horizontal, distance: 1 })
          .onActionStart((event: GestureEvent) => {
            this.multiInputAdapter.OnPanEvent(PanAction.kStart, this.xComponentId, event);
          })
          .onActionUpdate((event: GestureEvent) => {
            this.multiInputAdapter.OnPanEvent(PanAction.kUpdate, this.xComponentId, event);
          })
          .onActionEnd((event: GestureEvent) => {
            this.multiInputAdapter.OnPanEvent(PanAction.kEnd, this.xComponentId, event);
          })
          .onActionCancel(() => {
            this.multiInputAdapter.OnPanEvent(PanAction.kCancel, this.xComponentId, undefined);
          })
      )
      .gesture(
        PinchGesture()
          .onActionStart((event: GestureEvent) => {
            this.multiInputAdapter.OnPinchEvent("start", this.xComponentId, event);
          })
          .onActionUpdate((event: GestureEvent) => {
            this.multiInputAdapter.OnPinchEvent("update", this.xComponentId, event);
          })
          .onActionEnd((event: GestureEvent) => {
            this.multiInputAdapter.OnPinchEvent("end", this.xComponentId, event);
          })
      )
      .gesture(
        GestureGroup(GestureMode.Exclusive,
          TapGesture({ count: 2})
            .onAction((event: GestureEvent) => {
              if (event) {
                let offsetX = vp2px(event.fingerList[0].globalX);
                let offsetY = vp2px(event.fingerList[0].globalY);
                this.nativeContext.OnDoubleTapCB(this.xComponentId, offsetX, offsetY);
              }
            })
        )
      )
      .onDragEnter((event?: DragEvent) => {
        LogUtil.info(WebWindow.TAG, 'OhosDrag WebWindow onDragEnter xComponentId:' + this.xComponentId);
        this.dragDropAdapter.dragEnterData(this.xComponentId, (event as DragEvent).getSummary());
      })
      .onDragMove((event?: DragEvent) => {
        let windowX = event?.getWindowX();
        let windowY = event?.getWindowY();
        windowX = vp2px(windowX);
        windowY = vp2px(windowY);
        this.dragDropAdapter.dragMove(this.xComponentId, windowX, windowY);
      })
      .onDragLeave(() => {
        LogUtil.info(WebWindow.TAG, 'OhosDrag WebWindow onDragLeave xComponentId:' + this.xComponentId);
        this.dragDropAdapter.dragLeave(this.xComponentId);
      })
      .onDrop((event?: DragEvent) => {
        if (!event) {
          LogUtil.error(WebWindow.TAG, 'OhosDrag WebWindow DragDrop onDrop event is null');
          return;
        }
        LogUtil.info(WebWindow.TAG, 'OhosDrag WebWindow onDrop xComponentId:' + this.xComponentId);
        let dragData: UnifiedData = (event as DragEvent).getData() as UnifiedData;
        this.dragDropAdapter.dropData(this.xComponentId, dragData);
      })
      .id(this.MODULE_NAME)
  }
}
