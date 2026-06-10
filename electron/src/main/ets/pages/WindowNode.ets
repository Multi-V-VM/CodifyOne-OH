import { WebWindowNode } from 'web_engine';
import { KeyCode } from '@kit.InputKit';

let storage = LocalStorage.getShared();
@Entry(storage)
@Component
struct WindowNode {
  build() {
    Row() {
      WebWindowNode()
    }.onKeyEvent((event?: KeyEvent) => {
      if (event) {
        if (event.type == KeyType.Down && event.keyCode == KeyCode.KEYCODE_ESCAPE) {
          event.stopPropagation();
        }
      }
    })
    .width('100%')
    .height('100%')
  }
}
