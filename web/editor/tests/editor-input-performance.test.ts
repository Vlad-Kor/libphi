// @vitest-environment jsdom
/// <reference types="node" />
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { EditorState, RangeSet, Transaction } from "@codemirror/state";
import { type DecorationSet, EditorView } from "@codemirror/view";
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
    expect(markdownAnalysis(state).updateKind).not.toBe("mapped");
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

  it("keeps widget DOM below an edit and reveals at the shifted source", () => {
    const text = [
      "intro", "", "```js", "let a = 1;", "```", "", "$$", "x^2", "$$", "",
      "> [!note] Title", "> body", "", "- item", "",
    ].join("\n");
    const view = makeView(text, [livePreview]);
    view.dispatch({ selection: { anchor: 0 } });
    const code = view.dom.querySelector(".code-block-widget");
    const math = view.dom.querySelector(".math-display");
    const callout = view.dom.querySelector(".callout");
    const bullet = view.dom.querySelector(".list-bullet");
    expect(code && math && callout && bullet).toBeTruthy();

    for (const character of "typed ") {
      const at = view.state.selection.main.head;
      view.dispatch({
        changes: { from: at, insert: character },
        selection: { anchor: at + 1 },
        annotations: Transaction.userEvent.of("input.type"),
      });
    }
    for (const element of [code, math, callout, bullet])
      expect(element!.isConnected).toBe(true);
    expect(view.dom.querySelector(".code-block-widget")).toBe(code);

    callout!.querySelector(".callout-content")!.dispatchEvent(
      new MouseEvent("click", { bubbles: true, cancelable: true }));
    const body = view.state.doc.toString().indexOf("> body");
    expect(view.state.selection.main.head).toBe(body + 2);
  });

  it("edits a kept table widget at its shifted source position", () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const editor = new PhiMarkdownEditor(parent);
    views.push(editor.view);
    const table = "| A | B |\n| --- | --- |\n| 1 | 2 |\n| 3 | 4 |";
    editor.openDocument({
      documentId: "table-shift", path: "table.md",
      text: `intro\n\n${table}\n\nafter`, revision: 1, lineEnding: "LF",
    });
    const widget = parent.querySelector(".rich-table-widget");
    editor.view.dispatch({
      changes: { from: 0, insert: "typed above " },
      selection: { anchor: 12 },
      userEvent: "input.type",
    });
    expect(parent.querySelector(".rich-table-widget")).toBe(widget);

    let context: { payload: Record<string, unknown> } | null = null;
    window.addEventListener("phi-native-message", (event) => {
      const message = (event as CustomEvent).detail;
      if (message.type === "table/context") context = message;
    }, { once: true });
    parent.querySelector('.rich-table-row-handle[data-index="1"]')!
      .dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true }));
    expect(context!.payload.from).toBe(editor.view.state.doc.toString().indexOf("| A"));
    editor.receive({ protocol: 1, type: "table/remove", payload: context!.payload });
    expect(editor.view.state.doc.toString()).toBe(
      "typed above intro\n\n| A   | B   |\n| --- | --- |\n| 3   | 4   |\n\nafter");
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

describe("region reparse", () => {
  const blocks = [
    "Plain prose line.", "A line with **bold**, *em*, `code`, and $x^2$.",
    "- list item", "- [ ] task", "1. ordered", "# Heading", "> quote",
    "> [!note] Callout\n> body", "| a | b |\n| --- | --- |\n| 1 | 2 |",
    "```js\nlet a = 1;\n\nlet b = a < 2;\n```", "~~~\nraw\n~~~",
    "$$\n\\frac{a}{b}\n\\sum_i x_i\n$$", "\\[\nx\n\\]", "$$ inline display $$",
    "[^1]: note", "text[^1] ref", "<span>html</span>", "%% comment %%",
    "---", "{", "}", "tag #tag ^block-id", "[[Wiki]] and [link](target)",
    "![image](a.png)", "", "",
  ];
  const tokens = ["x", " ", "\n", "$", "$$", "`", "```", "*", "_", "[", "]",
    "(", ")", "#", "- ", "|", ">", "<", "%", "^", "\\", "~", "=", "{", "}",
    "---", "[^", "ab", "\n\n"];

  function random(seed: number) {
    return () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed / 0x7fffffff;
    };
  }

  it("matches a clean parse after every random edit", () => {
    const next = random(Number(process.env.PHI_REGION_SEED ?? 11));
    const pick = <T,>(values: readonly T[]) =>
      values[Math.floor(next() * values.length)];
    const kinds: Record<string, number> = { full: 0, mapped: 0, region: 0 };
    for (let run = 0; run < Number(process.env.PHI_REGION_RUNS ?? 150); run++) {
      const lines = Array.from({ length: 4 + Math.floor(next() * 14) },
        () => pick(blocks));
      if (next() < 0.1) lines.unshift("---\ntitle: x\n---");
      let state = EditorState.create({
        doc: lines.join("\n"), extensions: [markdownAnalysisField],
      });
      for (let step = 0; step < 40; step++) {
        const length = state.doc.length;
        const from = Math.floor(next() * (length + 1));
        const roll = next();
        const change = roll < 0.55
          ? { from, insert: next() < 0.7 ? pick(["a", "b", "x", " ", "1"]) : pick(tokens) }
          : roll < 0.8
            ? { from, to: Math.min(length, from + 1 + Math.floor(next() * 3)) }
            : { from, to: Math.min(length, from + Math.floor(next() * 2)),
                insert: pick(tokens) };
        const previousText = state.doc.toString();
        state = state.update({ changes: change }).state;
        const analysis = markdownAnalysis(state);
        kinds[analysis.updateKind]++;
        const expected = parseMarkdownNodes(state.doc.toString());
        if (process.env.PHI_DEBUG_REGION && JSON.stringify(analysis.nodes) !== JSON.stringify(expected)) {
          process.stdout.write(`BEFORE ${JSON.stringify(previousText)}\nCHANGE ${JSON.stringify(change)}\nAFTER ${JSON.stringify(state.doc.toString())}\nGOT ${JSON.stringify(analysis.nodes)}\nWANT ${JSON.stringify(expected)}\n`);
        }
        expect(analysis.nodes).toEqual(expected);
      }
    }
    expect(kinds.region).toBeGreaterThan(500);
  });

  it("reparses only the code block or equation being typed in", () => {
    const text = "# Title\n\nIntro $x$.\n\n```py\ndef f():\n    return 1\n```\n\n" +
      "$$\n\\begin{aligned}\na &= b \\\\\n\\end{aligned}\n$$\n\nEnd *here*.";
    let state = EditorState.create({ doc: text, extensions: [markdownAnalysisField] });
    for (const probe of ["return 1", "a &= b", "End *here*"]) {
      const at = state.doc.toString().indexOf(probe) + 3;
      state = state.update({ changes: { from: at, insert: "q" } }).state;
      expect(markdownAnalysis(state).updateKind).toBe("region");
      expect(markdownAnalysis(state).nodes)
        .toEqual(parseMarkdownNodes(state.doc.toString()));
    }
  });
});

describe("incremental live preview decorations", () => {
  function decorations(state: EditorState): DecorationSet {
    const sets = state.facet(EditorView.decorations)
      .filter((value): value is DecorationSet => typeof value !== "function");
    expect(sets).toHaveLength(1);
    return sets[0];
  }

  function random(seed: number) {
    return () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed / 0x7fffffff;
    };
  }

  it("equal a clean rebuild after random edits and selection changes", () => {
    const blocks = [
      "Plain prose line.", "A line with **bold**, *em*, `code`, and $x^2$.",
      "- list item with $$y$$ display", "- [ ] task", "1. ordered", "# Heading",
      "> quote", "> [!note] Callout\n> body", "| a | b |\n| --- | --- |\n| 1 | 2 |",
      "```js\nlet a = 1;\n\nlet b = 2;\n```", "$$\n\\frac{a}{b}\n$$",
      "[^1]: note", "text[^1] ref", "<span>html</span>", "%% comment %%",
      "---", "tag #tag ^block-id", "[[Wiki]] and [link](target)",
      "![image](a.png)", "![[embed.png]]", "==mark== ~~strike~~", "", "",
    ];
    const tokens = ["x", " ", "\n", "$", "$$", "`", "*", "_", "[", "]", "#",
      "- ", "|", ">", "^", "\\", "~", "=", "---", "[[", "]]", "\n\n"];
    const next = random(Number(process.env.PHI_REGION_SEED ?? 5));
    const pick = <T,>(values: readonly T[]) =>
      values[Math.floor(next() * values.length)];
    let checked = 0;
    for (let run = 0; run < Number(process.env.PHI_DECORATION_RUNS ?? 60); run++) {
      const lines = Array.from({ length: 4 + Math.floor(next() * 12) },
        () => pick(blocks));
      let state = EditorState.create({
        doc: lines.join("\n"),
        extensions: [markdown({ base: markdownLanguage }), livePreview],
      });
      for (let step = 0; step < 40; step++) {
        const length = state.doc.length;
        const at = Math.floor(next() * (length + 1));
        const roll = next();
        if (roll < 0.35) {
          const head = Math.min(length, at + (next() < 0.3 ? Math.floor(next() * 12) : 0));
          state = state.update({ selection: { anchor: at, head } }).state;
        } else {
          const change = roll < 0.75
            ? { from: at, insert: next() < 0.6 ? pick(["a", "b", " "]) : pick(tokens) }
            : { from: at, to: Math.min(length, at + 1 + Math.floor(next() * 3)) };
          const insert = "insert" in change ? change.insert?.length ?? 0 : 0;
          state = state.update({
            changes: change,
            selection: { anchor: Math.min(change.from + insert, length + insert) },
          }).state;
        }
        const clean = EditorState.create({
          doc: state.doc,
          selection: state.selection,
          extensions: [markdown({ base: markdownLanguage }), livePreview],
        });
        const atomic = (current: EditorState) => current.facet(EditorView.atomicRanges)
          .map((provider) => provider({ state: current } as EditorView));
        const cleanAtomic = atomic(clean);
        atomic(state).forEach((set, index) =>
          expect(RangeSet.eq([set], [cleanAtomic[index]])).toBe(true));
        const incremental = decorations(state);
        const expected = decorations(clean);
        if (!RangeSet.eq([incremental], [expected])) {
          const dump = (set: DecorationSet) => {
            const out: string[] = [];
            for (let it = set.iter(); it.value; it.next())
              out.push(`${it.from}-${it.to}:${it.value.spec.class ?? it.value.spec.widget?.constructor.name ?? "?"}`);
            return out.join(" ");
          };
          throw new Error(`mismatch in ${JSON.stringify(state.doc.toString())} ` +
            `sel ${JSON.stringify(state.selection.main)}\n` +
            `got  ${dump(incremental)}\nwant ${dump(expected)}`);
        }
        checked++;
      }
    }
    expect(checked).toBeGreaterThan(1000);
  });
});

describe("incremental LaTeX syntax decorations", () => {
  it("equal a fresh view after edits and selection moves in math", () => {
    let seed = 17;
    const next = () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed / 0x7fffffff;
    };
    const blocks = ["Prose $a_{1} + (b)$ text.", "$$\n\\frac{a}{b} + \\left(x\\right)\n$$",
      "$$\n\\begin{aligned}\na &= \\{b\\} \\\\\n\\end{aligned}\n$$", "Plain.", "",
      "Inline \\(x^2\\) and $[y]$.", "```\n$not math$\n```"];
    const tokens = ["x", "{", "}", "(", ")", "[", "]", "\\", "$", " ", "\n"];
    const extensions = [markdown({ base: markdownLanguage }), livePreview,
      latexEnhancements(false)];
    const viewDecorations = (view: EditorView) => view.state
      .facet(EditorView.decorations)
      .map((value) => typeof value === "function" ? value(view) : value);
    for (let run = 0; run < 12; run++) {
      const text = Array.from({ length: 6 + Math.floor(next() * 6) },
        () => blocks[Math.floor(next() * blocks.length)]).join("\n");
      const view = makeView(text, [livePreview, latexEnhancements(false)]);
      for (let step = 0; step < 30; step++) {
        const length = view.state.doc.length;
        const at = Math.floor(next() * (length + 1));
        if (next() < 0.4) {
          view.dispatch({ selection: { anchor: at } });
        } else {
          const insert = tokens[Math.floor(next() * tokens.length)];
          const typing = next() < 0.8;
          view.dispatch({
            changes: typing ? { from: at, insert }
              : { from: at, to: Math.min(length, at + 1) },
            selection: { anchor: typing ? at + insert.length : at },
          });
        }
        const fresh = new EditorView({ state: EditorState.create({
          doc: view.state.doc, selection: view.state.selection, extensions,
        }) });
        const expected = viewDecorations(fresh);
        viewDecorations(view).forEach((set, index) =>
          expect(RangeSet.eq([set], [expected[index]])).toBe(true));
        fresh.destroy();
      }
    }
  }, 120_000);
});
