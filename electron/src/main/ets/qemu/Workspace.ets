import { fileIo } from '@kit.CoreFileKit';

export const HOST_WORKSPACE_DIR: string = '/data/storage/el2/base/files/workspace';

export function ensureWorkspaceDir(): void {
  try {
    fileIo.mkdirSync(HOST_WORKSPACE_DIR);
  } catch (_) {
    // 目录已存在时忽略
  }
}