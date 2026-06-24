export interface WasiRunResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  error: string;
}

export interface EpollHookConfig {
  enabled: boolean;
  maxWaitMs: number;
  requireChromeIoThreadName: boolean;
}

export interface EpollHookStats extends EpollHookConfig {
  targetCallOffset: number;
  targetReturnOffset: number;
  lastCallerOffset: number;
  lastOriginalTimeoutMs: number;
  lastEffectiveTimeoutMs: number;
  targetHits: number;
  clampedHits: number;
  passThroughHits: number;
}

export const startWasmer: () => boolean;
export const isWasmerReady: () => boolean;
export const getWasmerLastError: () => string;
export const runWasiModule: (modulePath: string, args?: string[], preopenDir?: string) => WasiRunResult;
export const configureEpollHook: (
  enabled?: boolean,
  maxWaitMs?: number,
  requireChromeIoThreadName?: boolean
) => EpollHookConfig;
export const getEpollHookStats: () => EpollHookStats;
export const resetEpollHookStats: () => { success: boolean };
