// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { NativeContext, WindowBound } from '../interface/CommonInterface';
import lazy { DragDropAdapter } from '../adapter/DragDropAdapter';
import LogUtil from '../utils/LogUtil';
import { CommandType, ConfigData, ContextType, PanAction } from '../common/Constants';
import Inject from '../common/InjectModule';
import JsBindingUtils from '../utils/JsBindingUtils';
import { getV8PoolHook } from '../common/V8PoolHook';
import { MultiInputAdapter } from '../adapter/MultiInputAdapter';
import { DragParamManager } from '../common/DragParamManager';
import { UIExtensionContentSession } from '@kit.AbilityKit';

const STARTUP_BUILD_STAMP: string = 'diag-20260626-appsearch10';
const ELECTRON_APP_PATH: string = '--app-path=/data/storage/el1/bundle/electron/resources/resfile/resources/app';
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

@Component
export struct WebEmbeddedWindow {
  private static TAG: string = 'WebEmbeddedWindow';
  @LocalStorageLink('xcomponentId') xComponentId: string = '';
  @LocalStorageLink('startUri') startUri: string = '';
  @LocalStorageLink('session') session: UIExtensionContentSession | undefined = undefined;
  @LocalStorageLink('userData') userData: string = '';
  private LIBRARY_NAME: string = 'adapter';
  private XCOMPONENT_NAME: string = 'xcomponent';
  private MODULE_NAME: string = this.xComponentId + ':' + this.XCOMPONENT_NAME;
  private dragDropAdapter: DragDropAdapter = Inject.get(DragDropAdapter);
  private nativeContext: NativeContext = JsBindingUtils.getNativeContext(ContextType.kMainProcess);
  private multiInputAdapter: MultiInputAdapter = Inject.get(MultiInputAdapter);
  private dragParamManager = Inject.get(DragParamManager);

  aboutToAppear(): void {
    this.dragParamManager.addDragContextData({
      id: this.xComponentId,
      uiContext: this.getUIContext(),
    });
  }

  aboutToDisappear(): void {
    this.dragParamManager.removeDragContextData(this.xComponentId);
  }

  build() {
    XComponent({
      id: this.MODULE_NAME,
      type: XComponentType.SURFACE,
      libraryname: this.LIBRARY_NAME
    })
      .focusable(true)
      .defaultFocus(true)
      .backgroundColor(Color.White)
      .gesture(
        PanGesture({ direction: PanDirection.Vertical | PanDirection.Horizontal })
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
      .onDragEnter((event?: DragEvent) => {
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
        this.dragDropAdapter.dragLeave(this.xComponentId);
      })
      .onDrop((event?: DragEvent) => {
        if (!event) {
          LogUtil.error(WebEmbeddedWindow.TAG, 'OhosDrag WebWindow DragDrop onDrop event is null');
          return;
        }
        let dragData: UnifiedData = (event as DragEvent).getData() as UnifiedData;
        this.dragDropAdapter.dropData(this.xComponentId, dragData);
      })
      .onLoad(() => {
        LogUtil.info(WebEmbeddedWindow.TAG, 'onLoad xComponentId:' + this.xComponentId);
        if (this.xComponentId === ConfigData.DEFAULT_WINDOW_ID) {
          this.setDefaultBounds();
          let vec_args = this.buildArgs();
          LogUtil.info(WebEmbeddedWindow.TAG, 'node environment sanitized=' + JSON.stringify(getV8PoolHook().sanitizeNodeEnvironment()));
          LogUtil.info(WebEmbeddedWindow.TAG, 'electron PLT hook=' + JSON.stringify(getV8PoolHook().installElectronPltHooks()));
          this.nativeContext.runBrowser(vec_args);
          LogUtil.info(WebEmbeddedWindow.TAG, 'electron PLT hook after runBrowser=' + JSON.stringify(getV8PoolHook().getElectronPltHookStats()));
          setTimeout(() => {
            LogUtil.info(WebEmbeddedWindow.TAG, 'electron PLT hook delayed=' + JSON.stringify(getV8PoolHook().getElectronPltHookStats()));
          }, 500);
        } else {
          this.nativeContext.ExecuteCommand(
            CommandType.kNewWidget,
            {
              url: this.startUri,
              user_data: this.userData,
            });
        }
      })
      .id(this.XCOMPONENT_NAME)
      .renderFit(RenderFit.TOP_LEFT)
  }

  private setDefaultBounds() {
    if (this.xComponentId !== ConfigData.DEFAULT_WINDOW_ID) {
      return;
    }

    let windowProxy = this.session?.getUIExtensionWindowProxy();
    let prop = windowProxy?.properties;
    if (prop) {
      let windowRect: WindowBound = {
        left: prop.uiExtensionHostWindowProxyRect.left,
        top: prop.uiExtensionHostWindowProxyRect.top,
        width: prop.uiExtensionHostWindowProxyRect.width,
        height: prop.uiExtensionHostWindowProxyRect.height
      }

      let drawableRect: WindowBound = {
        left: 0,
        top: 0,
        width: prop.uiExtensionHostWindowProxyRect.width,
        height: prop.uiExtensionHostWindowProxyRect.height
      }

      this.nativeContext.OnWindowInitSize(windowRect, drawableRect);
    }
  }

  private buildArgs(): string[] {
    const override_ua = '--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) ' +
      'AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36';
    let default_uri = '';
    if (this.startUri.length !== 0) {
      default_uri = this.startUri;
    }
    let resDir = '--bundle-installation-dir=' + getContext().resourceDir;
    const jsFlags = '--js-flags=--max-old-space-size=512 --max-semi-space-size=16';
    let vec_args = [override_ua, resDir, ELECTRON_APP_PATH, jsFlags, default_uri];
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
