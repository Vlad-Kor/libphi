// @vitest-environment jsdom
/// <reference types="node" />
import { closeBrackets, insertBracket } from "@codemirror/autocomplete";
import { history, undo } from "@codemirror/commands";
import { getSearchQuery } from "@codemirror/search";
import { EditorSelection, EditorState } from "@codemirror/state";
import { EditorView, runScopeHandlers, showTooltip, type Tooltip } from "@codemirror/view";
import { readFileSync } from "node:fs";
import { afterEach, describe, expect, it, vi } from "vitest";
import { runEditingCommand } from "../src/commands";
import { acceptNativeResponse } from "../src/bridge";
import { PHI_MARKDOWN_CLIPBOARD_TYPE } from "../src/clipboard";
import { PhiMarkdownEditor } from "../src/editor";
import { latexSuite, setCustomSnippets } from "../src/latex-suite/engine";
import { renderMath } from "../src/math/mathjax";
import { smartPairs, smartPairTransaction } from "../src/markdown/pairs";
import { parseMarkdownTable } from "../src/markdown/table";
import {
  chooseHardPreview,
  selectedHardPreview,
} from "../src/markdown/preview-interaction";
import {
  LinkWidget,
  LineHeightEstimateWidget,
  MarkdownLinkWidget,
  MathWidget,
  estimatedHeadingHeight,
  estimatedListLineHeight,
  resetPreviewGeometryCaches,
  resizeImageMarkdown,
  seedPreviewImageGeometry,
  setPreviewGeometryContext,
  TaskWidget,
} from "../src/widgets/preview";

const views: EditorView[] = [];
afterEach(() => {
  for (const view of views.splice(0)) view.destroy();
  document.body.replaceChildren();
  document.body.className = "";
  delete document.documentElement.dataset.theme;
  document.documentElement.removeAttribute("style");
  delete (window as unknown as { MathJax?: unknown }).MathJax;
  resetPreviewGeometryCaches();
  setPreviewGeometryContext("", 780, 1);
  setCustomSnippets();
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

function key(
  view: EditorView,
  value: string,
  shiftKey = false,
  ctrlKey = false,
): boolean {
  return runScopeHandlers(
    view,
    new KeyboardEvent("keydown", { key: value, shiftKey, ctrlKey }),
    "editor",
  );
}

function input(view: EditorView, value: string): boolean {
  const range = view.state.selection.main;
  let defaultTransaction: ReturnType<EditorState["update"]> | undefined;
  const defaultInsert = () => defaultTransaction ??= view.state.update({
    changes: { from: range.from, to: range.to, insert: value },
    selection: { anchor: range.from + value.length },
    userEvent: "input.type",
  });
  const handled = view.state.facet(EditorView.inputHandler).some((handler) =>
    handler(view, range.from, range.to, value, defaultInsert));
  if (!handled) view.dispatch(defaultInsert());
  return handled;
}

async function settle(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

describe("CodeMirror document transactions", () => {
  it("reuses authored closers while preserving tracked nested pairs", () => {
    let authored = EditorState.create({
      doc: "If {b)",
      selection: { anchor: 5 },
      extensions: [closeBrackets()],
    });
    const reused = smartPairTransaction(authored, 5, 5, "(");
    expect(reused).not.toBeNull();
    authored = reused!.state;
    expect(authored.doc.toString()).toBe("If {b()");

    let nested = EditorState.create({
      doc: "",
      extensions: [closeBrackets()],
    });
    nested = insertBracket(nested, "(")!.state;
    expect(nested.doc.toString()).toBe("()");
    expect(smartPairTransaction(nested, 1, 1, "(")).toBeNull();
    nested = insertBracket(nested, "(")!.state;
    expect(nested.doc.toString()).toBe("(())");
  });

  it("closes an unmatched prose quote without creating another pair", () => {
    let state = EditorState.create({
      doc: "",
      extensions: [closeBrackets()],
    });
    state = insertBracket(state, '"')!.state;
    expect(state.doc.toString()).toBe('""');
    state = state.update({
      changes: { from: 1, to: 2 },
      selection: { anchor: 1 },
    }).state;
    state = state.update({
      changes: { from: 1, insert: "quoted text" },
      selection: { anchor: 12 },
      userEvent: "input.type",
    }).state;
    const closing = smartPairTransaction(state, 12, 12, '"');
    expect(closing).not.toBeNull();
    expect(closing!.state.doc.toString()).toBe('"quoted text"');
  });

  it("places the cursor after Markdown inserted by the paste handler", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "paste-selection",
      path: "paste-selection.md",
      text: "before after",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 7, head: 12 } });
    const paste = new Event("paste", {
      bubbles: true,
      cancelable: true,
    }) as ClipboardEvent;
    Object.defineProperty(paste, "clipboardData", { value: {
      types: [PHI_MARKDOWN_CLIPBOARD_TYPE],
      getData: (type: string) => type === PHI_MARKDOWN_CLIPBOARD_TYPE
        ? "**middle**" : "",
      items: [],
      files: [],
    } });

    editor.view.contentDOM.dispatchEvent(paste);

    expect(editor.getDocument()).toBe("before **middle**");
    expect(editor.view.state.selection.main.head).toBe(17);
    expect(editor.view.state.selection.main.empty).toBe(true);
  });

  it("safely restores an anchor beyond a shortened document", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const frame = vi.spyOn(window, "requestAnimationFrame").mockImplementation(
      (callback: FrameRequestCallback) => {
        callback(0);
        return 1;
      },
    );

    expect(() => editor.openDocument({
      documentId: "shortened",
      path: "shortened.md",
      text: "The replacement is much shorter.",
      revision: 2,
      lineEnding: "LF",
      scrollState: {
        anchor: 500_000,
        offset: Number.POSITIVE_INFINITY,
        top: Number.NaN,
      },
    })).not.toThrow();
    const state = editor.getState() as {
      scrollState: { anchor: number; offset: number; top: number };
    };
    expect(state.scrollState.anchor)
      .toBeLessThanOrEqual(editor.view.state.doc.length);
    expect(Number.isFinite(state.scrollState.offset)).toBe(true);
    expect(Number.isFinite(state.scrollState.top)).toBe(true);
    frame.mockRestore();
  });

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

  it("reserves realistic heights for unmounted block media", () => {
    expect(new LinkWidget("diagram.png", "diagram.png", 0, 10, true, true)
      .estimatedHeight).toBe(360);
    expect(new LinkWidget("diagram.png|400", "diagram.png", 0, 10, true, true)
      .estimatedHeight).toBe(240);
    expect(new MarkdownLinkWidget("diagram.png", "Diagram|300x180", 0, true, 10, true)
      .estimatedHeight).toBe(180);
    expect(new MarkdownLinkWidget("diagram.png", "Diagram", 0, true, 10, false)
      .estimatedHeight).toBe(-1);
    expect(new LinkWidget("Long note", "Long note", 0, 10, true, true)
      .estimatedHeight).toBe(360);
    expect(new MathWidget("x", true, 0).estimatedHeight).toBe(64);
  });

  it("gives cold headings and wrapped list lines realistic height-map hints", () => {
    setPreviewGeometryContext("Geometry.md", 320, 1);
    expect(estimatedHeadingHeight(1, "A heading")).toBeGreaterThan(50);
    expect(estimatedListLineHeight("word ".repeat(30), 2))
      .toBeGreaterThan(50);
    const marker = new LineHeightEstimateWidget(72);
    expect(marker.estimatedHeight).toBe(72);
    expect(marker.toDOM().className).toBe("cm-height-estimate");
  });

  it("reuses resolved image geometry when CodeMirror remounts a widget", async () => {
    const view = viewFor("![[Pictures/diagram.png]]");
    const requests: Array<{ id: string; type: string }> = [];
    const capture = (event: Event) => {
      const message = (event as CustomEvent).detail as {
        id: string;
        type: string;
      };
      if (message.type === "attachment/resolve") requests.push(message);
    };
    window.addEventListener("phi-native-message", capture);

    const first = new LinkWidget(
      "Pictures/diagram.png", "diagram.png", 0, 25, true, true,
    ).toDOM(view);
    expect(first.style.minHeight).toBe("360px");
    expect(requests).toHaveLength(1);
    acceptNativeResponse({
      protocol: 1,
      type: "request/response",
      id: requests[0].id,
      payload: { result: {
        path: "Pictures/diagram.png",
        width: 1200,
        height: 800,
      } },
    });
    await settle();

    const firstImage = first.querySelector<HTMLImageElement>("img")!;
    Object.defineProperties(firstImage, {
      naturalWidth: { value: 1200, configurable: true },
      naturalHeight: { value: 800, configurable: true },
    });
    firstImage.dispatchEvent(new Event("load"));
    expect(first.style.minHeight).toBe("");

    const remountedWidget = new LinkWidget(
      "Pictures/diagram.png", "diagram.png", 0, 25, true, true,
    );
    expect(remountedWidget.estimatedHeight).toBeCloseTo(522.67, 1);
    const remounted = remountedWidget.toDOM(view);
    const remountedImage = remounted.querySelector<HTMLImageElement>("img")!;
    expect(requests).toHaveLength(1);
    expect(remountedImage.getAttribute("src"))
      .toBe("vault:///Pictures/diagram.png");
    expect(remountedImage.width).toBe(1200);
    expect(remountedImage.height).toBe(800);
    expect(remounted.style.minHeight).toBe("");

    resetPreviewGeometryCaches();
    const coldAgain = new LinkWidget(
      "Pictures/diagram.png", "diagram.png", 0, 25, true, true,
    );
    expect(coldAgain.estimatedHeight).toBe(360);
    coldAgain.toDOM(view);
    expect(requests).toHaveLength(2);
    window.removeEventListener("phi-native-message", capture);
  });

  it("uses image-header geometry before a cold off-screen widget mounts", () => {
    setPreviewGeometryContext("Notes/Trees.md", 600, 1);
    seedPreviewImageGeometry([{
      target: "Pasted image.png",
      path: "Images/Pasted image.png",
      width: 1000,
      height: 500,
    }]);
    const widget = new LinkWidget(
      "Pasted image.png", "Pasted image.png", 0, 24, true, true,
    );
    expect(widget.estimatedHeight).toBeCloseTo(304, 0);
    const view = viewFor("![[Pasted image.png]]");
    const dom = widget.toDOM(view);
    expect(dom.querySelector("img")?.getAttribute("src"))
      .toBe("vault:///Images/Pasted%20image.png");
  });

  it("remeasures a note embed when its asynchronous body arrives", async () => {
    let view: EditorView;
    const dispatch = vi.fn();
    const focus = vi.fn();
    const requestMeasure = vi.fn((request?: {
      read(view: EditorView): unknown;
      write?(measurement: unknown, view: EditorView): void;
    }) => {
      if (request)
        request.write?.(request.read(view), view);
    });
    view = {
      requestMeasure,
      dispatch,
      focus,
      state: EditorState.create({ doc: " ".repeat(100) }),
    } as unknown as EditorView;
    let request: CustomEvent | undefined;
    window.addEventListener("phi-native-message", (event) => {
      request = event as CustomEvent;
    }, { once: true });

    const widget = new LinkWidget("Long note", "Long note", 0, 13, true, true);
    const dom = widget.toDOM(view);
    expect(dom.style.minHeight).toBe("360px");
    const sourceButton = dom.querySelector<HTMLButtonElement>(
      ".embed-source-button",
    );
    expect(sourceButton?.getAttribute("aria-label"))
      .toBe("Edit note embed source");
    sourceButton?.click();
    expect(dispatch).toHaveBeenCalledOnce();
    expect(dispatch.mock.calls[0]?.[0]).toMatchObject({
      selection: { anchor: 1 },
      scrollIntoView: true,
    });
    expect(focus).toHaveBeenCalled();
    vi.spyOn(dom, "getBoundingClientRect").mockReturnValue({
      height: 840,
    } as DOMRect);
    document.body.append(dom);
    const message = request?.detail as {
      protocol: number;
      type: string;
      id: string;
    };
    expect(message.type).toBe("embed/read");
    acceptNativeResponse({
      protocol: 1,
      type: "request/response",
      id: message.id,
      payload: { result: { text: "# Loaded\n\nA much taller body.", path: "Long note.md" } },
    });
    await settle();

    expect(dom.querySelector(".embed-body")?.textContent)
      .toContain("A much taller body.");
    expect(dom.style.minHeight).toBe("");
    expect(requestMeasure).toHaveBeenCalled();
    expect(new LinkWidget("Long note", "Long note", 0, 13, true, true)
      .estimatedHeight).toBe(840);
    widget.destroy(dom);

    let remountRequest: CustomEvent | undefined;
    window.addEventListener("phi-native-message", (event) => {
      remountRequest = event as CustomEvent;
    }, { once: true });
    const remountedWidget = new LinkWidget(
      "Long note", "Long note", 0, 13, true, true,
    );
    const remounted = remountedWidget.toDOM(view);
    expect(remounted.style.minHeight).toBe("840px");
    const remountMessage = remountRequest?.detail as { id: string };
    acceptNativeResponse({
      protocol: 1,
      type: "request/response",
      id: remountMessage.id,
      payload: { result: { text: "# Loaded again", path: "Long note.md" } },
    });
    await settle();
    remountedWidget.destroy(remounted);
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

  it("splits source lines around embedded display math", () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: () => document.createElement("mjx-container"),
    };
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = String.raw`before$$x$$between\[y\]after`;
    editor.openDocument({
      documentId: "embedded-display-math",
      path: "embedded-display-math.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 0 } });

    expect(editor.getDocument()).toBe(text);
    expect(parent.querySelectorAll(".math-display")).toHaveLength(2);
    expect([...parent.querySelectorAll<HTMLElement>(".cm-line")]
      .map((line) => line.textContent)).toEqual([
        "before",
        "between",
        "after",
      ]);
  });

  it("keeps text after embedded display math aligned with list content", () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: () => document.createElement("mjx-container"),
    };
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const listItem = String.raw`4. The path\[u-v-\cdots-w\]is the next ear \(P_{i+1}\).`;
    const text = `Elsewhere\n\n${listItem}`;
    editor.openDocument({
      documentId: "list-embedded-display-math",
      path: "list-embedded-display-math.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });

    const listLine = parent.querySelector<HTMLElement>(".cm-live-list-item");
    const continuation = parent.querySelector<HTMLElement>(
      ".cm-live-list-continuation",
    );
    expect(listLine?.textContent).toBe("4.The path");
    expect(continuation?.textContent).toBe("is the next ear P_{i+1}.");
    expect(continuation?.parentElement?.classList.contains("cm-line")).toBe(true);
    expect(continuation).not.toBeNull();
    expect(continuation?.style.getPropertyValue("--phi-list-content-indent"))
      .toBe("1.59em");
    expect(parent.querySelectorAll(".math-display")).toHaveLength(1);
    expect(parent.querySelectorAll(".math-inline")).toHaveLength(1);

    const css = readFileSync("src/styles/editor.css", "utf8");
    expect(css).toMatch(
      /\.cm-live-list-continuation\s*\{[^}]*display: inline-block[^}]*width: 100%[^}]*padding-inline-start:/s,
    );
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
    expect(parent.querySelector(".cm-latex-conceal")).toBeNull();

    editor.updateSettings({ executableSnippets: true });
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

    editor.updateSettings({ executableSnippets: false });
    editor.view.dispatch({ selection: { anchor: text.indexOf(" + ") + 1 } });
    expect(parent.querySelector(".cm-latex-conceal")).toBeNull();
    editor.updateSettings({ executableSnippets: true });
    expect(parent.querySelector(".cm-latex-conceal")).not.toBeNull();
  });

  it("keeps inline-code styling while its delimiters are visible", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "Use `test` and ``double`` here";
    editor.openDocument({
      documentId: "inline-code",
      path: "inline-code.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 0 } });
    expect([...parent.querySelectorAll(".cm-live-inline-code")]
      .map((element) => element.textContent)).toEqual(["test", "double"]);
    expect(parent.querySelector(".cm-line")?.textContent)
      .toBe("Use test and double here");

    editor.view.dispatch({
      selection: { anchor: text.indexOf("test") + 1 },
    });
    expect([...parent.querySelectorAll(".cm-live-inline-code")]
      .map((element) => element.textContent)).toEqual(["`test`", "double"]);

    editor.view.dispatch({
      selection: { anchor: text.indexOf("double") + 1 },
    });
    expect([...parent.querySelectorAll(".cm-live-inline-code")]
      .map((element) => element.textContent)).toEqual(["test", "``double``"]);
  });

  it("previews empty and unfinished backtick environments", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const lone = "`";
    const text = `Before\n\n\`\`\n\n${lone}\n\n\`\`\`js\nconst value = 1;`;
    editor.openDocument({
      documentId: "unfinished-code",
      path: "unfinished-code.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 0 } });

    expect(parent.querySelectorAll(".cm-live-inline-code-empty"))
      .toHaveLength(2);
    expect(parent.querySelector(".code-block-widget code")?.textContent)
      .toContain("const value = 1;");

    const loneAt = text.indexOf(`\n${lone}\n`) + 1;
    editor.view.dispatch({ selection: { anchor: loneAt + 1 } });
    expect([...parent.querySelectorAll(".cm-live-inline-code")]
      .some((element) => element.textContent === lone)).toBe(true);
    expect(parent.querySelectorAll(".cm-live-inline-code-empty"))
      .toHaveLength(1);

    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".code-block-widget")).toBeNull();
    expect(parent.querySelectorAll(".cm-live-code-block-source"))
      .toHaveLength(2);
    expect(parent.querySelector(".cm-live-code-block-source-first")
      ?.textContent).toBe("```js");
  });

  it("keeps the line entered after a closing code fence editable", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const source = "```text\ntest\n```";
    editor.openDocument({
      documentId: "code-block-following-line",
      path: "code-block-following-line.md",
      text: source,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: source.length } });

    expect(key(editor.view, "Enter")).toBe(true);
    expect(editor.getDocument()).toBe(`${source}\n`);
    expect(editor.view.state.selection.main.head).toBe(source.length + 1);
    expect(parent.querySelector(".code-block-widget")).not.toBeNull();
    const lines = [...parent.querySelectorAll<HTMLElement>(".cm-line")];
    /* A block replacement is a .cm-block sibling, so the only text line left
     * here must be the newly inserted empty line. The regression left none. */
    expect(lines).toHaveLength(1);
    expect(lines.at(-1)?.textContent).toBe("");
    expect(lines.at(-1)?.querySelector(".code-block-widget")).toBeNull();
    expect(editor.view.posAtDOM(lines.at(-1)!, 0)).toBe(source.length + 1);
  });

  it("keeps blank-line geometry stable after a rendered soft block", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const equation = String.raw`\[ \boxed{\text{rectangular dual of }G \;\Rightarrow\; \text{orthogonal drawing of }G^\*} \]`;
    const source = `${equation}\n\n\n# Orthogonal Drawings`;
    editor.openDocument({
      documentId: "soft-block-blank-lines",
      path: "soft-block-blank-lines.md",
      text: source,
      revision: 1,
      lineEnding: "LF",
    });
    const firstBlank = editor.view.state.doc.line(2).from;
    const secondBlank = editor.view.state.doc.line(3).from;

    editor.view.dispatch({ selection: { anchor: firstBlank } });
    expect(parent.querySelector(".math-display")).not.toBeNull();
    const firstLineCount = parent.querySelectorAll(".cm-line").length;

    editor.view.dispatch({ selection: { anchor: secondBlank } });
    expect(parent.querySelectorAll(".cm-line")).toHaveLength(firstLineCount);

    editor.view.dispatch({ selection: { anchor: firstBlank } });
    const beforeEnter = parent.querySelectorAll(".cm-line").length;
    expect(key(editor.view, "Enter")).toBe(true);
    expect(editor.getDocument()).toBe(
      `${equation}\n\n\n\n# Orthogonal Drawings`,
    );
    expect(parent.querySelectorAll(".cm-line"))
      .toHaveLength(beforeEnter + 1);
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
    expect(parent.querySelector<HTMLElement>(".cm-line")?.style
      .getPropertyValue("--phi-list-content-indent")).toBe("2.6em");
    expect(key(editor.view, "Tab", true)).toBe(true);
    expect(editor.getDocument()).toBe("- A long item that wraps beneath its content");
  });

  it("snaps indentation to four-column stops", () => {
    const list = viewFor("  - item", 8, 8, latexSuite);
    expect(key(list, "Tab")).toBe(true);
    expect(list.state.doc.toString()).toBe("    - item");

    const text = viewFor("  plain", 7, 7, latexSuite);
    expect(key(text, "Tab")).toBe(true);
    expect(text.state.doc.toString()).toBe("    plain");

    const outdent = viewFor("      plain", 11, 11, latexSuite);
    expect(key(outdent, "Tab", true)).toBe(true);
    expect(outdent.state.doc.toString()).toBe("    plain");
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
      .toBe("calc(18px + 0.4em)");
    expect(lines[1].textContent).toBe("task text");
    expect(lines[2].classList.contains("cm-live-ordered-list-item")).toBe(true);
    expect(lines[2].querySelector(".list-number")?.textContent).toBe("10.");

    const css = readFileSync("src/styles/editor.css", "utf8");
    expect(css).toMatch(/\.task-checkbox\s*\{[^}]*translateY\(-1px\)/s);
    expect(css).toMatch(/\.list-number, \.cm-live-list-number\s*\{[^}]*var\(--editor-muted\)/s);
    expect(css).toMatch(/\.list-number\s*\{[^}]*ui-monospace[^}]*tabular-nums/s);
    expect(css).toMatch(/\.cm-tooltip-autocomplete\s*\{[^}]*border-radius: 12px/s);
    expect(css).toMatch(/\.cm-completionIcon\s*\{\s*display: none/s);
    expect(css).toMatch(/\.cm-content\s*\{[^}]*caret-color: transparent !important/s);
    expect(css).toMatch(/\.note-embed\s*\{[^}]*max-width: 100%[^}]*overflow: hidden/s);
    expect(css).toMatch(/\.embed-body \.math-inline\s*\{[^}]*max-width: 100%/s);
    expect(css).not.toMatch(/\.math-display\s*\{[^}]*overflow-x: auto/s);
    expect(css).toMatch(/\.math-widget\.math-overflow\s*\{[^}]*overflow-x: auto/s);
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

    /* CodeMirror explicitly forbids vertical margins on block widgets: they
     * are outside the measured box and make clicks below the rule resolve to
     * the following document line. Keep the same visual spacing inside the
     * widget's measured padding instead. */
    const css = readFileSync("src/styles/editor.css", "utf8");
    const rule = /\.horizontal-rule-widget\s*\{([^}]*)\}/s.exec(css)?.[1] ?? "";
    expect(rule).toMatch(/margin:\s*0/);
    expect(rule).toMatch(/padding:\s*\.8em 0/);
    expect(rule).not.toMatch(/margin:\s*\.8em/);
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

  it("reveals hard source for text selections and Select All", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "![[diagram.png]]\n\nText below";
    editor.openDocument({
      documentId: "image-selection",
      path: "image-selection.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".image-widget")).not.toBeNull();

    editor.view.dispatch({
      selection: EditorSelection.range(text.length, 0),
    });
    expect(parent.querySelector(".image-widget")).toBeNull();

    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".image-widget")).not.toBeNull();

    expect(key(editor.view, "a", false, true)).toBe(true);
    expect(editor.view.state.selection.main).toMatchObject({
      from: 0,
      to: text.length,
    });
    expect(parent.querySelector(".image-widget")).toBeNull();
  });

  it("selects adjacent hard images one at a time with the arrow keys", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const first = "![[one.png]]";
    const second = "![[two.png]]";
    const text = `Before ${first}${second} after`;
    editor.openDocument({
      documentId: "hard-image-arrows",
      path: "hard-image-arrows.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.indexOf(first) } });

    expect(key(editor.view, "ArrowRight")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toMatchObject({
      from: text.indexOf(first),
      to: text.indexOf(first) + first.length,
    });
    expect(parent.querySelectorAll(".cm-hard-selected")).toHaveLength(1);
    expect(parent.querySelector(".cm-hard-preview-selection")).not.toBeNull();

    expect(key(editor.view, "ArrowRight")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toMatchObject({
      from: text.indexOf(second),
      to: text.indexOf(second) + second.length,
    });
    expect(key(editor.view, "ArrowRight")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toBeNull();
    expect(editor.view.state.selection.main.head)
      .toBe(text.indexOf(second) + second.length);
  });

  it("reveals soft blocks and selects hard blocks during vertical motion", () => {
    const softParent = document.createElement("div");
    document.body.append(softParent);
    const softEditor = new PhiMarkdownEditor(softParent);
    views.push(softEditor.view);
    const softText = "Before\n```text\nx = 1\n```\nAfter";
    softEditor.openDocument({
      documentId: "soft-arrow",
      path: "soft-arrow.md",
      text: softText,
      revision: 1,
      lineEnding: "LF",
    });
    softEditor.view.dispatch({ selection: { anchor: 0 } });
    vi.spyOn(softEditor.view, "moveVertically").mockReturnValue(
      EditorSelection.cursor(softText.indexOf("After")),
    );

    expect(key(softEditor.view, "ArrowDown")).toBe(true);
    expect(softParent.querySelector(".code-block-widget")).toBeNull();
    expect(softEditor.view.state.selection.main.head)
      .toBe(softText.indexOf("```"));

    const hardParent = document.createElement("div");
    document.body.append(hardParent);
    const hardEditor = new PhiMarkdownEditor(hardParent);
    views.push(hardEditor.view);
    const hardText = "Before\n![[diagram.png]]\nAfter";
    hardEditor.openDocument({
      documentId: "hard-arrow",
      path: "hard-arrow.md",
      text: hardText,
      revision: 1,
      lineEnding: "LF",
    });
    hardEditor.view.dispatch({ selection: { anchor: 0 } });
    vi.spyOn(hardEditor.view, "moveVertically").mockReturnValue(
      EditorSelection.cursor(hardText.indexOf("After")),
    );

    expect(key(hardEditor.view, "ArrowDown")).toBe(true);
    expect(selectedHardPreview(hardEditor.view.state)).toMatchObject({
      from: hardText.indexOf("![["),
      to: hardText.indexOf("![[") + "![[diagram.png]]".length,
    });
    expect(hardParent.querySelector(".image-widget")).not.toBeNull();
  });

  it("moves vertically from a selected standalone image to the next line", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const image = "![[Pasted image 2026-08-30 120343.png|177]]";
    const text = `## $st$-Ordering\n${image}\nVertex order`;
    editor.openDocument({
      documentId: "standalone-image-arrows",
      path: "standalone-image-arrows.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    const imageFrom = text.indexOf(image);
    const imageTo = imageFrom + image.length;
    const heading = editor.view.state.doc.line(1);
    const below = editor.view.state.doc.line(3);
    const move = vi.spyOn(editor.view, "moveVertically");

    editor.view.dispatch({ selection: { anchor: below.from + 4 } });
    move.mockReturnValue(EditorSelection.cursor(imageFrom + 4));
    expect(key(editor.view, "ArrowUp")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toMatchObject({
      from: imageFrom,
      to: imageTo,
    });
    expect(key(editor.view, "ArrowUp")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toBeNull();
    expect(editor.view.state.selection.main.head).toBe(heading.from + 4);

    editor.view.dispatch({ selection: { anchor: heading.from + 4 } });
    move.mockReturnValue(EditorSelection.cursor(imageFrom + 4));
    expect(key(editor.view, "ArrowDown")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toMatchObject({
      from: imageFrom,
      to: imageTo,
    });
    expect(key(editor.view, "ArrowDown")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toBeNull();
    expect(editor.view.state.selection.main.head).toBe(below.from + 4);

    chooseHardPreview(editor.view, { from: imageFrom, to: imageTo });
    expect(key(editor.view, "ArrowLeft")).toBe(true);
    expect(editor.view.state.selection.main.head).toBe(imageFrom);
    chooseHardPreview(editor.view, { from: imageFrom, to: imageTo });
    expect(key(editor.view, "ArrowRight")).toBe(true);
    expect(editor.view.state.selection.main.head).toBe(imageTo);
  });

  it("stops on blank lines crossed between or inside soft previews", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const equation = String.raw`\[ \boxed{\text{rectangular dual of }G \;\Rightarrow\; \text{orthogonal drawing of }G^\*} \]`;
    const text = `${equation}\n\n# Orthogonal Drawings\n\n`;
    editor.openDocument({
      documentId: "soft-arrow-blank-line",
      path: "soft-arrow-blank-line.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    const blank = editor.view.state.doc.line(2).from;
    const heading = text.indexOf("# Orthogonal");
    const move = vi.spyOn(editor.view, "moveVertically");

    editor.view.dispatch({ selection: { anchor: heading } });
    move.mockReturnValue(EditorSelection.cursor(0));
    expect(key(editor.view, "ArrowUp")).toBe(true);
    expect(editor.view.state.selection.main.head).toBe(blank);

    editor.view.dispatch({ selection: { anchor: 0 } });
    move.mockReturnValue(EditorSelection.cursor(heading));
    expect(key(editor.view, "ArrowDown")).toBe(true);
    expect(editor.view.state.selection.main.head).toBe(blank);

    const codeParent = document.createElement("div");
    document.body.append(codeParent);
    const codeEditor = new PhiMarkdownEditor(codeParent);
    views.push(codeEditor.view);
    const code = "```text\nfirst\n\nlast\n```\nAfter";
    codeEditor.openDocument({
      documentId: "soft-arrow-code-blank-line",
      path: "soft-arrow-code-blank-line.md",
      text: code,
      revision: 1,
      lineEnding: "LF",
    });
    const codeBlank = codeEditor.view.state.doc.line(3).from;
    codeEditor.view.dispatch({
      selection: { anchor: code.indexOf("last") },
    });
    vi.spyOn(codeEditor.view, "moveVertically").mockReturnValue(
      EditorSelection.cursor(code.indexOf("first")),
    );
    expect(key(codeEditor.view, "ArrowUp")).toBe(true);
    expect(codeEditor.view.state.selection.main.head).toBe(codeBlank);
  });

  it("keeps hard items rendered until edit and supports delete", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const first = "![[one.png]]";
    const second = "![[two.png]]";
    const text = `${first}${second}\nAfter`;
    editor.openDocument({
      documentId: "hard-image-actions",
      path: "hard-image-actions.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });

    editor.view.dispatch({ selection: { anchor: 3 } });
    expect(parent.querySelectorAll(".image-widget")).toHaveLength(2);

    chooseHardPreview(editor.view, {
      from: 0,
      to: first.length,
    });
    expect(key(editor.view, "Enter")).toBe(true);
    expect(parent.querySelectorAll(".image-widget")).toHaveLength(1);
    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelectorAll(".image-widget")).toHaveLength(2);

    chooseHardPreview(editor.view, {
      from: first.length,
      to: first.length + second.length,
    });
    expect(key(editor.view, "Delete")).toBe(true);
    expect(editor.getDocument()).toBe(`${first}\nAfter`);
  });

  it("selects an image body without revealing its source", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const source = "![[diagram.png]]";
    editor.openDocument({
      documentId: "hard-image-click",
      path: "hard-image-click.md",
      text: source,
      revision: 1,
      lineEnding: "LF",
    });

    const image = parent.querySelector<HTMLElement>(".image-widget")!;
    image.dispatchEvent(new MouseEvent("pointerdown", { bubbles: true }));
    expect(selectedHardPreview(editor.view.state)).toMatchObject({
      from: 0,
      to: source.length,
    });
    expect(image.classList.contains("cm-hard-selected")).toBe(true);
    expect(parent.querySelector(".image-widget")).not.toBeNull();
  });

  it("treats a linked Markdown image as one hard item", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const source = "[![Diagram](image.png)](https://example.com)";
    editor.openDocument({
      documentId: "hard-linked-image",
      path: "hard-linked-image.md",
      text: source,
      revision: 1,
      lineEnding: "LF",
    });

    expect(parent.querySelector(".linked-image-widget")).not.toBeNull();
    expect(parent.querySelector("[aria-label='Edit linked image source']"))
      .not.toBeNull();
    expect(key(editor.view, "ArrowRight")).toBe(true);
    expect(selectedHardPreview(editor.view.state)).toEqual({
      from: 0,
      to: source.length,
    });
  });

  it("keeps image source open while editing its line", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const source = "![[diagram.png]]";
    const text = `${source}\n\nText below`;
    editor.openDocument({
      documentId: "image-source-pin",
      path: "image-source-pin.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.length } });
    const edit = parent.querySelector<HTMLButtonElement>(".image-source-button");
    expect(edit).not.toBeNull();
    edit?.click();
    expect(parent.querySelector(".image-widget")).toBeNull();

    editor.view.dispatch({ selection: { anchor: source.length } });
    expect(parent.querySelector(".image-widget")).toBeNull();

    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".image-widget")).not.toBeNull();
  });

  it("keeps an image at the end of a paragraph inline", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "paragraph-image",
      path: "Triangulations.md",
      text: "The incoming edges have this order. ![Diagram|225](<../Images/Diagram.png>)",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 0 } });

    expect(parent.querySelector(".image-widget-inline")).not.toBeNull();
    expect(parent.querySelector(".image-widget-block")).toBeNull();
    expect(parent.querySelector(".cm-line")?.textContent)
      .toContain("The incoming edges have this order.");
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

  it("disables automatic MathJax equation numbering", () => {
    const source = readFileSync(
      "src/math/mathjax-config.js",
      "utf8",
    );
    new Function("window", source)(window);
    const configured = (window as unknown as {
      MathJax?: { tex?: { tags?: string } };
    }).MathJax;
    expect(configured?.tex?.tags).toBe("none");
  });

  it("loads dsfont and its SVG font extension from offline assets", () => {
    const source = readFileSync(
      "src/math/mathjax-config.js",
      "utf8",
    );
    new Function("window", source)(window);
    const configured = (window as unknown as {
      MathJax?: {
        loader?: { paths?: Record<string, string>; load?: string[] };
        tex?: { packages?: { "[+]"?: string[] } };
      };
    }).MathJax;
    expect(configured?.loader?.paths?.["mathjax-dsfont-extension"])
      .toBe("app://editor/mathjax-dsfont-font-extension");
    expect(configured?.loader?.load).toContain("[tex]/dsfont");
    expect(configured?.tex?.packages?.["[+]"]).toContain("dsfont");
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

  it("renders ordinary blockquotes subtly and reveals their source when active", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "Before\n\n> quoted **text**\n> second line\n\nAfter";
    editor.openDocument({
      documentId: "blockquote-preview",
      path: "blockquote-preview.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });

    const preview = parent.querySelector(".blockquote-widget");
    expect(preview?.querySelector("blockquote")?.textContent)
      .toContain("quoted text");
    expect(parent.querySelector(".callout")).toBeNull();

    editor.view.dispatch({ selection: { anchor: text.indexOf("quoted") + 2 } });
    expect(parent.querySelector(".blockquote-widget")).toBeNull();
    expect(parent.querySelector(".cm-content")?.textContent).toContain("> quoted");

    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".blockquote-widget blockquote")).not.toBeNull();
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

  it("reveals adjacent soft blocks on a single click", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "```text\nfirst\n```\n> quoted\n\nOutside";
    editor.openDocument({
      documentId: "soft-block-click",
      path: "soft-block-click.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.length } });

    parent.querySelector<HTMLElement>(".code-block-widget")?.dispatchEvent(
      new MouseEvent("pointerdown", { bubbles: true }),
    );
    expect(parent.querySelector(".code-block-widget")).toBeNull();
    expect(parent.querySelectorAll(".cm-live-code-block-source"))
      .toHaveLength(3);
    expect(parent.querySelector(".cm-live-code-block-source-first")
      ?.textContent).toBe("```text");
    expect(parent.querySelector(".blockquote-widget")).not.toBeNull();
    expect(editor.view.state.selection.main.head)
      .toBe(text.indexOf("first"));

    editor.view.dispatch({ selection: { anchor: text.length } });
    parent.querySelector<HTMLElement>(".blockquote-widget")?.dispatchEvent(
      new MouseEvent("pointerdown", { bubbles: true }),
    );
    expect(parent.querySelector(".blockquote-widget")).toBeNull();
    expect(parent.querySelector(".code-block-widget")).not.toBeNull();
  });

  it("keeps heading preview styling while its source is active", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const text = "# Heading\n\nOutside";
    editor.openDocument({
      documentId: "soft-heading",
      path: "soft-heading.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".cm-live-heading-1")).not.toBeNull();

    editor.view.dispatch({ selection: { anchor: 0 } });
    expect(parent.querySelector(".cm-live-heading-1")).not.toBeNull();
    expect(parent.querySelector(".cm-line")?.textContent).toContain("# Heading");

    editor.view.dispatch({ selection: { anchor: text.length } });
    expect(parent.querySelector(".cm-live-heading-1")).not.toBeNull();
  });

  it("edits rendered table cells directly and exposes a source button", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const table = "| A | B |\n| --- | --- |\n| 1 | 2 |";
    editor.openDocument({
      documentId: "hard-table",
      path: "hard-table.md",
      text: `${table}\n\nOutside`,
      revision: 1,
      lineEnding: "LF",
    });

    parent.querySelector<HTMLElement>(".table-widget")?.click();
    expect(parent.querySelector(".table-widget")).not.toBeNull();
    expect(parent.querySelector(".cm-hard-selected")).toBeNull();

    const first = parent.querySelector<HTMLElement>(
      '.rich-table-cell[data-row="0"][data-column="0"]',
    )!;
    first.focus();
    EditorView.findFromDOM(first.querySelector<HTMLElement>(".cm-editor")!)
      ?.contentDOM.blur();
    expect(editor.view.state.doc.toString()).toBe(`${table}\n\nOutside`);
    first.focus();
    const cellEditor = EditorView.findFromDOM(
      first.querySelector<HTMLElement>(".cm-editor")!,
    )!;
    cellEditor.dispatch({
      changes: { from: 0, to: cellEditor.state.doc.length, insert: "Left | right" },
      selection: { anchor: "Left | right".length },
      userEvent: "input.type",
    });
    expect(editor.view.state.doc.toString()).toContain("Left \\| right");
    expect(parent.querySelector(".table-widget")).not.toBeNull();
    expect(key(cellEditor, "z", false, true)).toBe(true);
    expect(editor.view.state.doc.toString()).toBe(`${table}\n\nOutside`);

    parent.querySelector<HTMLButtonElement>(".rich-table-source-button")
      ?.click();
    expect(parent.querySelector(".table-widget")).toBeNull();
    editor.view.dispatch({ selection: { anchor: table.length + 2 } });
    expect(parent.querySelector(".table-widget")).not.toBeNull();
  });

  it("uses live Markdown and LaTeX editing inside table cells", async () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: () => document.createElement("mjx-container"),
    };
    setCustomSnippets(JSON.stringify([{
      trigger: "@a",
      replacement: String.raw`\alpha`,
      options: "mA",
    }]));
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.updateSettings({ executableSnippets: true, latexConceal: true });
    editor.openDocument({
      documentId: "table-live-inline",
      path: "table-live-inline.md",
      text: "| *test* after |\n| --- |\n| $@ + \\beta$ |",
      revision: 1,
      lineEnding: "LF",
    });

    const emphasisCell = parent.querySelector<HTMLElement>(
      '.rich-table-cell[data-row="0"][data-column="0"]',
    )!;
    emphasisCell.focus();
    const emphasisEditor = EditorView.findFromDOM(
      emphasisCell.querySelector<HTMLElement>(".cm-editor")!,
    )!;
    expect(emphasisCell.querySelector(".cm-live-emphasis")?.textContent)
      .toBe("test");
    emphasisEditor.dispatch({ selection: { anchor: 2 } });
    expect(emphasisCell.querySelector(".cm-live-emphasis")).toBeNull();
    emphasisEditor.dispatch({ selection: { anchor: emphasisEditor.state.doc.length } });
    expect(emphasisCell.querySelector(".cm-live-emphasis")?.textContent)
      .toBe("test");

    const mathCell = parent.querySelector<HTMLElement>(
      '.rich-table-cell[data-row="1"][data-column="0"]',
    )!;
    mathCell.focus();
    const mathEditor = EditorView.findFromDOM(
      mathCell.querySelector<HTMLElement>(".cm-editor")!,
    )!;
    mathEditor.dispatch({ selection: { anchor: 2 } });
    input(mathEditor, "a");
    await settle();
    expect(parseMarkdownTable(editor.view.state.doc.toString())?.cells[1][0])
      .toBe(String.raw`$\alpha + \beta$`);
    mathEditor.dispatch({
      selection: { anchor: mathEditor.state.doc.toString().indexOf("+") },
    });
    expect(mathCell.querySelector(".cm-latex-conceal")).not.toBeNull();
    mathEditor.dispatch({ selection: { anchor: mathEditor.state.doc.length } });
    expect(mathCell.querySelector(".math-inline")).not.toBeNull();
  });

  it("blocks nested table insertion and keeps source-table navigation editable", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const table = "| A |\n| --- |\n| B |";
    editor.openDocument({
      documentId: "table-source-navigation",
      path: "table-source-navigation.md",
      text: `${table}\n\nOutside`,
      revision: 1,
      lineEnding: "LF",
    });

    parent.querySelector<HTMLElement>(".rich-table-cell")?.focus();
    editor.receive({ protocol: 1, type: "table/show-picker" });
    expect(document.querySelector(".phi-table-picker")).toBeNull();

    parent.querySelector<HTMLButtonElement>(".rich-table-source-button")?.click();
    expect(parent.querySelector(".rich-table-widget")).toBeNull();
    expect(parent.querySelectorAll(".cm-live-table-source")).toHaveLength(3);
    const lineBefore = editor.view.state.doc.lineAt(
      editor.view.state.selection.main.head,
    ).number;
    const nextLine = editor.view.state.doc.line(lineBefore + 1);
    vi.spyOn(editor.view, "moveVertically").mockReturnValue(
      EditorSelection.cursor(Math.min(nextLine.to, nextLine.from + 1)),
    );
    expect(key(editor.view, "ArrowDown")).toBe(true);
    const lineAfter = editor.view.state.doc.lineAt(
      editor.view.state.selection.main.head,
    ).number;
    expect(lineAfter).toBeGreaterThan(lineBefore);
  });

  it("moves vertically through empty rich table cells", async () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "empty-table-navigation",
      path: "empty-table-navigation.md",
      text: "|   |\n| --- |\n|   |",
      revision: 1,
      lineEnding: "LF",
    });
    const cells = [...parent.querySelectorAll<HTMLElement>(".rich-table-cell")];
    cells[0].focus();
    const cellEditor = EditorView.findFromDOM(
      cells[0].querySelector<HTMLElement>(".cm-editor")!,
    )!;
    expect(key(cellEditor, "ArrowDown")).toBe(true);
    await settle();
    expect(cells[1].contains(document.activeElement)).toBe(true);
  });

  it("inserts a selected table size without destroying surrounding text", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "insert-table",
      path: "insert-table.md",
      text: "alphaomega",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 5 } });
    editor.receive({ protocol: 1, type: "table/show-picker" });

    const picker = document.querySelector<HTMLElement>(".phi-table-picker");
    expect(picker).not.toBeNull();
    picker?.querySelector<HTMLButtonElement>(
      '.phi-table-picker-cell[data-row="3"][data-column="4"]',
    )?.click();

    const source = editor.view.state.doc.toString();
    expect(source.startsWith("alpha\n")).toBe(true);
    expect(source.endsWith("\nomega")).toBe(true);
    const table = parseMarkdownTable(source.slice(6, -6));
    expect(table?.cells).toHaveLength(3);
    expect(table?.alignments).toHaveLength(4);
    expect(document.querySelector(".phi-table-picker")).toBeNull();
    expect(undo(editor.view)).toBe(true);
    expect(editor.view.state.doc.toString()).toBe("alphaomega");
  });

  it("tabs through rich cells, leaves at the boundary, and supports Ctrl+A", async () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const table = "| A | B |\n| --- | --- |\n| 1 | 2 |";
    const text = `Before\n${table}\nAfter`;
    editor.openDocument({
      documentId: "table-navigation",
      path: "table-navigation.md",
      text,
      revision: 1,
      lineEnding: "LF",
    });
    const cells = () => [...parent.querySelectorAll<HTMLElement>(
      ".rich-table-cell",
    )];
    cells()[0].focus();
    for (let index = 1; index < 4; index++) {
      const cellEditor = EditorView.findFromDOM(
        cells()[index - 1].querySelector<HTMLElement>(".cm-editor")!,
      )!;
      expect(key(cellEditor, "Tab")).toBe(true);
      await settle();
      expect(cells()[index].contains(document.activeElement)).toBe(true);
    }
    const lastCellEditor = EditorView.findFromDOM(
      cells()[3].querySelector<HTMLElement>(".cm-editor")!,
    )!;
    expect(key(lastCellEditor, "Tab")).toBe(true);
    await settle();
    expect(editor.view.state.selection.main.head).toBe(text.indexOf("After"));
    expect(parent.querySelector(".cm-hard-selected")).toBeNull();

    cells()[0].focus();
    const firstCellEditor = EditorView.findFromDOM(
      cells()[0].querySelector<HTMLElement>(".cm-editor")!,
    )!;
    expect(key(firstCellEditor, "a", false, true)).toBe(true);
    expect(editor.view.state.selection.main).toMatchObject({
      from: 0,
      to: text.length,
    });
    expect(parent.querySelector(".table-widget")).toBeNull();
  });

  it("adds, removes, and reorders table rows and columns through one-step undoable edits", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const table = "| A | B |\n| :--- | ---: |\n| 1 | 2 |";
    editor.openDocument({
      documentId: "table-structure",
      path: "table-structure.md",
      text: table,
      revision: 1,
      lineEnding: "LF",
    });
    const parsed = () => parseMarkdownTable(editor.view.state.doc.toString())!;

    parent.querySelector<HTMLButtonElement>(".rich-table-add-row")?.click();
    expect(parsed().cells).toHaveLength(3);
    expect(undo(editor.view)).toBe(true);
    expect(parsed().cells).toHaveLength(2);

    parent.querySelector<HTMLButtonElement>(".rich-table-add-column")?.click();
    expect(parsed().alignments).toHaveLength(3);
    expect(undo(editor.view)).toBe(true);
    expect(parsed().alignments).toHaveLength(2);

    parent.querySelector<HTMLElement>(
      '.rich-table-row-handle[data-index="1"]',
    )?.dispatchEvent(new MouseEvent("contextmenu", {
      bubbles: true, cancelable: true,
    }));
    expect(parsed().cells).toHaveLength(1);
    expect(undo(editor.view)).toBe(true);

    const columns = parent.querySelectorAll<HTMLElement>(
      ".rich-table-column-handle",
    );
    columns[0].dispatchEvent(new Event("dragstart", {
      bubbles: true, cancelable: true,
    }));
    columns[1].dispatchEvent(new Event("dragover", {
      bubbles: true, cancelable: true,
    }));
    expect(parent.querySelector(".rich-table-drop-indicator")).not.toBeNull();
    expect(parent.querySelectorAll(".rich-table-drag-selected").length)
      .toBeGreaterThan(0);
    expect(columns[0].style.transform).toContain("translateX");
    columns[1].dispatchEvent(new Event("drop", {
      bubbles: true, cancelable: true,
    }));
    expect(parsed().cells).toEqual([["B", "A"], ["2", "1"]]);
    expect(parsed().alignments).toEqual(["right", "left"]);
    expect(undo(editor.view)).toBe(true);
    expect(editor.view.state.doc.toString()).toBe(table);
    const root = parent.querySelector<HTMLElement>(".rich-table-widget")!;
    expect(root.querySelector(".rich-table-scroll .rich-table-source-button"))
      .toBeNull();
    expect(root.querySelector(":scope > .rich-table-source-button"))
      .not.toBeNull();
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

  it("shows invalid LaTeX source in red instead of a blank MathJax box", async () => {
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: () => {
        const container = document.createElement("mjx-container");
        const error = document.createElement("span");
        error.setAttribute("data-mml-node", "merror");
        error.setAttribute("data-mjx-error", "Undefined control sequence");
        container.append(error);
        return container;
      },
    };
    const target = document.createElement("span");
    const source = String.raw`\notARealCommand{x`;

    await renderMath(source, false, target);

    expect(target.textContent).toBe(source);
    expect(target.classList.contains("math-render-error")).toBe(true);
    expect(target.title).toBe("Undefined control sequence");
    expect(target.querySelector("mjx-container")).toBeNull();
  });

  it("renders an inline align environment as its inline-safe equivalent", async () => {
    const calls: { latex: string; display?: boolean }[] = [];
    (window as unknown as { MathJax?: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg: (latex: string, options?: { display?: boolean }) => {
        calls.push({ latex, display: options?.display });
        return document.createElement("mjx-container");
      },
    };
    const target = document.createElement("span");
    await renderMath(String.raw`\begin{align}&\text{oben}\\&\text{unten}\end{align}`, false, target);
    expect(calls).toEqual([{
      latex: String.raw`\begin{aligned}&\text{oben}\\&\text{unten}\end{aligned}`,
      display: false,
    }]);
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
  it("keeps snippets disabled until the master setting is enabled", async () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const snippets = JSON.stringify([
      { trigger: "@a", replacement: "\\alpha", options: "mA" },
    ]);
    editor.updateSettings({ snippets });
    editor.openDocument({
      documentId: "snippets-disabled",
      path: "snippets-disabled.md",
      text: "$@$",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 2 } });

    input(editor.view, "a");
    await settle();
    expect(editor.getDocument()).toBe("$@a$");

    editor.updateSettings({ executableSnippets: true });
    editor.openDocument({
      documentId: "snippets-enabled",
      path: "snippets-enabled.md",
      text: "$@$",
      revision: 1,
      lineEnding: "LF",
    });
    editor.view.dispatch({ selection: { anchor: 2 } });
    input(editor.view, "a");
    await settle();
    expect(editor.getDocument()).toBe("$\\alpha$");
  });

  it("keeps text-mode snippets available after a math-heavy callout", async () => {
    setCustomSnippets(JSON.stringify([{
      trigger: "ml",
      replacement: "${\\displaystyle $0 }$$1",
      options: "tA",
    }]));
    const callout = [
      "> [!info] Special case: For a triangulation",
      ">",
      "> ${\\displaystyle G }$ is ${\\displaystyle 4- }$connected ${\\displaystyle \\iff }$${\\displaystyle v\\_{1},v\\_{2},\\dots,v\\_{n} }$ is the only filled triangle of",
      ">",
      "> Hence (G) 4-connected is a **sufficient (but stronger than necessary)** condition for (G') to have a proper box contact representation.",
    ].join("\n");
    const view = viewFor(`${callout}\n\n`, callout.length + 2,
      callout.length + 2, latexSuite);

    input(view, "m");
    await settle();
    input(view, "l");
    await settle();

    expect(view.state.doc.toString()).toBe(
      `${callout}\n\n${"${\\displaystyle  }$"}`,
    );
  });

  it("expands a typed math parenthesis once in the real input pipeline", async () => {
    setCustomSnippets(JSON.stringify([
      { trigger: "(", replacement: "($0)$1", options: "mA" },
    ]));
    const text = "If ${a}$ and ${b}$";
    const cursor = text.indexOf("{b}") + 2;
    const view = viewFor(text, cursor, cursor, [
      smartPairs,
      closeBrackets(),
      latexSuite,
    ]);

    expect(input(view, "(")).toBe(true);
    await settle();
    expect(view.state.doc.toString()).toBe("If ${a}$ and ${b()}$");
    expect(view.state.selection.main.head).toBe(cursor + 1);
  });

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
    expect(view.state.doc.toString()).toBe("$sq$");

    const fraction = viewFor("$x$", 2, 2, latexSuite);
    fraction.dispatch({ changes: { from: 2, insert: "/" }, selection: { anchor: 3 }, userEvent: "input.type" });
    await settle();
    expect(fraction.state.doc.toString()).toBe("$\\frac{x}{}$");
  });

  it("keeps nested snippet tabstops and resumes text snippets after math", async () => {
    setCustomSnippets(JSON.stringify([
      {
        trigger: "ml",
        replacement: "${\\displaystyle $0 }$$1",
        options: "tA",
      },
      { trigger: "_", replacement: "_{$0}$1", options: "mA" },
    ]));

    const undoView = viewFor("", 0, 0, latexSuite);
    undoView.dispatch({
      changes: { from: 0, insert: "m" },
      selection: { anchor: 1 },
      userEvent: "input.type",
    });
    await settle();
    undoView.dispatch({
      changes: { from: 1, insert: "l" },
      selection: { anchor: 2 },
      userEvent: "input.type",
    });
    await settle();
    expect(undoView.state.doc.toString()).toBe("${\\displaystyle  }$");
    expect(undo(undoView)).toBe(true);
    expect(undoView.state.doc.toString()).toBe("ml");

    const view = viewFor("m", 1, 1, latexSuite);
    view.dispatch({
      changes: { from: 1, insert: "l" },
      selection: { anchor: 2 },
      userEvent: "input.type",
    });
    await settle();
    expect(view.state.doc.toString()).toBe("${\\displaystyle  }$");

    let cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "x" },
      selection: { anchor: cursor + 1 },
      userEvent: "input.type",
    });
    await settle();
    cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "_" },
      selection: { anchor: cursor + 1 },
      userEvent: "input.type",
    });
    await settle();
    cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "b" },
      selection: { anchor: cursor + 1 },
      userEvent: "input.type",
    });

    expect(view.state.doc.toString()).toBe("${\\displaystyle x_{b} }$");
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head)
      .toBe(view.state.doc.toString().indexOf("}", cursor) + 1);
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head).toBe(view.state.doc.length);

    cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "ml" },
      selection: { anchor: cursor + 2 },
      userEvent: "input.type",
    });
    await settle();
    expect(view.state.doc.toString())
      .toBe("${\\displaystyle x_{b} }$${\\displaystyle  }$");
  });

  it("indents a continued list after tabbing out of inline math", async () => {
    setCustomSnippets(JSON.stringify([
      {
        trigger: "ml",
        replacement: "${\\displaystyle $0 }$$1",
        options: "tA",
      },
    ]));
    const view = viewFor("- ", 2, 2, latexSuite);
    view.dispatch({
      changes: { from: 2, insert: "ml" },
      selection: { anchor: 4 },
      userEvent: "input.type",
    });
    await settle();
    let cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "test" },
      selection: { anchor: cursor + 4 },
      userEvent: "input.type",
    });
    await settle();

    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head).toBe(view.state.doc.length);
    expect(key(view, "Enter")).toBe(true);
    expect(view.state.doc.toString()).toBe(
      "- ${\\displaystyle test }$\n- ",
    );
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.doc.toString()).toBe(
      "- ${\\displaystyle test }$\n    - ",
    );
    expect(view.state.selection.main.head).toBe(view.state.doc.length);
  });

  it("expands manual math snippets before leaving math or indenting a list", () => {
    setCustomSnippets(JSON.stringify([{
      trigger: "red",
      replacement: "\\textcolor{red}{$0}$1",
      options: "m",
    }]));
    const view = viewFor("- $red$", 6, 6, latexSuite);

    expect(key(view, "Tab")).toBe(true);
    expect(view.state.doc.toString()).toBe("- $\\textcolor{red}{}$");
    let cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "x" },
      selection: { anchor: cursor + 1 },
      userEvent: "input.type",
    });
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head)
      .toBe(view.state.doc.toString().lastIndexOf("$"));
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head).toBe(view.state.doc.length);
    expect(view.state.doc.toString()).toBe("- $\\textcolor{red}{x}$");
  });

  it("expands a manual snippet inside an active outer math tabstop", async () => {
    setCustomSnippets(JSON.stringify([
      {
        trigger: "ml",
        replacement: "${\\displaystyle $0 }$$1",
        options: "tA",
      },
      {
        trigger: "redm",
        replacement: "\\textcolor{#ed4564}{$0}$1",
        options: "t",
        priority: 2,
      },
      {
        trigger: "redm",
        replacement: "\\textcolor{#ed4564}{$0}$1",
        options: "m",
        priority: 2,
      },
    ]));
    const view = viewFor("", 0, 0, latexSuite);
    view.dispatch({
      changes: { from: 0, insert: "ml" },
      selection: { anchor: 2 },
      userEvent: "input.type",
    });
    await settle();
    const cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "redm" },
      selection: { anchor: cursor + 4 },
      userEvent: "input.type",
    });

    expect(key(view, "Tab")).toBe(true);
    expect(view.state.doc.toString())
      .toBe("${\\displaystyle \\textcolor{#ed4564}{} }$");
    expect(view.state.selection.main.head)
      .toBe(view.state.doc.toString().indexOf("{}") + 1);

    const nestedCursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: nestedCursor, insert: "x" },
      selection: { anchor: nestedCursor + 1 },
      userEvent: "input.type",
    });
    await settle();
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head)
      .toBe(view.state.doc.toString().indexOf("} }") + 1);
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head).toBe(view.state.doc.length);
  });

  it("ends remaining snippet navigation when continuing a list", () => {
    setCustomSnippets(JSON.stringify([
      {
        trigger: "field",
        replacement: "<span>$0</span>${1:tail}",
        options: "t",
      },
    ]));
    const view = viewFor("- field", 7, 7, latexSuite);
    expect(key(view, "Tab")).toBe(true);
    expect(key(view, "Tab")).toBe(true);
    view.dispatch({ selection: { anchor: view.state.doc.length } });

    expect(key(view, "Enter")).toBe(true);
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.doc.toString()).toBe(
      "- <span></span>tail\n    - ",
    );
    expect(view.state.selection.main.head).toBe(view.state.doc.length);
  });

  it("starts a manual snippet after tabbing out of a previous snippet", () => {
    setCustomSnippets(JSON.stringify([
      {
        trigger: "red",
        replacement: "<span style=color:#ed4564>$0</span>$1",
        options: "t",
      },
    ]));
    const view = viewFor("red", 3, 3, latexSuite);

    expect(key(view, "Tab")).toBe(true);
    let cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "first" },
      selection: { anchor: cursor + 5 },
      userEvent: "input.type",
    });
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head).toBe(view.state.doc.length);

    cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: " red" },
      selection: { anchor: cursor + 4 },
      userEvent: "input.type",
    });
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.doc.toString()).toBe(
      "<span style=color:#ed4564>first</span> <span style=color:#ed4564></span>",
    );
    expect(view.state.selection.main.head).toBe(
      view.state.doc.toString().lastIndexOf("</span>"),
    );
  });

  it("tracks typed placeholder text and abandons stale tabstops", async () => {
    setCustomSnippets(JSON.stringify([
      { trigger: "mark", replacement: "<i>$0</i>", options: "t" },
      { trigger: "wrap", replacement: "<b>$0</b>$1", options: "t" },
      { trigger: "xx", replacement: "X", options: "tA" },
    ]));
    const view = viewFor("mark", 4, 4, latexSuite);
    expect(key(view, "Tab")).toBe(true);
    let cursor = view.state.selection.main.head;
    view.dispatch({
      changes: { from: cursor, insert: "value" },
      selection: { anchor: cursor + 5 },
      userEvent: "input.type",
    });
    expect(key(view, "Tab")).toBe(true);
    expect(view.state.selection.main.head).toBe(
      view.state.doc.toString().indexOf("</i>"),
    );

    const stale = viewFor("mark", 4, 4, latexSuite);
    expect(key(stale, "Tab")).toBe(true);
    stale.dispatch({ selection: { anchor: stale.state.doc.length } });
    cursor = stale.state.selection.main.head;
    stale.dispatch({
      changes: { from: cursor, insert: " mark" },
      selection: { anchor: cursor + 5 },
      userEvent: "input.type",
    });
    expect(key(stale, "Tab")).toBe(true);
    expect(stale.state.doc.toString()).toBe("<i></i> <i></i>");

    const nested = viewFor("wrap", 4, 4, latexSuite);
    expect(key(nested, "Tab")).toBe(true);
    cursor = nested.state.selection.main.head;
    nested.dispatch({
      changes: { from: cursor, insert: "xx" },
      selection: { anchor: cursor + 2 },
      userEvent: "input.type",
    });
    await settle();
    expect(nested.state.doc.toString()).toBe("<b>X</b>");
    expect(key(nested, "Tab")).toBe(true);
    expect(nested.state.selection.main.head).toBe(nested.state.doc.length);
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

  it("keeps matrix Tab/Enter ahead of math exit without allowing indentation", () => {
    setCustomSnippets("[]");
    const matrixText = "$\\begin{pmatrix}a\\end{pmatrix}$";
    const cell = matrixText.indexOf("a\\end") + 1;
    const matrix = viewFor(matrixText, cell, cell, latexSuite);
    expect(key(matrix, "Tab")).toBe(true);
    expect(matrix.state.doc.toString()).toContain("a & \\end");

    const matrixEnterView = viewFor(matrixText, cell, cell, latexSuite);
    expect(key(matrixEnterView, "Enter")).toBe(true);
    expect(matrixEnterView.state.doc.toString()).toContain("a \\\\\n");

    const fractionText = "$\\frac{a}{b}$";
    const brace = fractionText.indexOf("}");
    const fraction = viewFor(fractionText, brace, brace, latexSuite);
    expect(key(fraction, "Tab")).toBe(true);
    expect(fraction.state.selection.main.head).toBe(brace + 1);
    expect(key(fraction, "Tab")).toBe(true);
    expect(fraction.state.selection.main.head).toBe(fractionText.length);

    const listText = "- before $x$ after";
    const mathCursor = listText.indexOf("x") + 1;
    const list = viewFor(listText, mathCursor, mathCursor, latexSuite);
    expect(key(list, "Tab")).toBe(true);
    expect(list.state.doc.toString()).toBe(listText);
    expect(list.state.selection.main.head)
      .toBe(listText.indexOf("$", mathCursor) + 1);
  });
});
