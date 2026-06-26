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

export interface V8InitializeHookConfig {
  enabled: boolean;
  serialize: boolean;
  startupFlags: string;
  startupFlagsApplied: boolean;
  startupFlagsResolveFailed: boolean;
}

export interface V8InitializeHookStats extends V8InitializeHookConfig {
  realSymbolResolved: boolean;
  snapshotMmapFallbackEnabled: boolean;
  snapshotMmapMinBytes: number;
  snapshotMmapMaxBytes: number;
  targetCallOffset: number;
  targetReturnOffset: number;
  lastCallerOffset: number;
  activeInitializations: number;
  maxConcurrentInitializations: number;
  initializeCalls: number;
  targetHits: number;
  serializedCalls: number;
  passThroughCalls: number;
  failedCalls: number;
  snapshotNothrowNewCalls: number;
  snapshotNothrowNewFailures: number;
  snapshotMmapFallbacks: number;
  snapshotMmapFallbackFailures: number;
  snapshotMmapDeletes: number;
  snapshotMmapBytes: number;
  snapshotDecompressCalls: number;
  snapshotDecompressBytesIn: number;
  lastSnapshotCompressedSize: number;
  lastSnapshotDecompressedSize: number;
  lastSnapshotAllocCallerOffset: number;
}

export interface ElectronPltHookInstallResult {
  installed: boolean;
  monitorStarted: boolean;
  attempts: number;
  patchedSlots: number;
  failures: number;
  runs: number;
}

export interface ElectronPltHookStats extends ElectronPltHookInstallResult {
  epollWaitSlots: number;
  v8InitializeSlots: number;
  snapshotDecompressSlots: number;
  arrayNothrowNewSlots: number;
  scalarNothrowNewSlots: number;
  arrayDeleteSlots: number;
  scalarDeleteSlots: number;
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
export const configureV8InitializeHook: (
  enabled?: boolean,
  serialize?: boolean
) => V8InitializeHookConfig;
export const getV8InitializeHookStats: () => V8InitializeHookStats;
export const resetV8InitializeHookStats: () => { success: boolean };
export const installElectronPltHooks: () => ElectronPltHookInstallResult;
export const getElectronPltHookStats: () => ElectronPltHookStats;
