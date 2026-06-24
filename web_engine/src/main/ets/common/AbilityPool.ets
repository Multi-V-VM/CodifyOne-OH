// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { injectable } from 'inversify';
import common from '@ohos.app.ability.common';
import { NewWindowParam, WindowBound, WindowPreferences } from '../interface/CommonInterface';
import { AbilityManager } from './AbilityManager';
import LogUtil from '../utils/LogUtil';
import Inject from './InjectModule';
import { getWindowPoolNative, NativePoolStats } from './WindowPoolNative';

const TAG: string = 'AbilityPool';

/**
 * Interface representing a pooled ability waiting to be reused
 */
export interface PooledAbility {
  windowId: string;
  abilityType: string; // 'EntryAbility' or 'StatelessAbility'
  context: common.UIAbilityContext;
  lastUsed: number;
  preferences: WindowPreferences;
  bounds: WindowBound;
  url?: string; // URL associated with this window
}

/**
 * Configuration options for the ability pool
 */
export interface PoolConfig {
  maxPoolSize?: number;        // Max idle abilities per type
  maxIdleTime?: number;        // Max idle time in ms before cleanup
  cleanupInterval?: number;    // Cleanup timer interval in ms
  enableMemoryPressure?: boolean; // Enable memory pressure monitoring
  lowMemoryThreshold?: number; // Threshold for low memory cleanup
  adaptiveSizing?: boolean;   // Enable adaptive pool sizing
}

/**
 * Pool statistics for monitoring
 */
export interface AbilityPoolStats {
  totalIdle: number;
  active: number;
  entryAbilityIdle: number;
  statelessAbilityIdle: number;
  totalEverCreated: number;
  totalReused: number;
  memoryPressureLevel: 'low' | 'normal' | 'high';
}

/**
 * Manages pooling of abilities to prevent V8 isolate churn
 * Adapts the SubWindowAdapter pooling pattern for main abilities
 */
@injectable()
export class AbilityPool {
  private abilityManager: AbilityManager;
  private nativePool = getWindowPoolNative();
  private idlePool: Map<string, PooledAbility[]> = new Map();
  private activePool: Map<string, PooledAbility> = new Map();

  // Pool configuration
  private maxPoolSize: number = 2;  // Reduced to limit memory usage
  private baseMaxPoolSize: number = 2; // Original max for adaptive sizing
  private maxIdleTime: number = 120000; // 2 minutes - more aggressive cleanup
  private cleanupInterval: number = 60000; // 1 minute

  // Memory pressure monitoring
  private enableMemoryPressure: boolean = true;
  private lowMemoryThreshold: number = 200; // MB
  private memoryPressureLevel: 'low' | 'normal' | 'high' = 'normal';

  // Adaptive sizing
  private adaptiveSizing: boolean = true;

  // Statistics
  private totalEverCreated: number = 0;
  private totalReused: number = 0;

  // Cleanup timer
  private cleanupTimer: number = -1;

  constructor() {
    this.abilityManager = Inject.get(AbilityManager);
    this.startCleanupTimer();
    this.startMemoryMonitoring();
  }

  /**
   * Acquires an ability from the pool or returns null if none available
   * @param abilityType The type of ability to acquire ('EntryAbility' or 'StatelessAbility')
   * @param params The window parameters for matching
   * @returns A pooled ability or null if pool is empty
   */
  acquire(abilityType: string, params: NewWindowParam): PooledAbility | null {
    // Check memory pressure before acquiring
    if (this.memoryPressureLevel === 'high') {
      LogUtil.warn(TAG, 'High memory pressure - forcing pool cleanup');
      this.emergencyCleanup();
    }

    const matching = this.findMatchingAbility(abilityType, params);
    if (matching) {
      // Track reuse statistics
      this.totalReused++;

      // Update last used time
      matching.lastUsed = Date.now();

      // Move from idle to active pool
      const idleList = this.idlePool.get(abilityType);
      if (idleList) {
        const idx = idleList.indexOf(matching);
        if (idx !== -1) {
          idleList.splice(idx, 1);
        }
      }

      // Update window ID for new use
      matching.windowId = params.window_id;
      this.activePool.set(params.window_id, matching);

      LogUtil.info(TAG, `Acquired pooled ability: ${matching.windowId} (type: ${abilityType})`);
      LogUtil.info(TAG, `Pool stats - Idle: ${this.getIdleCount(abilityType)}, Active: ${this.activePool.size}, Reuse rate: ${this.getReuseRate()}%`);
      return matching;
    }
    // Track new creation
    this.totalEverCreated++;
    return null;
  }

  /**
   * Releases an ability back to the pool instead of destroying it
   * Uses very conservative strategy - only pool EntryAbility, destroy stateless
   * @param windowId The window ID to release
   */
  release(windowId: string): void {
    const pooled = this.activePool.get(windowId);
    if (!pooled) {
      LogUtil.warn(TAG, `Ability not found in active pool: ${windowId}`);
      return;
    }

    // More aggressive: always destroy stateless abilities, only consider pooling EntryAbility
    if (pooled.abilityType !== 'EntryAbility') {
      LogUtil.info(TAG, `Destroying ${pooled.abilityType} (only EntryAbility is pooled)`);
      this.destroyAbility(pooled);
      this.activePool.delete(windowId);
      this.nativePool.notifyDestroyed(windowId);
      return;
    }

    // Check if pool is at capacity (very conservative)
    const idleList = this.idlePool.get(pooled.abilityType) || [];
    if (idleList.length >= this.maxPoolSize) {
      LogUtil.info(TAG, `Pool at capacity for ${pooled.abilityType}, destroying ability: ${windowId}`);
      this.destroyAbility(pooled);
      this.activePool.delete(windowId);
      this.nativePool.notifyDestroyed(windowId);
      return;
    }

    // Move to idle pool
    pooled.lastUsed = Date.now();
    idleList.push(pooled);
    this.idlePool.set(pooled.abilityType, idleList);
    this.activePool.delete(windowId);

    this.nativePool.poolWindow(windowId, 0, pooled.url ?? '');

    LogUtil.info(TAG, `Released ability to pool: ${windowId} (type: ${pooled.abilityType})`);
    LogUtil.info(TAG, `Pool stats - Idle: ${idleList.length}, Active: ${this.activePool.size}`);
  }

  /**
   * Notifies the pool that an ability was destroyed outside of pool control
   * @param windowId The window ID that was destroyed
   */
  notifyDestroyed(windowId: string): void {
    const pooled = this.activePool.get(windowId);
    if (pooled) {
      this.activePool.delete(windowId);
      LogUtil.info(TAG, `Notified of destroyed ability: ${windowId}`);
    }

    // Notify native pool of destruction
    this.nativePool.notifyDestroyed(windowId);

    // Also check idle pool
    this.idlePool.forEach((list, abilityType) => {
      const idx = list.findIndex(p => p.windowId === windowId);
      if (idx !== -1) {
        list.splice(idx, 1);
        this.idlePool.set(abilityType, list);
        LogUtil.info(TAG, `Removed destroyed ability from idle pool: ${windowId}`);
        // Notify native pool
        this.nativePool.notifyDestroyed(windowId);
      }
    });
  }

  /**
   * Starts the cleanup timer to remove stale idle abilities
   */
  private startCleanupTimer(): void {
    if (this.cleanupTimer !== -1) {
      return; // Already started
    }

    this.cleanupTimer = setInterval(() => {
      this.cleanupStaleAbilities();
    }, this.cleanupInterval);

    LogUtil.info(TAG, `Started cleanup timer (interval: ${this.cleanupInterval}ms)`);
  }

  /**
   * Stops the cleanup timer
   */
  stopCleanupTimer(): void {
    if (this.cleanupTimer !== -1) {
      clearInterval(this.cleanupTimer);
      this.cleanupTimer = -1;
      LogUtil.info(TAG, 'Stopped cleanup timer');
    }
  }

  /**
   * Removes abilities that have been idle too long
   */
  private cleanupStaleAbilities(): void {
    const now = Date.now();
    let totalCleaned = 0;

    this.idlePool.forEach((list, abilityType) => {
      const keep: PooledAbility[] = [];

      for (const pooled of list) {
        const idleTime = now - pooled.lastUsed;
        if (idleTime > this.maxIdleTime) {
          LogUtil.info(TAG, `Cleaning up stale ability: ${pooled.windowId} (idle: ${idleTime}ms)`);
          this.destroyAbility(pooled);
          totalCleaned++;
        } else {
          keep.push(pooled);
        }
      }

      this.idlePool.set(abilityType, keep);
    });

    if (totalCleaned > 0) {
      LogUtil.info(TAG, `Cleaned up ${totalCleaned} stale abilities`);
    }
  }

  /**
   * Destroys an ability and its resources
   */
  private destroyAbility(pooled: PooledAbility): void {
    // Notify native pool before destruction
    this.nativePool.notifyDestroyed(pooled.windowId);

    try {
      pooled.context?.terminateSelf()
        .catch((err: Error) => {
          LogUtil.error(TAG, `Failed to destroy pooled ability: ${pooled.windowId}, err: ${JSON.stringify(err)}`);
        });
    } catch (e) {
      LogUtil.error(TAG, `Exception destroying ability: ${JSON.stringify(e)}`);
    }
  }

  /**
   * Finds a matching ability from the idle pool
   * Uses size-based matching similar to SubWindowAdapter
   */
  private findMatchingAbility(abilityType: string, params: NewWindowParam): PooledAbility | null {
    const idleList = this.idlePool.get(abilityType);
    if (!idleList || idleList.length === 0) {
      return null;
    }

    // Size-based matching with tolerance (similar to SubWindowAdapter's maxDistance)
    const maxDistance: number = 2;

    for (let i = idleList.length - 1; i >= 0; i--) {
      const pooled = idleList[i];
      const bounds = pooled.bounds;

      // Check if size matches within tolerance
      if (Math.abs(bounds.width - params.bounds.width) <= maxDistance &&
          Math.abs(bounds.height - params.bounds.height) <= maxDistance) {
        return pooled;
      }
    }

    // No exact size match, return the most recently used
    return idleList[idleList.length - 1] || null;
  }

  /**
   * Gets the count of idle abilities for a given type
   */
  getIdleCount(abilityType: string): number {
    return this.idlePool.get(abilityType)?.length || 0;
  }

  /**
   * Gets the total count of idle abilities across all types
   */
  getTotalIdleCount(): number {
    let total = 0;
    this.idlePool.forEach(list => {
      total += list.length;
    });
    return total;
  }

  /**
   * Gets the count of active abilities
   */
  getActiveCount(): number {
    return this.activePool.size;
  }

  /**
   * Gets pool statistics for debugging
   */
  getPoolStats(): Record<string, number> {
    const stats: Record<string, number> = {
      'totalIdle': 0,
      'active': this.activePool.size,
      'EntryAbility': 0,
      'StatelessAbility': 0
    };

    this.idlePool.forEach((list, abilityType) => {
      if (abilityType === 'EntryAbility') {
        stats['EntryAbility'] = list.length;
      } else if (abilityType === 'StatelessAbility') {
        stats['StatelessAbility'] = list.length;
      }
      stats['totalIdle'] += list.length;
    });

    return stats;
  }

  /**
   * Clears all abilities from the pool (useful for testing or shutdown)
   */
  clear(): void {
    LogUtil.info(TAG, 'Clearing ability pool');

    // Destroy all idle abilities
    this.idlePool.forEach(list => {
      list.forEach(pooled => {
        this.destroyAbility(pooled);
      });
    });

    // Note: We don't destroy active abilities as they're still in use
    this.idlePool.clear();
    this.activePool.clear();

    this.totalEverCreated = 0;
    this.totalReused = 0;

    LogUtil.info(TAG, 'Pool cleared');
  }

  /**
   * Starts memory pressure monitoring
   * More frequent checks to catch memory pressure earlier
   */
  private startMemoryMonitoring(): void {
    if (!this.enableMemoryPressure) {
      return;
    }

    // Check memory every 10 seconds (more frequent)
    setInterval(() => {
      this.checkMemoryPressure();
    }, 10000);

    LogUtil.info(TAG, 'Memory pressure monitoring started (10s interval)');
  }

  /**
   * Checks current memory pressure and adjusts pool accordingly
   * Uses more aggressive strategy to prevent V8 OOM
   */
  private checkMemoryPressure(): void {
    const totalAbilities = this.getTotalIdleCount() + this.getActiveCount();

    // Much more aggressive - any idle abilities when we have active ones indicates potential pressure
    if (this.getActiveCount() > 0 && this.getTotalIdleCount() > 0) {
      this.memoryPressureLevel = 'high';
      // Destroy ALL idle abilities when any are active to minimize memory
      LogUtil.warn(TAG, `Memory pressure detected - clearing all idle abilities`);
      this.clearIdlePool();
      this.maxPoolSize = 1; // Minimize pool size
    } else if (totalAbilities > this.maxPoolSize) {
      this.memoryPressureLevel = 'normal';
      this.emergencyCleanup();
    } else {
      this.memoryPressureLevel = 'low';
    }
  }

  /**
   * Clear only idle pool without affecting active abilities
   */
  private clearIdlePool(): void {
    let totalCleared = 0;
    this.idlePool.forEach((list, abilityType) => {
      list.forEach(pooled => {
        this.destroyAbility(pooled);
        totalCleared++;
      });
    });
    this.idlePool.clear();
    LogUtil.warn(TAG, `Cleared ${totalCleared} idle abilities from pool`);
  }

  /**
   * Emergency cleanup when memory is critically low
   */
  private emergencyCleanup(): void {
    LogUtil.warn(TAG, 'Emergency cleanup triggered - destroying oldest pooled abilities');

    // Destroy half of the idle abilities
    this.idlePool.forEach((list, abilityType) => {
      const toDestroy = Math.floor(list.length / 2);
      for (let i = 0; i < toDestroy; i++) {
        const pooled = list.shift();
        if (pooled) {
          LogUtil.info(TAG, `Emergency destroying: ${pooled.windowId}`);
          this.destroyAbility(pooled);
        }
      }
    });
  }

  /**
   * Gets detailed pool statistics
   */
  getDetailedStats(): AbilityPoolStats {
    const entryIdle = this.getIdleCount('EntryAbility');
    const statelessIdle = this.getIdleCount('StatelessAbility');

    const stats: AbilityPoolStats = {
      totalIdle: this.getTotalIdleCount(),
      active: this.getActiveCount(),
      entryAbilityIdle: entryIdle,
      statelessAbilityIdle: statelessIdle,
      totalEverCreated: this.totalEverCreated,
      totalReused: this.totalReused,
      memoryPressureLevel: this.memoryPressureLevel
    };
    return stats;
  }

  /**
   * Gets the reuse rate as a percentage
   */
  private getReuseRate(): number {
    if (this.totalEverCreated === 0) {
      return 0;
    }
    return Math.round((this.totalReused / this.totalEverCreated) * 100);
  }

  /**
   * Manually triggers an immediate pool cleanup
   */
  forceCleanup(): void {
    LogUtil.info(TAG, 'Manual cleanup triggered');
    this.cleanupStaleAbilities();
  }

  /**
   * Adjusts pool configuration at runtime
   */
  updateConfig(config: Partial<PoolConfig>): void {
    if (config.maxPoolSize !== undefined) {
      this.baseMaxPoolSize = config.maxPoolSize;
      this.maxPoolSize = config.maxPoolSize;
    }
    if (config.maxIdleTime !== undefined) {
      this.maxIdleTime = config.maxIdleTime;
    }
    if (config.enableMemoryPressure !== undefined) {
      this.enableMemoryPressure = config.enableMemoryPressure;
    }
    LogUtil.info(TAG, `Pool config updated: maxPoolSize=${this.maxPoolSize}, maxIdleTime=${this.maxIdleTime}`);
  }
}
