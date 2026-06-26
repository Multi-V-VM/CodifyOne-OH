// Copyright (c) 2024 V8 Pool Hook wrapper for HarmonyOS Electron
// NDK wrapper for V8 isolate tracking and pooling

import entry from 'libentry.so';
import LogUtil from '../utils/LogUtil';

const TAG: string = 'V8PoolHook';

/**
 * V8 Pool Hook interface
 * Provides access to V8 isolate tracking from native layer
 */
export interface V8PoolHook {
  registerIsolate(windowId: string, isolatePtr: number): void;
  poolIsolate(windowId: string): PoolOperationResult;
  unregisterIsolate(windowId: string): PoolOperationResult;
  getPooledIsolate(windowId: string): number | null;
  hasIsolate(windowId: string): boolean;
  getPoolStats(): PoolStats;
  clearPool(): PoolOperationResult;
  sanitizeNodeEnvironment(): NodeEnvironmentSanitizeResult;
  installElectronPltHooks(): ElectronPltHookInstallResult;
  getElectronPltHookStats(): ElectronPltHookStats;
}

export interface PoolStats {
  active: number;
  pooled: number;
  total: number;
}

export interface PoolOperationResult {
  success: boolean;
  cleared?: number;
  message?: string;
}

export interface NodeEnvironmentSanitizeResult {
  success: boolean;
  cleared: number;
  clearedVariables: string;
  message?: string;
}

export interface ElectronPltHookInstallResult {
  installed: boolean;
  monitorStarted: boolean;
  electronPreloadAttempted?: boolean;
  electronPreloadSucceeded?: boolean;
  attempts: number;
  patchedSlots: number;
  failures: number;
  runs: number;
  v8InitializeCalls?: number;
  v8InitializeTargetHits?: number;
  v8InitializeSerializedCalls?: number;
  v8InitializeFailures?: number;
  v8InitializeForcedSuccesses?: number;
  v8InitializeForceSuccessEnabled?: boolean;
  disableNodeStartupSnapshot?: boolean;
  snapshotBlobClears?: number;
  snapshotBlobNulls?: number;
  lastCreateParamsAddress?: number;
  lastCreateParamsSlot0?: number;
  lastCreateParamsSlot1?: number;
  lastCreateParamsSlot2?: number;
  lastCreateParamsSlot3?: number;
  lastCreateParamsSlot4?: number;
  lastCreateParamsSlot5?: number;
  lastSnapshotBlobAddress?: number;
  createParamsSlotsHex?: string;
  v8StartupFlagsApplied?: boolean;
  v8StartupFlagsResolveFailed?: boolean;
  v8StartupFlagsSymbolResolved?: boolean;
  v8StartupFlagsEnvPresent?: boolean;
  v8StartupFlagsEnvEmpty?: boolean;
  v8StartupFlagsUsingDefault?: boolean;
  v8StartupFlags?: string;
  v8StartupFlagsLength?: number;
  v8StartupFlagsApplyAttempts?: number;
  v8StartupFlagsEmptySkips?: number;
  v8SetSnapshotDataBlobCalls?: number;
  lastV8SnapshotDataBlobAddress?: number;
  lastV8SnapshotDataBlobDataAddress?: number;
  lastV8SnapshotDataBlobRawSize?: number;
  lastEffectiveV8SnapshotDataBlobAddress?: number;
  lastEffectiveV8SnapshotDataBlobDataAddress?: number;
  lastEffectiveV8SnapshotDataBlobRawSize?: number;
  replaceContextSnapshotWithStartup?: boolean;
  skipV8SnapshotDataBlob?: boolean;
  snapshotBlobSkips?: number;
  snapshotBlobReplacementAttempts?: number;
  snapshotBlobReplacements?: number;
  snapshotBlobReplacementFailures?: number;
  baseStartupBlobRawSize?: number;
  snapshotBlobReplacementPath?: string;
  v8InitializeExternalStartupDataCalls?: number;
  v8InitializeExternalStartupDataFromFileCalls?: number;
  lastV8ExternalStartupDataPath?: string;
  snapshotNothrowNewCalls?: number;
  snapshotNothrowNewFailures?: number;
  snapshotMmapFallbacks?: number;
  snapshotMmapFallbackFailures?: number;
  snapshotMmapBytes?: number;
  snapshotDecompressCalls?: number;
  snapshotDecompressBytesIn?: number;
  lastSnapshotCompressedSize?: number;
  lastSnapshotDecompressedSize?: number;
  lastSnapshotAllocCallerOffset?: number;
  nodeInitializeContextSlots?: number;
  nodeInitializeContextInlineInstalled?: boolean;
  nodeInitializeContextInlineEntrypoints?: number;
  nodeInitializeContextInlineFailures?: number;
  nodePlatformForIsolateInlineInstalled?: boolean;
  nodePlatformForIsolateInlineEntrypoints?: number;
  nodePlatformForIsolateInlineFailures?: number;
  nodePlatformForIsolateCalls?: number;
  nodePlatformHookEnabled?: boolean;
  nodePlatformRegisterOnLookupEnabled?: boolean;
  nodePlatformResolveFailed?: boolean;
  nodeInitializeContextCalls?: number;
  nodePlatformRegisterAttempts?: number;
  nodePlatformRegisterSuccesses?: number;
  nodePlatformRegisterDuplicateSkips?: number;
  nodePlatformRegisterMissingVtable?: number;
  nodePlatformLookupHits?: number;
  nodePlatformLookupMisses?: number;
  nodePlatformLookupFallbacks?: number;
  nodePlatformLookupFakeFallbacks?: number;
  lastNodePlatformRegisterAddress?: number;
  lastNodePlatformDataAddress?: number;
  adapterChildProcessSlots?: number;
  adapterChildProcessCalls?: number;
  adapterChildProcessPassThrough?: number;
  adapterCrashpadBlocks?: number;
  message?: string;
}

export interface ElectronPltHookStats extends ElectronPltHookInstallResult {
  epollWaitSlots: number;
  v8InitializeSlots: number;
  v8SetSnapshotDataBlobSlots?: number;
  v8InitializeExternalStartupDataSlots?: number;
  v8InitializeExternalStartupDataFromFileSlots?: number;
  snapshotDecompressSlots: number;
  arrayNothrowNewSlots: number;
  scalarNothrowNewSlots: number;
  arrayDeleteSlots: number;
  scalarDeleteSlots: number;
  nodeInitializeContextSlots?: number;
  nodeInitializeContextInlineInstalled?: boolean;
  nodeInitializeContextInlineEntrypoints?: number;
  nodeInitializeContextInlineFailures?: number;
  nodePlatformHookEnabled?: boolean;
  nodePlatformRegisterOnLookupEnabled?: boolean;
  nodePlatformResolveFailed?: boolean;
  nodeInitializeContextCalls?: number;
  nodePlatformRegisterAttempts?: number;
  nodePlatformRegisterSuccesses?: number;
  nodePlatformRegisterDuplicateSkips?: number;
  nodePlatformRegisterMissingVtable?: number;
  nodePlatformLookupHits?: number;
  nodePlatformLookupMisses?: number;
  nodePlatformLookupFallbacks?: number;
  nodePlatformLookupFakeFallbacks?: number;
  lastNodePlatformIsolate?: number;
  lastNodePlatformAddress?: number;
  lastNodePlatformRegisterAddress?: number;
  lastNodePlatformDataAddress?: number;
  adapterChildProcessSlots?: number;
  adapterChildProcessCalls?: number;
  adapterChildProcessPassThrough?: number;
  adapterCrashpadBlocks?: number;
}

interface NativeV8PoolHookModule {
  registerIsolate: (windowId: string, isolatePtr: number) => void;
  poolIsolate: (windowId: string) => PoolOperationResult;
  unregisterIsolate: (windowId: string) => PoolOperationResult;
  getPooledIsolate: (windowId: string) => number | null;
  hasIsolate: (windowId: string) => boolean;
  getPoolStats: () => PoolStats;
  clearPool: () => PoolOperationResult;
  sanitizeNodeEnvironment: () => NodeEnvironmentSanitizeResult;
  installElectronPltHooks: () => ElectronPltHookInstallResult;
  getElectronPltHookStats: () => ElectronPltHookStats;
}

class V8PoolHookImpl implements V8PoolHook {
  private nativeModule: NativeV8PoolHookModule | undefined = undefined;
  private isReady: boolean = false;

  constructor() {
    this.loadNativeModule();
  }

  private loadNativeModule(): void {
    try {
      // The v8poolhook module is built into libentry.so
      this.nativeModule = entry as NativeV8PoolHookModule;
      if (this.nativeModule) {
        this.isReady = true;
        LogUtil.info(TAG, 'V8 Pool Hook module loaded');
      } else {
        LogUtil.warn(TAG, 'V8 Pool Hook module not available');
      }
    } catch (e) {
      LogUtil.warn(TAG, `Failed to load V8 Pool Hook: ${JSON.stringify(e)}`);
    }
  }

  ready(): boolean {
    return this.isReady && this.nativeModule !== undefined;
  }

  registerIsolate(windowId: string, isolatePtr: number): void {
    if (!this.ready() || this.nativeModule === undefined) {
      LogUtil.warn(TAG, 'Module not ready for registerIsolate');
      return;
    }

    try {
      // Pass windowId and isolate pointer as BigInt
      this.nativeModule.registerIsolate(windowId, isolatePtr);
      LogUtil.info(TAG, `Registered isolate for window: ${windowId}`);
    } catch (e) {
      LogUtil.error(TAG, `registerIsolate failed: ${JSON.stringify(e)}`);
    }
  }

  poolIsolate(windowId: string): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Module not ready' };
    }

    try {
      const result = this.nativeModule.poolIsolate(windowId);
      LogUtil.info(TAG, `Pooled isolate for window: ${windowId}`);
      return result || { success: false, message: 'No result' };
    } catch (e) {
      LogUtil.error(TAG, `poolIsolate failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  unregisterIsolate(windowId: string): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Module not ready' };
    }

    try {
      const result = this.nativeModule.unregisterIsolate(windowId);
      LogUtil.info(TAG, `Unregistered isolate for window: ${windowId}`);
      return result || { success: false, message: 'No result' };
    } catch (e) {
      LogUtil.error(TAG, `unregisterIsolate failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  getPooledIsolate(windowId: string): number | null {
    if (!this.ready() || this.nativeModule === undefined) {
      return null;
    }

    try {
      const result = this.nativeModule.getPooledIsolate(windowId);
      // Result is a BigInt representing the pointer
      if (result !== null && result !== undefined) {
        return Number(result);
      }
      return null;
    } catch (e) {
      LogUtil.error(TAG, `getPooledIsolate failed: ${JSON.stringify(e)}`);
      return null;
    }
  }

  hasIsolate(windowId: string): boolean {
    if (!this.ready() || this.nativeModule === undefined) {
      return false;
    }

    try {
      return this.nativeModule.hasIsolate(windowId) || false;
    } catch (e) {
      LogUtil.error(TAG, `hasIsolate failed: ${JSON.stringify(e)}`);
      return false;
    }
  }

  getPoolStats(): PoolStats {
    if (!this.ready() || this.nativeModule === undefined) {
      return { active: 0, pooled: 0, total: 0 };
    }

    try {
      const result = this.nativeModule.getPoolStats();
      return result || { active: 0, pooled: 0, total: 0 };
    } catch (e) {
      LogUtil.error(TAG, `getPoolStats failed: ${JSON.stringify(e)}`);
      return { active: 0, pooled: 0, total: 0 };
    }
  }

  clearPool(): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Module not ready' };
    }

    try {
      const result = this.nativeModule.clearPool();
      LogUtil.info(TAG, 'Cleared V8 isolate pool');
      return result || { success: false, message: 'No result' };
    } catch (e) {
      LogUtil.error(TAG, `clearPool failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  sanitizeNodeEnvironment(): NodeEnvironmentSanitizeResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return {
        success: false,
        cleared: 0,
        clearedVariables: '',
        message: 'Module not ready'
      };
    }

    try {
      const result = this.nativeModule.sanitizeNodeEnvironment();
      LogUtil.info(TAG, `sanitizeNodeEnvironment: ${JSON.stringify(result)}`);
      return result || {
        success: false,
        cleared: 0,
        clearedVariables: '',
        message: 'No result'
      };
    } catch (e) {
      LogUtil.error(TAG, `sanitizeNodeEnvironment failed: ${JSON.stringify(e)}`);
      return {
        success: false,
        cleared: 0,
        clearedVariables: '',
        message: `Exception: ${JSON.stringify(e)}`
      };
    }
  }

  installElectronPltHooks(): ElectronPltHookInstallResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return {
        installed: false,
        monitorStarted: false,
        attempts: 0,
        patchedSlots: 0,
        failures: 0,
        runs: 0,
        message: 'Module not ready'
      };
    }

    try {
      const result = this.nativeModule.installElectronPltHooks();
      LogUtil.info(TAG, `installElectronPltHooks: ${JSON.stringify(result)}`);
      return result || {
        installed: false,
        monitorStarted: false,
        attempts: 0,
        patchedSlots: 0,
        failures: 0,
        runs: 0,
        message: 'No result'
      };
    } catch (e) {
      LogUtil.error(TAG, `installElectronPltHooks failed: ${JSON.stringify(e)}`);
      return {
        installed: false,
        monitorStarted: false,
        attempts: 0,
        patchedSlots: 0,
        failures: 0,
        runs: 0,
        message: `Exception: ${JSON.stringify(e)}`
      };
    }
  }

  getElectronPltHookStats(): ElectronPltHookStats {
    if (!this.ready() || this.nativeModule === undefined) {
      return {
        installed: false,
        monitorStarted: false,
        attempts: 0,
        patchedSlots: 0,
        failures: 0,
        runs: 0,
        epollWaitSlots: 0,
        v8InitializeSlots: 0,
        snapshotDecompressSlots: 0,
        arrayNothrowNewSlots: 0,
        scalarNothrowNewSlots: 0,
        arrayDeleteSlots: 0,
        scalarDeleteSlots: 0
      };
    }

    try {
      const result = this.nativeModule.getElectronPltHookStats();
      return result || {
        installed: false,
        monitorStarted: false,
        attempts: 0,
        patchedSlots: 0,
        failures: 0,
        runs: 0,
        epollWaitSlots: 0,
        v8InitializeSlots: 0,
        snapshotDecompressSlots: 0,
        arrayNothrowNewSlots: 0,
        scalarNothrowNewSlots: 0,
        arrayDeleteSlots: 0,
        scalarDeleteSlots: 0
      };
    } catch (e) {
      LogUtil.error(TAG, `getElectronPltHookStats failed: ${JSON.stringify(e)}`);
      return {
        installed: false,
        monitorStarted: false,
        attempts: 0,
        patchedSlots: 0,
        failures: 0,
        runs: 0,
        epollWaitSlots: 0,
        v8InitializeSlots: 0,
        snapshotDecompressSlots: 0,
        arrayNothrowNewSlots: 0,
        scalarNothrowNewSlots: 0,
        arrayDeleteSlots: 0,
        scalarDeleteSlots: 0
      };
    }
  }
}

// Singleton instance
let hookInstance: V8PoolHook | undefined = undefined;

export function getV8PoolHook(): V8PoolHook {
  if (!hookInstance) {
    hookInstance = new V8PoolHookImpl();
  }
  return hookInstance;
}

export default V8PoolHookImpl;
