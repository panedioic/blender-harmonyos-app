// entry/src/main/ets/pages/Index.ets

import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import EntryAbility from '../entryability/EntryAbility';

const DOMAIN = 0x0000;
const TAG = 'BlenderIndex';

const POLL_INTERVAL_MS = 100;
const MAX_WAIT_MS = 60000;

enum SplashPhase {
  CopyingFiles = 1,   // 阶段1: 拷贝 rawfile
  InitializingBlender = 2,  // 阶段2: Blender_Init 中
  Done = 3,
}

@Entry
@Component
struct Index {
  private ctrl: XComponentController = new XComponentController();

  @State phase: SplashPhase = SplashPhase.CopyingFiles;
  @State statusText: string = '正在准备 Blender 数据文件...';
  @State isReady: boolean = false;
  @State showOverlay: boolean = true;
  private datafilesNotified: boolean = false;

  private pollTimer: number = -1;
  private pollElapsed: number = 0;
  private surfaceCreated: boolean = false;

  aboutToAppear(): void {
    hilog.info(DOMAIN, TAG, 'aboutToAppear: start polling for datafiles');
    this.startPolling();
  }

  aboutToDisappear(): void {
    this.stopPolling();
  }

  private startPolling(): void {
    this.pollTimer = setInterval(() => {
      this.pollElapsed += POLL_INTERVAL_MS;

      // 阶段 1 -> 阶段 2:datafiles 拷贝完成后,通知 C++
      if (!this.datafilesNotified && EntryAbility.datafilesReady) {
        this.datafilesNotified = true;
        this.notifyNative(EntryAbility.datafilesDestPath);
        this.phase = SplashPhase.InitializingBlender;
        this.statusText = 'Blender 引擎启动中,大约需要 10 秒...';
        return;
      }
      // 阶段 2 -> 完成:Blender_Init 返回后隐藏启动图
      if (this.phase === SplashPhase.InitializingBlender) {
        try {
          const ready: boolean = (testNapi).isBlenderReady();
          if (ready) {
            hilog.info(DOMAIN, TAG, 'Blender ready, hiding splash.');
            this.phase = SplashPhase.Done;
            this.stopPolling();
            return;
          }
        } catch (e) {
          hilog.error(DOMAIN, TAG, 'isBlenderReady call failed');
        }
      }
      // 状态文字更新
      const secs = (this.pollElapsed / 1000).toFixed(1);
      if (this.phase === SplashPhase.CopyingFiles) {
        this.statusText = `正在准备 Blender 数据文件... (${secs}s)`;
      } else if (this.phase === SplashPhase.InitializingBlender) {
        this.statusText = `Blender 引擎启动中... (${secs}s)`;
      }
      // 超时保护
      if (this.pollElapsed >= MAX_WAIT_MS) {
        hilog.warn(DOMAIN, TAG, 'Splash timeout, hiding anyway.');
        this.phase = SplashPhase.Done;
        this.stopPolling();
      }
    }, POLL_INTERVAL_MS);
  }

  private stopPolling(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  private notifyNative(path: string): void {
    hilog.info(DOMAIN, TAG, `notifyNative: path=${path}`);
    if (path && path.length > 0) {
      try {
        const pythonHome = EntryAbility.filesDir + '/python_home';
        (testNapi).setPythonHome(pythonHome);
        (testNapi).setDatafilesPath(path);
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'set path error');
      }
    }
    try {
      (testNapi).notifyDatafilesReady();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'notifyDatafilesReady error');
    }
  }

  // 根据当前阶段选择图片资源
  private getSplashImage(): Resource {
    if (this.phase === SplashPhase.CopyingFiles) {
      return $r('app.media.splash_phase1');
    }
    return $r('app.media.splash_phase2');
  }

  private onDatafilesReady(path: string): void {
    hilog.info(DOMAIN, TAG, `onDatafilesReady: path = ${path}`);

    if (path && path.length > 0) {
      try {
        // Python 标准库在 filesDir/python3.11，和 datafiles 是平级的
        // const pythonHome = EntryAbility.filesDir + '/python3.11';
        const pythonHome = EntryAbility.filesDir + '/python_home';
        (testNapi).setPythonHome(pythonHome);
        hilog.info(DOMAIN, TAG, `setPythonHome: ${pythonHome}`);
        (testNapi).setDatafilesPath(path);
        hilog.info(DOMAIN, TAG, 'setDatafilesPath OK');
      } catch (e) {
        hilog.error(DOMAIN, TAG, `setDatafilesPath error`);
      }
    }

    try {
      (testNapi).notifyDatafilesReady();
      hilog.info(DOMAIN, TAG, 'notifyDatafilesReady OK');
    } catch (e) {
      hilog.error(DOMAIN, TAG, `notifyDatafilesReady error`);
    }

    this.statusText = 'Blender 启动中（该过程可能持续至少10秒，黑屏是正常的。之后若提示显卡不支持是bug，请忽略）...';
    this.isReady = true;

    setTimeout(() => {
      this.showOverlay = false;
    }, 10000);
  }
  build() {
    Stack({ alignContent: Alignment.Center }) {
      // ── 主渲染画布 ──
      XComponent({
        id: 'blender_canvas',
        type: XComponentType.SURFACE,
        libraryname: 'entry',
        controller: this.ctrl
      })
        .width('100%')
        .height('100%')
        .onLoad(() => {
          hilog.info(DOMAIN, TAG, 'XComponent onLoad');
        })
        .onDestroy(() => {
          hilog.info(DOMAIN, TAG, 'XComponent onDestroy');
        })
      // ── 启动图覆盖层 (只在未完成时显示) ──
      if (this.phase !== SplashPhase.Done) {
        Stack({ alignContent: Alignment.BottomStart }) {
          // 启动图,铺满整个屏幕
          Image(this.getSplashImage())
            .width('100%')
            .height('100%')
            .objectFit(ImageFit.Cover)
          // 左下角状态文字 (叠在图片上)
          Column({ space: 6 }) {
            Row({ space: 8 }) {
              LoadingProgress()
                .width(20)
                .height(20)
                .color('#FFFFFF')
              Text(this.statusText)
                .fontSize(14)
                .fontColor('#FFFFFF')
            }
          }
          .padding({ left: 24, bottom: 32 })
          .alignItems(HorizontalAlign.Start)
        }
        .width('100%')
        .height('100%')
      }
    }
    .width('100%')
    .height('100%')
    .onKeyEvent((event: KeyEvent) => {
      if (event.type === KeyType.Down || event.type === KeyType.Up) {
        const action = event.type === KeyType.Down ? 1 : 0;
        (testNapi).sendKeyEvent(action, event.keyCode, event.metaKey);
        hilog.debug(DOMAIN, TAG,
          `KeyEvent: type=${event.type} code=${event.keyCode} meta=${event.metaKey}`);
      }
      // 返回 true 表示消费事件，不再向上传递
      return true;
    })
  }
}
