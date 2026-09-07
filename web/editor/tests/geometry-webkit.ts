// Real-layout regression harness, bundled by tests/run-geometry-webkit.py.
import { verifyPointerConceal } from "./pointer-conceal-webkit";
import { parseMarkdownNodes, type MarkdownNodeKind } from "../src/markdown/parser";
import { acceptNativeResponse } from "../src/bridge";
import { PhiMarkdownEditor } from "../src/editor";
import { previewGeometry, previewGeometryStatus } from "../src/markdown/geometry";
import { EditorView } from "@codemirror/view";
import * as widgets from "../src/widgets/preview";
import { RichTableWidget } from "../src/widgets/table";
import { isMathScrollbarEvent, wireMathScroll } from "../src/math/mathjax";

const wait = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));
for (const Widget of [...Object.values(widgets), RichTableWidget]) {
  if (typeof Widget !== "function" || !Widget.prototype?.toDOM) continue;
  const original = Widget.prototype.toDOM;
  Widget.prototype.toDOM = function(view: EditorView) {
    const dom = original.call(this, view);
    if (typeof this.from === "number") dom.dataset.probeFrom = String(this.from);
    return dom;
  };
}

async function run() {
  await verifyPointerConceal();
  window.addEventListener("phi-native-message", ((event: CustomEvent) => {
    const message = event.detail;
    if (!message.id) return;
    const result = message.type === "embed/read"
      ? { text: "## Nested note\n\nA paragraph with $\\frac{a}{b}$.\n\n![nested](image.svg)", path: "/embed.md" }
      : { path: "image.svg", width: 300, height: 150 };
    queueMicrotask(() => acceptNativeResponse({ protocol: 1, type: "request/response", id: message.id, payload: { result } }));
  }) as EventListener);
  const text = [
    '---\ntitle: Geometry\ncount: 12\nchecked: true\n---',
    ...[1, 2, 3, 4, 5, 6].map(n => '#'.repeat(n) + ' A heading with **bold text** ' + 'wide words '.repeat(8)),
    '- List with **bold** text ' + 'wrapping words '.repeat(30),
    '- [x] Task item',
    '---',
    '$$x$$',
    '$$\\frac{\\frac{a}{b}}{\\sqrt{x^2+1}}$$',
    '$$\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}$$',
    '$$\\ce{H2O + CO2 -> H2CO3}$$',
    '$$\\unknowncommand{x}$$',
    'Text with $\\frac{a}{b}$ inline math.',
    '```js\nconst a = 1;\nconsole.log(a);\n```',
    '> A quote with **bold** and $\\frac{a}{b}$.',
    '> [!note] A title\n> Callout body with $\\frac{a}{b}$.',
    '> [!warning]- Closed\n> Hidden body.',
    '| Heading | Another |\n|---|---|\n| Some **bold** | $\\frac{a}{b}$ |\n| words words words | 123 |',
    '```mermaid\ngraph TD\nA --> B\nB --> C\n```',
    '<div style="padding: 13px">HTML with <strong>bold</strong><br>second row</div>',
    '[^1]: Footnote with **bold** and $x$.',
    '[^1] and [[Internal note]] and #tag',
    'A paragraph with **bold**, *italic*, ~~strike~~, ==highlight==, %%hidden%% and `code`. ^block',
    'Inline footnote ^[Some text] and [Markdown link](https://example.com).',
    '$$\\mathds{R}$$',
    '- Before $$x$$ after display math in a list.',
    '![[Note]]',
    '![[image.svg|230]]',
    '- ![[image.svg|130]]',
    '![local](image.svg)',
    '[![linked](image.svg)](https://example.com)',
    '[<img src="image.svg" width="210">](https://example.com)',
    '<iframe src="https://example.com" width="400" height="250"></iframe>',
    '![[document.pdf]]',
    '![[audio.wav]]',
    '![[video.webm]]',
    '![small](data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIzMDAiIGhlaWdodD0iMTUwIj48cmVjdCB3aWR0aD0iMzAwIiBoZWlnaHQ9IjE1MCIgZmlsbD0icmVkIi8+PC9zdmc+)',
    ...Array.from({ length: 80 }, (_, i) => `Plain paragraph ${i}.`),
    'end',
  ].join('\n\n');
  const supportedKinds = {
    "frontmatter": true,
    "heading": true,
    "emphasis": true,
    "strong": true,
    "strike": true,
    "highlight": true,
    "comment": true,
    "wikilink": true,
    "embed": true,
    "markdown-link": true,
    "markdown-image": true,
    "footnote-reference": true,
    "inline-footnote": true,
    "footnote-definition": true,
    "math": true,
    "display-math": true,
    "inline-code": true,
    "task": true,
    "callout": true,
    "blockquote": true,
    "table": true,
    "mermaid": true,
    "code-block": true,
    "html": true,
    "horizontal-rule": true,
    "list-item": true,
    "tag": true,
    "block-id": true,
  } satisfies Record<MarkdownNodeKind, boolean>;
  const parsedKinds = new Set(parseMarkdownNodes(text).map(node => node.kind));
  const missingKinds = Object.keys(supportedKinds).filter(kind => !parsedKinds.has(kind as MarkdownNodeKind));
  if (missingKinds.length) throw new Error(`Geometry fixture missing Markdown kinds: ${missingKinds}`);
  const started = performance.now();
  const editor = new PhiMarkdownEditor(document.querySelector('#editor')!);
  editor.openDocument({ documentId: 'geometry', path: '/geometry.md', text, revision: 0, lineEnding: '\n' } as never);
  editor.updateTheme({ dark: false, fontScale: Number(new URLSearchParams(location.search).get("scale") ?? 1) });
  const view = editor.view;
  (window as any).probe = () => ({status:previewGeometryStatus(view),count:view.state.field(previewGeometry).size, viewport:view.viewport, top:view.scrollDOM.scrollTop, height:view.scrollDOM.clientHeight, pending:[...document.querySelectorAll("[aria-hidden=true] .math-loading, [data-geometry-pending], .dimmed")].map(x=>x.outerHTML.slice(0,100))});
  view.dispatch({ selection: { anchor: text.length }, effects: EditorView.scrollIntoView(text.length) });
  const openMs = performance.now() - started;
  const firstFrameMs = await new Promise<number>(resolve => requestAnimationFrame(() => resolve(performance.now() - started)));
  let previous = 0, stable = 0;
  for (let i = 0; i < 1000; i++) {
    await wait(50);
    const count = view.state.field(previewGeometry).size;
    stable = count === previous && count > 20 ? stable + 1 : 0;
    previous = count;
    if (stable > 5 && previewGeometryStatus(view)?.pending === false) break;
  }
  const preflightMs = performance.now() - started;
  const heights = view.state.field(previewGeometry);
  const results: unknown[] = [];
  const oracleErrors: unknown[] = [];
  const failures: unknown[] = [];
  // Real WebKit layout must reserve a measurable gutter, including with GTK
  // overlay scrollbars. Synthetic DOM events below verify dispatch only;
  // physical touchpad momentum still requires manual hardware testing.
  const equation = document.createElement("div");
  equation.className = "math-widget math-display";
  equation.style.cssText = "position:fixed;left:0;top:0;width:300px";
  equation.innerHTML = '<div style="width:900px;height:40px"></div>';
  document.body.append(equation);
  wireMathScroll(equation);
  const box = equation.getBoundingClientRect();
  const gutter = equation.offsetHeight - equation.clientHeight;
  const scrollbarPress = new MouseEvent("pointerdown", {
    bubbles: true, cancelable: true, clientX: box.left + 20,
    clientY: box.bottom - gutter / 2,
  });
  let scrollbarHit = false;
  equation.addEventListener("pointerdown", event => {
    scrollbarHit = isMathScrollbarEvent(equation, event);
  });
  equation.dispatchEvent(scrollbarPress);
  if (gutter <= 0 || !scrollbarHit)
    failures.push({ error: "Math scrollbar has no hit-testable gutter", gutter, scrollbarHit });
  for (const start of [100, 600]) {
    equation.scrollLeft = start;
    const pan = new WheelEvent("wheel", {
      bubbles: true, cancelable: true, deltaX: 48, deltaY: 3,
    });
    equation.dispatchEvent(pan);
    if (!pan.defaultPrevented || equation.scrollLeft !== Math.min(600, start + 48))
      failures.push({ error: "Math horizontal pan escaped", start, left: equation.scrollLeft });
  }
  equation.remove();
  for (const set of view.state.facet(EditorView.decorations)) {
    if (typeof set === 'function') continue;
    for (let it = set.iter(); it.value; it.next()) {
      const widget = it.value.spec.widget;
      if (!widget || (!('from' in widget) && !it.value.spec.phiLineGeometry) || widget.estimatedHeight < 0) continue;
      const coldHeight = view.lineBlockAt(it.from).height;
      view.dispatch({ effects: EditorView.scrollIntoView(it.from, { y: 'center' }) });
      await wait(200);
      const point = view.domAtPos(it.from);
      const element = point.node.nodeType === 1 ? point.node as HTMLElement : point.node.parentElement;
      const dom = it.value.spec.phiLineGeometry
        ? element?.closest(".cm-line") ?? (point.node.childNodes[point.offset] as HTMLElement)?.closest?.(".cm-line")
        : view.contentDOM.querySelector<HTMLElement>(`[data-probe-from="${widget.from}"]`);
      if (!dom) { failures.push({ from: it.from, error: 'not mounted' }); continue; }
      const actual = dom.getBoundingClientRect().height;
      const mountedHeight = view.lineBlockAt(it.from).height;
      if (Math.abs(coldHeight - mountedHeight) > 0.02)
        oracleErrors.push({ from: it.from, type: widget.constructor.name, coldHeight, mountedHeight });
      const record = { type: widget.constructor.name, from: it.from, estimate: widget.estimatedHeight, actual, delta: actual - widget.estimatedHeight, ...(Math.abs(actual - widget.estimatedHeight) > 0.02 ? {html:dom.outerHTML.slice(0,600)} : {}) };
      results.push(record);
      if (Math.abs(actual - widget.estimatedHeight) > 0.02) failures.push(record);
    }
  }
  (window as any).webkit.messageHandlers.test.postMessage((failures.length || oracleErrors.length || previewGeometryStatus(view)?.pending || previewGeometryStatus(view)?.skipped || heights.size < 20 ? 'FAIL ' : 'PASS ') + JSON.stringify({ width: innerWidth, scale: getComputedStyle(document.body).fontSize, openMs, firstFrameMs, preflightMs, cached: heights.size, results, failures, oracleErrors }));
}
run().catch(error => (window as any).webkit.messageHandlers.test.postMessage('FAIL ' + error.stack));
