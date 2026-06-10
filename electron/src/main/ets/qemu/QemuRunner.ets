import common from '@ohos.app.ability.common';
import native from 'libentry.so';
import { prepareQemuImages } from './prepareQemuImages';
import { ensureWorkspaceDir } from './Workspace';

let started: boolean = false;
let starting: Promise<boolean> | null = null;

export async function startQemu(context: common.UIAbilityContext): Promise<boolean> {
  if (started) {
    return true;
  }

  if (starting) {
    return starting;
  }

  starting = (async (): Promise<boolean> => {
    await prepareQemuImages(context);

    ensureWorkspaceDir();

    const ok: boolean = native.startQemu();
    started = ok;

    console.info(`[QEMU] startQemu ret=${ok}`);
    return ok;
  })().finally(() => {
    starting = null;
  });


  return starting;
}