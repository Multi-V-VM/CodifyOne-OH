// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { inject, injectable } from 'inversify';
import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import bundleManager from '@ohos.bundle.bundleManager';
import { BusinessError } from '@ohos.base';
import common from '@ohos.app.ability.common';
import { contextConstant, StartOptions } from '@kit.AbilityKit';
import { display, window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { image } from '@kit.ImageKit';
import Want from '@ohos.app.ability.Want';

import { AbilityManager } from '../common/AbilityManager';
import { AbilityPool, PooledAbility } from '../common/AbilityPool';
import { BaseAdapter } from '../common/BaseAdapter';
import { ContextAdapter } from './ContextAdapter';
import { IStyleData, IUpdateStyle, WindowStyle } from '../common/WindowStyle';
import LaunchHelper from '../utils/LaunchHelper';
import LogMethod from '../common/LogDecorator';
import LogUtil from '../utils/LogUtil';
import Inject from '../common/InjectModule';
import { NewWindowParam, PointCoordinate, WindowType, WindowBound, WindowPreferences, ILoginInfo, ILogin } from "../interface/CommonInterface"
import promptAction from '@ohos.promptAction';
import { WebProxy } from '../interface/WebProxy';

const TAG: string = 'AppWindow';
const PHONE_UNSUPPORTED_WINDOW_CAPABILITIES: string[] = [
  'setWindowTitleMoveEnabled',
  'setWindowTitleButtonVisible',
  'setWindowTopmost',
  'setWindowModal'
];

/**
 * Window limit configuration to prevent V8 OOM crashes
 * Each window creates a new V8 isolate (~50-100MB), so we must limit concurrent windows
 *
 * NOTE: Set to 1 due to memory constraints on some devices.
 * Devices with limited free memory (~1-2GB) can crash even with 2 windows.
 * Increase only if you have confirmed sufficient memory available.
 */
const MAX_CONCURRENT_WINDOWS: number = 1;
const WINDOW_LIMIT_WARNING_MS: number = 3000; // Show warning for 3 seconds

@injectable()
export class AppWindowAdapter extends BaseAdapter {
  private abilityManager: AbilityManager;
  private abilityPool: AbilityPool | undefined = undefined;
  private bundleName: string = 'com.huawei.ohos_electron';
  private bundleFlags = bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION |
                        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_METADATA;
  private queryWindowNum: number = 10;
  private ctxAdapter: ContextAdapter;
  private defaultBackgroundColor: string = '#00000000';
  private loginWindow: window.Window | null = null;

  // Window limit tracking
  private activeWindowCount: number = 0;
  private windowIdSet: Set<string> = new Set<string>();

  constructor(
    @inject(AbilityManager) abilityManager: AbilityManager,
    @inject(ContextAdapter) ctxAdapter: ContextAdapter
  ) {
    super();
    this.abilityManager = abilityManager;
    this.ctxAdapter = ctxAdapter;

    try {
      let data = bundleManager.getBundleInfoForSelfSync(this.bundleFlags);
      this.bundleName = data.name;
    } catch (err) {
      let message = (err as BusinessError).message;
      LogUtil.error(TAG, `getBundleInfoForSelfSync failed.: ${message}`);
    }
  }

  /**
   * Lazily gets the AbilityPool instance
   * Uses Inject.get() pattern to avoid DI binding conflicts
   */
  private getPool(): AbilityPool {
    if (!this.abilityPool) {
      this.abilityPool = Inject.get(AbilityPool);
    }
    return this.abilityPool;
  }

  private getWindow(id: number): window.Window | undefined {
    return this.abilityManager.getProxy(id)?.getWindow();
  }

  private applyWindowCapability(name: string, action: () => void): void {
    if (this.isPhoneUnsupportedWindowCapability(name)) {
      LogUtil.info(TAG, `Window capability ${name} skipped on phone`);
      return;
    }
    try {
      action();
    } catch (exception) {
      LogUtil.warn(TAG, `Window capability ${name} is unavailable: ${JSON.stringify(exception)}`);
    }
  }

  private isPhoneUnsupportedWindowCapability(name: string): boolean {
    if (deviceInfo.deviceType !== 'phone') {
      return false;
    }
    return PHONE_UNSUPPORTED_WINDOW_CAPABILITIES.indexOf(name) >= 0;
  }

  private setTitleButtonVisible(windowClass: window.Window | undefined, preferences: WindowPreferences): void {
    this.applyWindowCapability('setWindowTitleButtonVisible', () => {
      windowClass?.setWindowTitleButtonVisible(preferences.maximizable, preferences.minimizable, preferences.closable);
    });
  }

  @LogMethod
  createWindow(param: NewWindowParam) {
    let abilityName: string = "EntryAbility";
    if (param.is_stateless) {
      abilityName = "StatelessAbility";
    }

    // Check window limit before creating new window
    if (!this.checkWindowLimit(param.window_id, true)) {
      this.showWindowLimitWarning();
      return;
    }

    // Check pool first for reusable ability
    let pooled = this.getPool().acquire(abilityName, param);
    if (pooled) {
      this.reusePooledAbility(pooled, param);
      // Track the reused window
      this.trackWindow(param.window_id);
      return;
    }

    // No pooled ability available, create new one
    const want: Want = {
      bundleName: this.bundleName,
      abilityName: abilityName,
      parameters: {
        'xcomponentId': param.window_id,
        'windowType': WindowType.MAIN_WINDOW,
        instanceKey: param.window_id,
        'hideTitleBar': param.hide_title_bar,
        'useDarkMode': param.use_dark_mode,
        'minimizable': param.minimizable,
        'maximizable': param.maximizable,
        'closable': param.closable,
        'alwaysOntop': param.always_on_top,
        'resizable': param.resizable,
        'isModal': param.is_modal,
        'isPanel': param.is_panel,
      }
    };

    let windowClass = this.ctxAdapter.getActiveProxy().getWindow();
    const windowProp = windowClass?.getWindowProperties();
    let leftBorder: number = param.hide_title_bar ? 0 : windowProp?.drawableRect.left || 0;
    let topBorder: number = param.hide_title_bar ? 0 : windowProp?.drawableRect.top || 0;
    let imagePixelMap: image.PixelMap;
    let color = new ArrayBuffer(0);

    image.createPixelMap(color, {
      size: {
        height: param.bounds.height + leftBorder + topBorder,
        width: param.bounds.width + leftBorder + leftBorder,
      }
    }).then((data) => {
      imagePixelMap = data;
      let options: StartOptions = {
        processMode: contextConstant.ProcessMode.ATTACH_TO_STATUS_BAR_ITEM,
        startupVisibility: param.show ? contextConstant.StartupVisibility.STARTUP_SHOW : contextConstant.StartupVisibility.STARTUP_HIDE,
        windowLeft: param.bounds.left - leftBorder,
        windowTop: param.bounds.top - topBorder,
        windowWidth: param.bounds.width + leftBorder + leftBorder,
        windowHeight: param.bounds.height + leftBorder + topBorder,
        startWindowIcon: imagePixelMap,
        startWindowBackgroundColor: this.defaultBackgroundColor
      }
      if (param.is_panel) {
        options.windowMode = AbilityConstant.WindowMode.WINDOW_MODE_FULLSCREEN;
      }
      if (param.display_id != -1) {
        options.displayId = param.display_id;
      }
      const launchSuccess = LaunchHelper.LaunchWithOptions(this.ctxAdapter.getActiveContext(), want, options);
      if (launchSuccess) {
        // Track the newly created window
        this.trackWindow(param.window_id);
        LogUtil.info(TAG, `Successfully launched window: ${param.window_id}`);
      } else {
        LogUtil.error(TAG, 'Failed to launch window');
        // Untrack since launch failed
        this.untrackWindow(param.window_id);
      }
      // Always release PixelMap to prevent native memory leaks
      imagePixelMap?.release?.();
    }).catch((err: BusinessError) => {
      LogUtil.error(TAG, `Failed to create PixelMap. Cause: ${JSON.stringify(err)}`);
      // Untrack since creation failed
      this.untrackWindow(param.window_id);
      // Still release PixelMap on error
      imagePixelMap?.release?.();
    })
  }

  /**
   * Reuses a pooled ability for a new window
   * Shows the hidden ability and notifies native context to reuse WebContents
   */
  private reusePooledAbility(pooled: PooledAbility, param: NewWindowParam): void {
    let context = pooled.context as common.UIAbilityContext;

    // Pass reuse information through Want parameters
    const want: Want = {
      bundleName: this.bundleName,
      abilityName: pooled.abilityType,
      parameters: {
        'xcomponentId': param.window_id,
        'windowType': WindowType.MAIN_WINDOW,
        'reuseWindow': 'true',  // Signal to reuse instead of create new
        'originWindowId': pooled.windowId,  // Original window ID for WebContents lookup
        'instanceKey': param.window_id,
      }
    };

    // Show the hidden ability with reuse parameters
    context?.startAbility(want).then(() => {
      LogUtil.info(TAG, `Reusing pooled ability: ${param.window_id} (type: ${pooled.abilityType})`);
      // Tracking is already done before this call
    }).catch((err: BusinessError) => {
      LogUtil.error(TAG, `Failed to show pooled ability. Cause: ${JSON.stringify(err)}`);
      // Untrack since reuse failed
      this.untrackWindow(param.window_id);
      // If showing fails, create new window instead
      this.createWindow(param);
    });
  }

  @LogMethod
  closeWindow(id: number) {
    let windowId = this.paramNormalization(id);

    // Check if ability should be pooled
    if (this.shouldPoolAbility(id)) {
      let proxy = this.abilityManager.getProxy(id);
      let context = proxy?.getWindowContext() as common.UIAbilityContext;

      if (context) {
        // Hide and return to pool instead of destroying
        context.hideAbility().then(() => {
          LogUtil.info(TAG, `Hiding ability for pooling: ${windowId}`);
          this.getPool().release(windowId);
          // Note: We keep tracking pooled windows since they can be reshown
        }).catch((err: BusinessError) => {
          LogUtil.error(TAG, `Failed to hide ability. Cause: ${JSON.stringify(err)}`);
          // If hide fails, fall through to terminate
          this.untrackWindow(windowId);
          this.destroyAbility(id);
        });
        return;
      }
    }

    // Not pooling or pooling failed - destroy the ability
    this.untrackWindow(windowId);
    this.destroyAbility(id);
  }

  /**
   * Determines if an ability should be pooled instead of destroyed
   */
  private shouldPoolAbility(id: number): boolean {
    let proxy = this.abilityManager.getProxy(id);
    if (!proxy) {
      return false;
    }

    // Get ability type from window ID or preferences
    // For now, pool stateless abilities always, EntryAbility only if under limit
    let preferences = proxy.getWindowPreferences();
    let poolStats = this.getPool().getPoolStats();
    let idleCount = poolStats['StatelessAbility'];

    // Pool stateless abilities more aggressively
    // Pool EntryAbility only if under the limit (max 5 by default)
    return idleCount < 5;
  }

  /**
   * Destroys an ability and its resources
   */
  private destroyAbility(id: number): void {
    // First remove from manager to prevent dangling references and ensure cleanup
    let proxy = this.abilityManager.getProxy(id);
    let context = proxy?.getWindowContext() as common.UIAbilityContext;

    // Explicitly remove proxy before termination to ensure proper cleanup
    this.abilityManager.removeProxy(id);

    context?.terminateSelf()
      .catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to close window. Cause: ' + JSON.stringify(err));
        // Re-add proxy if termination failed so it can be retried
        if (proxy) {
          this.abilityManager.addProxy(proxy);
        }
      });
  }

  /**
   * Normalizes numeric ID to string format (browser{id})
   */
  private paramNormalization(id: string | number): string {
    if (typeof id === 'number') {
      return `browser${id}`;
    }
    return id;
  }

  @LogMethod
  setMaximizable(id: number, maximizable: boolean) {
    let proxy = this.abilityManager.getProxy(id);
    let preferences: WindowPreferences | undefined = proxy?.getWindowPreferences();
    if (preferences) {
      preferences.maximizable = maximizable;
      proxy?.setWindowPreferences(preferences);
      if (!preferences.hideTitleBar)
        this.setTitleButtonVisible(proxy?.getWindow(), preferences);
    }
    let windowStage = proxy?.getWindowStage();
    this.applyWindowCapability('setSupportedWindowModes', () => {
      if (!maximizable) {
        windowStage?.setSupportedWindowModes([bundleManager.SupportWindowMode.FLOATING]);
      } else {
        windowStage?.setSupportedWindowModes([bundleManager.SupportWindowMode.FLOATING, bundleManager.SupportWindowMode.SPLIT, bundleManager.SupportWindowMode.FULL_SCREEN]);
      }
    });
  }

  @LogMethod
  setMinimizable(id: number, minimizable: boolean) {
    let proxy = this.abilityManager.getProxy(id);
    let preferences: WindowPreferences | undefined = proxy?.getWindowPreferences();
    if (preferences) {
      preferences.minimizable = minimizable;
      proxy?.setWindowPreferences(preferences);
      if (!preferences.hideTitleBar)
        this.setTitleButtonVisible(proxy?.getWindow(), preferences);
    }
  }

  @LogMethod
  setClosable(id: number, closable: boolean) {
    let proxy = this.abilityManager.getProxy(id);
    let preferences: WindowPreferences | undefined = proxy?.getWindowPreferences();
    if (preferences) {
      preferences.closable = closable;
      proxy?.setWindowPreferences(preferences);
      if (!preferences.hideTitleBar)
        this.setTitleButtonVisible(proxy?.getWindow(), preferences);
    }
  }

  @LogMethod
  setWindowButtonVisibility(id: number, visible: boolean) {
    let proxy = this.abilityManager.getProxy(id);
    let preferences: WindowPreferences | undefined = proxy?.getWindowPreferences();
    if (preferences) {
      preferences.maximizable = visible;
      preferences.minimizable = visible;
      preferences.closable = visible;
      proxy?.setWindowPreferences(preferences);
      this.setTitleButtonVisible(proxy?.getWindow(), preferences);
    }
  }

  @LogMethod
  getWindowButtonVisibility(id: number): boolean {
    let proxy = this.abilityManager.getProxy(id);
    let preferences: WindowPreferences | undefined = proxy?.getWindowPreferences();
    if (preferences) {
      return preferences.maximizable || preferences.minimizable || preferences.closable;
    }
    return false;
  }

  @LogMethod
  setAlwaysOnTop(id: number, onTop: boolean) {
    let windowClass = this.getWindow(id);
    this.applyWindowCapability('setWindowTopmost', () => {
      let promise = windowClass?.setWindowTopmost(onTop);
      promise?.then(() => {
        LogUtil.info(TAG, `Succeeded in setting the window to be topmost.`);
      }).catch((err: BusinessError) => {
        LogUtil.error(TAG, `Failed to set the window to be topmost. Cause code: ${err.code}, message: ${err.message}`);
      });
    });
  }

  @LogMethod
  showWindow(id: number) {
    if (deviceInfo.deviceType === 'phone') {
      LogUtil.info(TAG, 'showAbility skipped on phone; foreground ability is already system-managed');
      return;
    }

    let context = this.abilityManager.getProxy(id)?.getWindowContext() as common.UIAbilityContext;
    context?.showAbility().then(() => {
      LogUtil.info(TAG, `showAbility success`);
    }).catch((err: BusinessError) => {
      LogUtil.error(TAG, `showAbility fail, err: ${JSON.stringify(err)}`);
    });
  }

  @LogMethod
  hideWindow(id: number) {
    if (deviceInfo.deviceType === 'phone') {
      LogUtil.info(TAG, 'hideAbility skipped on phone; foreground ability is system-managed');
      return;
    }

    let context = this.abilityManager.getProxy(id)?.getWindowContext() as common.UIAbilityContext;
    context?.hideAbility().then(() => {
      LogUtil.info(TAG, `hideAbility success`);
    }).catch((err: BusinessError) => {
      LogUtil.error(TAG, `hideAbility fail, err: ${JSON.stringify(err)}`);
    });
  }

  @LogMethod
  activateWindow(id: number) {
    this.showWindow(id);
  }

  @LogMethod
  setFullScreen(fullScreen: boolean, id: number) {
    if (fullScreen) {
      let windowClass = this.getWindow(id);
      if (!windowClass) {
        LogUtil.error(TAG, 'setFullScreen failed to obtain the main window.');
        return;
      }

      windowClass.maximize(window.MaximizePresentation.ENTER_IMMERSIVE)
      .catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to maximize the window. Cause:' + JSON.stringify(err));
      });
    } else {
      this.unMaximize(id);
    }
  }

  @LogMethod
  setSimpleFullScreen(fullScreen: boolean, id: number) {
    if (fullScreen) {
      let windowClass = this.getWindow(id);
      if (!windowClass) {
        LogUtil.error(TAG, 'setSimpleFullScreen failed to obtain the main window.');
        return;
      }

      windowClass.maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER)
        .then(() => {
          LogUtil.info(TAG, 'Succeeded in maximizing the window.');
        }).catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to maximize the window. Cause:' + JSON.stringify(err));
      });
    } else {
      this.unMaximize(id);
    }
  }

  @LogMethod
  restore(id: number) {
    let windowClass = this.getWindow(id);
    if (!windowClass) {
      LogUtil.error(TAG, 'restore failed to obtain the main window.');
      return;
    }

    windowClass.restore().then(() => {
      LogUtil.info(TAG, 'Succeeded in restoring the window.');
    }).catch((err: BusinessError) => {
      LogUtil.error(TAG, `Failed to restore the window. Cause code: ${err.code}, message: ${err.message}`);
    })
  }

  @LogMethod
  setBounds(id: number,
            hasFrame: boolean,
            bounds: WindowBound,
            callback: () => void) {
    if (deviceInfo.deviceType === 'phone') {
      LogUtil.info(TAG, 'setBounds skipped on phone; top-level window bounds are system-managed');
      callback && callback();
      return;
    }

    let windowClass = this.getWindow(id);
    if (!windowClass) {
      LogUtil.error(TAG, 'setBounds failed to obtain the main window.');
      return;
    }

    const windowProp = windowClass?.getWindowProperties();

    // xComponent bounds need to add a border
    let leftBorder: number = hasFrame ? windowProp?.drawableRect.left || 0 : 0;
    let topBorder: number = hasFrame ? windowProp?.drawableRect.top || 0 : 0;
    bounds.left -= leftBorder;
    bounds.top -= topBorder;
    bounds.width = bounds.width + leftBorder + leftBorder;
    bounds.height = bounds.height + topBorder + leftBorder;

    windowClass.moveWindowToAsync(bounds.left, bounds.top)
      .then(() => windowClass?.resizeAsync(bounds.width, bounds.height))
      .then(() => {
        callback && callback();
      })
      .catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to set the window bounds. Cause:' + JSON.stringify(err));
        callback && callback();
      });
  }

  @LogMethod
  setEnabled(enable: boolean, id: number) {
    let windowClass = this.getWindow(id);
    if (!windowClass) {
      LogUtil.error(TAG, 'setEnabled failed to obtain the main window.');
      return;
    }
    windowClass.setWindowTouchable(enable)
      .catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to set the window to be touchable. Cause:' + JSON.stringify(err));
      });
  }

  @LogMethod
  setWindowLimits(minWidth: number,
                  minHeight: number,
                  maxWidth: number,
                  maxHeight: number,
                  id: number) {
    let windowClass = this.getWindow(id);
    if (!windowClass) {
      LogUtil.error(TAG, 'setWindowLimits failed to obtain the main window.');
      return;
    }

    const windowProp = windowClass.getWindowProperties();
    let xComBorder: number = windowProp?.drawableRect.left || 0;
    let xComTop: number = windowProp?.drawableRect.top || 0;

    try {
      let windowLimits: window.WindowLimits = {
        minWidth: minWidth === 0 ? minWidth : minWidth + xComBorder + xComBorder,
        minHeight: minHeight === 0 ? minHeight : minHeight + xComTop + xComBorder,
        maxWidth: maxWidth === 0 ? maxWidth : maxWidth + xComBorder + xComBorder,
        maxHeight: maxHeight === 0 ? maxHeight : maxHeight + xComTop + xComBorder,
      };

      windowClass.setWindowLimits(windowLimits)
        .catch((err: BusinessError) => {
          LogUtil.error(TAG, 'Failed to change the window limits. Cause: ' + JSON.stringify(err));
        });
    } catch (exception) {
      LogUtil.error(TAG, 'change the window limits exception. Cause:' + JSON.stringify(exception));
    }
  }

  @LogMethod
  maximize(id: number) {
    let windowClass = this.getWindow(id);
    if (!windowClass) {
      LogUtil.error(TAG, 'maximize failed to obtain the main window.');
      return;
    }

    if (windowClass.getWindowStatus() === window.WindowStatusType.MAXIMIZE) {
      return;
    }

    try {
      windowClass.maximize(window.MaximizePresentation.EXIT_IMMERSIVE).then(() => {
        LogUtil.info(TAG, 'Succeeded in maximizing the window.');
      }).catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to maximize the window. Cause:' + JSON.stringify(err));
      });
    } catch (exception) {
      LogUtil.error(TAG, 'maximize the window exception. Cause:' + JSON.stringify(exception));
    }
  }

  @LogMethod
  unMaximize(id: number) {
    let windowClass = this.getWindow(id);
    if (!windowClass) {
      LogUtil.error(TAG, 'unMaximize failed to obtain the main window.');
      return;
    }

    try {
      windowClass.recover().then(() => {
        LogUtil.info(TAG, 'Succeeded in setting the system bar to be invisible.');
      }).catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to recover the window. Cause: ' + JSON.stringify(err));
      });
    } catch (exception) {
      LogUtil.error(TAG, 'recover the window exception. Cause:' + JSON.stringify(exception));
    }
  }

  @LogMethod
  minimize(id: number) {
    let windowClass = this.getWindow(id);
    if (!windowClass) {
      LogUtil.error(TAG, 'minimize failed to obtain the main window.');
      return;
    }
    windowClass.minimize()
      .catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to minimize the window. Cause: ' + JSON.stringify(err));
      });
  }

  @LogMethod
  setTitle(id: number, title: string) {
    let proxy = this.abilityManager.getProxy(id);
    proxy?.setWindowTitle(title);
  }

  @LogMethod
  setAspectRatio(id: number, aspectRatio: number) {
    let windowClass = this.getWindow(id);
    try {
      let promise = aspectRatio > 0 ? windowClass?.setAspectRatio(aspectRatio) : windowClass?.resetAspectRatio();
      promise?.then(() => {
        LogUtil.info(TAG, 'Succeeded in setting aspect ratio of window.');
      }).catch((err: BusinessError) => {
        LogUtil.error(TAG, 'Failed to set the aspect ratio of window. Cause:' + JSON.stringify(err));
      });
    } catch (exception) {
      LogUtil.error(TAG, 'Failed to set the aspect ratio of window. Cause: ' + JSON.stringify(exception));
    }
  }

  @LogMethod
  setUseNativeFrame(id: number, useNativeFrame: boolean) {
    let windowClass = this.getWindow(id);
    this.applyWindowCapability('setWindowDecorVisible', () => {
      windowClass?.setWindowDecorVisible(useNativeFrame);
    });
    this.applyWindowCapability('setWindowTitleMoveEnabled', () => {
      windowClass?.setWindowTitleMoveEnabled(useNativeFrame);
    });
  }

  @LogMethod
  setBackgroundColor(id: number, argbColor: number) {
    let windowClass = this.getWindow(id);
    let data = WindowStyle.getStyleData(id);
    if (data !== undefined) {
      data.backgroundColor = `${WindowStyle.decimalColorToHex(argbColor)}`;
      WindowStyle.getUpdateStyleFunc(id)?.(data);
      windowClass?.setWindowBackgroundColor(`#${WindowStyle.decimalColorToHex(argbColor)}`);
      WindowStyle.updateStyleData(id, data);
    }
  }

  @LogMethod
  setOpacity(id: number, opacity: number) {
    let windowClass = this.getWindow(id);
    let data = WindowStyle.getStyleData(id);
    if (data !== undefined) {
      data.opacity = opacity;
      WindowStyle.getUpdateStyleFunc(id)?.(data);
      let lastColor = data.backgroundColor;
      windowClass?.setWindowBackgroundColor(`#${WindowStyle.decimalOpacityToHex(opacity)}${lastColor.slice(2)}`);
      WindowStyle.updateStyleData(id, data);
    }
  }

  @LogMethod
  setContentProtection(id: number, enable: boolean) {
    let windowClass = this.getWindow(id);
    try {
      windowClass?.setWindowPrivacyMode(enable, (err: BusinessError) => {
        const errCode: number = err.code;
        if (errCode) {
          LogUtil.error(TAG, `Failed to set the window to privacy mode. Cause code: ${err.code}, message: ${err.message}`);
          return;
        }
        LogUtil.info(TAG, 'Succeeded in setting the window to privacy mode.');
      });
    } catch (exception) {
      LogUtil.error(TAG, `Failed to set the window to privacy mode. Cause code: ${exception.code}, message: ${exception.message}`);
    }
  }

  @LogMethod
  setMovable(id: number, movable: boolean) {
    let windowClass = this.getWindow(id);
    this.applyWindowCapability('setWindowTitleMoveEnabled', () => {
      windowClass?.setWindowTitleMoveEnabled(movable);
    });
  }

  @LogMethod
  setResizable(id: number, resizable: boolean) {
    let windowClass = this.getWindow(id);
    try {
      windowClass?.setResizeByDragEnabled(resizable).then(() => {}).catch((err: BusinessError) => {
        LogUtil.error(TAG, `Failed to set the window resizable. Cause ${JSON.stringify(err)}`);
      });
    } catch (exception) {
      LogUtil.error(TAG, `Failed to set the window resizable. Cause code: ${exception.code}, message: ${exception.message}`);
    }
  }

  @LogMethod
  startWindowMoving(id: number) {
    let window = this.getWindow(id);
    window?.startMoving().then(() => {
    }).catch((err: BusinessError) => {
      LogUtil.error(TAG, `startWindowMoving fail, error info: ${JSON.stringify(err)}`);
    });
  }

  @LogMethod
  startWindowMovingWithOffset(id: number, offsetX: number, offsetY: number) {
    let window = this.getWindow(id);
    if (deviceInfo.sdkApiVersion >= 15) {
      window?.startMoving(offsetX, offsetY).then(() => {
      }).catch((err: BusinessError) => {
        LogUtil.error(TAG, `startWindowMovingWithOffset fail, error info: ${JSON.stringify(err)}`);
      });
    } else {
      LogUtil.error(TAG, 'startWindowMovingWithOffset fail, sdkApiVersion is less than 15');
    }
  }

  @LogMethod
  getWindowsByCoordinate(coordinate: PointCoordinate,
                         callback: (windowIds: Array<string>) => void) {
    let windowIds = new Array<string>();
    try {
        window.getWindowsByCoordinate(coordinate.displayId, this.queryWindowNum,
                                      coordinate.x, coordinate.y).then((windows) => {
        for (let win of windows) {
          let originWindowId: number = win.getWindowProperties().id
          let windowId = this.abilityManager.getWindowIdByOriginWindow(originWindowId);
          if (windowId) {
            windowIds.push(windowId);
          }
        }
        callback(windowIds);
      }).catch((err: BusinessError) => {
        LogUtil.error(TAG, 'window.getWindowsByCoordinate fail, code:' + JSON.stringify(err.code) +
          ',message:' + JSON.stringify(err.message));
        callback(windowIds);
      });
    } catch (err) {
      LogUtil.error(TAG, 'getWindowsByCoordinate fail, error:' + JSON.stringify(err));
      callback(windowIds);
    }
	}

  @LogMethod
  setWindowModal(modal: boolean, id: number) {
    let windowStage = this.abilityManager.getProxy(id)?.getWindowStage();
    this.applyWindowCapability('setWindowModal', () => {
      windowStage?.setWindowModal(modal);
    });
  }

  @LogMethod
  getWindowStatus(id: number): window.WindowStatusType {
    let windowClass = this.getWindow(id);
    let type = windowClass?.getWindowStatus();
    if (type !== undefined) {
      return type;
    }
    return window.WindowStatusType.UNDEFINED;
  }

  @LogMethod
  getOriginWindowIds(windowIds: Array<number>): Array<number> {
    let originWindowIds = new Array<number>();
    try {
      for (let i = 0; i < windowIds.length; i++) {
        let originWindowId = this.abilityManager.getProxy(windowIds[i])?.getOriginWindowId();
        originWindowIds.push(originWindowId ? originWindowId : -1);
      }
    } catch (err) {
      let message = (err as BusinessError).message;
      LogUtil.error(TAG, `getOriginWindowIds failed.: ${message}`);
    }
    return originWindowIds;
  }

  @LogMethod
  shiftWindowEvent(sourceId: number, targetId: number,
    callback: (result: boolean) => void): void {
    if (deviceInfo.sdkApiVersion >= 15) {
      try {
        let sourceOriginWindowId = this.abilityManager.getProxy(sourceId)?.getOriginWindowId();
        let targetOriginWindowId = this.abilityManager.getProxy(targetId)?.getOriginWindowId();
        window.shiftAppWindowPointerEvent(sourceOriginWindowId, targetOriginWindowId).then(() => {
          LogUtil.info(TAG, 'shiftWindowEvent success.');
          callback(true);
        }).catch((err: BusinessError) => {
          LogUtil.error(TAG, 'Failed to shiftWindowEvent. Cause: ' + JSON.stringify(err));
          callback(false);
        })
      } catch (err) {
        let message = (err as BusinessError).message;
        LogUtil.error(TAG, `shiftWindowEvent failed.: ${message}`);
        callback(false);
      }
    } else {
      LogUtil.error(TAG, 'shiftWindowEvent fail, sdkApiVersion is less than 15');
      callback(false);
    }
  }

  @LogMethod
  showHuaweiQuickLogin(
      id: number,
      show: boolean,
      bounds: WindowBound,
      callback: (loginInfo: ILoginInfo) => void) {
    const loginFailInfo: ILoginInfo = {
      status: false,
      authCode: '',
      unionId: '',
      openId: '',
      idToken: '',
      anonymousPhone: ''
    };

    if (!show && this.loginWindow) {
      this.loginWindow.destroyWindow((err: BusinessError) => {
        let errCode: number = err.code;
        if (errCode) {
          LogUtil.error(TAG, 'Failed to destroy the window. Cause: ' + JSON.stringify(err));
          return;
        }
        LogUtil.info(TAG, 'Succeeded in destroying the window.');

        this.loginWindow = null;
      });
      return;
    };

    if (this.loginWindow) {
      callback(loginFailInfo);
      return;
    }

    let activeContext = this.ctxAdapter.getActiveContext() as common.UIAbilityContext;
    if (!activeContext) {
      LogUtil.error(TAG, 'Failed to get active context');
      return;
    }
    let config: window.Configuration = {
      name: 'login',
      windowType: window.WindowType.TYPE_DIALOG,
      ctx: activeContext
    };
    window.createWindow(config, (err: BusinessError, data) => {
      if (err.code) {
        LogUtil.error(TAG, 'Failed to create window. Cause: ' + JSON.stringify(err));
        callback(loginFailInfo);
        return;
      }
      let windowClass: window.Window = data;
      if (!this.loginWindow) {
        this.loginWindow = windowClass;
      }

      // Cleanup function to destroy window if setup fails
      const cleanupWindow = (reason: string) => {
        LogUtil.error(TAG, `Login window setup failed: ${reason}, destroying window`);
        windowClass.destroyWindow((destroyErr: BusinessError) => {
          if (this.loginWindow === windowClass) {
            this.loginWindow = null;
          }
          if (destroyErr.code) {
            LogUtil.error(TAG, `Failed to destroy window: ${JSON.stringify(destroyErr)}`);
          }
        });
        callback(loginFailInfo);
      };

      windowClass.moveWindowTo(bounds.left, bounds.top, (err: BusinessError) => {
        let errCode: number = err.code;
        if (errCode) {
          LogUtil.error(TAG, `Failed to move the window. Cause:${JSON.stringify(err)}`);
        }
      });
      windowClass.resize(bounds.width, bounds.height, (err: BusinessError) => {
        let errCode: number = err.code;
        if (errCode) {
          LogUtil.error(TAG, `Failed to change the window size. Cause:${JSON.stringify(err)}`);
        }
      });
      windowClass.showWindow((err: BusinessError) => {
        const errCode: number = err.code;
        if (errCode) {
          LogUtil.error(TAG, `Failed to show the window. Cause: ${JSON.stringify(err)}`);
          cleanupWindow('showWindow failed');
          return;
        }
        LogUtil.info(TAG, 'Succeeded in showing the window.');
      });
      const para: Record<string, ILogin> = {
        'login': {
          'loginCallbackFunc': callback
        }
      };
      let storage: LocalStorage = new LocalStorage(para);
      windowClass.loadContent("pages/Login", storage, (err, data) => {
        let errCode: number = err.code;
        if (errCode) {
          LogUtil.error(TAG, 'Failed to load the content. Cause:' + JSON.stringify(err));
          cleanupWindow('loadContent failed');
          return;
        }
        windowClass.setWindowBackgroundColor('#00000000'); // argb
      });
    });
  }

  @LogMethod
  getWindowId(id: number): number {
    let windowClass = this.getWindow(id);
    let windowId = windowClass?.getWindowProperties().id;
    if (windowId === undefined) {
      LogUtil.error(TAG, 'getWindowId failed, return -1.');
      return -1;
    }
    return windowId;
  }

  /**
   * Window limit tracking - prevent V8 OOM by limiting concurrent windows
   */
  private checkWindowLimit(windowId: string, isNewWindow: boolean): boolean {
    // Clean up stale window IDs from set (in case windows were destroyed without proper cleanup)
    this.cleanupStaleWindows();

    if (isNewWindow) {
      // Check if adding this window would exceed the limit
      if (this.windowIdSet.size >= MAX_CONCURRENT_WINDOWS && !this.windowIdSet.has(windowId)) {
        LogUtil.warn(TAG, `Window limit reached: ${this.windowIdSet.size}/${MAX_CONCURRENT_WINDOWS}. Rejecting new window: ${windowId}`);
        return false;
      }
    }

    return true;
  }

  /**
   * Track a new or reused window
   */
  private trackWindow(windowId: string): void {
    this.windowIdSet.add(windowId);
    this.activeWindowCount = this.windowIdSet.size;
    LogUtil.info(TAG, `Tracking window: ${windowId}, active count: ${this.activeWindowCount}/${MAX_CONCURRENT_WINDOWS}`);
  }

  /**
   * Remove a window from tracking (called when window is destroyed)
   */
  private untrackWindow(windowId: string): void {
    if (this.windowIdSet.has(windowId)) {
      this.windowIdSet.delete(windowId);
      this.activeWindowCount = this.windowIdSet.size;
      LogUtil.info(TAG, `Untracked window: ${windowId}, active count: ${this.activeWindowCount}/${MAX_CONCURRENT_WINDOWS}`);
    }
  }

  /**
   * Clean up stale window IDs that no longer exist
   */
  private cleanupStaleWindows(): void {
    // Get current actual window count from AbilityManager
    const actualProxies: WebProxy[] = this.abilityManager.getProxyList();
    if (actualProxies && actualProxies.length > 0) {
      // Rebuild set from actual active windows
      const newSet: Set<string> = new Set<string>();
      actualProxies.forEach((proxy: WebProxy) => {
        const windowId: string = proxy.getWindowId();
        if (this.windowIdSet.has(windowId)) {
          newSet.add(windowId);
        }
      });

      // If counts differ, update the tracking set
      if (newSet.size !== this.windowIdSet.size) {
        LogUtil.info(TAG, `Cleanup stale windows: tracked ${this.windowIdSet.size}, actual ${newSet.size}`);
        this.windowIdSet = newSet;
        this.activeWindowCount = newSet.size;
      }
    }
  }

  /**
   * Show warning dialog when window limit is reached
   */
  private showWindowLimitWarning(): void {
    LogUtil.warn(TAG, `Window limit reached (${MAX_CONCURRENT_WINDOWS}). Showing warning to user.`);

    // Show prompt dialog to user
    promptAction.showToast({
      message: `已达到最大窗口数限制 (${MAX_CONCURRENT_WINDOWS})。请关闭一些窗口后再打开新窗口。`,
      duration: WINDOW_LIMIT_WARNING_MS,
      bottom: '50%'
    });

    // Also log to console for debugging
    console.warn(`[AppWindow] Window limit reached: ${this.activeWindowCount}/${MAX_CONCURRENT_WINDOWS}`);
    console.warn(`[AppWindow] Active windows: ${Array.from(this.windowIdSet).join(', ')}`);
  }

  /**
   * Override destroyAbility to also untrack the window
   */
  private destroyAbilityWithUntrack(id: number): void {
    let windowId = this.paramNormalization(id);
    this.untrackWindow(windowId);
    // Call original destroyAbility
    this.destroyAbility(id);
  }
};
