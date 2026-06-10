// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import JsBindingUtils from '../utils/JsBindingUtils';
import Inject from '../common/InjectModule';
import lazy { DisplayAdapter, CompleteDisplay } from '../adapter/DisplayAdapter';

function implDisplayAdapter(): DisplayAdapter {
  return Inject.getOrCreate(DisplayAdapter);
}

function getDefaultDisplay(callback: (completeDisplay: CompleteDisplay) => void) {
  implDisplayAdapter().getDefaultDisplay(callback);
}

function getALLDisplays(callback: (completeDisplays: Array<CompleteDisplay>,
                                   length: number) => void) {
  implDisplayAdapter().getALLDisplays(callback);
}

function registerDisplayMonitor() {
  implDisplayAdapter().registerDisplayMonitor();
}

function getFontSizeScale(): number {
  return implDisplayAdapter().getFontSizeScale();
}

export class DisplayAdapterBind {
  static bind() {
    JsBindingUtils.bindFunction('OhosDisplayAdapter.GetDefaultDisplay', getDefaultDisplay);
    JsBindingUtils.bindFunction('OhosDisplayAdapter.GetALLDisplays', getALLDisplays);
    JsBindingUtils.bindFunction('OhosDisplayAdapter.registerDisplayMonitor', registerDisplayMonitor);
    JsBindingUtils.bindFunction('OhosDisplayAdapter.getFontSizeScale', getFontSizeScale);
  }
}
