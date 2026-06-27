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

export interface NodeEnvironmentSanitizeResult {
  success: boolean;
  cleared: number;
  clearedVariables: string;
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
  nodeNewContextSlots?: number;
  nodeNewContextCalls?: number;
  nodeNewContextNulls?: number;
  lastNodeNewContextIsolate?: number;
  lastNodeNewContextTemplate?: number;
  lastNodeNewContextResult?: number;
  nodeCreateEnvironmentSlots?: number;
  nodeCreateEnvironmentCalls?: number;
  nodeCreateEnvironmentNulls?: number;
  lastNodeCreateEnvironmentIsolateData?: number;
  lastNodeCreateEnvironmentContext?: number;
  lastNodeCreateEnvironmentResult?: number;
  nodeLoadEnvironmentStringSlots?: number;
  nodeLoadEnvironmentCallbackSlots?: number;
  nodeLoadEnvironmentStringCalls?: number;
  nodeLoadEnvironmentCallbackCalls?: number;
  nodeLoadEnvironmentNulls?: number;
  nodePostLoadTraceAttempts?: number;
  nodePostLoadTraceSuccesses?: number;
  nodePostLoadTraceFailures?: number;
  lastNodeLoadEnvironmentEnv?: number;
  lastNodeLoadEnvironmentSource?: number;
  lastNodeLoadEnvironmentPreload?: number;
  lastNodeLoadEnvironmentResult?: number;
  v8ObjectSetPrivateSlots?: number;
  v8ObjectSetPrivateCalls?: number;
  browserAppSearchPathPatchAttempts?: number;
  browserAppSearchPathPatches?: number;
  browserAppSearchPathPatchFailures?: number;
  browserAppSearchPathAsarOnlyPatches?: number;
  browserAppSearchPathNameReadFailures?: number;
  entryPathProbeOpenSlots?: number;
  entryPathProbeFopenSlots?: number;
  entryPathProbeAccessSlots?: number;
  entryPathProbeStatSlots?: number;
  entryPathProbeLstatSlots?: number;
  entryPathProbeHits?: number;
  entryPathProbePackageJsonHits?: number;
  entryPathProbeEntryJsHits?: number;
  entryPathProbeOutMainHits?: number;
  entryPathProbeElectronMainHits?: number;
  entryPathProbeNullSlotPatches?: number;
  lastEntryPathProbeOp?: string;
  lastEntryPathProbePath?: string;
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
export const sanitizeNodeEnvironment: () => NodeEnvironmentSanitizeResult;
