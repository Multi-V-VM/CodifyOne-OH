import LogUtil from '../utils/LogUtil';
import window from '@ohos.window';
import Want from '@ohos.app.ability.Want';
import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import { deviceInfo } from '@kit.BasicServicesKit';
import { DisplayAdapter } from  '../adapter/DisplayAdapter';
import { NativeThemeAdapter } from '../adapter/NativeThemeAdapter';
import { AbilityManager } from '../common/AbilityManager';
import { AbilityPool } from '../common/AbilityPool';
import { Configuration } from '@ohos.app.ability.Configuration';
import Inject from '../common/InjectModule';
import { WindowBound } from '../interface/CommonInterface';
import CheckEmptyUtils from '../utils/CheckEmptyUtils';
import { WebBaseAbility } from './WebBaseAbility';
import { WindowStyle, IStyleData, IUpdateStyle } from '../common/WindowStyle';
import { CommandType } from '../common/Constants';
import { PermissionManagerAdapter } from '../adapter/PermissionManagerAdapter';
import { ConfigurationConstant } from '@kit.AbilityKit';

const TAG: string = 'WebAbility';
const PHONE_UNSUPPORTED_WINDOW_CAPABILITIES: string[] = [
  'setWindowTitleMoveEnabled',
  'setWindowTitleButtonVisible',
  'setWindowTopmost',
  'setWindowModal'
];

export class WebAbility extends WebBaseAbility {
  private static WINDOW_CLOSE: number = 1000;
  private abilityManager: AbilityManager | undefined = undefined;
  private abilityPool: AbilityPool | undefined = undefined;
  private displayAdapter : DisplayAdapter | undefined = undefined;
  private nativeThemeAdapter: NativeThemeAdapter | undefined = undefined;

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

  onConfigurationUpdate(config: Configuration) {
    if (config.colorMode !== undefined) {
      this.nativeThemeAdapter?.setSystemNativeTheme(config.colorMode);
    }
    if (config.fontSizeScale !== undefined) {
      this.displayAdapter?.onSystemFontSizeChange(config.fontSizeScale);
    } else {
      this.displayAdapter?.onSystemFontSizeChange(1.0);
    }
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam) {
    super.onCreate(want, launchParam);

    this.abilityManager = Inject.get(AbilityManager);
    this.abilityPool = Inject.getOrCreate(AbilityPool);
    this.nativeThemeAdapter = Inject.get(NativeThemeAdapter);

    Inject.get(PermissionManagerAdapter).initPermissions();
  }

  onWindowStageCreate(windowStage: window.WindowStage) {
    super.onWindowStageCreate(windowStage);

    if (this.checkSingleInstance()) {
      LogUtil.warn(TAG, 'repeat launch from desktop, need terminate');
      return;
    }

    this.abilityManager?.addProxy(this);

    windowStage.getMainWindow((err, data) => {
      if (err.code) {
        LogUtil.error(TAG, 'Failed to obtain the main window: ' + JSON.stringify(err));
        return;
      }

      try {
        data.on('windowStatusChange', (status) => {
          this.nativeContext.OnWindowStatusChange(this.xcomponentId, status);
        })
      } catch (exception) {
        LogUtil.error(TAG,'Failed to listener for window status changes: ' + JSON.stringify(exception));
      }

      try {
        data.on('windowSizeChange', (size) => {
          const prop = data.getWindowProperties();
          let windowBound: WindowBound = {
            left: prop.windowRect.left + prop.drawableRect.left,
            top: prop.windowRect.top + prop.drawableRect.top,
            width: size.width - prop.drawableRect.left - prop.drawableRect.left,
            height: size.height - prop.drawableRect.left - prop.drawableRect.top,
          };
          this.nativeContext.OnWindowSizeChange(this.xcomponentId, windowBound);
        });
      } catch (exception) {
        LogUtil.error(TAG,'Failed to listener for window size changes: ' + JSON.stringify(exception));
      }

      try {
        data.on("windowRectChange", (option) => {
          const prop = data.getWindowProperties();
          const rect = option.rect;
          let windowBound: window.Rect = {
            left: rect.left + prop.drawableRect.left,
            top: rect.top + prop.drawableRect.top,
            width: rect.width - prop.drawableRect.left - prop.drawableRect.left,
            height: rect.height - prop.drawableRect.left - prop.drawableRect.top,
          };
          this.nativeContext.OnWindowRectChange(this.xcomponentId,
            windowBound,
            option.reason.valueOf());
        });
      } catch (exception) {
        LogUtil.error(TAG,'Failed to listener for window rect changes: ' + JSON.stringify(exception));
      }

      try {
        data.on('windowEvent', (type) => {
          if (type == window.WindowEventType.WINDOW_SHOWN) {
            const prop = data.getWindowProperties();
            let windowBound: WindowBound = {
              left: prop.windowRect.left + prop.drawableRect.left,
              top: prop.windowRect.top + prop.drawableRect.top,
              width: prop.windowRect.width - prop.drawableRect.left - prop.drawableRect.left,
              height: prop.windowRect.height - prop.drawableRect.left - prop.drawableRect.top,
            };
            this.nativeContext.OnWindowRectChange(this.xcomponentId, windowBound, 0);
          }
          this.nativeContext.OnWindowEvent(this.xcomponentId, type.valueOf());
        });
      } catch (exception) {
        LogUtil.error(TAG, 'Failed to register callback: ' + JSON.stringify(exception));
      }

      try {
        data.on("windowVisibilityChange", (visible) => {
          this.nativeContext.OnWindowVisibilityChange(this.xcomponentId, visible);
        });
      } catch (exception) {
       LogUtil.error(TAG, 'Failed to register callback: ' + JSON.stringify(exception));
      }
    });

    const para: Record<string, Object | string[] | IUpdateStyle> = {
      'xcomponentId': this.xcomponentId,
      'startUri': this.startUri,
      'electronRelaunchArgs': this.electronRelaunchArgs,
      'updateStyle': {
        'registerUpdateStyleFunction': WindowStyle.registerUpdateStyleFunction,
        'removeUpdateStyleFunction': WindowStyle.removeUpdateStyleFunction
      }
    };
    let storage: LocalStorage = new LocalStorage(para);
    windowStage.loadContent(this.getContentPath(), storage, (err, data) => {
      if (err.code) {
        LogUtil.error(TAG, 'Failed to load the content: ' + JSON.stringify(err));
        return;
      }

      let window: window.Window = windowStage.getMainWindowSync();
      // hide title bar
      this.applyWindowCapability('setWindowDecorVisible', () => {
        window.setWindowDecorVisible(!this.hideTitleBar);
      });
      if (this.useDarkMode) {
        let buttonStyle: window.DecorButtonStyle = {
          colorMode: ConfigurationConstant.ColorMode.COLOR_MODE_DARK
        };
        this.applyWindowCapability('setDecorButtonStyle', () => {
          window.setDecorButtonStyle(buttonStyle);
        });
      }
      this.applyWindowCapability('setWindowTitleMoveEnabled', () => {
        window.setWindowTitleMoveEnabled(!this.hideTitleBar);
      });
      this.applyWindowCapability('setWindowTitleButtonVisible', () => {
        window.setWindowTitleButtonVisible(true, true, true);
      });
      this.applyWindowCapability('setWindowTopmost', () => {
        window.setWindowTopmost(this.alwaysOntop);
      });
      this.applyWindowCapability('setResizeByDragEnabled', () => {
        window.setResizeByDragEnabled(this.resizable);
      });
      this.applyWindowCapability('setWindowModal', () => {
        windowStage.setWindowModal(this.isModal);
      });
      if (this.isPanel) {
        this.applyWindowCapability('setTitleAndDockHoverShown', () => {
          window.setTitleAndDockHoverShown(false, false);
        });
      }
      if (deviceInfo.sdkApiVersion >= 15) {
        this.nativeContext.RegisterWindowEventFilter(window.getWindowProperties().id);
      }
    });

    try {
      windowStage.on('windowStageClose', () => {
        this.nativeContext.OnWindowEvent(this.xcomponentId, WebAbility.WINDOW_CLOSE);
        return true;
      });
    } catch (exception) {
      console.error(`Failed to enable the listener for window stage close event. Cause code: ${exception.code}, message: ${exception.message}`);
    }
  }

  onWindowStageDestroy() {
    // Always attempt cleanup, even if xcomponentId is empty
    // This prevents memory leaks if the ID was cleared prematurely
    let componentId = this.xcomponentId;

    if (CheckEmptyUtils.isEmpty(componentId)) {
      LogUtil.warn(TAG, 'onWindowStageDestroy component is not instantiated, attempting cleanup anyway');
    }

    // Notify pool that this ability is being destroyed
    if (this.abilityPool && componentId) {
      this.abilityPool.notifyDestroyed(componentId);
    }

    if (deviceInfo.sdkApiVersion >= 15) {
      let originWindowId = this.abilityManager?.getOriginWindowId(componentId);
      this.nativeContext.ClearWindowEventFilter(originWindowId);
    }
    // Main window is destroyed, release UI related resources
    this.abilityManager?.removeProxy(componentId);
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    let uriNotNull = want["uri"] !== undefined && want["uri"] !== '';
    let params = want.parameters || {};
    let reuseWindow = params["reuseWindow"] === 'true';

    if(want!== undefined && want !== null) {
      LogUtil.info(TAG, 'onNewWant begin, want: ' +
        'bundleName: ' + JSON.stringify(want.bundleName) +
        ', abilityName: ' + JSON.stringify(want.abilityName) +
        ', deviceId: ' + JSON.stringify(want.deviceId) +
        ', uri not null: ' + JSON.stringify(uriNotNull) +
        ', reuseWindow: ' + JSON.stringify(reuseWindow) +
        ', type: ' + JSON.stringify(want.type) +
        ', flags: ' + JSON.stringify(want.flags) +
        ', action: ' + JSON.stringify(want.action) +
        ', parameters: ' + JSON.stringify(want.parameters) +
        ', entities: ' + JSON.stringify(want.entities) +
        ', moduleName: ' + JSON.stringify(want.moduleName) +
        ', LaunchParam: ' + JSON.stringify(launchParam));
    }

    let url = (want['uri'] || this.startUri) as string;

    if (reuseWindow) {
      // Reuse existing WebContents instead of creating new
      let originWindowId = params['originWindowId'] as string;
      LogUtil.info(TAG, `Reusing WebContents from window: ${originWindowId}`);
      // Pass originWindowId via user_data field since CommandParameter is limited
      this.nativeContext.ExecuteCommand(CommandType.kReuseWindow, {
        url: url,
        user_data: originWindowId  // Use user_data to pass originWindowId
      });
    } else if (uriNotNull || url) {
      this.openNewWindow(url);
    }
  }

  onPrepareToTerminate(): boolean {
    this.nativeContext.OnWindowEvent(this.xcomponentId, WebAbility.WINDOW_CLOSE);
    return true;
  }

  private checkSingleInstance(): boolean {
    if (!CheckEmptyUtils.isEmpty(this.xcomponentId)) {
      return false;
    }
    this.openNewWindow(this.startUri);
    this.closeWindow();
    return true;
  }

  private openNewWindow(open_url: string) {
    this.nativeContext.ExecuteCommand(CommandType.kNewWindow, { url: open_url });
  }
};
