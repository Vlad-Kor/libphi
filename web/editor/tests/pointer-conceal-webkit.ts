import { EditorSelection, EditorState } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { markdown } from "@codemirror/lang-markdown";
import { livePreview } from "../src/markdown/live-preview";
import { latexEnhancements } from "../src/latex-suite/enhancements";

const frame = () => new Promise<void>(resolve => setTimeout(resolve, 35));
export async function verifyPointerConceal(): Promise<void> {
  let stage = "creating";
  const onerror = window.onerror;
  window.onerror = (message, ...args) => onerror?.call(window, `${stage}: ${message}`, ...args);
  const parent = document.createElement("div");
  parent.style.cssText = "position:fixed;top:0;left:0;width:1000px;height:300px;z-index:100";
  document.body.append(parent);
  const text = String.raw`Looking at the rectangle boundaries gives a <span style=color:#ed4564>drawing of the planar dual</span> \(G^*\):`;
  const from = text.indexOf("G^");
  // Use the same presentation extensions, excluding background geometry
  // preflight: this test exercises pointer layout, not the idle resize pass.
  const view = new EditorView({ parent, state: EditorState.create({
    doc: text, selection: { anchor: from },
    extensions: [markdown(), livePreview.slice(0, -1), latexEnhancements(true), EditorView.lineWrapping],
  }) });
  try {
    let original: DOMRect | undefined;
    for (let spacing = 0; spacing <= 5; spacing += 0.02) {
      stage = `spacing ${spacing}`;
      view.contentDOM.style.letterSpacing = `${spacing}px`;
      view.dispatch({ selection: { anchor: from } });
      view.requestMeasure();
      await frame();
      const span = view.contentDOM.querySelector<HTMLElement>(".cm-latex-conceal-script");
      if (!span) throw new Error("Concealed script missing before selection");
      const before = span.getBoundingClientRect();
      view.dispatch({ selection: EditorSelection.range(from, from + 3) });
      await frame();
      const after = view.coordsAtPos(from + 1)!;
      if (after.top >= before.bottom) { original = before; break; }
    }
    if (!original) throw new Error("Could not reproduce the wrap boundary for G^*");
    view.dispatch({ selection: { anchor: from } });
    await frame();
    stage = "drag";
    const caret = view.coordsAtPos(from)!;
    // Start CodeMirror's real mouse selection, then move over the concealed
    // script. Its scheduled re-hit-tests must not start a render/reveal loop.
    view.contentDOM.dispatchEvent(new MouseEvent("mousedown", {
      bubbles: true, cancelable: true, button: 0, buttons: 1, detail: 1,
      clientX: caret.left, clientY: (caret.top + caret.bottom) / 2,
    }));
    document.dispatchEvent(new MouseEvent("mousemove", {
      bubbles: true, cancelable: true, buttons: 1,
      clientX: original.right, clientY: (caret.top + caret.bottom) / 2,
    }));
    // Ensure we enter the script even when WebKit maps the superscript's
    // trailing coordinate to its neighboring replacement boundary.
    view.dispatch({ selection: EditorSelection.range(from, from + 3), userEvent: "select.pointer" });
    if (view.contentDOM.querySelector(".cm-latex-conceal-script"))
      throw new Error("Pointer selection did not reveal the script immediately");
    for (let sample = 0; sample < 8; sample++) {
      await new Promise(resolve => setTimeout(resolve, 25));
      if (view.contentDOM.querySelector(".cm-latex-conceal-script"))
        throw new Error("Wrapped script re-concealed under stationary pointer");
    }
    // Precision remains ordinary source selection, including the single ^.
    view.dispatch({ selection: EditorSelection.range(from + 1, from + 2), userEvent: "select.pointer" });
    if (view.state.sliceDoc(view.state.selection.main.from, view.state.selection.main.to) !== "^")
      throw new Error("Cannot select an individual revealed script character");
    document.dispatchEvent(new MouseEvent("mouseup", { bubbles: true, button: 0 }));
  } finally {
    stage = "cleanup";
    view.destroy();
    parent.remove();
    window.onerror = onerror;
  }
}
