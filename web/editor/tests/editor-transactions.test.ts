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

  it("updates wiki embeds and regular Markdown image widths", () => {
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
    const text = String.raw`Formula $\frac{a_1}{b^2} + \alpha + \dot{x}^{2} + {\displaystyle \Gamma : V\cup E\to{2}^{\mathbb{R}^{2}} } + \text{hällo}$`;
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
      expect.arrayContaining(["(", ")", "/", "1", "α", "x\u0307", "2", "ℝ", "hällo"]),
    );
    expect(concealed.some((element) =>
      element.title === String.raw`^{\mathbb{R}^{2}}`)).toBe(false);

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
    let attachmentRequest: { type: string; payload?: Record<string, unknown> } | undefined;
    const captureRequest = (event: Event) => {
      const message = (event as CustomEvent).detail as {
        type: string;
        payload?: Record<string, unknown>;
      };
      if (message.type === "attachment/resolve") attachmentRequest = message;
    };
    window.addEventListener("phi-native-message", captureRequest);
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
    window.removeEventListener("phi-native-message", captureRequest);
    expect(attachmentRequest?.payload).toMatchObject({
      target: "Pasted image.png",
      relative: true,
    });
    expect(parent.querySelectorAll(".list-bullet").length).toBeGreaterThanOrEqual(3);
    const image = parent.querySelector<HTMLElement>(".image-widget");
    expect(image).not.toBeNull();
    expect(image?.classList.contains("image-widget-inline")).toBe(true);
    expect(image?.tagName).toBe("SPAN");
    expect(parent.querySelectorAll(".math-display")).toHaveLength(1);
  });

  it("gives list lines a hanging indent and uses four-space nesting", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "list-layout",
      path: "list-layout.md",
      text: "- A long item that wraps beneath its content",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: editor.view.state.doc.length } });
    const line = parent.querySelector<HTMLElement>(".cm-line");
    expect(line?.classList.contains("cm-live-list-item")).toBe(true);
    expect(line?.style.getPropertyValue("--phi-list-content-indent")).toBe("1.1em");

    editor.view.dispatch({ selection: { anchor: 8 } });
    expect(key(editor.view, "Tab")).toBe(true);
    expect(editor.getDocument()).toBe("    - A long item that wraps beneath its content");
    expect(key(editor.view, "Tab", true)).toBe(true);
    expect(editor.getDocument()).toBe("- A long item that wraps beneath its content");
  });

  it("aligns task and ordered list continuations with their first line", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "all-list-layouts",
      path: "all-list-layouts.md",
      text: "- bullet text\n- [ ] task text\n10. ordered text",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: editor.view.state.doc.length } });
    const lines = [...parent.querySelectorAll<HTMLElement>(".cm-line")];
    expect(lines).toHaveLength(3);
    expect(lines[0].style.getPropertyValue("--phi-list-content-indent"))
      .toBe("1.1em");
    expect(lines[1].style.getPropertyValue("--phi-list-content-indent"))
      .toBe("1.95em");
    expect(lines[2].classList.contains("cm-live-ordered-list-item")).toBe(true);
    expect(lines[2].querySelector(".list-number")?.textContent).toBe("10.");

    const css = readFileSync("src/styles/editor.css", "utf8");
    expect(css).toMatch(/\.task-checkbox\s*\{[^}]*translateY\(1px\)/s);
    expect(css).toMatch(/\.list-number, \.cm-live-list-number\s*\{[^}]*var\(--editor-muted\)/s);
    expect(css).toMatch(/\.cm-tooltip-autocomplete\s*\{[^}]*border-radius: 12px/s);
    expect(css).toMatch(/\.cm-completionIcon\s*\{\s*display: none/s);
  });

  it("reveals the complete task prefix only when the cursor enters it", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "- [ ] task text";
    editor.openDocument({
      documentId: "task-prefix",
      path: "task-prefix.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });

    editor.view.dispatch({ selection: { anchor: text.indexOf("text") } });
    expect(parent.querySelector(".task-checkbox")).not.toBeNull();
    expect(parent.querySelector(".cm-line")?.textContent).not.toContain("-");

    editor.view.dispatch({ selection: { anchor: 0 } });
    expect(parent.querySelector(".task-checkbox")).toBeNull();
    expect(parent.querySelector(".cm-line")?.textContent).toContain("- [ ] task text");
  });

  it("renders a standalone triple-dash line as a horizontal rule", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "horizontal-rule",
      path: "horizontal-rule.md",
      text: "Before\n\n---\n\nAfter",
      revision: 1,
      lineEnding: "LF",
    });
    expect(parent.querySelector("hr.horizontal-rule-widget")).not.toBeNull();
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

  it("reveals the selected source word instead of the front of rich blocks", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "```text\nfirst second\n```\n\n> [!note] Title\n> alpha beta\n\nOutside";
    editor.openDocument({
      documentId: "block-cursor-mapping",
      path: "block-cursor-mapping.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.length } });

    const selectWord = (root: Element, word: string) => {
      const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
      let node = walker.nextNode() as Text | null;
      while (node && !node.data.includes(word))
        node = walker.nextNode() as Text | null;
      expect(node).not.toBeNull();
      const at = node!.data.indexOf(word);
      const range = document.createRange();
      range.setStart(node!, at);
      range.setEnd(node!, at + word.length);
      const selection = window.getSelection();
      selection?.removeAllRanges();
      selection?.addRange(range);
    };

    const code = parent.querySelector(".code-block-widget")!;
    selectWord(code, "second");
    code.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    expect(editor.view.state.selection.main.head).toBe(text.indexOf("second"));

    editor.view.dispatch({ selection: { anchor: text.length } });
    const callout = parent.querySelector(".callout-content")!;
    selectWord(callout, "beta");
    callout.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    expect(editor.view.state.selection.main.head).toBe(text.indexOf("beta"));
  });

  it("syntax-highlights an HTML span while its source is active", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "<span style=color:#8279fc>value</span>\n\nOutside";
    editor.openDocument({
      documentId: "html-source-highlighting",
      path: "html-source-highlighting.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.indexOf("#8279fc") } });
    expect(parent.querySelectorAll(".cm-html-tag")).toHaveLength(2);
    expect(parent.querySelector(".cm-html-attribute")?.textContent).toBe("style");
    expect(parent.querySelector(".cm-html-value")?.textContent).toBe("color:#8279fc");
  });

  it("renders inline LaTeX in Live Preview callout titles", async () => {
    const calls: string[] = [];
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: (latex: string) => {
        calls.push(latex);
        return document.createElement("mjx-container");
      },
    };
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "> [!info] Energy $callout_title_unique$\n> body\n\nOutside";
    editor.openDocument({
      documentId: "callout-title-math",
      path: "callout-title-math.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.length } });
    await settle();

    expect(parent.querySelector(".callout-title .math-inline")).not.toBeNull();
    expect(calls).toContain("callout_title_unique");
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
    expect(document.activeElement).toBe(search);

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
    expect(editor.view.state.doc.toString()).toBe("- parent\n    - child");
    expect(key(editor.view, "Tab", true)).toBe(true);
    expect(editor.view.state.doc.toString()).toBe("- parent\n- child");
  });

  it("continues task items even when text touches the closing bracket", () => {
    const compact = viewFor("- [ ]word", 9, 9, latexSuite);
    expect(key(compact, "Enter")).toBe(true);
    expect(compact.state.doc.toString()).toBe("- [ ]word\n- [ ] ");

    const spaced = viewFor("    - [x] finished", 18, 18, latexSuite);
    expect(key(spaced, "Enter")).toBe(true);
    expect(spaced.state.doc.toString())
      .toBe("    - [x] finished\n    - [ ] ");
  });

  it("continues every list type without a blank line and advances numbers", () => {
    const bullet = viewFor("- first", 7, 7, latexSuite);
    expect(key(bullet, "Enter")).toBe(true);
    expect(bullet.state.doc.toString()).toBe("- first\n- ");

    const task = viewFor("- [x] done", 10, 10, latexSuite);
    expect(key(task, "Enter")).toBe(true);
    expect(task.state.doc.toString()).toBe("- [x] done\n- [ ] ");

    const ordered = viewFor("9. ninth", 8, 8, latexSuite);
    expect(key(ordered, "Enter")).toBe(true);
    expect(ordered.state.doc.toString()).toBe("9. ninth\n10. ");
  });

  it("starts an indented ordered sublist at one", () => {
    const view = viewFor("1. first\n2. second", 18, 18, latexSuite);
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.doc.toString()).toBe("1. first\n    1. second");

    const multiple = viewFor("7. alpha\n8. beta", 0, 16, latexSuite);
    expect(key(multiple, "Tab")).toBe(true);
    expect(multiple.state.doc.toString()).toBe("    1. alpha\n    2. beta");
  });

  it("cycles selected text from emphasis to strong with asterisks", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "asterisk-cycle",
      path: "asterisk-cycle.md",
      text: "this",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 0, head: 4 } });
    expect(key(editor.view, "*", true)).toBe(true);
    expect(editor.getDocument()).toBe("*this*");
    expect(editor.view.state.sliceDoc(
      editor.view.state.selection.main.from,
      editor.view.state.selection.main.to,
    )).toBe("this");
    expect(key(editor.view, "*", true)).toBe(true);
    expect(editor.getDocument()).toBe("**this**");
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

  it("runs visual snippets directly from shifted character shortcuts", () => {
    const selected = "${VISUAL}";
    setCustomSnippets(String.raw`[
      {trigger: "B", replacement: "<span style='color:blue'>${selected}</span>", options: "tA"}
    ]`);
    const visual = viewFor("blue text", 0, 9, latexSuite);
    visual.contentDOM.dispatchEvent(new KeyboardEvent("keydown", {
      key: "B", shiftKey: true, bubbles: true, cancelable: true,
    }));
    expect(visual.state.doc.toString())
      .toBe("<span style='color:blue'>blue text</span>");
  });

  it("only expands visual shortcuts when text is selected", async () => {
    const selected = "${VISUAL}";
    setCustomSnippets(String.raw`[
      {trigger: "U", replacement: "\\underbrace{ ${selected} }_{ $0 }", options: "mA"},
      {trigger: "X", replacement: "\\boxed{ ${selected} }", options: "mA"},
      {trigger: "B", replacement: "{\\textcolor{#8279fc}{ ${selected} }$0}", options: "m"}
    ]`);

    const typedU = viewFor("$$", 1, 1, latexSuite);
    typedU.dispatch({
      changes: { from: 1, insert: "U" },
      selection: { anchor: 2 },
      userEvent: "input.type",
    });
    await settle();
    expect(typedU.state.doc.toString()).toBe("$U$");

    const typedB = viewFor("$$", 1, 1, latexSuite);
    typedB.dispatch({
      changes: { from: 1, insert: "B" },
      selection: { anchor: 2 },
      userEvent: "input.type",
    });
    await settle();
    expect(typedB.state.doc.toString()).toBe("$B$");

    const underbrace = viewFor("$value$", 1, 6, latexSuite);
    underbrace.contentDOM.dispatchEvent(new KeyboardEvent("keydown", {
      key: "U", shiftKey: true, bubbles: true, cancelable: true,
    }));
    expect(underbrace.state.doc.toString())
      .toBe("$\\underbrace{ value }_{  }$");

    const blue = viewFor("$value$", 1, 6, latexSuite);
    blue.contentDOM.dispatchEvent(new KeyboardEvent("keydown", {
      key: "B", shiftKey: true, bubbles: true, cancelable: true,
    }));
    expect(blue.state.doc.toString())
      .toBe("${\\textcolor{#8279fc}{ value }}$");
  });

  it("runs bundled default snippets and function handlers", async () => {
    setCustomSnippets();

    const greek = viewFor("$@$", 2, 2, latexSuite);
    greek.dispatch({
      changes: { from: 2, insert: "a" },
      selection: { anchor: 3 },
      userEvent: "input.type",
    });
    await settle();
    expect(greek.state.doc.toString()).toBe("$\\alpha$");

    const identity = viewFor("$iden$", 5, 5, latexSuite);
    identity.dispatch({
      changes: { from: 5, insert: "3" },
      selection: { anchor: 6 },
      userEvent: "input.type",
    });
    await settle();
    expect(identity.state.doc.toString()).toContain(
      "\\begin{pmatrix}\n1 & 0 & 0 \\\\\n0 & 1 & 0 \\\\\n0 & 0 & 1\n\\end{pmatrix}",
    );
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
