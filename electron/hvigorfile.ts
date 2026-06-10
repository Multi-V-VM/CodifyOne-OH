import { hapTasks } from '@ohos/hvigor-ohos-plugin';
import { execFileSync } from 'child_process';
import * as path from 'path';

const repackHapWithHnpPlugin = {
    pluginId: 'ohcode.repack-hap-with-hnp',
    apply(node) {
        node.registerTask({
            name: 'RepackHapWithHnp',
            dependencies: ['default@PackageHap'],
            postDependencies: ['default@SignHap'],
            run: (context) => {
                const modulePath = context.modulePath.toString();
                const projectRoot = path.resolve(modulePath, '..');
                const scriptPath = path.resolve(projectRoot, 'scripts', 'repack-hap-with-hnp.ps1');
                const sdkHome = process.env.DEVECO_SDK_HOME;

                execFileSync('powershell.exe', [
                    '-NoProfile',
                    '-ExecutionPolicy',
                    'Bypass',
                    '-File',
                    scriptPath,
                    '-ModuleName',
                    context.moduleName,
                    '-TargetName',
                    'default',
                    '-SdkHome',
                    sdkHome,
                    '-InPlace'
                ], {
                    cwd: projectRoot,
                    stdio: 'inherit'
                });
            }
        });
    }
};

export default {
    system: hapTasks,  /* Built-in plugin of Hvigor. It cannot be modified. */
    plugins:[repackHapWithHnpPlugin]         /* Custom plugin to extend the functionality of Hvigor. */
}
