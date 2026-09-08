import { EditorSelection, EditorState } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { markdown } from "@codemirror/lang-markdown";
import { livePreview } from "../src/markdown/live-preview";
import { latexEnhancements } from "../src/latex-suite/enhancements";

const frame = () => new Promise<void>(resolve => setTimeout(resolve, 35));
export async function verifyPointerConceal(): Promise<void> {
  await verifyCommandDrag();
  await verifyPreviewEntry();
  await verifyWrappedEquation(String.raw`\(G^*\)`);
  await verifyWrappedEquation("$G^*$");
}

async function verifyWrappedEquation(math: string): Promise<void> {
  let stage = "creating";
  const onerror = window.onerror;
  window.onerror = (message, ...args) => onerror?.call(window, `${stage}: ${message}`, ...args);
  const parent = document.createElement("div");
  parent.style.cssText = "position:fixed;top:0;left:0;width:1000px;height:300px;z-index:100";
  document.body.append(parent);
  const text = `Looking at the rectangle boundaries gives a <span style=color:#ed4564>drawing of the planar dual</span> ${math}:`;
  const from = text.indexOf("G^");
  const anchor = text.indexOf(math);
  // Use the same presentation extensions, excluding background geometry
  // preflight: this test exercises pointer layout, not the idle resize pass.
  const view = new EditorView({ parent, state: EditorState.create({
    doc: text, selection: { anchor: from },
    extensions: [markdown(), livePreview.slice(0, -1), latexEnhancements(true), EditorView.lineWrapping],
  }) });
  try {
    // Find the expanded word's first wrap threshold without sweeping hundreds
    // of frames (background WebKit windows may throttle their timers).
    let low = 0, high = 5;
    view.dispatch({ selection: EditorSelection.range(from, from + 3) });
    for (let iteration = 0; iteration < 14; iteration++) {
      const spacing = (low + high) / 2;
      stage = `spacing ${spacing}`;
      view.contentDOM.style.letterSpacing = `${spacing}px`;
      view.requestMeasure();
      await frame();
      if (view.coordsAtPos(from + 1)!.top > view.coordsAtPos(0)!.bottom)
        high = spacing;
      else low = spacing;
    }
    view.contentDOM.style.letterSpacing = `${high}px`;
    view.dispatch({ selection: { anchor: from } });
    await frame();
    const span = view.contentDOM.querySelector<HTMLElement>(".cm-latex-conceal-script");
    if (!span) throw new Error("Concealed script missing before selection");
    const original = span.getBoundingClientRect();
    view.dispatch({ selection: EditorSelection.range(from, from + 3) });
    await frame();
    if (view.coordsAtPos(from + 1)!.top < original.bottom)
      throw new Error("Could not reproduce the wrap boundary for G^*");
    view.dispatch({ selection: { anchor } });
    await frame();
    stage = "drag";
    const caret = view.coordsAtPos(from)!;
    const start = view.coordsAtPos(anchor)!;
    // Start CodeMirror's real mouse selection, then move over the concealed
    // script. Its scheduled re-hit-tests must not start a render/reveal loop.
    view.contentDOM.dispatchEvent(new MouseEvent("mousedown", {
      bubbles: true, cancelable: true, button: 0, buttons: 1, detail: 1,
      clientX: start.left, clientY: (start.top + start.bottom) / 2,
    }));
    document.dispatchEvent(new MouseEvent("mousemove", {
      bubbles: true, cancelable: true, buttons: 1,
      clientX: original.right, clientY: (caret.top + caret.bottom) / 2,
    }));
    // Ensure we enter the script even when WebKit maps the superscript's
    // trailing coordinate to its neighboring replacement boundary.
    view.dispatch({ selection: EditorSelection.range(anchor, from + 3), userEvent: "select.pointer" });
    if (view.contentDOM.querySelector(".cm-latex-conceal-script"))
      throw new Error("Pointer selection did not reveal the script immediately");
    for (let sample = 0; sample < 8; sample++) {
      await new Promise(resolve => setTimeout(resolve, 25));
      if (view.contentDOM.querySelector(".cm-latex-conceal-script"))
        throw new Error("Wrapped script re-concealed under stationary pointer");
    }
    // The former base G is left of the superscript. Moving by even one pixel
    // here used to drop the guard although the same word had moved as a unit.
    for (const offset of [1, 2, 1, 0.5, 1.5, 1]) {
      document.dispatchEvent(new MouseEvent("mousemove", {
        bubbles: true, cancelable: true, buttons: 1,
        clientX: caret.left + offset, clientY: (caret.top + caret.bottom) / 2,
      }));
      await frame();
      if (view.contentDOM.querySelector(".cm-latex-conceal-script"))
        throw new Error(`Wrapped ${math} re-concealed over its former G position`);
    }
    const wrappedBase = view.coordsAtPos(from)!;
    if (wrappedBase.top <= caret.top) throw new Error("Expected G on the next row");
    for (const offset of [1, 2, 1, 0.5, 1.5, 1]) {
      document.dispatchEvent(new MouseEvent("mousemove", {
        bubbles: true, cancelable: true, buttons: 1,
        clientX: wrappedBase.left + offset,
        clientY: (wrappedBase.top + wrappedBase.bottom) / 2,
      }));
      await frame();
      if (view.contentDOM.querySelector(".cm-latex-conceal-script") ||
          view.coordsAtPos(from)!.top < wrappedBase.top - 1)
        throw new Error(`Wrapped ${math} jumped while moving over its new G position`);
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

async function verifyCommandDrag(): Promise<void> {
  const text = "testtestestestestetstesttesttesttesttestioajwefepiofjapfieo${\\displaystyle \\mathbb{R}^{2} }$";
  const parent = document.createElement("div");
  parent.style.cssText = "position:fixed;top:0;left:0;width:1000px;height:300px;z-index:100";
  document.body.append(parent);
  const anchor = text.indexOf("$");
  const view = new EditorView({ parent, state: EditorState.create({
    doc: text, selection: { anchor },
    extensions: [markdown(), livePreview.slice(0, -1), latexEnhancements(true), EditorView.lineWrapping],
  }) });
  const move = (x: number, y: number) => document.dispatchEvent(new MouseEvent("mousemove", {
    bubbles: true, cancelable: true, buttons: 1, clientX: x, clientY: y,
  }));
  try {
    await frame();
    const glyph = [...view.contentDOM.querySelectorAll(".cm-latex-conceal")]
      .find(element => element.textContent === "ℝ");
    if (!glyph) throw new Error("Missing concealed mathbb in command-drag fixture");
    const glyphBox = glyph.getBoundingClientRect();
    const start = view.coordsAtPos(anchor)!;
    view.contentDOM.dispatchEvent(new MouseEvent("mousedown", {
      bubbles: true, cancelable: true, button: 0, buttons: 1, detail: 1,
      clientX: start.left, clientY: (start.top + start.bottom) / 2,
    }));
    move(glyphBox.right, (glyphBox.top + glyphBox.bottom) / 2);
    await frame();
    for (const offset of [2, 3, 4, 3]) {
      const target = text.indexOf("mathbb") + offset;
      const caret = view.coordsAtPos(target)!;
      move(caret.left + 0.1, (caret.top + caret.bottom) / 2);
      await frame();
      if (view.state.selection.main.head !== target)
        throw new Error(`Command drag snapped from ${target} to ${view.state.selection.main.head}`);
    }
  } finally {
    document.dispatchEvent(new MouseEvent("mouseup", { bubbles: true, button: 0 }));
    view.destroy();
    parent.remove();
  }
}

async function verifyPreviewEntry(): Promise<void> {
  for (const source of ["$\\frac{abcdefgh}{ijklmnop}$", '<span style="color:#ed4564">drawing of the planar dual</span>']) {
    const text = `Before ${source} after`;
    const parent = document.createElement("div");
    parent.style.cssText = "position:fixed;top:0;left:0;width:1000px;height:300px;z-index:100";
    document.body.append(parent);
    const view = new EditorView({ parent, state: EditorState.create({
      doc: text, selection: { anchor: 0 },
      extensions: [markdown(), livePreview.slice(0, -1), latexEnhancements(true), EditorView.lineWrapping],
    }) });
    try {
      for (let attempt = 0; attempt < 100; attempt++) {
        await frame();
        if (!view.contentDOM.querySelector(".math-loading")) break;
      }
      const widget = view.contentDOM.querySelector<HTMLElement>(".math-widget, .raw-html-widget");
      if (!widget) throw new Error("Missing rendered preview in edge-entry fixture");
      const box = widget.getBoundingClientRect();
      const start = view.coordsAtPos(0)!;
      view.contentDOM.dispatchEvent(new MouseEvent("mousedown", {
        bubbles: true, cancelable: true, button: 0, buttons: 1, detail: 1,
        clientX: start.left, clientY: (start.top + start.bottom) / 2,
      }));
      document.dispatchEvent(new MouseEvent("mousemove", {
        bubbles: true, cancelable: true, buttons: 1,
        clientX: box.left + 1, clientY: (box.top + box.bottom) / 2,
      }));
      if (view.contentDOM.contains(widget))
        throw new Error(`Preview did not reveal at its first pixel: ${source}`);
      // Moving back out must not undo the reveal during the same gesture.
      document.dispatchEvent(new MouseEvent("mousemove", {
        bubbles: true, cancelable: true, buttons: 1,
        clientX: start.left, clientY: (start.top + start.bottom) / 2,
      }));
      await frame();
      if (view.contentDOM.querySelector(".math-widget, .raw-html-widget"))
        throw new Error("Preview re-rendered before drag release");
      document.dispatchEvent(new MouseEvent("mouseup", { bubbles: true, button: 0 }));
      if (!view.contentDOM.querySelector(".math-widget, .raw-html-widget"))
        throw new Error("Preview did not render again after selection left it and drag ended");
    } finally {
      document.dispatchEvent(new MouseEvent("mouseup", { bubbles: true, button: 0 }));
      view.destroy();
      parent.remove();
    }
  }
}
