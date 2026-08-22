// @vitest-environment jsdom
/// <reference types="node" />
import { history, undo } from "@codemirror/commands";
import { getSearchQuery } from "@codemirror/search";
import { EditorSelection, EditorState } from "@codemirror/state";
import { EditorView, runScopeHandlers, showTooltip, type Tooltip } from "@codemirror/view";
import { readFileSync } from "node:fs";
import { afterEach, describe, expect, it, vi } from "vitest";
import { runEditingCommand } from "../src/commands";
import { PhiMarkdownEditor } from "../src/editor";
import { latexSuite, setCustomSnippets } from "../src/latex-suite/engine";
import { renderMath } from "../src/math/mathjax";
import { resizeImageMarkdown, TaskWidget } from "../src/widgets/preview";

const views: EditorView[] = [];
afterEach(() => {
  for (const view of views.splice(0)) view.destroy();
  document.body.replaceChildren();
  document.body.className = "";
  delete document.documentElement.dataset.theme;
  document.documentElement.removeAttribute("style");
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
    checkbox.click();
    expect(view.state.doc.toString()).toBe("- [x] Item");

    const customView = viewFor("- [-] Cancelled");
    const custom = new TaskWidget("-", 2).toDOM(customView) as HTMLInputElement;
    custom.dispatchEvent(new Event("change"));
    expect(customView.state.doc.toString()).toBe("- [-] Cancelled");
  });

  it("updates Obsidian and regular Markdown image widths", () => {
    expect(resizeImageMarkdown("![[diagram.png]]", 341.6)).toBe("![[diagram.png|342]]");
    expect(resizeImageMarkdown("![[diagram.png|200]]", 480)).toBe("![[diagram.png|480]]");
    expect(resizeImageMarkdown("![Diagram](<images/my diagram.png>)", 275))
      .toBe("![Diagram|275](<images/my diagram.png>)");
    expect(resizeImageMarkdown("![Diagram|200](image.png)", 20))
      .toBe("![Diagram|40](image.png)");
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
      tex2svg: () => document.createElement("mjx-container"),
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

  it("syntax-highlights LaTeX and enables cursor-aware conceal on demand", () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: () => document.createElement("mjx-container"),
    };
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = String.raw`Formula $\frac{a_1}{b^2} + \alpha + \dot{x}^{2}$`;
    editor.openDocument({
      documentId: "latex-enhancements",
      path: "latex-enhancements.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.indexOf(" + ") + 1 } });

    expect(parent.querySelector(".cm-latex-command")).not.toBeNull();
    expect(parent.querySelector(".cm-latex-bracket")).not.toBeNull();
    expect(parent.querySelector(".cm-latex-script-operator")).not.toBeNull();
    expect(parent.querySelector(".cm-latex-conceal")).toBeNull();
    editor.view.dispatch({ selection: { anchor: text.indexOf("a_1") + 1 } });
    expect(parent.querySelectorAll(".cm-latex-bracket-active")).toHaveLength(2);
    editor.view.dispatch({ selection: { anchor: text.indexOf(" + ") + 1 } });

    editor.updateSettings({ latexConceal: true });
    const concealed = [...parent.querySelectorAll<HTMLElement>(".cm-latex-conceal")];
    expect(concealed.map((element) => element.textContent)).toEqual(
      expect.arrayContaining(["(", ")", "/", "1", "α", "x\u0307", "2"]),
    );

    const alpha = text.indexOf(String.raw`\alpha`);
    editor.view.dispatch({ selection: { anchor: alpha + 2 } });
    expect([...parent.querySelectorAll<HTMLElement>(".cm-latex-conceal")]
      .some((element) => element.title === String.raw`\alpha`)).toBe(false);
    expect(parent.querySelector(".cm-latex-command")).not.toBeNull();

    editor.view.dispatch({ selection: { anchor: 0 } });
    expect(parent.querySelector(".math-inline")).not.toBeNull();
    expect(parent.querySelector(".cm-latex-conceal")).toBeNull();
  });

  it("builds sorted previews for nested rich lists and keeps list images inline", () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: () => document.createElement("mjx-container"),
    };
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = `# Speech Processing
## **Tasks**
- Automatic Speech Recognition
\t- recognize: <span style=color:#ed4564>audio stream -&gt; text</span>
* **Formants** use $F_1,F_2$
- ![[Pasted image.png|400]]

$$
x = 0
$$`;
    expect(() => editor.openDocument({
      documentId: "nested-rich-list",
      path: "nested-rich-list.md",
      text,
      revision: 1,
      lineEnding: "LF",
    })).not.toThrow();
    expect(parent.querySelectorAll(".list-bullet").length).toBeGreaterThanOrEqual(3);
    const image = parent.querySelector<HTMLElement>(".image-widget");
    expect(image).not.toBeNull();
    expect(image?.classList.contains("image-widget-inline")).toBe(true);
    expect(image?.tagName).toBe("SPAN");
    expect(parent.querySelectorAll(".math-display")).toHaveLength(1);
  });

  it("keeps standalone images in line geometry and remeasures after loading", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const measure = vi.spyOn(editor.view, "requestMeasure");
    editor.openDocument({
      documentId: "image-hitbox",
      path: "01 Introduction.md",
      text: "###### 2012 +\n![[Pasted image 20260724164322.png]]",
      revision: 1,
      lineEnding: "LF",
    });

    const widget = parent.querySelector<HTMLElement>(".image-widget-block");
    expect(widget?.tagName).toBe("SPAN");
    widget?.querySelector("img")?.dispatchEvent(new Event("load"));
    expect(measure).toHaveBeenCalled();
  });

  it("shows the active math preview in a floating bubble above the source", () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: () => document.createElement("mjx-container"),
    };
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "$$\nx = 0\n$$";
    editor.openDocument({ documentId: "math", path: "math.md", text, revision: 1, lineEnding: "LF" });

    editor.view.dispatch({ selection: { anchor: 5 } });
    const tooltips = editor.view.state.facet(showTooltip)
      .filter((tooltip): tooltip is Tooltip => tooltip != null);
    expect(tooltips).toHaveLength(1);
    expect(tooltips[0].above).toBe(true);
    expect(tooltips[0].strictSide).toBe(true);
    const tooltipView = tooltips[0].create(editor.view);
    expect(tooltipView.dom.classList.contains("math-preview-bubble")).toBe(true);
    expect(tooltipView.dom.querySelector(".math-edit-preview")).not.toBeNull();
    tooltipView.destroy?.();
    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(editor.view.state.facet(showTooltip).filter(Boolean)).toHaveLength(0);
    expect(parent.querySelectorAll(".math-display")).toHaveLength(1);
  });

  it("disables MathJax inline line breaking so lone relation symbols are not clipped", () => {
    const source = readFileSync(
      "src/math/mathjax-config.js",
      "utf8",
    );
    new Function("window", source)(window);
    const configured = (window as unknown as {
      MathJax?: { svg?: { linebreaks?: { inline?: boolean } } };
    }).MathJax;
    expect(configured?.svg?.linebreaks?.inline).toBe(false);
  });

  it("keeps the final callout source line active when clicking its empty area", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "Before\n\n> [!info] test\n> test";
    editor.openDocument({
      documentId: "callout-click",
      path: "callout-click.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    expect(parent.querySelector(".callout")).not.toBeNull();

    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".callout")).toBeNull();
    editor.view.dispatch({ selection: { anchor: text.lastIndexOf("test") + 2 } });
    expect(parent.querySelector(".callout")).toBeNull();
  });

  it("can disable and restore the readable editor width", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    expect(document.body.classList.contains("full-width-editor")).toBe(false);
    editor.updateSettings({ readableLineWidth: false });
    expect(document.body.classList.contains("full-width-editor")).toBe(true);
    editor.updateSettings({ readableLineWidth: true });
    expect(document.body.classList.contains("full-width-editor")).toBe(false);
  });

  it("restructures search as a centered GNOME-style grid without changing its behavior", async () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "search-ui",
      path: "search-ui.md",
      text: "alpha beta alpha",
      revision: 1,
      lineEnding: "LF",
    });
    editor.runCommand("editor.find");
    await settle();

    const panel = parent.querySelector<HTMLElement>(".cm-panel.cm-search");
    const grid = panel?.querySelector<HTMLElement>(".phi-search-grid");
    const search = grid?.querySelector<HTMLInputElement>('input[name="search"]');
    const replace = grid?.querySelector<HTMLInputElement>('input[name="replace"]');
    const navigation = grid?.querySelector<HTMLElement>(".phi-search-navigation");
    expect(grid).not.toBeNull();
    expect(search?.parentElement?.classList.contains("phi-search-field")).toBe(true);
    expect(replace?.parentElement?.classList.contains("phi-replace-field")).toBe(true);
    expect([...navigation!.querySelectorAll("button")].map((button) => button.name))
      .toEqual(["prev", "next"]);
    expect(grid?.querySelectorAll(".phi-symbolic-icon").length).toBeGreaterThanOrEqual(6);
    expect(panel?.querySelector(':scope > label')).toBeNull();

    const optionsButton = grid?.querySelector<HTMLButtonElement>(".phi-search-options-button");
    const options = grid?.querySelector<HTMLElement>(".phi-search-options-popover");
    expect(options?.hidden).toBe(true);
    optionsButton?.click();
    expect(options?.hidden).toBe(false);
    expect(options?.querySelectorAll('input[type="checkbox"]')).toHaveLength(3);

    search!.value = "alpha";
    search!.dispatchEvent(new KeyboardEvent("keyup", { bubbles: true, key: "a" }));
    expect(getSearchQuery(editor.view.state).search).toBe("alpha");
    grid?.querySelector<HTMLButtonElement>('button[name="next"]')?.click();
    expect(editor.view.state.selection.main).toMatchObject({ from: 0, to: 5 });

    const toggle = grid?.querySelector<HTMLButtonElement>(".phi-replace-toggle");
    toggle?.click();
    expect(panel?.classList.contains("phi-show-replace")).toBe(true);
    expect(toggle?.getAttribute("aria-pressed")).toBe("true");
    replace!.value = "omega";
    replace!.dispatchEvent(new KeyboardEvent("keyup", { bubbles: true, key: "a" }));
    grid?.querySelector<HTMLButtonElement>('button[name="replace"]')?.click();
    expect(editor.view.state.doc.toString()).toBe("omega beta alpha");
  });

  it("marks CodeMirror dark so floating previews use its dark tooltip theme", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.updateTheme({
      dark: true,
      foreground: "rgb(238, 238, 236)",
      toolbar: "rgb(48, 48, 48)",
      entry: "rgb(58, 58, 58)",
      border: "rgba(255, 255, 255, .12)",
      accent: "rgb(120, 174, 237)",
    });
    expect(editor.view.state.facet(EditorView.darkTheme)).toBe(true);
    expect(document.documentElement.dataset.theme).toBe("dark");
    expect(document.documentElement.style.getPropertyValue("--editor-toolbar-bg"))
      .toBe("rgb(48, 48, 48)");
    expect(document.documentElement.style.getPropertyValue("--editor-accent"))
      .toBe("rgb(120, 174, 237)");
    editor.updateTheme({ dark: false });
    expect(editor.view.state.facet(EditorView.darkTheme)).toBe(false);
    expect(document.documentElement.style.getPropertyValue("--editor-toolbar-bg")).toBe("");
  });

  it("converts display math directly without exposing its delimiters", async () => {
    const calls: string[] = [];
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: (latex: string) => {
        calls.push(latex);
        return document.createElement("mjx-container");
      },
    };
    const target = document.createElement("div");
    await renderMath("theta_unique = 7", true, target);
    expect(calls).toEqual(["theta_unique = 7"]);
    expect(target.querySelector("mjx-container")).not.toBeNull();
    expect(target.textContent).not.toContain("$$");
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
