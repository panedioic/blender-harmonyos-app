// entry/src/main/ets/entryability/EntryAbility.ets

import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import fs from '@ohos.file.fs';
import { resourceManager } from '@kit.LocalizationKit';

const DOMAIN = 0x0000;
const TAG = 'EntryAbility';

/**
 * 手动可靠的递归创建目录树
 */
function mkdirsSync(dirPath: string): void {
  const parts = dirPath.split('/');
  let current = '';
  for (let i = 0; i < parts.length; i++) {
    if (parts[i] === '') {
      current += '/';
      continue;
    }
    if (current !== '/' && current !== '') {
      current += '/';
    }
    current += parts[i];
    try {
      fs.statSync(current);
    } catch (e) {
      // statSync 抛异常通常表示不存在，尝试创建
      try {
        fs.mkdirSync(current);
      } catch (err2) {
        // 由于并发或者其它原因创建失败，忽略继续
      }
    }
  }
}

async function copyRawfileDirRecursive(
  rm: resourceManager.ResourceManager,
  rawDir: string,
  destDir: string
): Promise<void> {

  mkdirsSync(destDir);

  let fileList: string[] = [];
  try {
    fileList = await rm.getRawFileList(rawDir);
  } catch (e) {
    hilog.error(DOMAIN, TAG, `getRawFileList failed for: ${rawDir}`);
    return;
  }

  for (const name of fileList) {
    const rawPath = `${rawDir}/${name}`;
    const destPath = `${destDir}/${name}`;

    let isDir = false;
    try {
      const subList = await rm.getRawFileList(rawPath);
      // 有些鸿蒙版本对文件调 getRawFileList 会返回 [] 而不是抛异常
      // 但它如果是文件，后续会按目录递归为空而自然跳过，保险起见都走异常判定
      if (subList && subList.length > 0) {
        isDir = true;
      }
    } catch (_) {
      isDir = false;
    }

    if (isDir) {
      await copyRawfileDirRecursive(rm, rawPath, destPath);
      continue;
    }

    try {
      const stat = fs.statSync(destPath);
      if (stat.size > 0) {
        // hilog.debug(DOMAIN, TAG, `skip (exists): ${destPath}`);
        continue;
      }
    } catch (_) {}

    try {
      await copyRawfileFile(rm, rawPath, destPath);
      hilog.info(DOMAIN, TAG, `copied: ${rawPath} -> ${destPath}`);
    } catch (e) {
      hilog.error(DOMAIN, TAG, `copy failed: ${rawPath}, err: ${e?.message ?? JSON.stringify(e)}`);
    }
  }
}

async function copyRawfileFile(
  rm: resourceManager.ResourceManager,
  rawPath: string,
  destPath: string
): Promise<void> {
  try {
    // 确保文件的父目录一定存在
    const parentDir = destPath.substring(0, destPath.lastIndexOf('/'));
    mkdirsSync(parentDir);

    // 类型断言为 Uint8Array
    const uint8Buf = await rm.getRawFileContent(rawPath) as Uint8Array;

    let file: fs.File | null = null;
    try {
      file = fs.openSync(destPath, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      // ⚠️ 极其关键：必须提取 .buffer (类型为 ArrayBuffer) 给系统底层的 writeSync
      const arrayBuffer = uint8Buf.buffer;
      fs.writeSync(file.fd, arrayBuffer);
    } finally {
      if (file) {
        try {
          fs.closeSync(file);
        } catch (_) {}
      }
    }
  } catch (e) {
    hilog.error(DOMAIN, TAG, `copyRawfileFile error - path: ${rawPath}, msg: ${e?.message ?? JSON.stringify(e)}`);
    throw new Error(`Failed to copy ${rawPath}`);
  }
}

export default class EntryAbility extends UIAbility {
  static datafilesDestPath: string = '';
  static datafilesReady: boolean = false;
  static filesDir: string = '';          // ← 新增

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {}
  onDestroy(): void {}

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) return;
      EntryAbility.filesDir = this.context.filesDir;  // ← 新增
      this.prepareBlenderDatafiles();
    });
  }

  private async prepareBlenderDatafiles(): Promise<void> {
    const blenderBase = `${this.context.filesDir}/blender`;
    // 拷 datafiles
    await copyRawfileDirRecursive(
      this.context.resourceManager,
      'blender/datafiles',
      `${blenderBase}/datafiles`
    );
    hilog.info(DOMAIN, TAG, 'datafiles copy complete');
    // 拷 scripts（新增）
    await copyRawfileDirRecursive(
      this.context.resourceManager,
      'blender/scripts',
      `${blenderBase}/scripts`
    );
    hilog.info(DOMAIN, TAG, 'scripts copy complete');
    // 拷 python 标准库
    await copyRawfileDirRecursive(
      this.context.resourceManager,
      'python_home',
      `${this.context.filesDir}/python_home`
    );
    hilog.info(DOMAIN, TAG, 'python_home copy complete');
    // 验证关键文件是否拷成功
    try {
      const stat = fs.statSync(`${blenderBase}/scripts/modules/bpy_types.py`);
      hilog.info(DOMAIN, TAG, `bpy_types.py copied OK, size=${stat.size}`);
    } catch(e) {
      hilog.error(DOMAIN, TAG, `bpy_types.py NOT found after copy! path=${blenderBase}/scripts/modules/bpy_types.py`);
    }
    EntryAbility.datafilesDestPath = `${blenderBase}/datafiles`;
    EntryAbility.datafilesReady = true;
  }

  onWindowStageDestroy(): void {}
  onForeground(): void {}
  onBackground(): void {}
}