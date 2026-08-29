// @vitest-environment jsdom
/// <reference types="node" />
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { EditorState, Transaction } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { afterEach, describe, expect, it, vi } from "vitest";
import { PhiMarkdownEditor } from "../src/editor";
import { latexSuite, setCustomSnippets } from "../src/latex-suite/engine";
import { latexEnhancements } from "../src/latex-suite/enhancements";
import {
  markdownAnalysis,
  markdownAnalysisField,
} from "../src/markdown/analysis";
import { livePreview } from "../src/markdown/live-preview";
import { parseMarkdownNodes } from "../src/markdown/parser";

const views: EditorView[] = [];

afterEach(() => {
  for (const view of views.splice(0)) view.destroy();
  document.body.replaceChildren();
  setCustomSnippets();
  vi.useRealTimers();
});

function makeView(text: string, extensions: unknown[]): EditorView {
  const parent = document.createElement("div");
  document.body.append(parent);
  const view = new EditorView({
    parent,
    state: EditorState.create({
      doc: text,
      selection: { anchor: text.length },
      extensions: [
        markdown({ base: markdownLanguage }),
        ...(extensions as never[]),
      ],
    }),
  });
  views.push(view);
  return view;
}

describe("editor input performance invariants", () => {
  it("maps plain prose edits and remains equivalent to a clean parse", () => {
    const original = "Plain prose before widgets.\n\n$x_1$\n\n## Heading";
    let state = EditorState.create({
      doc: original,
      extensions: [markdownAnalysisField],
    });
    expect(markdownAnalysis(state).updateKind).toBe("full");

    const at = original.indexOf("prose") + "prose".length;
    state = state.update({ changes: { from: at, insert: " content" } }).state;
    const analysis = markdownAnalysis(state);
    expect(analysis.updateKind).toBe("mapped");
    expect(analysis.nodes).toEqual(parseMarkdownNodes(state.doc.toString()));
  });

  it("falls back to one full analysis for structural edits", () => {
    let state = EditorState.create({
      doc: "ordinary prose",
      extensions: [markdownAnalysisField],
    });
    state = state.update({ changes: { from: 0, insert: "# " } }).state;
    expect(markdownAnalysis(state).updateKind).toBe("full");
    expect(markdownAnalysis(state).nodes.some((node) =>
      node.kind === "heading")).toBe(true);
  });

  it("shares one analysis field across preview and LaTeX extensions", () => {
    const view = makeView("$x$\n\nordinary prose", [
      livePreview,
      latexEnhancements(true),
    ]);
    const initial = markdownAnalysis(view.state);
    view.dispatch({ selection: { anchor: 1 } });
    expect(markdownAnalysis(view.state)).toBe(initial);

    expect(() => view.dispatch({
      changes: { from: view.state.doc.length, insert: " more" },
      selection: { anchor: view.state.doc.length + 5 },
      annotations: Transaction.userEvent.of("input.type"),
    })).not.toThrow();
    expect(markdownAnalysis(view.state).updateKind).toBe("mapped");
  });

  it("does not create empty CodeMirror marks for empty delimiters", () => {
    expect(() => makeView("****\n\n____\n\nOutside", [livePreview]))
      .not.toThrow();
  });

  it("notifies native code once per clean-to-dirty transition", () => {
    vi.useFakeTimers();
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    editor.openDocument({
      documentId: "dirty-coalescing",
      path: "dirty-coalescing.md",
      text: "plain",
      revision: 1,
      lineEnding: "LF",
    });
    const messages: Array<{ type: string; payload?: Record<string, unknown> }> = [];
    const capture = (event: Event) => messages.push(
      (event as CustomEvent).detail,
    );
    window.addEventListener("phi-native-message", capture);
    editor.view.dispatch({
      changes: { from: 5, insert: "a" },
      selection: { anchor: 6 },
      annotations: Transaction.userEvent.of("input.type"),
    });
    editor.view.dispatch({
      changes: { from: 6, insert: "b" },
      selection: { anchor: 7 },
      annotations: Transaction.userEvent.of("input.type"),
    });
    const dirty = messages.filter((message) =>
      message.type === "document/changed" && message.payload?.text == null);
    expect(dirty).toHaveLength(1);
    expect(dirty[0].payload?.dirty).toBe(true);
    window.removeEventListener("phi-native-message", capture);
  });
});

function mixedDocument(minimumBytes: number): string {
  const section = [
    "## Stable heading",
    "- A list item with enough ordinary words to exercise line geometry.",
    "- [ ] A task item",
    "",
    "$$",
    String.raw`\frac{x_1 + \alpha}{\sqrt{1 + y^2}}`,
    "$$",
    "",
    "A normal paragraph with **strong text** and an [[Internal link]].",
    "",
  ].join("\n");
  let text = "";
  while (text.length < minimumBytes) text += section;
  return `${text}\nperformance probe`;
}

function p95(values: number[]): number {
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.floor(sorted.length * 0.95)];
}

it.skipIf(process.env.PHI_PERF_BENCHMARK !== "1")(
  "keeps plain input within the reference JavaScript budget",
  async () => {
    setCustomSnippets();
    for (const bytes of [4_000, 20_000, 50_000]) {
      const view = makeView(mixedDocument(bytes), [
        livePreview,
        latexEnhancements(false),
        latexSuite,
      ]);
      const samples: number[] = [];
      for (let index = 0; index < 65; index++) {
        const position = view.state.selection.main.head;
        const started = performance.now();
        view.dispatch({
          changes: { from: position, insert: "x" },
          selection: { anchor: position + 1 },
          annotations: Transaction.userEvent.of("input.type"),
        });
        await Promise.resolve();
        if (index >= 5) samples.push(performance.now() - started);
      }
      const measured = p95(samples);
      process.stdout.write(
        `PHI input benchmark bytes=${bytes} p95=${measured.toFixed(2)}ms\n`,
      );
      expect(measured).toBeLessThan(8);
      view.destroy();
      views.splice(views.indexOf(view), 1);
      document.body.replaceChildren();
    }
  },
  30_000,
);
