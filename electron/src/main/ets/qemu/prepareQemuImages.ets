import { fileIo } from '@kit.CoreFileKit';
import common from '@ohos.app.ability.common';

const QEMU_DIR: string = '/data/storage/el2/base/files/qemu';

function ensureDir(path: string): void {
  try {
    fileIo.mkdirSync(path);
  } catch (_) {}
}

function fileSize(path: string): number {
  try {
    return fileIo.statSync(path).size;
  } catch (_) {
    return -1;
  }
}

async function copyRawFile(
  context: common.UIAbilityContext,
  rawPath: string,
  targetPath: string,
  overwriteIfChanged: boolean
): Promise<void> {
  const targetSize: number = fileSize(targetPath);

  if (!overwriteIfChanged && targetSize > 0) {
    return;
  }

  const content = await context.resourceManager.getRawFileContent(rawPath);

  if (overwriteIfChanged && targetSize === content.byteLength) {
    return;
  }

  const file = fileIo.openSync(
    targetPath,
    fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE
  );

  fileIo.writeSync(file.fd, content.buffer);
  fileIo.closeSync(file);
}

export async function prepareQemuImages(context: common.UIAbilityContext): Promise<void> {
  ensureDir(QEMU_DIR);

  await copyRawFile(context, 'qemu/Image', `${QEMU_DIR}/Image`, false);
  await copyRawFile(context, 'qemu/rootfs.cpio.gz', `${QEMU_DIR}/rootfs.cpio.gz`, true);
}