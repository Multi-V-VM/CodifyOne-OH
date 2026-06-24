export interface WasiRunResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  error: string;
}

export const startWasmer: () => boolean;
export const isWasmerReady: () => boolean;
export const getWasmerLastError: () => string;
export const runWasiModule: (modulePath: string, args?: string[], preopenDir?: string) => WasiRunResult;
