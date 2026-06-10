// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import window from '@ohos.window';
import Want from '@ohos.app.ability.Want';
import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';
import { Configuration } from '@ohos.app.ability.Configuration';
import { startOhPtyChild } from '../ohpty/OhPtyChild';
import { WebAbility } from 'web_engine';
import { startQemu } from '../qemu/QemuRunner';
import { fileIo, fileUri } from '@kit.CoreFileKit';

// 文件管理器"用 OHcode 打开"文件/文件夹时写入的一次性标记，WebWindow.buildArgs 启动时消费。
const OPEN_MARKER: string = '/data/storage/el2/base/files/ohcode-open-target.txt';

export default class EntryAbility extends WebAbility {
  onConfigurationUpdate(config: Configuration) {
    super.onConfigurationUpdate(config);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam) {
    super.onCreate(want, launchParam);

    this.handleOpenWant(want);

    startQemu(this.context)
      .then((ok: boolean) => {
        console.info(`[QEMU] started=${ok}`);
      })
      .catch((err: Error) => {
        console.error(`[QEMU] failed=${JSON.stringify(err)}`);
      });
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam) {
    super.onNewWant(want, launchParam);
    this.handleOpenWant(want);
  }

  // 文件管理器"用 OHcode 打开"文件/文件夹：want.uri 携带文件 URI。
  // 转成 file:// 路径写入一次性标记，由 WebWindow.buildArgs 注入 --file-uri / --folder-uri。
  private handleOpenWant(want: Want) {
    const uri: string = want.uri ?? '';
    if (uri.length === 0) {
      return;
    }
    try {
      const realPath: string = new fileUri.FileUri(uri).path;
      if (!realPath || realPath.length === 0) {
        return;
      }
      let kind: string = 'file';
      try {
        if (fileIo.statSync(realPath).isDirectory()) {
          kind = 'folder';
        }
      } catch (_) { /* 不可直接 stat 时按文件处理 */ }
      const payload: string = `${kind}|file://${realPath}`;
      const file = fileIo.openSync(OPEN_MARKER,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
      fileIo.writeSync(file.fd, payload);
      fileIo.closeSync(file);
      console.info(`[OHcode][open] ${payload}`);
    } catch (err) {
      console.error(`[OHcode][open] handle want.uri failed: ${JSON.stringify(err)}`);
    }
  }

  onDestroy() {
    super.onDestroy();
  }

  onWindowStageCreate(windowStage: window.WindowStage) {
    super.onWindowStageCreate(windowStage);
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
};
