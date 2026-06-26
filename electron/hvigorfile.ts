import { hapTasks } from '@ohos/hvigor-ohos-plugin';

declare const require: any;
declare const process: any;

const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

function hasHnpFiles(dir: string): boolean {
    if (!fs.existsSync(dir)) {
        return false;
    }

    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const entryPath = path.join(dir, entry.name);
        if (entry.isFile() && entry.name.endsWith('.hnp')) {
            return true;
        }
        if (entry.isDirectory() && hasHnpFiles(entryPath)) {
            return true;
        }
    }

    return false;
}

function requirePath(label: string, targetPath: string): void {
    if (!fs.existsSync(targetPath)) {
        throw new Error(`${label} not found: ${targetPath}`);
    }
}

function findPackingTool(sdkHome: string | undefined): string {
    if (!sdkHome) {
        throw new Error('DEVECO_SDK_HOME, HARMONYOS_SDK_HOME, or OHOS_SDK_HOME must be set');
    }

    const candidates = [
        path.join(sdkHome, 'default', 'openharmony', 'toolchains', 'lib', 'app_packing_tool.jar'),
        path.join(sdkHome, 'openharmony', 'toolchains', 'lib', 'app_packing_tool.jar'),
        path.join(sdkHome, 'toolchains', 'lib', 'app_packing_tool.jar')
    ];

    const found = candidates.find(candidate => fs.existsSync(candidate));
    if (!found) {
        throw new Error(`app_packing_tool.jar not found under SDK home: ${sdkHome}`);
    }

    return found;
}

function stageHnpPath(modulePath: string, buildRoot: string, targetName: string): string {
    const requestedPath = process.env.OHCODE_HNP_PATH;
    if (requestedPath) {
        if (!hasHnpFiles(requestedPath)) {
            throw new Error(`No .hnp files found under OHCODE_HNP_PATH: ${requestedPath}`);
        }
        return requestedPath;
    }

    const hnpRoot = process.env.OHCODE_HNP_ROOT ?? path.join(modulePath, 'hnp');
    const abi = process.env.OHCODE_ABI ?? 'arm64-v8a';

    if (abi === 'all') {
        if (!hasHnpFiles(hnpRoot)) {
            throw new Error(`No .hnp files found under: ${hnpRoot}`);
        }
        return hnpRoot;
    }

    const sourceAbiDir = path.join(hnpRoot, abi);
    if (!hasHnpFiles(sourceAbiDir)) {
        throw new Error(
            `No .hnp files found for ABI "${abi}" under ${sourceAbiDir}. ` +
            `Use OHCODE_ABI=arm64-v8a for the default aarch64 build, ` +
            `or set OHCODE_HNP_PATH to an explicit HNP directory.`
        );
    }

    const stagedRoot = path.join(buildRoot, 'intermediates', 'ohcode_hnp', targetName);
    const stagedAbiDir = path.join(stagedRoot, abi);
    fs.rmSync(stagedRoot, { recursive: true, force: true });
    fs.mkdirSync(stagedRoot, { recursive: true });
    fs.cpSync(sourceAbiDir, stagedAbiDir, { recursive: true });
    return stagedRoot;
}

function patchAppAsarMinimist(context: any): void {
    const modulePath = context.modulePath.toString();
    const projectRoot = path.resolve(modulePath, '..');
    const patchScript = path.join(projectRoot, 'scripts', 'patch-app-asar-minimist.js');
    requirePath('app.asar minimist patch script', patchScript);

    execFileSync(process.execPath, [patchScript, projectRoot], {
        cwd: projectRoot,
        stdio: 'inherit'
    });
}

function repackHapWithHnp(context: any): void {
    const modulePath = context.modulePath.toString();
    const projectRoot = path.resolve(modulePath, '..');
    const targetName = process.env.OHCODE_TARGET_NAME ?? 'default';
    const buildRoot = path.join(modulePath, 'build', targetName);
    const outputsRoot = path.join(buildRoot, 'outputs', targetName);
    const sdkHome = process.env.DEVECO_SDK_HOME ?? process.env.HARMONYOS_SDK_HOME ?? process.env.OHOS_SDK_HOME;
    const packingTool = findPackingTool(sdkHome);
    const hnpPath = stageHnpPath(modulePath, buildRoot, targetName);
    const outPath = path.join(outputsRoot, `${context.moduleName}-${targetName}-unsigned.hap`);
    const pkgContextPath = path.join(buildRoot, 'intermediates', 'loader', targetName, 'pkgContextInfo.json');

    const requiredPaths = [
        path.join(buildRoot, 'intermediates', 'stripped_native_libs', targetName),
        path.join(buildRoot, 'intermediates', 'package', targetName, 'module.json'),
        path.join(buildRoot, 'intermediates', 'res', targetName, 'resources'),
        path.join(buildRoot, 'intermediates', 'res', targetName, 'resources.index'),
        path.join(outputsRoot, 'pack.info'),
        path.join(buildRoot, 'intermediates', 'loader_out', targetName, 'ets'),
        hnpPath
    ];

    for (const required of requiredPaths) {
        requirePath('Required build path', required);
    }

    const args = [
        '-Dfile.encoding=UTF-8',
        '-jar', packingTool,
        '--mode', 'hap',
        '--force', 'true',
        '--lib-path', path.join(buildRoot, 'intermediates', 'stripped_native_libs', targetName),
        '--json-path', path.join(buildRoot, 'intermediates', 'package', targetName, 'module.json'),
        '--resources-path', path.join(buildRoot, 'intermediates', 'res', targetName, 'resources'),
        '--index-path', path.join(buildRoot, 'intermediates', 'res', targetName, 'resources.index'),
        '--pack-info-path', path.join(outputsRoot, 'pack.info'),
        '--out-path', outPath,
        '--ets-path', path.join(buildRoot, 'intermediates', 'loader_out', targetName, 'ets')
    ];

    if (fs.existsSync(pkgContextPath)) {
        args.push('--pkg-context-path', pkgContextPath);
    }

    args.push('--hnp-path', hnpPath);

    console.info(`[OHcode] Repacking HAP with HNP path: ${hnpPath}`);
    execFileSync('java', args, {
        cwd: projectRoot,
        stdio: 'inherit'
    });
}

const patchAppAsarMinimistPlugin = {
    pluginId: 'ohcode.patch-app-asar-minimist',
    apply(node: any) {
        node.registerTask({
            name: 'PatchAppAsarMinimist',
            postDependencies: ['default@CompileResource'],
            run: (context: any) => {
                patchAppAsarMinimist(context);
            }
        });
    }
};

const repackHapWithHnpPlugin = {
    pluginId: 'ohcode.repack-hap-with-hnp',
    apply(node: any) {
        node.registerTask({
            name: 'RepackHapWithHnp',
            dependencies: ['default@PackageHap'],
            postDependencies: ['default@SignHap'],
            run: (context: any) => {
                repackHapWithHnp(context);
            }
        });
    }
};

export default {
    system: hapTasks,  /* Built-in plugin of Hvigor. It cannot be modified. */
    plugins:[patchAppAsarMinimistPlugin, repackHapWithHnpPlugin]         /* Custom plugin to extend the functionality of Hvigor. */
}
