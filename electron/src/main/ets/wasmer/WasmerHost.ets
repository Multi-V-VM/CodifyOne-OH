import native from 'libentry.so';
import { ensureWasmerWorkspace, WASMER_WORKSPACE_DIR } from './Workspace';

export interface WasiRunResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  error: string;
}

let started: boolean = false;
let starting: Promise<boolean> | null = null;

export async function startWasmer(): Promise<boolean> {
  if (started) {
    return true;
  }

  if (starting) {
    return starting;
  }

  starting = (async (): Promise<boolean> => {
    ensureWasmerWorkspace();

    const ok: boolean = native.startWasmer();
    started = ok;

    if (ok) {
      console.info('[Wasmer] runtime ready');
    } else {
      console.error(`[Wasmer] runtime unavailable: ${native.getWasmerLastError()}`);
    }

    return ok;
  })().finally(() => {
    starting = null;
  });

  return starting;
}

export function isWasmerReady(): boolean {
  return native.isWasmerReady();
}

export function getWasmerLastError(): string {
  return native.getWasmerLastError();
}

export function runWasiModule(modulePath: string, args: string[] = []): WasiRunResult {
  ensureWasmerWorkspace();
  return native.runWasiModule(modulePath, args, WASMER_WORKSPACE_DIR) as WasiRunResult;
}
