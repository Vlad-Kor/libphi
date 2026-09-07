import { EditorState, Facet, type Range, StateEffect, StateField } from "@codemirror/state";
import {
  Decoration, type DecorationSet, EditorView, ViewPlugin, type ViewUpdate, WidgetType,
} from "@codemirror/view";

export const previewGeometryEnvironment = Facet.define<() => void>();

/** Heights belong to a document revision and its exact browser layout. */
export const measuredPreviewGeometry = StateEffect.define<ReadonlyMap<string, number>>();
export const clearPreviewGeometry = StateEffect.define<null>();
export const previewGeometry = StateField.define<ReadonlyMap<string, number>>({
  create: () => new Map(),
  update(value, transaction) {
    if (transaction.docChanged || transaction.reconfigured || transaction.effects.some(e => e.is(clearPreviewGeometry)))
      return new Map();
    for (const effect of transaction.effects)
      if (effect.is(measuredPreviewGeometry)) return new Map([...value, ...effect.value]);
    return value;
  },
});

export function lineGeometryKey(
  text: string, decorations: DecorationSet, from: number, to: number,
): string {
  const signature: unknown[] = ["line", text];
  decorations.between(from, to, (a, b, decoration) => {
    const widget = decoration.spec.widget as WidgetType | undefined;
    if (decoration.spec.phiLineGeometry) return;
    signature.push([a - from, b - from, decoration.spec.class,
      decoration.spec.attributes, widget?.constructor.name]);
  });
  return JSON.stringify(signature);
}

interface PreflightStatus { pending: boolean; measured: number; skipped: number }
const preflightStatus = new WeakMap<EditorView, PreflightStatus>();
export function previewGeometryStatus(view: EditorView) {
  return preflightStatus.get(view);
}

const geometryIdentity = Symbol("preview geometry");
type GeometryWidget = WidgetType & { [geometryIdentity]?: string };

/** Preserve the widget's class and interaction methods. A height is a snapshot:
 * changing a getter behind CodeMirror's back does not invalidate its height map. */
export function withMeasuredGeometry(widget: WidgetType, key: string,
                                      heights: ReadonlyMap<string, number>): void {
  (widget as GeometryWidget)[geometryIdentity] = key;
  const height = heights.get(key) ?? widget.estimatedHeight;
  const equal = widget.eq.bind(widget);
  Object.defineProperty(widget, "estimatedHeight", { value: height });
  widget.eq = other => other.estimatedHeight === height && equal(other);
}

function pending(root: HTMLElement): boolean {
  return !!root.querySelector(".math-loading, .dimmed, [data-geometry-pending]") ||
    root.matches(".math-loading, .dimmed, [data-geometry-pending]") ||
    [...root.querySelectorAll("img")].some(img => !!img.src && !img.complete) ||
    [...root.querySelectorAll("video, audio")].some(element => {
      const media = element as HTMLMediaElement;
      return !!media.src && !media.error && media.readyState === 0;
    });
}

/** Wait for actual renderer/image completion, including math inside embeds.
 * Never keep a missing attachment or renderer alive indefinitely. */
export function settlePreview(root: HTMLElement, signal: AbortSignal): Promise<boolean> {
  return new Promise(resolve => {
    let quiet = 0;
    const start = performance.now();
    const finish = (ready: boolean) => {
      window.clearTimeout(timer);
      observer.disconnect();
      signal.removeEventListener("abort", abort);
      resolve(ready);
    };
    const abort = () => finish(false);
    const observer = new MutationObserver(() => { quiet = 0; });
    observer.observe(root, { subtree: true, childList: true, attributes: true, characterData: true });
    let timer: number;
    const check = () => {
      if (signal.aborted) return finish(false);
      if (!pending(root) && ++quiet >= 2) return finish(true);
      if (performance.now() - start > 10_000) return finish(false);
      timer = window.setTimeout(check, 32);
    };
    signal.addEventListener("abort", abort, { once: true });
    check();
  });
}

/** One offscreen widget per idle slice, with the editor's actual CSS ancestry.
 * No whole-document DOM, synchronous MathJax sizing pass, or scroll interception. */
export function previewGeometryPreflight(decorations: StateField<DecorationSet>) {
  return ViewPlugin.fromClass(class {
    private timer = 0;
    private frame = 0;
    private generation = new AbortController();
    private destroyed = false;
    private host: HTMLElement | null = null;
    private width = 0;
    private resize: ResizeObserver | undefined;
    private lastInput = 0;
    private onInput = () => { this.lastInput = performance.now(); };
    private onFonts = () => this.invalidate();

    constructor(private view: EditorView) {
      view.dom.addEventListener("keydown", this.onInput, true);
      view.scrollDOM.addEventListener("scroll", this.onInput, { passive: true });
      document.fonts?.addEventListener("loadingdone", this.onFonts);
      if (typeof ResizeObserver !== "undefined") {
        this.resize = new ResizeObserver(() => {
          const width = view.contentDOM.getBoundingClientRect().width;
          if (width !== this.width) { this.width = width; this.invalidate(); }
        });
        this.resize.observe(view.contentDOM);
      }
      this.schedule();
    }

    update(update: ViewUpdate) {
      if (update.docChanged || update.transactions.some(t => t.reconfigured ||
          t.effects.some(e => e.is(clearPreviewGeometry)))) {
        this.cancel();
        this.schedule();
      } else if (update.state.field(decorations) !== update.startState.field(decorations) &&
                 !update.transactions.some(t => t.effects.some(e => e.is(measuredPreviewGeometry)))) {
        this.cancel();
        this.schedule();
      }
    }

    private invalidate() {
      if (this.destroyed) return;
      for (const synchronize of this.view.state.facet(previewGeometryEnvironment)) synchronize();
      this.view.dispatch({ effects: clearPreviewGeometry.of(null) });
    }

    private schedule() {
      if (this.destroyed) return;
      preflightStatus.set(this.view, { pending: true, measured: 0, skipped: 0 });
      window.clearTimeout(this.timer);
      window.cancelAnimationFrame(this.frame);
      this.frame = window.requestAnimationFrame(() => {
        this.timer = window.setTimeout(() => {
          // Start after the first paint; give active input priority even on
          // WebKit versions without requestIdleCallback.
          if (performance.now() - this.lastInput < 150) return this.schedule();
          const signal = this.generation.signal;
          void this.run(signal).catch(error => {
            if (!signal.aborted) {
              const status = preflightStatus.get(this.view);
              if (status) { status.pending = false; status.skipped++; }
              console.error("Preview geometry:", error);
            }
          });
        }, 160);
      });
    }

    private async run(signal: AbortSignal) {
      const view = this.view;
      if (!view.dom.isConnected || !view.contentDOM.getBoundingClientRect().width) {
        preflightStatus.set(view, { pending: false, measured: 0, skipped: 0 });
        return;
      }
      const status = preflightStatus.get(view)!;
      const heights = view.state.field(previewGeometry);
      const jobs = new Map<string, {
        widget?: WidgetType; block: boolean; from: number; line?: number;
      }>();
      const decorationsNow = view.state.field(decorations);
      for (let it = view.state.field(decorations).iter(); it.value; it.next()) {
        const widget = it.value.spec.widget as GeometryWidget | undefined;
        const key = widget?.[geometryIdentity];
        if (widget && key && !heights.has(key)) jobs.set(key, { widget, block: !!it.value.spec.block, from: it.from });
      }
      // Measure styled source lines with CodeMirror itself, so concealed syntax,
      // proportional glyph widths, inline math and list indentation are identical.
      for (let it = decorationsNow.iter(); it.value; it.next()) {
        if (!it.value.spec.phiLineGeometry) continue;
        const line = view.state.doc.lineAt(it.from);
        const key = lineGeometryKey(line.text, decorationsNow, line.from, line.to);
        if (!heights.has(key)) jobs.set(key, { block: false, from: line.from, line: line.number });
      }
      const results = new Map<string, number>();
      const flush = () => {
        if (!results.size || signal.aborted) return;
        view.dispatch({ effects: [view.scrollSnapshot(), measuredPreviewGeometry.of(new Map(results))] });
        results.clear();
      };
      for (const [key, job] of [...jobs].sort((a, b) =>
        Math.abs(a[1].from - view.viewport.from) - Math.abs(b[1].from - view.viewport.from))) {
        if (signal.aborted) return;
        while (performance.now() - this.lastInput < 150) {
          await new Promise(resolve => window.setTimeout(resolve, 50));
          if (signal.aborted) return;
        }
        const host = view.dom.cloneNode(false) as HTMLElement;
        host.removeAttribute("id");
        host.setAttribute("aria-hidden", "true");
        host.inert = true;
        host.dataset.previewMeasurement = "true";
        host.style.cssText = "position:fixed;left:-100000px;top:0;height:auto;visibility:hidden;pointer-events:none;";
        host.style.width = `${view.dom.getBoundingClientRect().width}px`;
        const scroller = view.scrollDOM.cloneNode(false) as HTMLElement;
        const content = view.contentDOM.cloneNode(false) as HTMLElement;
        content.removeAttribute("contenteditable");
        content.style.setProperty("width", `${view.contentDOM.getBoundingClientRect().width}px`, "important");
        content.style.setProperty("min-height", "0", "important");
        content.style.setProperty("padding-top", "0", "important");
        content.style.setProperty("padding-bottom", "0", "important");
        scroller.append(content);
        host.append(scroller);
        view.dom.parentElement!.append(host);
        this.host = host;
        let dom: HTMLElement | undefined;
        let probe: EditorView | undefined;
        let measuredDOM: HTMLElement | undefined;
        try {
          if (job.line != null || !job.block) {
            const line = job.line != null ? view.state.doc.line(job.line) : view.state.doc.lineAt(job.from);
            class RecordingWidget extends WidgetType {
              toDOM(probeView: EditorView) {
                measuredDOM = job.widget!.toDOM(probeView);
                return measuredDOM;
              }
              destroy(dom: HTMLElement) { job.widget!.destroy(dom); }
            }
            const ranges: Range<Decoration>[] = [];
            decorationsNow.between(line.from, line.to, (from, to, decoration) => {
              if (from >= line.from && to <= line.to)
                ranges.push((decoration.spec.widget === job.widget && job.widget
                  ? Decoration.replace({ ...decoration.spec, widget: new RecordingWidget() })
                  : decoration).range(from - line.from, to - line.from));
            });
            scroller.remove();
            probe = new EditorView({ parent: host, state: EditorState.create({
              doc: line.text,
              extensions: [view.lineWrapping ? EditorView.lineWrapping : [], EditorView.decorations.of(Decoration.set(ranges, true))],
            }) });
            probe.dom.className = view.dom.className;
            probe.dom.style.height = "auto";
            probe.contentDOM.style.cssText = content.style.cssText;
            dom = probe.contentDOM;
          } else {
            dom = job.widget!.toDOM(view);
            content.append(dom);
          }
          if (await settlePreview(dom, signal)) {
            const height = (measuredDOM ?? (probe ? probe.contentDOM.querySelector(".cm-line")! : dom)).getBoundingClientRect().height;
            if (Number.isFinite(height) && height >= 0 && !signal.aborted) {
              results.set(key, height);
              status.measured++;
            } else status.skipped++;
          } else status.skipped++;
        } finally {
          if (probe) probe.destroy();
          else if (dom) job.widget!.destroy(dom);
          host.remove();
          if (this.host === host) this.host = null;
        }
        if (results.size >= 8 && performance.now() - this.lastInput >= 150) flush();
        // Explicit idle yield even when the renderer was already cached.
        await new Promise<void>(resolve => {
          if (typeof window.requestIdleCallback === "function")
            window.requestIdleCallback(() => resolve());
          else window.setTimeout(resolve, 16);
        });
      }
      flush();
      if (!signal.aborted) status.pending = false;
    }

    private cancel() {
      window.clearTimeout(this.timer);
      window.cancelAnimationFrame(this.frame);
      this.generation.abort();
      this.generation = new AbortController();
      this.host?.remove();
      this.host = null;
    }

    destroy() {
      this.destroyed = true;
      this.cancel();
      preflightStatus.delete(this.view);
      this.resize?.disconnect();
      document.fonts?.removeEventListener("loadingdone", this.onFonts);
      this.view.dom.removeEventListener("keydown", this.onInput, true);
      this.view.scrollDOM.removeEventListener("scroll", this.onInput);
    }
  });
}
