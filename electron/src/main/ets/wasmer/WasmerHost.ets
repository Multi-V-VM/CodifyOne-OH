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

    const previousPhasePath: string = '/data/storage/el2/base/files/wasmer/run-phase.txt';
    const previousStderrPath: string = '/data/storage/el2/base/files/wasmer/run-native-stderr.txt';
    try {
      if (fileIo.accessSync(previousPhasePath)) {
        console.error('[Wasmer] previous native phase: ' + fileIo.readTextSync(previousPhasePath));
      }
      if (fileIo.accessSync(previousStderrPath)) {
        console.error('[Wasmer] previous native stderr: ' + fileIo.readTextSync(previousStderrPath));
      }
    } catch (_) { }

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

const REQUEST_PATH: string = '/data/storage/el2/base/files/wasmer/run-request.txt';
const DONE_PATH: string = '/data/storage/el2/base/files/wasmer/run-done.txt';

function dispatchRun(payload: string): void {
  try {
    if (fileIo.accessSync(DONE_PATH)) {
      fileIo.unlinkSync(DONE_PATH);
    }
  } catch (_) { }
  const request = fileIo.openSync(REQUEST_PATH,
    fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
  fileIo.writeSync(request.fd, payload);
  fileIo.closeSync(request);
  native.startWasmer();
}

function waitForRunResult(timeoutMs: number): Promise<WasiRunResult> {
  return new Promise<WasiRunResult>((resolve, reject) => {
    let waitedMs: number = 0;
    const timer: number = setInterval(() => {
      waitedMs += 100;
      try {
        if (fileIo.accessSync(DONE_PATH)) {
          fileIo.unlinkSync(DONE_PATH);
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
      if (waitedMs >= timeoutMs) {
        clearInterval(timer);
        reject(new Error('Wasmer worker timed out after ' + Math.floor(timeoutMs / 1000) + 's'));
      }
    }, 100);
  });
}

export async function runWasiModuleAsync(modulePath: string, args: string[] = []): Promise<WasiRunResult> {
  ensureWasmerWorkspace();
  dispatchRun(modulePath + '\n' + WASMER_WORKSPACE_DIR + '\n' + args.join('\n'));
  return waitForRunResult(180000);
}

export async function runBundledWasiModuleAsync(
  fd: number,
  offset: number,
  length: number,
  programName: string,
  args: string[] = []
): Promise<WasiRunResult> {
  ensureWasmerWorkspace();
  const payload: string = '@rawfd\n' + WASMER_WORKSPACE_DIR + '\n' +
    fd + '\n' + offset + '\n' + length + '\n' + programName + '\n' + args.join('\n');
  dispatchRun(payload);
  return waitForRunResult(300000);
}
