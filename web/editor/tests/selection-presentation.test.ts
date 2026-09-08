// @vitest-environment jsdom
import { EditorSelection } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { afterEach, describe, expect, it } from "vitest";
import { PhiMarkdownEditor } from "../src/editor";


if (!Range.prototype.getClientRects)
  Range.prototype.getClientRects = () => [] as unknown as DOMRectList;

const editors: PhiMarkdownEditor[] = [];
afterEach(() => {
  for (const editor of editors.splice(0)) editor.view.destroy();
  document.body.replaceChildren();
});
function open(text: string) {
  const parent = document.createElement("div");
  document.body.append(parent);
  const editor = new PhiMarkdownEditor(parent);
  editors.push(editor);
  editor.openDocument({ documentId: "selection", path: "selection.md", text, revision: 1, lineEnding: "LF" });
  return editor;
}

describe("immediate pointer selection presentation", () => {
  it("reveals scripts immediately and permits selecting their individual source characters", () => {
    const text = String.raw`Looking at the rectangle boundaries gives a <span style=color:#ed4564>drawing of the planar dual</span> \(G^*\):`;
    const editor = open(text);
    const { view } = editor;
    editor.updateSettings({ executableSnippets: true, latexConceal: true });
    const from = text.indexOf("G^");
    view.dispatch({ selection: { anchor: from } });
    expect(view.dom.querySelector(".cm-latex-conceal-script")).not.toBeNull();
    view.dispatch({ selection: EditorSelection.range(from, from + 2), userEvent: "select.pointer" });
    expect(view.dom.querySelector(".cm-latex-conceal-script")).toBeNull();
    expect(view.state.sliceDoc(view.state.selection.main.from, view.state.selection.main.to)).toBe("G^");
    view.dispatch({ selection: EditorSelection.range(from + 1, from + 2), userEvent: "select.pointer" });
    expect(view.state.sliceDoc(view.state.selection.main.from, view.state.selection.main.to)).toBe("^");
  });
});

it.each(["$x$", "\\(x\\)", "$$x$$", "\\[x\\]"])("reveals math at its closing edge: %s", (math) => {
  const { view } = open(`Before ${math} after`);
  view.dispatch({ selection: { anchor: 0 } });
  expect(view.dom.querySelector(".cm-content .math-inline, .cm-content .math-display")).not.toBeNull();
  view.dispatch({ selection: { anchor: 7 + math.length } });
  expect(view.dom.querySelector(".cm-content .math-inline, .cm-content .math-display")).toBeNull();
});

it.each([" ", "   ", "\t\t"])("preserves extra whitespace following a task checkbox: %j", (spaces) => {
  const { view } = open(`- [x]${spaces}done\n\nend`);
  view.dispatch({ selection: { anchor: view.state.doc.length } });
  expect(view.dom.querySelector(".task-checkbox")).not.toBeNull();
  expect(view.dom.querySelector(".cm-line")?.textContent).toBe(`${spaces.slice(1)}done`);
});


it("does not snapshot conceal atoms into a drag that will reveal command source", () => {
  const text = "testtestestestestetstesttesttesttesttestioajwefepiofjapfieo${\\displaystyle \\mathbb{R}^{2} }$";
  const editor = open(text);
  const { view } = editor;
  editor.updateSettings({ executableSnippets: true, latexConceal: true });
  view.dispatch({ selection: { anchor: text.indexOf("$") } });
  const atoms = () => view.state.facet(EditorView.atomicRanges)
    .reduce((count, get) => count + get(view).size, 0);
  expect(atoms()).toBeGreaterThan(0);
  // The document capture listener runs before CodeMirror takes its snapshot.
  // Suppress only jsdom's unsupported native coordinate hit testing.
  view.contentDOM.addEventListener("mousedown", event => event.stopImmediatePropagation(), { once: true });
  view.contentDOM.dispatchEvent(new MouseEvent("mousedown", { bubbles: true, button: 0, buttons: 1 }));
  expect(atoms()).toBe(0);
  const from = text.indexOf("mathbb") + 2;
  view.dispatch({ selection: EditorSelection.range(from, from + 1), userEvent: "select.pointer" });
  expect(view.state.sliceDoc(from, from + 1)).toBe("t");
  window.dispatchEvent(new MouseEvent("mouseup"));
  view.dispatch({ selection: { anchor: text.indexOf("$") } });
  expect(atoms()).toBeGreaterThan(0);
});
