import { StateEffect } from "@codemirror/state";
import type { EditorView } from "@codemirror/view";
import type { LatexConcealSpec } from "./enhancements";

export const refreshPointerConceal = StateEffect.define<null>();
interface RevealRegion {
  from: number;
  wordFrom: number;
  wordTo: number;
  left: number;
  right: number;
  top: number;
  bottom: number;
}

// Reveal immediately. If revealing moves a fragment onto the following row,
// retain its source over both the former and the new visual positions.
// This affects presentation only; CodeMirror still selects literal offsets.
export class ConcealPointerGuard {
  private destroyed = false;
  private active = false;
  private x = 0;
  private y = 0;
  private before = new Map<number, RevealRegion>();
  private revealed = new Map<number, RevealRegion>();

  constructor(private readonly view: EditorView) {
    const doc = view.dom.ownerDocument;
    doc.addEventListener("mousedown", this.down, true);
    doc.addEventListener("mousemove", this.move, true);
    doc.defaultView!.addEventListener("mouseup", this.release);
    doc.defaultView!.addEventListener("blur", this.release);
    view.contentDOM.addEventListener("keydown", this.release, true);
  }

  private inside(region: RevealRegion): boolean {
    return this.x >= region.left && this.x <= region.right &&
      this.y >= region.top && this.y <= region.bottom;
  }

  private insideReflow(region: RevealRegion): boolean {
    if (this.inside(region)) return true;
    const start = this.view.domAtPos(region.wordFrom);
    const end = this.view.domAtPos(region.wordTo);
    const range = this.view.dom.ownerDocument.createRange();
    range.setStart(start.node, start.offset);
    range.setEnd(end.node, end.offset);
    const boxes = [...range.getClientRects()].filter(box => box.width > 0);
    // Endpoints can straddle rows (the delimiter stays above while G wraps).
    // Use the browser's actual text fragments, including the new base glyph.
    if (boxes.some(box => this.x >= box.left && this.x <= box.right &&
        this.y >= box.top && this.y <= box.bottom)) return true;
    // Preserve the transition through the line gap as well.
    return boxes.some(box => box.top >= region.bottom - 1 &&
      this.y >= region.bottom && this.y <= box.top &&
      this.x >= Math.min(region.left, box.left) &&
      this.x <= Math.max(region.right, box.right));
  }

  private capture(event: MouseEvent): void {
    this.x = event.clientX;
    this.y = event.clientY;
    this.before.clear();
    const right = this.view.contentDOM.getBoundingClientRect().right;
    for (const dom of this.view.contentDOM.querySelectorAll<HTMLElement>(".cm-latex-conceal")) {
      const box = dom.getBoundingClientRect();
      // Ask CodeMirror: mapped decorations may reuse DOM after earlier edits.
      const from = this.view.posAtDOM(dom);
      const caret = this.view.coordsAtPos(from);
      if (!caret || !box.width) continue;
      // A script can push its whole unbroken word (e.g. $G^*) to the
      // following row. Protect the former G/delimiter position too, not just
      // the superscript's box, or tiny movements there clear the guard.
      const line = this.view.state.doc.lineAt(from);
      const prefix = this.view.state.sliceDoc(line.from, from);
      const wordFrom = from - (/\S+$/.exec(prefix)?.[0].length ?? 0);
      const wordTo = from + (/^\S+/.exec(this.view.state.sliceDoc(from, line.to))?.[0].length ?? 0);
      const wordCaret = this.view.coordsAtPos(wordFrom);
      const sameRow = wordCaret && wordCaret.top < caret.bottom &&
        wordCaret.bottom > caret.top;
      this.before.set(from, {
        from, wordFrom, wordTo,
        left: sameRow ? Math.min(box.left, wordCaret.left) : box.left, right,
        top: Math.min(box.top, caret.top), bottom: Math.max(box.bottom, caret.bottom),
      });
    }
  }

  private down = (event: MouseEvent): void => {
    if (event.button !== 0 || !(event.target instanceof Node) ||
        !this.view.contentDOM.contains(event.target)) return;
    this.active = true;
    this.capture(event);
  };

  private move = (event: MouseEvent): void => {
    if (!this.active) return;
    if (!(event.buttons & 1)) { this.release(); return; }
    this.capture(event);
    let changed = false;
    for (const [key, region] of this.revealed) {
      if (!this.insideReflow(region)) {
        this.revealed.delete(key);
        changed = true;
      }
    }
    if (changed) this.refresh();
  };

  reveal(spec: LatexConcealSpec, selected: boolean): boolean {
    const key = spec[0].from;
    if (!this.active) return selected;
    if (selected && !this.revealed.has(key)) {
      const region = spec.map(part => this.before.get(part.from)).find(Boolean);
      if (region) {
        for (const part of spec) this.before.delete(part.from);
        this.revealed.set(key, region);
        this.view.requestMeasure({
          key: this,
          read: () => [...this.revealed].filter(([, old]) => {
            const current = this.view.coordsAtPos(old.from);
            // Only a change of visual row warrants hysteresis. A wider inline
            // replacement on the same row keeps ordinary cursor behavior.
            return !current || current.top < old.bottom - 1;
          }).map(([id]) => id),
          write: (unchanged) => {
            let changed = false;
            for (const id of unchanged) changed = this.revealed.delete(id) || changed;
            if (changed) queueMicrotask(() => {
              if (!this.destroyed) this.refresh();
            });
          },
        });
      }
    }
    return selected || this.revealed.has(key);
  }

  clear(): void {
    this.active = false;
    this.before.clear();
    this.revealed.clear();
  }
  private refresh(): void {
    this.view.dispatch({ effects: refreshPointerConceal.of(null) });
  }
  private release = (): void => {
    if (!this.active) return;
    this.clear();
    this.refresh();
  };
  destroy(): void {
    this.destroyed = true;
    this.clear();
    const doc = this.view.dom.ownerDocument;
    doc.removeEventListener("mousedown", this.down, true);
    doc.removeEventListener("mousemove", this.move, true);
    doc.defaultView!.removeEventListener("mouseup", this.release);
    doc.defaultView!.removeEventListener("blur", this.release);
    this.view.contentDOM.removeEventListener("keydown", this.release, true);
  }
}
