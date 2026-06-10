import { WebChildProcess } from 'web_engine/childProcess';

export default class CustomChildProcess extends WebChildProcess {
  onStart(): void {
    super.onStart();
  }

  static toString(): string {
    return "CustomChildProcess";
  }
}