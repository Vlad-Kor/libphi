// @vitest-environment jsdom
import { foldEffect, foldedRanges } from "@codemirror/language";
import { runScopeHandlers } from "@codemirror/view";
import { afterEach, expect, it } from "vitest";
import { PhiMarkdownEditor } from "../src/editor";

const editors: PhiMarkdownEditor[] = [];
afterEach(() => {
  for (const editor of editors.splice(0)) editor.view.destroy();
  document.body.replaceChildren();
});
function editorFor(text: string): PhiMarkdownEditor {
  const parent = document.body.appendChild(document.createElement("div"));
  const editor = new PhiMarkdownEditor(parent);
  editors.push(editor);
  editor.openDocument({ documentId: "search", path: "search.md", text,
    revision: 1, lineEnding: "LF" });
  return editor;
}
it("selects the existing query through both native find and the search-panel keymap", async () => {
  const editor = editorFor("first match, second match");
  editor.runCommand("editor.find");
  await Promise.resolve();
  const input = editor.view.dom.querySelector<HTMLInputElement>('input[name="search"]')!;
  input.value = "match";
  input.dispatchEvent(new Event("input", { bubbles: true }));
  input.setSelectionRange(5, 5);
  expect(runScopeHandlers(editor.view, new KeyboardEvent("keydown", {
    key: "f", ctrlKey: true, cancelable: true,
  }), "search-panel")).toBe(true);
  await Promise.resolve();
  expect(document.activeElement).toBe(input);
  expect([input.selectionStart, input.selectionEnd]).toEqual([0, 5]);
  input.setSelectionRange(2, 2);
  editor.runCommand("editor.find");
  await Promise.resolve();
  expect([input.selectionStart, input.selectionEnd]).toEqual([0, 5]);
});
it("reveals and selects a native result inside a folded section", () => {
  const editor = editorFor("# Heading\ninside match\n# Next\nend");
  editor.view.dispatch({ effects: foldEffect.of({ from: 9, to: 22 }) });
  expect(foldedRanges(editor.view.state).size).toBe(1);
  editor.receive({ version: 1, type: "navigation/reveal-range", payload: { from: 17, to: 22 } });
  expect(editor.view.state.sliceDoc(editor.view.state.selection.main.from,
    editor.view.state.selection.main.to)).toBe("match");
  expect(foldedRanges(editor.view.state).size).toBe(0);
});
