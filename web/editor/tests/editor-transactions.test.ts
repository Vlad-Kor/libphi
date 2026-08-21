// @vitest-environment jsdom
import { history, undo } from "@codemirror/commands";
import { EditorSelection, EditorState } from "@codemirror/state";
import { EditorView, runScopeHandlers } from "@codemirror/view";
import { afterEach, describe, expect, it } from "vitest";
import { runEditingCommand } from "../src/commands";
import { PhiMarkdownEditor } from "../src/editor";
import { latexSuite, setCustomSnippets } from "../src/latex-suite/engine";
import { TaskWidget } from "../src/widgets/preview";

const views: EditorView[] = [];
afterEach(() => {
  for (const view of views.splice(0)) view.destroy();
  document.body.replaceChildren();
  document.body.className = "";
  delete (window as unknown as { MathJax?: unknown }).MathJax;
});

function viewFor(text: string, anchor = text.length, head = anchor, extensions: unknown[] = []): EditorView {
  const parent = document.createElement("div");
  document.body.append(parent);
  const view = new EditorView({
    parent,
    state: EditorState.create({
      doc: text,
      selection: EditorSelection.range(anchor, head),
      extensions: [history(), ...(extensions as never[])],
    }),
  });
  views.push(view);
  return view;
}

function key(view: EditorView, value: string, shiftKey = false): boolean {
  return runScopeHandlers(view, new KeyboardEvent("keydown", { key: value, shiftKey }), "editor");
}

async function settle(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

describe("CodeMirror document transactions", () => {
  it("formats selections and undoes without touching unrelated text", () => {
    const view = viewFor("before hello after", 7, 12);
    expect(runEditingCommand("editor.bold", view)).toBe(true);
    expect(view.state.doc.toString()).toBe("before **hello** after");
    expect(undo(view)).toBe(true);
    expect(view.state.doc.toString()).toBe("before hello after");
  });

  it("toggles task source through the live checkbox widget", () => {
    const view = viewFor("- [ ] Item");
    const checkbox = new TaskWidget(" ", 2).toDOM(view) as HTMLInputElement;
    checkbox.dispatchEvent(new Event("change"));
    expect(view.state.doc.toString()).toBe("- [x] Item");

    const customView = viewFor("- [-] Cancelled");
    const custom = new TaskWidget("-", 2).toDOM(customView) as HTMLInputElement;
    custom.dispatchEvent(new Event("change"));
    expect(customView.state.doc.toString()).toBe("- [-] Cancelled");
  });

  it("round-trips untouched CRLF and Unicode byte-for-byte", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "---\r\ncssclasses: [wide-page]\r\n---\r\n# Prüfung\r\n\r\n日本語 🧪 and %%source%%\r\n";
    editor.openDocument({
      documentId: "fixture",
      path: "fixture.md",
      text,
      revision: 4,
      lineEnding: "CRLF",
    });
    expect(editor.getDocument()).toBe(text);
    expect(document.body.classList.contains("wide-page")).toBe(true);
  });

  it("keeps a one-megabyte note editable", { timeout: 15_000 }, () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const paragraph = "A research paragraph with Unicode 日本語, ä, and $x$.\n\n";
    const text = `# Large note\n\n${paragraph.repeat(Math.ceil(1_000_000 / paragraph.length))}`;
    editor.openDocument({
      documentId: "large",
      path: "large.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ changes: { from: text.length, insert: "tail" }, userEvent: "input.type" });
    expect(editor.getDocument().endsWith("tail")).toBe(true);
  });

  it("creates Live Preview widgets for bullets and both math modes", () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      typesetPromise: () => Promise.resolve(),
    };
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "preview",
      path: "preview.md",
      text: "# Preview\n\n- bullet\n\nInline $x^2$\n\n$$\n\\boxed{y}\n$$\n",
      revision: 1,
      lineEnding: "LF",
    });
    expect(parent.querySelectorAll(".list-bullet")).toHaveLength(1);
    expect(parent.querySelectorAll(".math-inline")).toHaveLength(1);
    expect(parent.querySelectorAll(".math-display")).toHaveLength(1);
  });

  it("indents and outdents hierarchical list items with Tab", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "lists",
      path: "lists.md",
      text: "- parent\n- child",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 11 } });
    expect(key(editor.view, "Tab")).toBe(true);
    expect(editor.view.state.doc.toString()).toBe("- parent\n  - child");
    expect(key(editor.view, "Tab", true)).toBe(true);
    expect(editor.view.state.doc.toString()).toBe("- parent\n- child");
  });
});

describe("LaTeX Suite transactions", () => {
  it("expands automatic snippets, tabstops, and auto-fractions", async () => {
    setCustomSnippets(String.raw`[
      {trigger: "sq", replacement: "\\sqrt{$0}$1", options: "mA"},
      {trigger: "@a", replacement: "\\alpha", options: "mA"}
    ]`);
    const view = viewFor("$s$", 2, 2, latexSuite);
    view.dispatch({ changes: { from: 2, insert: "q" }, selection: { anchor: 3 }, userEvent: "input.type" });
    await settle();
    expect(view.state.doc.toString()).toBe("$\\sqrt{}$");
    expect(undo(view)).toBe(true);
    expect(view.state.doc.toString()).toBe("$s$");

    const fraction = viewFor("$x$", 2, 2, latexSuite);
    fraction.dispatch({ changes: { from: 2, insert: "/" }, selection: { anchor: 3 }, userEvent: "input.type" });
    await settle();
    expect(fraction.state.doc.toString()).toBe("$\\frac{x}{}$");
  });

  it("supports regex variables and visual selections", async () => {
    const greek = "${GREEK}";
    const selected = "${VISUAL}";
    setCustomSnippets(String.raw`[
      {trigger: "(x)(${greek})", replacement: "[[0]]\\[[1]]", options: "rmA"},
      {trigger: "C", replacement: "\\cancel{${selected}}$0", options: "mA"}
    ]`);
    const regex = viewFor("$xalph", 6, 6, latexSuite);
    regex.dispatch({ changes: { from: 6, insert: "a" }, selection: { anchor: 7 }, userEvent: "input.type" });
    await settle();
    expect(regex.state.doc.toString()).toBe("$x\\alpha");

    const visual = viewFor("$x$", 1, 2, latexSuite);
    visual.dispatch({ changes: { from: 1, to: 2, insert: "C" }, selection: { anchor: 2 }, userEvent: "input.type" });
    await settle();
    expect(visual.state.doc.toString()).toBe("$\\cancel{x}$");
  });

  it("uses matrix-aware Tab/Enter and logical tab-out", () => {
    setCustomSnippets("[]");
    const matrixText = "$\\begin{pmatrix}a\\end{pmatrix}$";
    const cell = matrixText.indexOf("a\\end") + 1;
    const matrix = viewFor(matrixText, cell, cell, latexSuite);
    expect(key(matrix, "Tab")).toBe(true);
    expect(matrix.state.doc.toString()).toContain("a & \\end");
    expect(key(matrix, "Enter")).toBe(true);
    expect(matrix.state.doc.toString()).toContain(" &  \\\\\n");

    const fractionText = "$\\frac{a}{b}$";
    const brace = fractionText.indexOf("}");
    const fraction = viewFor(fractionText, brace, brace, latexSuite);
    expect(key(fraction, "Tab")).toBe(true);
    expect(fraction.state.selection.main.head).toBe(brace + 1);
  });
});
