// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Native NDK wrapper for window pooling

import entry from 'libentry.so';
import LogUtil from '../utils/LogUtil';

const TAG: string = 'WindowPoolNative';

/**
 * Result of acquiring a window from the pool
 */
export interface AcquireResult {
  found: boolean;
  windowId?: string;
  url?: string;
  originWindowId?: number;
  message?: string;
}

/**
 * Pool statistics
 */
export interface NativePoolStats {
  totalIdle: number;
  active: number;
  maxPoolSize: number;
  enabled: boolean;
}

/**
 * Result of pool operations
 */
export interface PoolOperationResult {
  success: boolean;
  message?: string;
  clearedCount?: number;
  enabled?: boolean;
  maxPoolSize?: number;
}

/**
 * Native module interface for window pool
 */
interface NativePoolModule {
  poolWindow: (windowId: string, originWindowId: number, url?: string) => PoolOperationResult;
  acquireFromPool: (abilityType: string) => AcquireResult;
  notifyDestroyed: (windowId: string) => PoolOperationResult;
  getPoolStats: () => NativePoolStats;
  configurePool: (enabled: boolean, maxPoolSize: number) => PoolOperationResult;
  clearPool: () => PoolOperationResult;
  reuseWindow: (windowId: string, url?: string, boundsLeft?: number, boundsTop?: number, boundsWidth?: number) => PoolOperationResult;
}

/**
 * Native window pool handler
 * Interfaces with the NDK module for window tracking and reuse
 */
export class WindowPoolNative {
  private nativeModule: NativePoolModule | undefined = undefined;
  private isReady: boolean = false;

  constructor() {
    this.loadNativeModule();
  }

  /**
   * Load the native windowpool module
   */
  private loadNativeModule(): void {
    try {
      // The native module is imported from libentry.so
      this.nativeModule = entry as NativePoolModule;
      if (this.nativeModule) {
        this.isReady = true;
        LogUtil.info(TAG, 'Native window pool module loaded successfully');
      } else {
        LogUtil.warn(TAG, 'Native window pool module not available - using fallback');
      }
    } catch (e) {
      LogUtil.warn(TAG, `Failed to load native window pool module: ${JSON.stringify(e)}`);
    }
  }

  /**
   * Check if native module is available
   */
  ready(): boolean {
    return this.isReady && this.nativeModule !== undefined;
  }

  /**
   * Register a window as available for pooling
   * @param windowId The window ID to pool
   * @param originWindowId The native window ID
   * @param url Optional URL that was loaded
   */
  poolWindow(windowId: string, originWindowId: number, url?: string): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Native module not ready' };
    }

    try {
      const result = this.nativeModule.poolWindow(windowId, originWindowId, url || '');
      return result || { success: false, message: 'No result from native module' };
    } catch (e) {
      LogUtil.error(TAG, `poolWindow failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  /**
   * Acquire a window from the pool for reuse
   * @param abilityType The type of ability ('EntryAbility' or 'StatelessAbility')
   */
  acquireFromPool(abilityType: string): AcquireResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { found: false, message: 'Native module not ready' };
    }

    try {
      const result = this.nativeModule.acquireFromPool(abilityType);
      return result || { found: false, message: 'No result from native module' };
    } catch (e) {
      LogUtil.error(TAG, `acquireFromPool failed: ${JSON.stringify(e)}`);
      return { found: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  /**
   * Notify that a window was destroyed (not pooled)
   * @param windowId The window ID that was destroyed
   */
  notifyDestroyed(windowId: string): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Native module not ready' };
    }

    try {
      const result = this.nativeModule.notifyDestroyed(windowId);
      return result || { success: false, message: 'No result from native module' };
    } catch (e) {
      LogUtil.error(TAG, `notifyDestroyed failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  /**
   * Get current pool statistics
   */
  getPoolStats(): NativePoolStats {
    if (!this.ready() || this.nativeModule === undefined) {
      return { totalIdle: 0, active: 0, maxPoolSize: 5, enabled: false };
    }

    try {
      const result = this.nativeModule.getPoolStats();
      return result || { totalIdle: 0, active: 0, maxPoolSize: 5, enabled: false };
    } catch (e) {
      LogUtil.error(TAG, `getPoolStats failed: ${JSON.stringify(e)}`);
      return { totalIdle: 0, active: 0, maxPoolSize: 5, enabled: false };
    }
  }

  /**
   * Configure pool settings
   * @param enabled Whether pooling is enabled
   * @param maxPoolSize Maximum number of windows to keep in pool
   */
  configurePool(enabled: boolean, maxPoolSize: number): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Native module not ready' };
    }

    try {
      const result = this.nativeModule.configurePool(enabled, maxPoolSize);
      return result || { success: false, message: 'No result from native module' };
    } catch (e) {
      LogUtil.error(TAG, `configurePool failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  /**
   * Clear all windows from pool
   */
  clearPool(): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Native module not ready' };
    }

    try {
      const result = this.nativeModule.clearPool();
      return result || { success: false, message: 'No result from native module' };
    } catch (e) {
      LogUtil.error(TAG, `clearPool failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }

  /**
   * Handle window reuse at native level
   * This is a placeholder for when libelectron.so source is available
   *
   * @param windowId The window ID to reuse
   * @param url Optional new URL to load
   * @param boundsLeft Optional window left position
   * @param boundsTop Optional window top position
   * @param boundsWidth Optional window width
   */
  reuseWindow(windowId: string, url?: string, boundsLeft?: number, boundsTop?: number, boundsWidth?: number): PoolOperationResult {
    if (!this.ready() || this.nativeModule === undefined) {
      return { success: false, message: 'Native module not ready' };
    }

    try {
      LogUtil.info(TAG, `reuseWindow called: windowId=${windowId}, url=${url || 'none'}`);
      const result = this.nativeModule.reuseWindow(windowId, url || '', boundsLeft || 0, boundsTop || 0, boundsWidth || 0);
      return result || { success: false, message: 'No result from native module' };
    } catch (e) {
      LogUtil.error(TAG, `reuseWindow failed: ${JSON.stringify(e)}`);
      return { success: false, message: `Exception: ${JSON.stringify(e)}` };
    }
  }
}

// Singleton instance
let nativePoolInstance: WindowPoolNative | undefined = undefined;

/**
 * Get the singleton WindowPoolNative instance
 */
export function getWindowPoolNative(): WindowPoolNative {
  if (!nativePoolInstance) {
    nativePoolInstance = new WindowPoolNative();
  }
  return nativePoolInstance;
}

export default WindowPoolNative;
