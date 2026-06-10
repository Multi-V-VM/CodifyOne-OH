import { ILoginInfo, ILogin } from 'web_engine';
import { QuickLoginButtonComponent } from './QuickLoginButtonComponent'


let storage = LocalStorage.getShared();
@Entry(storage)
@Component
struct Index {
  @LocalStorageLink('login') login: ILogin = {
    loginCallbackFunc: (loginInfo: ILoginInfo) => {}
  };

  private loginCallback_: (loginInfo: ILoginInfo) => void = (loginInfo: ILoginInfo) => {};

  aboutToAppear() {
    this.loginCallback_ = this.login.loginCallbackFunc;
  }

  aboutToDisappear() {

  }

  build() {
    RelativeContainer() {
      QuickLoginButtonComponent({ loginCallback: this.loginCallback_ })
        .backgroundColor(0xffffff)
    }
    .height('100%')
    .width('100%')
  }
}
