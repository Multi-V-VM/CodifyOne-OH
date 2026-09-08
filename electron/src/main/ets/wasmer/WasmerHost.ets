import native from 'libwasmerhost.so';
import { fileIo } from '@kit.CoreFileKit';
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

    const ok: boolean = native.startWasmer() as boolean;
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
  const requestPath: string = '/data/storage/el2/base/files/wasmer/run-request.txt';
  const payload: string = modulePath + '\n' + WASMER_WORKSPACE_DIR + '\n' + args.join('\n');
  const request = fileIo.openSync(requestPath,
    fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
  fileIo.writeSync(request.fd, payload);
  fileIo.closeSync(request);
  return native.startWasmer() as WasiRunResult;
}

export async function runWasiModuleAsync(modulePath: string, args: string[] = []): Promise<WasiRunResult> {
  return Promise.resolve(runWasiModule(modulePath, args));
}
