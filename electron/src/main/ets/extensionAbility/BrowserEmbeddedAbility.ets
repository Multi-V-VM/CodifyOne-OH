import Want from '@ohos.app.ability.Want';
import { WebEmbeddedAbility } from 'web_engine';
import { AbilityConstant, UIExtensionContentSession } from '@kit.AbilityKit';

export default class BrowserEmbeddedAbility extends WebEmbeddedAbility {
  onCreate(launchParam: AbilityConstant.LaunchParam) {
    super.onCreate(launchParam)
  }

  onForeground() {
    super.onForeground()
  }

  onBackground() {
    super.onBackground()
  }

  onDestroy() {
    super.onDestroy()
  }

  onSessionCreate(want: Want, session: UIExtensionContentSession) {
    super.onSessionCreate(want, session)
  }

  onSessionDestroy(session: UIExtensionContentSession) {
    super.onSessionDestroy(session)
  }
};
