import { Transaction, type Extension } from "@codemirror/state";
import { EditorView, ViewPlugin, type ViewUpdate } from "@codemirror/view";

interface PerformanceSummary {
  count: number;
  mean: number;
  p50: number;
  p95: number;
  max: number;
}

interface PhiEditorPerformance {
  enabled: boolean;
  reset(): void;
  snapshot(): Record<string, PerformanceSummary>;
}

const sampleLimit = 240;
const samples = new Map<string, number[]>();
let beforeInputAt: number | null = null;

function configuredAtStartup(): boolean {
  if (typeof window === "undefined") return false;
  try {
    return new URLSearchParams(window.location.search).get("phi-perf") === "1";
  } catch {
    return false;
  }
}

function percentile(sorted: readonly number[], fraction: number): number {
  if (!sorted.length) return 0;
  return sorted[Math.min(sorted.length - 1,
    Math.floor(sorted.length * fraction))];
}

const controller: PhiEditorPerformance = {
  enabled: configuredAtStartup(),
  reset() {
    samples.clear();
    beforeInputAt = null;
  },
  snapshot() {
    return Object.fromEntries([...samples].map(([name, values]) => {
      const sorted = [...values].sort((left, right) => left - right);
      return [name, {
        count: values.length,
        mean: values.reduce((sum, value) => sum + value, 0) / values.length,
        p50: percentile(sorted, 0.5),
        p95: percentile(sorted, 0.95),
        max: sorted.at(-1) ?? 0,
      }];
    }));
  },
};

if (typeof window !== "undefined") window.phiEditorPerformance = controller;

export function recordPerformance(name: string, duration: number): void {
  if (!controller.enabled || !Number.isFinite(duration)) return;
  const values = samples.get(name) ?? [];
  values.push(duration);
  if (values.length > sampleLimit) values.splice(0, values.length - sampleLimit);
  samples.set(name, values);
}

export function measurePerformance<T>(name: string, operation: () => T): T {
  if (!controller.enabled) return operation();
  const started = performance.now();
  try {
    return operation();
  } finally {
    recordPerformance(name, performance.now() - started);
  }
}

const inputLatencyPlugin = ViewPlugin.fromClass(class {
  update(update: ViewUpdate): void {
    if (!controller.enabled || !update.transactions.some((transaction) =>
      transaction.isUserEvent("input.type"))) return;
    const completed = performance.now();
    if (beforeInputAt != null) {
      recordPerformance("input/event-to-dispatch", completed - beforeInputAt);
      beforeInputAt = null;
    }
    window.requestAnimationFrame(() =>
      recordPerformance("input/dispatch-to-frame", performance.now() - completed));
  }
});

/** Opt-in diagnostics. Enable with `?phi-perf=1` or from the inspector with
 * `window.phiEditorPerformance.enabled = true`, then call `.snapshot()`. */
export const inputPerformanceExtension: Extension = [
  EditorView.domEventHandlers({
    beforeinput: () => {
      if (controller.enabled) beforeInputAt = performance.now();
      return false;
    },
  }),
  inputLatencyPlugin,
  EditorView.updateListener.of((update) => {
    if (!controller.enabled || !update.docChanged) return;
    /* Retain a distinct count for transactions that are not ordinary typing,
     * which makes pasted/command edits visible without polluting key latency. */
    if (!update.transactions.some((transaction) =>
      transaction.annotation(Transaction.userEvent)?.startsWith("input.type")))
      recordPerformance("input/non-type-transaction", 0);
  }),
];
