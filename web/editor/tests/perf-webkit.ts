// Real-WebKit typing/scrolling benchmark, bundled by tests/run-perf-webkit.py.
// Measures the main-thread work CodeMirror and Phi perform per keystroke and
// per scrolled frame: synchronous dispatch plus CodeMirror's measure cycle
// (which forces WebKit style and layout). Paint/compositing is not included.
import { EditorView } from "@codemirror/view";
import { acceptNativeResponse } from "../src/bridge";
import { PhiMarkdownEditor } from "../src/editor";
import { parseMarkdownNodes } from "../src/markdown/parser";
import { previewGeometryStatus } from "../src/markdown/geometry";
import * as widgets from "../src/widgets/preview";
import { RichTableWidget } from "../src/widgets/table";

const post = (message: string) =>
  (window as any).webkit.messageHandlers.test.postMessage(message);
const wait = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));
/* rAF stops while the test window is occluded; report instead of hanging. */
const frame = () => new Promise<number>((resolve, reject) => {
  const watchdog = setTimeout(() => reject(new Error("no animation frame for 5 s")), 5000);
  requestAnimationFrame((time) => { clearTimeout(watchdog); resolve(time); });
});

let measureTime = 0;
const proto = EditorView.prototype as unknown as { measure(flush?: boolean): void };
const originalMeasure = proto.measure;
proto.measure = function (this: EditorView, ...args: [boolean?]) {
  const started = performance.now();
  try { return originalMeasure.apply(this, args); }
  finally { measureTime += performance.now() - started; }
};

let widgetRenders = 0;
/** Synchronous toDOM time per widget class, reset per scenario. */
let widgetTime: Record<string, { count: number; ms: number }> = {};
for (const Widget of [...Object.values(widgets), RichTableWidget]) {
  if (typeof Widget !== "function" || !Widget.prototype?.toDOM) continue;
  const original = Widget.prototype.toDOM;
  const name = Widget.name;
  Widget.prototype.toDOM = function (this: unknown, view: EditorView) {
    widgetRenders++;
    const started = performance.now();
    try { return original.call(this, view); }
    finally {
      const entry = widgetTime[name] ??= { count: 0, ms: 0 };
      entry.count++;
      entry.ms = Math.round((entry.ms + performance.now() - started) * 10) / 10;
    }
  };
}

function section(index: number): string {
  return [
    `## Section ${index} with **bold** and $x_${index}$`,
    "",
    "A paragraph with *emphasis*, `code`, a [[Wiki link]], and inline math " +
      `$\\frac{a_${index}}{b}$. ` + "More ordinary prose words. ".repeat(6),
    "",
    "- A list item with enough ordinary words to wrap around the line once.",
    "- [ ] A task item",
    "  - Nested item with $\\alpha^2$",
    "",
    "$$",
    `\\int_0^{${index}} \\frac{x^2}{\\sqrt{1 + x^2}}\\,dx`,
    "$$",
    "",
    "```ts",
    `export function f${index}(value: number): number {`,
    "  return value * 2;",
    "}",
    "```",
    "",
    "> [!note] Callout",
    "> Body with **bold** text.",
    "",
    "| a | b |", "| --- | --- |", `| ${index} | $y$ |`,
    "",
  ].join("\n");
}

function documentText(bytes: number): string {
  let text = "# Benchmark\n\n";
  let index = 0;
  while (text.length < bytes / 2) text += section(index++);
  text += "```python\n" + Array.from({ length: 30 }, (_, line) =>
    `def g${line}(x):\n    return x + ${line}`).join("\n") +
    "\nPROBE_CODE\n```\n\n";
  text += "$$\n\\begin{aligned}\n" + Array.from({ length: 10 }, (_, line) =>
    `a_${line} &= \\sum_{k=0}^{${line}} \\frac{k}{k+1} \\\\`).join("\n") +
    "\nPROBE_MATH\n\\end{aligned}\n$$\n\n";
  text += "Plain prose with $x$ and **bold** where PROBEPROSE sits.\n\n";
  while (text.length < bytes) text += section(index++);
  return text;
}

function summary(values: number[]) {
  const sorted = [...values].sort((a, b) => a - b);
  const at = (fraction: number) =>
    sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * fraction))] ?? 0;
  const round = (value: number) => Math.round(value * 100) / 100;
  return {
    mean: round(values.reduce((sum, value) => sum + value, 0) / (values.length || 1)),
    p50: round(at(0.5)), p95: round(at(0.95)), max: round(sorted.at(-1) ?? 0),
  };
}

async function typeAt(view: EditorView, probe: string, keys: number) {
  let position = view.state.doc.toString().indexOf(probe) + probe.length;
  view.dispatch({
    selection: { anchor: position },
    effects: EditorView.scrollIntoView(position, { y: "center" }),
  });
  for (let index = 0; index < 5; index++) await frame();
  const work: number[] = [];
  const dispatchWork: number[] = [];
  const measureWork: number[] = [];
  const renders: number[] = [];
  const stages = (window as any).phiEditorPerformance;
  stages.enabled = true;
  stages.reset();
  for (let index = 0; index < keys; index++) {
    const insert = index % 7 === 6 ? " " : "x";
    measureTime = 0;
    const rendersBefore = widgetRenders;
    const started = performance.now();
    view.dispatch({
      changes: { from: position, insert },
      selection: { anchor: position + 1 },
      userEvent: "input.type",
    });
    position++;
    const dispatched = performance.now() - started;
    await Promise.resolve();
    await frame();
    await wait(0);
    work.push(dispatched + measureTime);
    dispatchWork.push(dispatched);
    measureWork.push(measureTime);
    renders.push(widgetRenders - rendersBefore);
  }
  stages.enabled = false;
  const stageMeans = Object.fromEntries(Object.entries(stages.snapshot() as
    Record<string, { mean: number }>).map(([name, value]) =>
    [name, Math.round(value.mean * 100) / 100]));
  return {
    work: summary(work.slice(5)), dispatch: summary(dispatchWork.slice(5)),
    measure: summary(measureWork.slice(5)), widgetRenders: summary(renders.slice(5)),
    stageMeans,
  };
}

async function scroll(view: EditorView, step: number) {
  const scroller = view.scrollDOM;
  scroller.scrollTop = 0;
  for (let index = 0; index < 10; index++) await frame();
  const work: number[] = [];
  const gaps: number[] = [];
  widgetTime = {};
  const stages = (window as any).phiEditorPerformance;
  stages.enabled = true;
  stages.reset();
  let last = await frame();
  for (let index = 0; index < 400 &&
       scroller.scrollTop + scroller.clientHeight < scroller.scrollHeight - 2; index++) {
    measureTime = 0;
    scroller.scrollTop += step;
    const now = await frame();
    await wait(0);
    work.push(measureTime);
    gaps.push(now - last);
    last = now;
  }
  stages.enabled = false;
  const stageMeans = Object.fromEntries(Object.entries(stages.snapshot() as
    Record<string, { mean: number; count: number }>).map(([name, value]) =>
    [name, `${Math.round(value.mean * 100) / 100}x${value.count}`]));
  return { frames: work.length, measure: summary(work), frameGap: summary(gaps),
    slowFrames: gaps.filter(gap => gap > 25).length, widgetTime, stageMeans };
}

async function run() {
  window.addEventListener("phi-native-message", ((event: CustomEvent) => {
    const message = event.detail;
    if (!message.id) return;
    queueMicrotask(() => acceptNativeResponse({
      protocol: 1, type: "request/response", id: message.id,
      payload: { result: message.type === "embed/read" ? { text: "Embedded" } : {} },
    }));
  }) as EventListener);
  const parameters = new URLSearchParams(location.search);
  const bytes = Number(parameters.get("bytes") ?? 40000);
  const conceal = parameters.get("conceal") === "1";
  const editor = new PhiMarkdownEditor(document.getElementById("editor")!);
  editor.updateSettings({ executableSnippets: true, latexConceal: conceal } as never);
  const text = documentText(bytes);
  editor.openDocument({ documentId: "perf", path: "/vault/perf.md", text,
    revision: 1, lineEnding: "LF" } as never);
  const view = editor.view;
  const opened = performance.now();
  while (parameters.get("engine") !== "1" &&
         previewGeometryStatus(view)?.pending !== false && performance.now() - opened < 60_000)
    await wait(100);
  post(`LOG opened; preflight ${JSON.stringify(previewGeometryStatus(view))}`);
  const results: Record<string, unknown> = {
    bytes: text.length, conceal, preflightMs: Math.round(performance.now() - opened),
  };
  /* Engine sanity check: JavaScriptCore without its JIT is several times
   * slower than the numbers profiled under node/V8. */
  const loopStarted = performance.now();
  let checksum = 0;
  for (let index = 0; index < 20_000_000; index++) checksum = (checksum + index * 7) | 0;
  results.loop20M = Math.round(performance.now() - loopStarted) + (checksum === 1 ? 1 : 0);
  const regexStarted = performance.now();
  for (let index = 0; index < 20; index++) [...text.matchAll(/(?<!\*)\*[^\n*]+\*(?!\*)/g)];
  results.regex20 = Math.round(performance.now() - regexStarted);
  if (parameters.get("engine") === "1") {
    const timed = (label: string, run: () => unknown) => {
      const started = performance.now();
      for (let index = 0; index < 10; index++) run();
      results[label] = Math.round((performance.now() - started) * 10) / 100;
    };
    timed("parseMarkdownNodes", () => parseMarkdownNodes(text));
    const patterns: Record<string, RegExp> = {
      inlineMath: /(?<!\\)(\$|\\\()([^\n]+?)(?<!\\)(\$|\\\))/g,
      strong: /(?<!\*)\*\*([^\n*]|\*(?!\*))+?\*\*(?!\*)/g,
      emphasis: /(?<!\*)\*[^\n*]+\*(?!\*)/g,
      emphasisNoLookbehind: /\*[^\n*]+\*(?!\*)/g,
      underscore: /(?<!_)_[^\n_]+_(?!_)/g,
      tag: /(^|[\s(])#([\p{L}\p{N}_-]+(?:\/[\p{L}\p{N}_-]+)*)/gmu,
      blockId: /(?:^|\s)\^([A-Za-z0-9-]+)(?=\s*$)/gm,
      list: /^([ \t]*)(?:([-+*])|(\d+)([.)]))([ \t]+)/gm,
      rawHtml: /<(span|div|kbd|details|summary|sup|sub|small|mark|table|thead|tbody|tr|th|td|iframe)(?:\s[^>]*)?>[\s\S]*?<\/\1\s*>/gi,
      comments: /%%[\s\S]*?%%/g,
      wiki: /(!)?\[\[([^\]\n]+)\]\]/g,
      blockquote: /^ {0,3}>.*$/gm,
    };
    for (const [name, pattern] of Object.entries(patterns))
      timed(`re:${name}`, () => [...text.matchAll(pattern)]);
    return post("PASS " + JSON.stringify(results));
  }
  const idleGaps: number[] = [];
  let previous = await frame();
  for (let index = 0; index < 60; index++) {
    const now = await frame();
    idleGaps.push(now - previous);
    previous = now;
  }
  results.idleFrameGap = summary(idleGaps);
  const scrollDown = await scroll(view, 90);
  post("LOG scrolled");
  if (parameters.get("scroll") === "1") {
    results.scroll = scrollDown;
    results.scrollAgain = await scroll(view, 90);
    return post("PASS " + JSON.stringify(results));
  }
  for (const probe of ["PROBE_CODE", "PROBE_MATH", "PROBEPROSE"]) {
    results[probe] = await typeAt(view, probe, 45);
    post(`LOG typed ${probe}`);
  }
  results.scroll = scrollDown;
  post("LOG scrolling again");
  results.scrollAgain = await scroll(view, 90);
  post("PASS " + JSON.stringify(results));
}

run().catch(error => post("FAIL " + error.message + "\n" + error.stack));
