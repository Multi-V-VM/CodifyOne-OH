// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import JsBindingUtils from '../utils/JsBindingUtils';
import Inject from '../common/InjectModule';
import lazy { FileManagerAdapter } from '../adapter/FileManagerAdapter';

function implFileManagerAdapter(): FileManagerAdapter {
  return Inject.getOrCreate(FileManagerAdapter);
}

function openItemInFolder(filePath: string) {
  implFileManagerAdapter().openItemInFolder(filePath);
}

function openVerifiedItem(filePath: string) {
  implFileManagerAdapter().openVerifiedItem(filePath);
}

function openUrlInDefaultBrowser(url: string) {
  implFileManagerAdapter().openUrlInDefaultBrowser(url);
}

export class FileManagerAdapterBind {
  static bind() {
    JsBindingUtils.bindFunction("FileManagerAdapter.OpenItemInFolder", openItemInFolder);
    JsBindingUtils.bindFunction("FileManagerAdapter.OpenVerifiedItem", openVerifiedItem);
    JsBindingUtils.bindFunction("FileManagerAdapter.OpenUrlInDefaultBrowser", openUrlInDefaultBrowser);
  }
}