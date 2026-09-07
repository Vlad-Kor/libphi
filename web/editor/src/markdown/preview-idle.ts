/** Passive activity gate for speculative work. This never consumes input or
 * invents gesture phases: ongoing scroll events also cover native momentum. */
export class PreviewIdleGate {
  private lastActivity = -Infinity;

  noteActivity(): void { this.lastActivity = performance.now(); }

  async wait(signal: AbortSignal): Promise<boolean> {
    while (!signal.aborted) {
      const remaining = 150 - (performance.now() - this.lastActivity);
      if (remaining <= 0) return true;
      await new Promise<void>(resolve => {
        const finish = () => {
          window.clearTimeout(timer);
          signal.removeEventListener("abort", finish);
          resolve();
        };
        const timer = window.setTimeout(finish, remaining);
        signal.addEventListener("abort", finish, { once: true });
      });
    }
    return false;
  }
}
