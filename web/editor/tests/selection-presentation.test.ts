// @vitest-environment jsdom
import { EditorSelection } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { afterEach, describe, expect, it } from "vitest";
import { PhiMarkdownEditor } from "../src/editor";
import { holdSelectionPresentation, heldPresentationSelection } from "../src/markdown/selection-presentation";

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

describe("stable pointer selection presentation", () => {
  it("holds concealed scripts while selecting their exact source, then reveals on release", () => {
    const text = String.raw`Looking at the rectangle boundaries gives a <span style=color:#ed4564>drawing of the planar dual</span> \(G^*\):`;
    const editor = open(text);
    const { view } = editor;
    editor.updateSettings({ executableSnippets: true, latexConceal: true });
    const from = text.indexOf("G^");
    view.dispatch({ selection: { anchor: from } });
    expect(view.dom.querySelector(".cm-latex-conceal-script")).not.toBeNull();
    view.dispatch({ effects: holdSelectionPresentation.of(true) });
    view.dispatch({ selection: EditorSelection.range(from, from + 3), userEvent: "select.pointer" });
    expect(view.dom.querySelector(".cm-latex-conceal-script")).not.toBeNull();
    expect(view.state.sliceDoc(view.state.selection.main.from, view.state.selection.main.to)).toBe("G^*");
    window.dispatchEvent(new MouseEvent("mouseup"));
    expect(view.dom.querySelector(".cm-latex-conceal-script")).toBeNull();
    expect(view.state.selection.main.from).toBe(from);
    expect(view.state.selection.main.to).toBe(from + 3);
  });

  it("holds rendered math across a forward or backward drag and releases on blur", () => {
    const text = "Before $a^2$ after";
    const { view } = open(text);
    for (const [anchor, head] of [[0, text.length], [text.length, 0]]) {
      view.dispatch({ selection: { anchor } });
      expect(view.dom.querySelector(".cm-content .math-inline")).not.toBeNull();
      view.dispatch({ effects: holdSelectionPresentation.of(true) });
      view.dispatch({ selection: EditorSelection.range(anchor, head), userEvent: "select.pointer" });
      expect(view.dom.querySelector(".cm-content .math-inline")).not.toBeNull();
      window.dispatchEvent(new Event("blur"));
      expect(view.dom.querySelector(".cm-content .math-inline")).toBeNull();
      expect(view.state.selection.main.anchor).toBe(anchor);
      expect(view.state.selection.main.head).toBe(head);
    }
  });

  it("releases the held selection when editing starts", () => {
    const { view } = open("$x^2$");
    view.dispatch({ effects: holdSelectionPresentation.of(true) });
    view.dispatch({ changes: { from: 2, insert: "y" } });
    expect(view.state.field(heldPresentationSelection)).toBeNull();
  });

  it("installs a mouse selection hook without replacing CodeMirror hit testing", () => {
    const { view } = open("text $x^2$");
    const hooks = view.state.facet(EditorView.mouseSelectionStyle);
    for (const hook of hooks) expect(hook(view, new MouseEvent("mousedown", { button: 0 }))).toBeNull();
    expect(view.state.field(heldPresentationSelection)).not.toBeNull();
    window.dispatchEvent(new MouseEvent("mouseup"));
    expect(view.state.field(heldPresentationSelection)).toBeNull();
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
