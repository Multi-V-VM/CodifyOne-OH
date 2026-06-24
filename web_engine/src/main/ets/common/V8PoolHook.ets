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
}

export interface PoolStats {
  active: number;
  pooled: number;
  total: number;
}

export interface PoolOperationResult {
  success: boolean;
  cleared?: number;
  [key: string]: any;
}

class V8PoolHookImpl implements V8PoolHook {
  private nativeModule: any = undefined;
  private isReady: boolean = false;

  constructor() {
    this.loadNativeModule();
  }

  private loadNativeModule(): void {
    try {
      // The v8poolhook module is built into libentry.so
      this.nativeModule = entry;
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
    if (!this.ready()) {
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
    if (!this.ready()) {
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
    if (!this.ready()) {
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
    if (!this.ready()) {
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
    if (!this.ready()) {
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
    if (!this.ready()) {
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
    if (!this.ready()) {
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
