import { fileIo } from '@kit.CoreFileKit';

export const WASMER_DIR: string = '/data/storage/el2/base/files/wasmer';
export const WASMER_WORKSPACE_DIR: string = `${WASMER_DIR}/workspace`;

function ensureDir(path: string): void {
  try {
    fileIo.mkdirSync(path);
  } catch (_) {
    // Existing directories are fine.
  }
}

export function ensureWasmerWorkspace(): void {
  ensureDir(WASMER_DIR);
  ensureDir(WASMER_WORKSPACE_DIR);
}
