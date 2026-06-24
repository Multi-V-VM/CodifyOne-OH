import { appTasks } from '@ohos/hvigor-ohos-plugin';

declare const require: any;
declare const process: any;

const fs = require('fs');
const path = require('path');

function isSdkRoot(candidate: string): boolean {
    return fs.existsSync(path.join(candidate, 'default', 'hms', 'toolchains', 'uni-package.json')) &&
        fs.existsSync(path.join(candidate, 'default', 'openharmony', 'toolchains', 'oh-uni-package.json'));
}

function findSdkRoot(candidate: string | undefined): string | undefined {
    if (!candidate) {
        return undefined;
    }

    let current = path.resolve(candidate);
    for (let i = 0; i < 5; i++) {
        if (isSdkRoot(current)) {
            return current;
        }
        const parent = path.dirname(current);
        if (parent === current) {
            break;
        }
        current = parent;
    }

    return undefined;
}

function normalizeDevecoSdkHome(): void {
    const configured = process.env.DEVECO_SDK_HOME ?? process.env.HARMONYOS_SDK_HOME ?? process.env.OHOS_SDK_HOME;
    const sdkRoot = findSdkRoot(configured) ?? findSdkRoot('/Applications/DevEco-Studio.app/Contents/sdk');
    if (sdkRoot) {
        process.env.DEVECO_SDK_HOME = sdkRoot;
    }
}

normalizeDevecoSdkHome();

export default {
    system: appTasks,  /* Built-in plugin of Hvigor. It cannot be modified. */
    plugins:[]         /* Custom plugin to extend the functionality of Hvigor. */
}
