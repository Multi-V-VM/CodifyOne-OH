// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { injectable, inject } from 'inversify';
import { MODULE_TYPE } from "../common/ModuleType";
import { BaseAdapter } from '../common/BaseAdapter';
import { AbilityManager } from '../common/AbilityManager';
import LogMethod from '../common/LogDecorator';
import LogUtil from '../utils/LogUtil';
import { WebProxy, FakeWebProxy } from '../interface/WebProxy';

const TAG: string = 'ContextAdapter';

@injectable()
export class ContextAdapter extends BaseAdapter {
    private abilityManager: AbilityManager;
    private context: Context;
    private activeWindow: string = '';

    constructor(
        @inject(AbilityManager) abilityManager: AbilityManager,
        @inject(MODULE_TYPE.Context) ctx: Context
    ) {
        super();
        this.abilityManager = abilityManager;
        this.context = ctx;
    }

    public getContext(): Context {
        return this.context;
    }

    @LogMethod
    public setActiveWindow(activeWindow: string) {
        LogUtil.info(TAG, 'set current active window:' + activeWindow);
        this.activeWindow = activeWindow;
    }

    public getActiveContext(): Context | undefined {
        let activeProxy = this.abilityManager.getProxy(this.activeWindow);
        if (!activeProxy) {
            LogUtil.error(TAG, 'no active window context');
        }
        return activeProxy?.getWindowContext();
    }

    public getActiveProxy(): WebProxy {
        let activeProxy = this.abilityManager.getProxy(this.activeWindow);
        if (!activeProxy) {
            LogUtil.error(TAG, 'no active window');
            return new FakeWebProxy()
        }
        return activeProxy;
    }
}