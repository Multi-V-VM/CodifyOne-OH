import adapter from 'libadapter.so'
import { WebSubWindow } from 'web_engine';

let subStorage = LocalStorage.getShared();

@Entry(subStorage)
@Component
struct SubWindow {
  build(){
    Column() {
      WebSubWindow()
    }
    .width('100%')
    .height('100%')
  }
}