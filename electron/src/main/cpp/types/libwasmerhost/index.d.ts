export interface WasiRunResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  error: string;
}

export function startWasmer(): boolean | WasiRunResult;
export const isWasmerReady: () => boolean;
export const getWasmerLastError: () => string;
export const runWasiModule: (modulePath: string, args?: string[], preopenDir?: string) => WasiRunResult;
export const runWasiModuleAsync: (modulePath: string, args?: string[], preopenDir?: string) => Promise<WasiRunResult>;
