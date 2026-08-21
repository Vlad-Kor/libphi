import { Prec, StateEffect, StateField, Transaction } from "@codemirror/state";
import { EditorView, keymap, ViewPlugin, type Command, type ViewUpdate } from "@codemirror/view";
import { indentLess, indentMore } from "@codemirror/commands";
import defaultSnippets from "./default-snippets.txt";
import { codeModeAt, mathModeAt } from "../obsidian/parser";
import { reportError } from "../bridge";
import { expandReplacement, parseSnippetFile, type LatexSnippet } from "./snippet-parser";

interface Tabstop { index: number; from: number; to: number }
interface TabstopState { stops: Tabstop[]; active: number }

const setTabstops = StateEffect.define<TabstopState | null>();
const tabstopState = StateField.define<TabstopState | null>({
  create: () => null,
  update(value, transaction) {
    for (const effect of transaction.effects)
      if (effect.is(setTabstops)) return effect.value;
    if (!value) return null;
    const stops = value.stops.map((stop) => ({
      ...stop,
      from: transaction.changes.mapPos(stop.from, 1),
      to: transaction.changes.mapPos(stop.to, -1),
    }));
    return { ...value, stops };
  },
});

let snippets: LatexSnippet[] = [];
try {
  const bundledSnippets = typeof PHI_USER_SNIPPETS === "string"
    ? PHI_USER_SNIPPETS
    : defaultSnippets;
  snippets = parseSnippetFile(bundledSnippets);
} catch (error) {
  reportError(error, "latex-suite/snippets");
}

export function setCustomSnippets(source?: string): void {
  if (!source) return;
  try { snippets = parseSnippetFile(source); }
  catch (error) { reportError(error, "latex-suite/snippets"); }
}

const snippetVariables: Record<string, string> = {
  GREEK: "alpha|beta|gamma|delta|epsilon|varepsilon|zeta|eta|theta|vartheta|iota|kappa|lambda|mu|nu|xi|pi|varpi|rho|varrho|sigma|varsigma|tau|upsilon|phi|varphi|chi|psi|omega",
  SYMBOL: "times|cdot|pm|mp|to|mapsto|in|notin|subset|subseteq|supset|supseteq|cup|cap|leq|geq|neq|sim|approx|infty|partial|nabla",
  SHORT_SYMBOL: "sin|cos|tan|log|ln|exp|det|dim|ker",
};

function expandTriggerVariables(trigger: string): string {
  return trigger.replace(/\$\{([A-Z_]+)\}/g, (_match, name: string) =>
    snippetVariables[name] ? `(?:${snippetVariables[name]})` : "(?!)");
}

function contextAllows(snippet: LatexSnippet, text: string, position: number): boolean {
  const math = mathModeAt(text, position);
  const code = codeModeAt(text, position);
  if (snippet.options.includes("c") && code !== "block") return false;
  if (snippet.options.includes("C") && code !== "inline") return false;
  if (snippet.options.includes("t") && (math !== "none" || code !== "none")) return false;
  if (snippet.options.includes("M") && math !== "display") return false;
  if (snippet.options.includes("n") && math !== "inline") return false;
  if (snippet.options.includes("m") && math === "none") return false;
  return true;
}

interface Match { snippet: LatexSnippet; from: number; captures: string[] }

function findMatch(text: string, position: number, automatic: boolean): Match | null {
  const start = Math.max(0, position - 512);
  const before = text.slice(start, position);
  for (const snippet of snippets) {
    if (automatic !== snippet.options.includes("A")) continue;
    if (!contextAllows(snippet, text, position)) continue;
    if (snippet.options.includes("r")) {
      try {
        const flags = (snippet.flags ?? "").replace(/[gy]/g, "");
        const expression = new RegExp(`(?:${expandTriggerVariables(snippet.trigger)})$`, flags);
        const match = expression.exec(before);
        if (match) return { snippet, from: start + match.index, captures: match.slice(1) };
      } catch (error) {
        reportError(error, "latex-suite/regex");
      }
    } else if (before.endsWith(snippet.trigger)) {
      const from = position - snippet.trigger.length;
      if (snippet.options.includes("w") && from > 0 && /[\p{L}\p{N}_]/u.test(text[from - 1])) continue;
      return { snippet, from, captures: [] };
    }
  }
  return null;
}

function applyMatch(view: EditorView, match: Match, to: number, visual = "",
                    automatic = false): boolean {
  const expanded = expandReplacement(match.snippet.replacement, match.captures, visual);
  const stops = expanded.tabstops.map((stop) => ({ ...stop, from: match.from + stop.from, to: match.from + stop.to }));
  const first = stops[0] ?? { from: match.from + expanded.text.length, to: match.from + expanded.text.length };
  view.dispatch({
    changes: { from: match.from, to, insert: expanded.text },
    selection: { anchor: first.from, head: first.to },
    effects: setTabstops.of(stops.length ? { stops, active: 0 } : null),
    annotations: Transaction.userEvent.of(automatic ? "input.type" : "input.complete"),
  });
  return true;
}

const nextTabstop: Command = (view) => {
  const value = view.state.field(tabstopState);
  if (!value?.stops.length) return false;
  if (value.active >= value.stops.length - 1) {
    const stop = value.stops[value.active];
    view.dispatch({ selection: { anchor: stop.to }, effects: setTabstops.of(null) });
    return true;
  }
  const active = Math.min(value.active + 1, value.stops.length - 1);
  const stop = value.stops[active];
  view.dispatch({ selection: { anchor: stop.from, head: stop.to }, effects: setTabstops.of({ ...value, active }) });
  return true;
};

const previousTabstop: Command = (view) => {
  const value = view.state.field(tabstopState);
  if (!value?.stops.length) return false;
  const active = Math.max(value.active - 1, 0);
  const stop = value.stops[active];
  view.dispatch({ selection: { anchor: stop.from, head: stop.to }, effects: setTabstops.of({ ...value, active }) });
  return true;
};

const expandManualSnippet: Command = (view) => {
  const range = view.state.selection.main;
  const match = findMatch(view.state.doc.toString(), range.head, false);
  return match ? applyMatch(view, match, range.head) : false;
};

function currentEnvironment(text: string, position: number): string | null {
  const prefix = text.slice(Math.max(0, position - 3000), position);
  const openings = [...prefix.matchAll(/\\begin\{(matrix|pmatrix|bmatrix|vmatrix|Vmatrix|array|aligned|align|cases)\}/g)];
  const last = openings.at(-1)?.[1];
  if (!last) return null;
  const after = prefix.slice(openings.at(-1)!.index);
  return after.includes(`\\end{${last}}`) ? null : last;
}

const matrixTab: Command = (view) => {
  const position = view.state.selection.main.head;
  if (!currentEnvironment(view.state.doc.toString(), position)) return false;
  view.dispatch({ changes: { from: position, insert: " & " }, selection: { anchor: position + 3 }, userEvent: "input" });
  return true;
};

const matrixEnter: Command = (view) => {
  const position = view.state.selection.main.head;
  if (!currentEnvironment(view.state.doc.toString(), position)) return false;
  const insert = " \\\\\n";
  view.dispatch({ changes: { from: position, insert }, selection: { anchor: position + insert.length }, userEvent: "input" });
  return true;
};

const matrixShiftEnter: Command = (view) => {
  const position = view.state.selection.main.head;
  const text = view.state.doc.toString();
  const environment = currentEnvironment(text, position);
  if (!environment) return false;
  const suffix = text.slice(position);
  const row = /\\\\\s*\n/.exec(suffix);
  const end = suffix.indexOf(`\\end{${environment}}`);
  const offset = row ? row.index + row[0].length : end;
  if (offset < 0) return false;
  view.dispatch({ selection: { anchor: position + offset } });
  return true;
};

const tabOut: Command = (view) => {
  const position = view.state.selection.main.head;
  const suffix = view.state.sliceDoc(position, Math.min(position + 2, view.state.doc.length));
  const match = /^(\}|\]|\)|\$)/.exec(suffix);
  if (!match || mathModeAt(view.state.doc.toString(), position) === "none") return false;
  view.dispatch({ selection: { anchor: position + match[0].length } });
  return true;
};

function autoFraction(view: EditorView): boolean {
  const position = view.state.selection.main.head;
  const text = view.state.doc.toString();
  if (text[position - 1] !== "/" || text[position - 2] === "/" || mathModeAt(text, position - 1) === "none") return false;
  const prefix = text.slice(Math.max(0, position - 200), position - 1);
  const expression = /(\\[A-Za-z]+(?:\{[^{}]*\})*|\([^()]+\)|\[[^\[\]]+\]|[A-Za-z0-9]+(?:\^\{?[^\s{}]+\}?|_\{?[^\s{}]+\}?)?)$/.exec(prefix);
  if (!expression) return false;
  const from = position - 1 - expression[0].length;
  const replacement = `\\frac{${expression[0]}}{}`;
  view.dispatch({
    changes: { from, to: position, insert: replacement },
    selection: { anchor: from + replacement.length - 1 },
    annotations: Transaction.userEvent.of("input.type"),
  });
  return true;
}

const automaticPlugin = ViewPlugin.fromClass(class {
  private dispatching = false;
  update(update: ViewUpdate): void {
    if (this.dispatching || !update.docChanged ||
        !update.transactions.some((transaction) => transaction.isUserEvent("input.type"))) return;
    const view = update.view;
    const previous = update.startState.selection.main;
    const visual = previous.empty
      ? ""
      : update.startState.sliceDoc(previous.from, previous.to);
    queueMicrotask(() => {
      if (view.state.selection.ranges.length !== 1) return;
      this.dispatching = true;
      try {
        if (!autoFraction(view)) {
          const position = view.state.selection.main.head;
          const match = findMatch(view.state.doc.toString(), position, true);
          if (match) applyMatch(view, match, position, visual, true);
        }
      } finally {
        this.dispatching = false;
      }
    });
  }
});

export const latexSuite = [
  tabstopState,
  automaticPlugin,
  Prec.high(keymap.of([
    { key: "Tab", run: nextTabstop },
    { key: "Tab", run: expandManualSnippet },
    { key: "Tab", run: matrixTab },
    { key: "Tab", run: tabOut },
    { key: "Tab", run: indentMore },
    { key: "Shift-Tab", run: previousTabstop },
    { key: "Shift-Tab", run: indentLess },
    { key: "Enter", run: matrixEnter },
    { key: "Shift-Enter", run: matrixShiftEnter },
  ])),
];
