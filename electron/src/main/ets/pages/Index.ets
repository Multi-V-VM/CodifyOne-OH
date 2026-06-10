import { WebWindow } from 'web_engine';
import { KeyCode } from '@kit.InputKit';
import { fileIo } from '@kit.CoreFileKit';
import CustomChildProcess from '../process/CustomChildProcess';

// VS Code 工作台 onStartupFinished 时由 ohcode-splash 扩展写入；加载页轮询到它即淡出。
const READY_FLAG: string = '/data/storage/el2/base/files/ohcode-ready.flag';

interface IStyleData {
  backgroundColor: string,
  opacity: number
};

interface IUpdateStyle {
  registerUpdateStyleFunction: Function,
  removeUpdateStyleFunction: Function
};

CustomChildProcess.toString();
let storage = LocalStorage.getShared();
@Entry(storage)
@Component
struct Index {
  @LocalStorageLink('updateStyle') updateStyle: IUpdateStyle = {
    registerUpdateStyleFunction: (_id: string, _initStyle: IStyleData, _func: (style: IStyleData) => void) => {},
    removeUpdateStyleFunction: (_id: string) => {}
  };
  @LocalStorageLink('xcomponentId') xcomponentId: string = '';

  private initStyle: IStyleData = {
    backgroundColor: 'ff1e1e1e',
    opacity: 1
  };

  @State private backgroundColor_: string = `#${this.initStyle.backgroundColor}`;
  @State private opacity_: number = this.initStyle.opacity;
  @State private showLoading: boolean = true;
  private loadingTimer: number = -1;
  private pollTimer: number = -1;

  aboutToAppear() {
    this.updateStyle.registerUpdateStyleFunction(this.xcomponentId, this.initStyle, (style: IStyleData) => {
      this.backgroundColor_ = `#${style.backgroundColor}`;
      this.opacity_ = style.opacity;
    });

    try {
      if (fileIo.accessSync(READY_FLAG)) {
        fileIo.unlinkSync(READY_FLAG);
      }
    } catch (_) { }

    this.pollTimer = setInterval(() => {
      try {
        if (fileIo.accessSync(READY_FLAG)) {
          try { fileIo.unlinkSync(READY_FLAG); } catch (_) { }
          this.hideLoading();
        }
      } catch (_) { }
    }, 200);

    this.loadingTimer = setTimeout(() => {
      this.hideLoading();
    }, 20000);
  }

  private hideLoading(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
    if (this.loadingTimer !== -1) {
      clearTimeout(this.loadingTimer);
      this.loadingTimer = -1;
    }
    if (this.showLoading) {
      this.showLoading = false;
    }
  }

  aboutToDisappear() {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
    if (this.loadingTimer !== -1) {
      clearTimeout(this.loadingTimer);
      this.loadingTimer = -1;
    }
    this.updateStyle.removeUpdateStyleFunction(this.xcomponentId);
  }

  @Builder
  LoadingOverlay() {
    Stack({ alignContent: Alignment.Center }) {
      Column()
        .width('100%')
        .height('100%')
        .linearGradient({
          angle: 128,
          colors: [['#080A14', 0.0], ['#111A45', 0.30], ['#241044', 0.62], ['#08121F', 1.0]]
        })

      Column()
        .width('100%')
        .height('100%')
        .linearGradient({
          angle: 28,
          colors: [['#002BFF00', 0.0], ['#006DFFB8', 0.22], ['#9D4EDDCC', 0.48], ['#FF4D6D80', 0.72], ['#FFB70300', 1.0]]
        })
        .opacity(0.72)
        .blur(52)

      Column()
        .width('100%')
        .height('100%')
        .linearGradient({
          angle: 154,
          colors: [['#00F5D400', 0.0], ['#00F5D48C', 0.26], ['#38BDF8B8', 0.45], ['#7C3AEDAA', 0.66], ['#00000000', 1.0]]
        })
        .opacity(0.68)
        .blur(64)

      Column()
        .width('100%')
        .height('100%')
        .linearGradient({
          angle: 86,
          colors: [['#00000000', 0.0], ['#F9731680', 0.34], ['#EC4899AA', 0.56], ['#22D3EE66', 0.78], ['#00000000', 1.0]]
        })
        .opacity(0.44)
        .blur(78)

      // 柔和暗角让文字保持清晰，同时避免退场时出现 backdropBlur 残影。
      Column()
        .width('100%')
        .height('100%')
        .linearGradient({
          angle: 180,
          colors: [['#2A000000', 0.0], ['#00000000', 0.44], ['#52000000', 1.0]]
        })

      // 中心品牌区域：logo 本体不加底座/阴影，避免 logo 下方出现额外痕迹。
      Column({ space: 28 }) {
        Image($r('app.media.startIcon'))
          .width(112)
          .height(112)
          .objectFit(ImageFit.Contain)

        Column({ space: 10 }) {
          Text('OHcode')
            .fontSize(36)
            .fontWeight(FontWeight.Bold)
            .fontColor('#FFFFFFFF')
            .letterSpacing(3)
          Text('让 HarmonyOS PC 有更得心应手的编辑体验')
            .fontSize(15)
            .fontColor('#D9FFFFFF')
            .letterSpacing(0.6)
            .textAlign(TextAlign.Center)
        }
      }
      .alignItems(HorizontalAlign.Center)
      .transition(TransitionEffect.OPACITY
        .combine(TransitionEffect.scale({ x: 0.94, y: 0.94 }))
        .animation({ duration: 600, curve: Curve.EaseOut }))
    }
    .width('100%')
    .height('100%')
    .linearGradient({
      angle: 135,
      colors: [['#070A12', 0.0], ['#101B3D', 0.38], ['#221240', 0.68], ['#08131F', 1.0]]
    })
    .transition(TransitionEffect.OPACITY.animation({ duration: 500, curve: Curve.EaseInOut }))
  }

  build() {
    Stack() {
      Row() {
        WebWindow()
      }.onKeyEvent((event?: KeyEvent) => {
        if (event) {
          if (event.type == KeyType.Down && event.keyCode == KeyCode.KEYCODE_ESCAPE) {
            event.stopPropagation();
          }
        }
      })
      .backgroundColor(this.backgroundColor_)
      .opacity(this.opacity_)
      .width('100%')
      .height('100%')

      if (this.showLoading) {
        this.LoadingOverlay()
      }
    }
    .width('100%')
    .height('100%')
  }
}
