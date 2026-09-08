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
  const donePath: string = '/data/storage/el2/base/files/wasmer/run-done.txt';
  try {
    if (fileIo.accessSync(donePath)) {
      fileIo.unlinkSync(donePath);
    }
  } catch (_) { }
  const payload: string = modulePath + '\n' + WASMER_WORKSPACE_DIR + '\n' + args.join('\n');
  const request = fileIo.openSync(requestPath,
    fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
  fileIo.writeSync(request.fd, payload);
  fileIo.closeSync(request);
  native.startWasmer();
  const result: WasiRunResult = {
    exitCode: Number(fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-exit.txt')),
    stdout: fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-stdout.txt'),
    stderr: fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-stderr.txt'),
    error: fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-error.txt')
  };
  return result;
}

export async function runWasiModuleAsync(modulePath: string, args: string[] = []): Promise<WasiRunResult> {
  ensureWasmerWorkspace();
  const requestPath: string = '/data/storage/el2/base/files/wasmer/run-request.txt';
  const donePath: string = '/data/storage/el2/base/files/wasmer/run-done.txt';
  try {
    if (fileIo.accessSync(donePath)) {
      fileIo.unlinkSync(donePath);
    }
  } catch (_) { }
  const payload: string = modulePath + '\n' + WASMER_WORKSPACE_DIR + '\n' + args.join('\n');
  const request = fileIo.openSync(requestPath,
    fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
  fileIo.writeSync(request.fd, payload);
  fileIo.closeSync(request);
  native.startWasmer();

  return new Promise<WasiRunResult>((resolve, reject) => {
    let waitedMs: number = 0;
    const timer: number = setInterval(() => {
      waitedMs += 100;
      try {
        if (fileIo.accessSync(donePath)) {
          fileIo.unlinkSync(donePath);
          clearInterval(timer);
          resolve({
            exitCode: Number(fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-exit.txt')),
            stdout: fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-stdout.txt'),
            stderr: fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-stderr.txt'),
            error: fileIo.readTextSync('/data/storage/el2/base/files/wasmer/run-error.txt')
          });
          return;
        }
      } catch (_) { }
      if (waitedMs >= 180000) {
        clearInterval(timer);
        reject(new Error('isolated Wasmer runner timed out after 180s'));
      }
    }, 100);
  });
}
