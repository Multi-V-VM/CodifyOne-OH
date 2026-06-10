import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import { Configuration } from '@ohos.app.ability.Configuration';
import Want from '@ohos.app.ability.Want';
import window from '@ohos.window';

import { deviceInfo } from '@kit.BasicServicesKit';

import { WebAbility } from 'web_engine';

export default class StatelessAbility extends WebAbility {
  onConfigurationUpdate(config: Configuration) {
    super.onConfigurationUpdate(config);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam) {
    super.onCreate(want, launchParam);
  }

  onDestroy() {
    super.onDestroy();
  }

  onWindowStageCreate(windowStage: window.WindowStage) {
    super.onWindowStageCreate(windowStage);
    if (deviceInfo.sdkApiVersion >= 14) {
      windowStage.setWindowRectAutoSave(false);
    }
  }

  onWindowStageDestroy() {
    super.onWindowStageDestroy();
  }

  onForeground() {
    super.onForeground();
  }

  onBackground() {
    super.onBackground();
  }

  override getContentPath(): string {
    return 'pages/Index';
  }
};
