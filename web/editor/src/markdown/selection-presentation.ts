import { EditorSelection, StateEffect, StateField, type EditorState } from "@codemirror/state";
import { EditorView, ViewPlugin } from "@codemirror/view";

// Source positions continue to track the real selection. Only presentation is
// held steady: exposing delimiters under a moving pointer changes wrapping and
// makes CodeMirror repeatedly select and deselect the same concealed text.
export const holdSelectionPresentation = StateEffect.define<boolean>();
export const heldPresentationSelection = StateField.define<EditorSelection | null>({
  create: () => null,
  update(value, transaction) {
    if (transaction.docChanged) return null;
    for (const effect of transaction.effects) {
      if (effect.is(holdSelectionPresentation))
        value = effect.value ? transaction.startState.selection : null;
    }
    return value;
  },
});

export function presentationSelection(state: EditorState): EditorSelection {
  return state.field(heldPresentationSelection, false) ?? state.selection;
}

const dragLifecycle = ViewPlugin.fromClass(class {
  constructor(readonly view: EditorView) {
    // Window bubbling runs after CodeMirror's document-level final selection.
    view.dom.ownerDocument.defaultView!.addEventListener("mouseup", this.release);
    view.dom.ownerDocument.defaultView!.addEventListener("blur", this.release);
    view.dom.ownerDocument.addEventListener("mousemove", this.move);
    view.contentDOM.addEventListener("keydown", this.release, true);
  }
  release = (): void => {
    if (this.view.state.field(heldPresentationSelection, false))
      this.view.dispatch({ effects: holdSelectionPresentation.of(false) });
  };
  move = (event: MouseEvent): void => {
    if (!(event.buttons & 1)) this.release();
  };
  destroy(): void {
    const win = this.view.dom.ownerDocument.defaultView!;
    win.removeEventListener("mouseup", this.release);
    win.removeEventListener("blur", this.release);
    this.view.dom.ownerDocument.removeEventListener("mousemove", this.move);
    this.view.contentDOM.removeEventListener("keydown", this.release, true);
  }
});

export const stableSelectionPresentation = [
  heldPresentationSelection,
  dragLifecycle,
  EditorView.mouseSelectionStyle.of((view, event) => {
    if (event.button === 0)
      view.dispatch({ effects: holdSelectionPresentation.of(true) });
    // Keep CodeMirror's hit testing and atomic replacement boundaries. Those
    // boundaries include hidden LaTeX syntax in the selected source range.
    return null;
  }),
];
