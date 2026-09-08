import { StateEffect, StateField } from "@codemirror/state";
import { EditorView, ViewPlugin } from "@codemirror/view";
import { markdownAnalysis } from "./analysis";

interface SourceRange { from: number; to: number }
export const revealDraggedPreview = StateEffect.define<SourceRange | null>();
export const draggedPreviewSources = StateField.define<readonly SourceRange[]>({
  create: () => [],
  update(value, transaction) {
    if (transaction.docChanged) return [];
    for (const effect of transaction.effects) {
      if (!effect.is(revealDraggedPreview)) continue;
      if (!effect.value) value = [];
      else if (!value.some(range => range.from === effect.value!.from && range.to === effect.value!.to))
        value = [...value, effect.value];
    }
    return value;
  },
});

const dragReveal = ViewPlugin.fromClass(class {
  dragging = false;
  constructor(readonly view: EditorView) {
    const doc = view.dom.ownerDocument;
    doc.addEventListener("mousemove", this.move, true);
    doc.defaultView!.addEventListener("mouseup", this.release);
    doc.defaultView!.addEventListener("blur", this.release);
    view.contentDOM.addEventListener("keydown", this.release, true);
  }
  move = (event: MouseEvent): void => {
    if (!this.dragging) return;
    if (!(event.buttons & 1)) { this.release(); return; }
    const target = this.view.dom.ownerDocument.elementFromPoint(event.clientX, event.clientY);
    const widget = target?.closest(".math-widget, .raw-html-widget");
    if (!widget || !this.view.contentDOM.contains(widget) ||
        widget.classList.contains("cm-hard-rendered-item")) return;
    const from = this.view.posAtDOM(widget);
    const node = markdownAnalysis(this.view.state).nodes.find(node =>
      node.from === from && (node.kind === "math" || node.kind === "display-math" ||
        (node.kind === "html" && !/<(?:iframe|table)\b/i.test(node.text))));
    if (node) this.view.dispatch({ effects: revealDraggedPreview.of({ from: node.from, to: node.to }) });
  };
  release = (): void => {
    this.dragging = false;
    if (this.view.state.field(draggedPreviewSources).length)
      this.view.dispatch({ effects: revealDraggedPreview.of(null) });
  };
  destroy(): void {
    const doc = this.view.dom.ownerDocument;
    doc.removeEventListener("mousemove", this.move, true);
    doc.defaultView!.removeEventListener("mouseup", this.release);
    doc.defaultView!.removeEventListener("blur", this.release);
    this.view.contentDOM.removeEventListener("keydown", this.release, true);
  }
}, {
  // Only CodeMirror text-selection gestures enter here. Widget controls and
  // native equation scrollbars keep their own pointer handling.
  eventHandlers: { mousedown(event) { if (event.button === 0) this.dragging = true; return false; } },
});

// Replacements normally hit-test to the nearer source edge, so the left half
// maps to `from` and selects nothing. Reveal on entry, before that hit test.
// Only re-rendering waits for release; the source and selection stay live.
export const revealPreviewOnDrag = [draggedPreviewSources, dragReveal];
