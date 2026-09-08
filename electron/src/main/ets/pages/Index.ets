import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import CustomChildProcess from '../process/CustomChildProcess';

// VS Code 工作台 onStartupFinished 时由 ohcode-splash 扩展写入；加载页轮询到它即淡出。
const READY_FLAG: string = '/data/storage/el2/base/files/ohcode-ready.flag';
const STARTUP_STATUS_FILE: string = '/data/storage/el2/base/files/ohcode-startup-status.txt';
const STARTUP_BUILD_STAMP: string = 'diag-20260715-directv8main1';
const LOG_DOMAIN: number = 0x0000;
const LOG_TAG: string = 'OHcodeStartup';

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
  @State private loadingStatus: string = 'Starting OHcode shell [' + STARTUP_BUILD_STAMP + ']';
  @State private loadingElapsedSec: number = 0;
  @State private localCode: string = "import { app } from './runtime';\n\ninterface Device {\n  name: string;\n  arch: 'arm64';\n}\n\nconst device: Device = {\n  name: 'Mate 60',\n  arch: 'arm64'\n};\n\napp.start(device);\n";
  private loadingStartedAt: number = 0;
  private loadingTimer: number = -1;
  private pollTimer: number = -1;

  aboutToAppear() {
    this.loadingStartedAt = Date.now();
    this.writeStartupStatus('Index aboutToAppear');
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
      this.loadingElapsedSec = Math.floor((Date.now() - this.loadingStartedAt) / 1000);
      this.refreshStartupStatus();
      try {
        if (fileIo.accessSync(READY_FLAG)) {
          try { fileIo.unlinkSync(READY_FLAG); } catch (_) { }
          this.writeStartupStatus('ready flag observed');
          this.hideLoading();
        }
      } catch (_) { }
    }, 200);

    // The preload-free local editor is complete as soon as its small document
    // finishes loading. Native startup diagnostics can overwrite the shared
    // status file before this ArkUI poll observes did-finish-load, so use a
    // short bounded fallback instead of leaving an opaque splash over a live
    // editor forever.
    this.loadingTimer = setTimeout(() => {
      this.writeStartupStatus('local editor splash fallback elapsed');
      this.hideLoading();
    }, 2500);
  }

  private writeStartupStatus(status: string): void {
    try {
      const file = fileIo.openSync(STARTUP_STATUS_FILE,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
      fileIo.writeSync(file.fd, `${Date.now()} [${STARTUP_BUILD_STAMP}] ${status}`);
      fileIo.closeSync(file);
    } catch (_) { }
    hilog.info(LOG_DOMAIN, LOG_TAG, '[%{public}s] %{public}s', STARTUP_BUILD_STAMP, status);
    this.loadingStatus = '[' + STARTUP_BUILD_STAMP + '] ' + status;
  }

  private refreshStartupStatus(): void {
    try {
      if (fileIo.accessSync(STARTUP_STATUS_FILE)) {
        const raw: string = fileIo.readTextSync(STARTUP_STATUS_FILE).trim();
        const firstSpace: number = raw.indexOf(' ');
        this.loadingStatus = firstSpace >= 0 ? raw.substring(firstSpace + 1) : raw;
        if (this.showLoading && this.isStartupReadyStatus(raw)) {
          this.writeStartupStatus('ready status observed');
          this.hideLoading();
        }
      }
    } catch (_) { }
  }

  private isStartupReadyStatus(raw: string): boolean {
    return raw.indexOf('[OHcode] workbench dom-ready') >= 0 ||
      raw.indexOf('[OHcode] workbench did-finish-load') >= 0 ||
      raw.indexOf('[ohcode-splash] ready flag written') >= 0;
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
  SurfaceDiagnosticRibbon() {
    Column({ space: 2 }) {
      Text('OHcode ArkUI surface visible [' + STARTUP_BUILD_STAMP + ']')
        .fontSize(13)
        .fontColor('#FFFFFFFF')
        .fontWeight(FontWeight.Medium)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
      Text(this.loadingStatus)
        .fontSize(11)
        .fontColor('#D9FFFFFF')
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
    }
    .width('100%')
    .padding({ left: 10, right: 10, top: 7, bottom: 7 })
    .alignItems(HorizontalAlign.Start)
    .backgroundColor('#D0111827')
    .position({ x: 0, y: 0 })
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
          Text(`${this.loadingStatus} (${this.loadingElapsedSec}s)`)
            .fontSize(13)
            .fontColor('#B8FFFFFF')
            .letterSpacing(0)
            .textAlign(TextAlign.Center)
            .maxLines(3)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .width('80%')
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

  @Builder
  LocalCodeEditor() {
    Column() {
      Row({ space: 10 }) {
        Text('⌁').fontSize(22).fontColor('#23A8F2').fontWeight(FontWeight.Bold)
        Text('OHcode').fontSize(14).fontColor('#E8E8E8').fontWeight(FontWeight.Medium)
        Blank()
        Text('main.ts — OHcode').fontSize(12).fontColor('#A8A8A8')
        Blank()
        Text('SAVE').fontSize(11).fontColor('#FFFFFF').padding({ left: 9, right: 9, top: 5, bottom: 5 }).backgroundColor('#0E639C').borderRadius(3)
      }
      .width('100%').height(42).padding({ left: 12, right: 12 }).backgroundColor('#181818')

      Row() {
        Column({ space: 18 }) {
          Text('▱').fontSize(24).fontColor('#FFFFFF')
          Text('⌕').fontSize(24).fontColor('#888888')
          Text('⑂').fontSize(22).fontColor('#888888')
          Text('▷').fontSize(22).fontColor('#888888')
          Text('⊞').fontSize(22).fontColor('#888888')
          Blank()
          Text('⚙').fontSize(21).fontColor('#AAAAAA')
        }
        .width(48).height('100%').padding({ top: 14, bottom: 12 }).backgroundColor('#181818')

        Column() {
          Text('EXPLORER').width('100%').height(38).padding({ left: 10, top: 12 }).fontSize(11).fontColor('#CCCCCC')
          Text('⌄  OHCODE').width('100%').padding({ left: 8, top: 7, bottom: 7 }).fontSize(12).fontColor('#DDDDDD').fontWeight(FontWeight.Medium)
          Text('TS  main.ts').width('100%').padding({ left: 16, top: 8, bottom: 8 }).fontSize(12).fontColor('#D8D8D8').backgroundColor('#37373D')
          Text('{}  app.json').width('100%').padding({ left: 16, top: 8, bottom: 8 }).fontSize(12).fontColor('#BDBDBD')
          Text('M↓  README.md').width('100%').padding({ left: 16, top: 8, bottom: 8 }).fontSize(12).fontColor('#BDBDBD')
          Blank()
        }
        .width(126).height('100%').backgroundColor('#252526')

        Column() {
          Row({ space: 7 }) {
            Text('TS').fontSize(11).fontColor('#4FC1FF')
            Text('main.ts').fontSize(12).fontColor('#DDDDDD')
            Text('×').fontSize(15).fontColor('#AAAAAA')
          }
          .width('100%').height(36).padding({ left: 12 }).backgroundColor('#1E1E1E')
          Text('OHCODE  ›  main.ts').width('100%').height(28).padding({ left: 12, top: 7 }).fontSize(11).fontColor('#999999').backgroundColor('#1E1E1E')
          Row() {
            Text('1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14')
              .width(36).height('100%').padding({ right: 7, top: 12 }).fontSize(14).fontColor('#858585').fontFamily('monospace').textAlign(TextAlign.End)
            TextArea({ text: this.localCode, placeholder: 'Start typing code…' })
              .onChange((value: string) => { this.localCode = value; })
              .layoutWeight(1).height('100%').padding({ left: 7, right: 10, top: 8, bottom: 20 })
              .fontSize(14).fontColor('#D4D4D4').fontFamily('monospace').backgroundColor('#1E1E1E')
          }
          .layoutWeight(1).width('100%').backgroundColor('#1E1E1E')
        }
        .layoutWeight(1).height('100%').backgroundColor('#1E1E1E')
      }
      .layoutWeight(1).width('100%')

      Row({ space: 13 }) {
        Text('⑂ main*').fontSize(11).fontColor('#FFFFFF')
        Text('↻').fontSize(12).fontColor('#FFFFFF')
        Blank()
        Text('Ln 1, Col 1').fontSize(11).fontColor('#FFFFFF')
        Text('Spaces: 2').fontSize(11).fontColor('#FFFFFF')
        Text('TypeScript').fontSize(11).fontColor('#FFFFFF')
        Text('HarmonyOS arm64').fontSize(11).fontColor('#FFFFFF')
      }
      .width('100%').height(25).padding({ left: 8, right: 8 }).backgroundColor('#007ACC')
    }
    .width('100%').height('100%').backgroundColor('#1E1E1E')
  }

  build() {
    Stack() {
      if (this.showLoading) {
        this.LoadingOverlay()
      } else {
        this.LocalCodeEditor()
      }
    }
    .width('100%')
    .height('100%')
  }
}
