// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { inject, injectable } from 'inversify';
import common from '@ohos.app.ability.common';
import { NewWindowParam, WindowBound, WindowPreferences } from '../interface/CommonInterface';
import { AbilityManager } from './AbilityManager';
import LogUtil from '../utils/LogUtil';

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
export interface PoolStats {
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
  private idlePool: Map<string, PooledAbility[]> = new Map();
  private activePool: Map<string, PooledAbility> = new Map();

  // Pool configuration
  private maxPoolSize: number = 5;
  private baseMaxPoolSize: number = 5; // Original max for adaptive sizing
  private maxIdleTime: number = 300000; // 5 minutes
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

  constructor(
    @inject(AbilityManager) abilityManager: AbilityManager,
    config?: PoolConfig
  ) {
    this.abilityManager = abilityManager;

    if (config) {
      this.maxPoolSize = config.maxPoolSize ?? this.maxPoolSize;
      this.baseMaxPoolSize = this.maxPoolSize;
      this.maxIdleTime = config.maxIdleTime ?? this.maxIdleTime;
      this.cleanupInterval = config.cleanupInterval ?? this.cleanupInterval;
      this.enableMemoryPressure = config.enableMemoryPressure ?? this.enableMemoryPressure;
      this.lowMemoryThreshold = config.lowMemoryThreshold ?? this.lowMemoryThreshold;
      this.adaptiveSizing = config.adaptiveSizing ?? this.adaptiveSizing;
    }

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
   * @param windowId The window ID to release
   */
  release(windowId: string): void {
    const pooled = this.activePool.get(windowId);
    if (!pooled) {
      LogUtil.warn(TAG, `Ability not found in active pool: ${windowId}`);
      return;
    }

    // Check if pool is at capacity
    const idleList = this.idlePool.get(pooled.abilityType) || [];
    if (idleList.length >= this.maxPoolSize) {
      LogUtil.info(TAG, `Pool at capacity for ${pooled.abilityType}, destroying ability: ${windowId}`);
      this.destroyAbility(pooled);
      this.activePool.delete(windowId);
      return;
    }

    // Move to idle pool
    pooled.lastUsed = Date.now();
    idleList.push(pooled);
    this.idlePool.set(pooled.abilityType, idleList);
    this.activePool.delete(windowId);

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

    // Also check idle pool
    this.idlePool.forEach((list, abilityType) => {
      const idx = list.findIndex(p => p.windowId === windowId);
      if (idx !== -1) {
        list.splice(idx, 1);
        this.idlePool.set(abilityType, list);
        LogUtil.info(TAG, `Removed destroyed ability from idle pool: ${windowId}`);
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
      totalIdle: 0,
      active: this.activePool.size
    };

    this.idlePool.forEach((list, abilityType) => {
      stats[abilityType] = list.length;
      stats.totalIdle += list.length;
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
   */
  private startMemoryMonitoring(): void {
    if (!this.enableMemoryPressure) {
      return;
    }

    // Check memory every 30 seconds
    setInterval(() => {
      this.checkMemoryPressure();
    }, 30000);

    LogUtil.info(TAG, 'Memory pressure monitoring started');
  }

  /**
   * Checks current memory pressure and adjusts pool accordingly
   */
  private checkMemoryPressure(): void {
    // This is a simplified check - in real implementation you'd query actual memory
    // For now, use pool size as a proxy for memory pressure
    const totalAbilities = this.getTotalIdleCount() + this.getActiveCount();

    if (totalAbilities > this.maxPoolSize * 2) {
      this.memoryPressureLevel = 'high';
      if (this.adaptiveSizing) {
        this.maxPoolSize = Math.max(2, Math.floor(this.baseMaxPoolSize / 2));
        LogUtil.warn(TAG, `High memory pressure - Reduced pool size to ${this.maxPoolSize}`);
      }
      this.emergencyCleanup();
    } else if (totalAbilities > this.maxPoolSize * 1.5) {
      this.memoryPressureLevel = 'normal';
      if (this.adaptiveSizing && this.maxPoolSize < this.baseMaxPoolSize) {
        this.maxPoolSize = Math.min(this.baseMaxPoolSize, this.maxPoolSize + 1);
        LogUtil.info(TAG, `Memory pressure normal - Adjusted pool size to ${this.maxPoolSize}`);
      }
    } else {
      this.memoryPressureLevel = 'low';
      if (this.adaptiveSizing && this.maxPoolSize < this.baseMaxPoolSize) {
        this.maxPoolSize = Math.min(this.baseMaxPoolSize, this.maxPoolSize + 1);
      }
    }
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
  getDetailedStats(): PoolStats {
    const entryIdle = this.getIdleCount('EntryAbility');
    const statelessIdle = this.getIdleCount('StatelessAbility');

    return {
      totalIdle: this.getTotalIdleCount(),
      active: this.getActiveCount(),
      entryAbilityIdle: entryIdle,
      statelessAbilityIdle: statelessIdle,
      totalEverCreated: this.totalEverCreated,
      totalReused: this.totalReused,
      memoryPressureLevel: this.memoryPressureLevel
    };
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
