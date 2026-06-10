import { childProcessManager, ChildProcessArgs } from '@kit.AbilityKit';

let ohPtyPid: number = 0;
let starting: Promise<number> | null = null;

export async function startOhPtyChild(port: number = 39001): Promise<number> {
  if (ohPtyPid > 0) {
    return ohPtyPid;
  }

  if (starting) {
    return starting;
  }

  const args: ChildProcessArgs = {
    entryParams: `port=${port}`,
    fds: {}
  };

  starting = childProcessManager.startNativeChildProcess(
    'libentry.so:Main',
    args,
    {}
  )
    .then((pid: number) => {
      ohPtyPid = pid;
      console.info(`[OHPTY] native child started, pid=${pid}, port=${port}`);
      return pid;
    })
    .catch((error: Error) => {
      console.error(`[OHPTY] start native child failed: ${JSON.stringify(error)}`);
      ohPtyPid = 0;
      throw error;
    })
    .finally(() => {
      starting = null;
    });

  return starting;
}

export function getOhPtyPid(): number {
  return ohPtyPid;
}