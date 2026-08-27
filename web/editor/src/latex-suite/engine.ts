import { Prec, StateEffect, StateField, Transaction } from "@codemirror/state";
import { EditorView, keymap, ViewPlugin, type Command, type ViewUpdate } from "@codemirror/view";
import { indentLess, indentMore, isolateHistory } from "@codemirror/commands";
import defaultSnippets from "./default-snippets.txt?raw";
import defaultSnippetVariables from "./default-snippet-variables.txt?raw";
import { codeModeAt, mathModeAt } from "../markdown/parser";
import { reportError } from "../bridge";
import {
  expandReplacement,
  parseSnippetFile,
  parseSnippetVariables,
  type ExpandedSnippet,
  type LatexSnippet,
} from "./snippet-parser";

interface Tabstop { index: number; from: number; to: number }
interface TabstopState { stops: Tabstop[]; active: number }

const setTabstops = StateEffect.define<TabstopState | null>();
const startTabstops = StateEffect.define<Tabstop[]>();
const tabstopState = StateField.define<TabstopState | null>({
  create: () => null,
  update(value, transaction) {
    if (transaction.isUserEvent("undo") ||
        transaction.isUserEvent("redo")) return null;

    let current = value && {
      ...value,
      stops: value.stops.map((stop, index) => {
        const active = index === value.active;
        const empty = stop.from === stop.to;
        const toAssociation = active || empty ? 1 : -1;
        return {
          ...stop,
          /* Text entered at the active placeholder becomes part of that
           * placeholder. Empty, unvisited stops at the same boundary follow
           * the inserted text instead of turning into reversed ranges. */
          from: transaction.changes.mapPos(stop.from, active ? -1 : 1),
          to: transaction.changes.mapPos(stop.to, toAssociation),
        };
      }),
    };
    for (const effect of transaction.effects) {
      if (effect.is(setTabstops)) {
        current = effect.value;
      } else if (effect.is(startTabstops)) {
        /* A snippet may expand while the cursor is inside another snippet.
         * Visit its stops first, then resume at the unvisited outer stops. */
        const remaining = current
          ? current.stops.slice(current.active + 1)
          : [];
        const stops = [...effect.value, ...remaining];
        current = stops.length
          ? { stops, active: effect.value.length ? 0 : -1 }
          : null;
      }
    }
    return current;
  },
});

interface RuntimeSnippet extends LatexSnippet { expression?: RegExp }

let snippets: RuntimeSnippet[] = [];
let snippetVariables: Record<string, string> = {};

function isVisualSnippet(snippet: LatexSnippet): boolean {
  return snippet.options.includes("v") ||
    snippet.replacement.includes("${VISUAL}");
}

function expandTriggerVariables(trigger: string): string {
  return trigger.replace(/\$\{([A-Z_]+)\}/g, (_match, name: string) =>
    snippetVariables[name] ? `(?:${snippetVariables[name]})` : "(?!)");
}

function compileSnippets(parsed: LatexSnippet[]): RuntimeSnippet[] {
  return parsed.flatMap((snippet) => {
    if (!snippet.options.includes("r")) return [snippet];
    try {
      const flags = (snippet.flags ?? "").replace(/[gy]/g, "");
      return [{
        ...snippet,
        expression: new RegExp(`(?:${expandTriggerVariables(snippet.trigger)})$`, flags),
      }];
    } catch (error) {
      reportError(error, "latex-suite/regex");
      return [];
    }
  });
}

export function setCustomSnippets(source?: string, variableSource?: string): void {
  try {
    const variables = parseSnippetVariables(
      variableSource?.trim() ? variableSource : defaultSnippetVariables,
    );
    const parsed = parseSnippetFile(source?.trim() ? source : defaultSnippets);
    snippetVariables = variables;
    snippets = compileSnippets(parsed);
  } catch (error) {
    reportError(error, "latex-suite/snippets");
  }
}

setCustomSnippets();

function activeMacros(text: string, position: number): Set<string> {
  const start = Math.max(0, position - 4000);
  const stack: (string | null)[] = [];
  for (let index = start; index < position; index++) {
    if (text[index] === "\\") {
      const match = /^\\([A-Za-z]+)\s*\{/.exec(text.slice(index, position));
      if (match) {
        stack.push(match[1]);
        index += match[0].length - 1;
        continue;
      }
      index++;
      continue;
    }
    if (text[index] === "{") stack.push(null);
    else if (text[index] === "}") stack.pop();
  }
  return new Set(stack.filter((name): name is string => Boolean(name)));
}

interface SnippetContext {
  math: ReturnType<typeof mathModeAt>;
  code: ReturnType<typeof codeModeAt>;
}

function contextAllows(snippet: LatexSnippet, context: SnippetContext,
                       macros?: Set<string>): boolean {
  if (snippet.options.includes("c") && context.code !== "block") return false;
  if (snippet.options.includes("C") && context.code !== "inline") return false;
  if (snippet.options.includes("t") &&
      (context.math !== "none" || context.code !== "none")) return false;
  if (snippet.options.includes("M") && context.math !== "display") return false;
  if (snippet.options.includes("n") && context.math !== "inline") return false;
  if (snippet.options.includes("m") && context.math === "none") return false;
  if (snippet.excludedMacros?.some((macro) => macros?.has(macro))) return false;
  if (snippet.includedMacros?.length &&
      !snippet.includedMacros.some((macro) => macros?.has(macro))) return false;
  return true;
}

interface Match {
  snippet: RuntimeSnippet;
  from: number;
  captures: string[];
  groups?: Record<string, string>;
  matchedText: string;
}

function findMatch(text: string, position: number, automatic: boolean,
                   visualAvailable = false): Match | null {
  const start = Math.max(0, position - 512);
  const before = text.slice(start, position);
  const context = {
    math: mathModeAt(text, position),
    code: codeModeAt(text, position),
  };
  let macros: Set<string> | undefined;
  for (const snippet of snippets) {
    if (automatic !== snippet.options.includes("A")) continue;
    if (automatic && isVisualSnippet(snippet) && !visualAvailable) continue;
    if ((snippet.excludedMacros?.length || snippet.includedMacros?.length) && !macros)
      macros = activeMacros(text, position);
    if (!contextAllows(snippet, context, macros)) continue;
    if (snippet.options.includes("r")) {
      const match = snippet.expression?.exec(before);
      if (match) return {
        snippet,
        from: start + match.index,
        captures: match.slice(1),
        groups: match.groups,
        matchedText: match[0],
      };
    } else if (before.endsWith(snippet.trigger)) {
      const from = position - snippet.trigger.length;
      if (snippet.options.includes("w") && from > 0 && /[\p{L}\p{N}_]/u.test(text[from - 1])) continue;
      return { snippet, from, captures: [], matchedText: snippet.trigger };
    }
  }
  return null;
}

function handledReplacement(match: Match): string | null {
  switch (match.snippet.handler) {
    case "space-after-symbol": {
      const withoutLetter = expandTriggerVariables(match.snippet.trigger)
        .replace("([A-Za-z])", "");
      if (new RegExp(withoutLetter).test(match.matchedText))
        return match.matchedText;
      return `\\${match.captures[0]} ${match.captures[1]}`;
    }
    case "protect-macro-prefix": {
      const macro = match.captures[0] ?? "";
      const candidates = expandTriggerVariables(match.snippet.trigger)
        .replace("\\\\([A-Za-z]+)", "");
      return new RegExp(`\\b${macro}`).test(candidates)
        ? match.matchedText : null;
    }
    case "identity-matrix": {
      const size = Number(match.captures[0]);
      if (!Number.isInteger(size) || size < 1 || size > 9) return null;
      const rows = Array.from({ length: size }, (_row, row) =>
        Array.from({ length: size }, (_column, column) =>
          row === column ? "1" : "0").join(" & "));
      return `\\begin{pmatrix}\n${rows.join(" \\\\\n")}\n\\end{pmatrix}`;
    }
    case "display-math-list": {
      const groups = match.groups;
      if (!groups) return null;
      const firstLine = groups.marker + groups.whitespace + groups.text;
      const indent = " ".repeat(groups.marker.length) + groups.whitespace;
      return `${groups.positive_lookbehind}${firstLine}\n${indent}$$\n${indent}$0\n${indent}$$`;
    }
    default:
      return match.snippet.replacement;
  }
}

function expandMatch(match: Match, visual: string): ExpandedSnippet | null {
  const replacement = handledReplacement(match);
  return replacement === null
    ? null : expandReplacement(replacement, match.captures, visual);
}

function applyMatch(view: EditorView, match: Match, to: number, visual = "",
                    automatic = false): boolean {
  const expanded = expandMatch(match, visual);
  if (!expanded) return true;
  const stops = expanded.tabstops.map((stop) => ({ ...stop, from: match.from + stop.from, to: match.from + stop.to }));
  const first = stops[0] ?? { from: match.from + expanded.text.length, to: match.from + expanded.text.length };
  view.dispatch({
    changes: { from: match.from, to, insert: expanded.text },
    selection: { anchor: first.from, head: first.to },
    effects: startTabstops.of(stops),
    annotations: [
      Transaction.userEvent.of(automatic ? "input.type" : "input.complete"),
      ...(automatic ? [isolateHistory.of("before")] : []),
    ],
  });
  return true;
}

const nextTabstop: Command = (view) => {
  const value = view.state.field(tabstopState);
  if (!value?.stops.length) return false;
  if (value.active < 0) {
    /* A nested snippet without placeholders leaves us immediately before the
     * remaining outer stops. */
    const stop = value.stops[0];
    const finished = value.stops.length === 1 && stop.from === stop.to;
    view.dispatch({
      selection: { anchor: stop.from, head: stop.to },
      effects: setTabstops.of(finished ? null : { ...value, active: 0 }),
    });
    return true;
  }
  const current = value.stops[value.active];
  const selection = view.state.selection.main;
  const currentFrom = Math.min(current.from, current.to);
  const currentTo = Math.max(current.from, current.to);
  if (selection.from < currentFrom || selection.to > currentTo) {
    /* A click or edit outside the active placeholder ends this snippet. Let
     * the remaining Tab bindings handle the cursor's current location. */
    view.dispatch({ effects: setTabstops.of(null) });
    return false;
  }
  if (value.active >= value.stops.length - 1) {
    view.dispatch({ selection: { anchor: current.to }, effects: setTabstops.of(null) });
    return true;
  }
  const active = Math.min(value.active + 1, value.stops.length - 1);
  const stop = value.stops[active];
  const finished = active === value.stops.length - 1 && stop.from === stop.to;
  view.dispatch({
    selection: { anchor: stop.from, head: stop.to },
    effects: setTabstops.of(finished ? null : { ...value, active }),
  });
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

function expandVisualShortcut(view: EditorView, event: KeyboardEvent): boolean {
  if (event.ctrlKey || event.metaKey || event.altKey || event.key.length !== 1)
    return false;
  const range = view.state.selection.main;
  if (range.empty || view.state.selection.ranges.length !== 1)
    return false;
  const text = view.state.doc.toString();
  const context = {
    math: mathModeAt(text, range.head),
    code: codeModeAt(text, range.head),
  };
  for (const snippet of snippets) {
    if (!isVisualSnippet(snippet) || snippet.options.includes("r") ||
        snippet.trigger !== event.key ||
        !contextAllows(snippet, context)) continue;
    return applyMatch(
      view,
      { snippet, from: range.from, captures: [], matchedText: event.key },
      range.to,
      view.state.sliceDoc(range.from, range.to),
      true,
    );
  }
  return false;
}

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
          const match = findMatch(
            view.state.doc.toString(), position, true, visual.length > 0,
          );
          if (match) applyMatch(view, match, position, visual, true);
        }
      } finally {
        this.dispatching = false;
      }
    });
  }
});

const indentList: Command = (view) => adjustListIndent(view, false);
const outdentList: Command = (view) => adjustListIndent(view, true);

const continueList: Command = (view) => {
  const range = view.state.selection.main;
  if (!range.empty || view.state.selection.ranges.length !== 1)
    return false;
  const line = view.state.doc.lineAt(range.head);
  const match = /^(\s*)([-+*]|(\d+)([.)]))([ \t]+)(?:\[([^\]])\]([ \t]*))?/.exec(
    line.text,
  );
  if (!match || range.head < line.from + match[0].length)
    return false;
  const content = line.text.slice(match[0].length);
  if (range.head === line.to && !content.trim()) {
    view.dispatch({
      changes: { from: line.from, to: line.from + match[0].length, insert: "" },
      selection: { anchor: line.from },
      effects: setTabstops.of(null),
      annotations: Transaction.userEvent.of("input.type"),
    });
    return true;
  }
  const marker = match[3]
    ? `${Number(match[3]) + 1}${match[4]}`
    : match[2];
  const task = match[6] == null ? "" : "[ ] ";
  const prefix = `${match[1]}${marker} ${task}`;
  view.dispatch({
    changes: { from: range.head, insert: `\n${prefix}` },
    selection: { anchor: range.head + prefix.length + 1 },
    effects: setTabstops.of(null),
    annotations: Transaction.userEvent.of("input.type"),
  });
  return true;
};

function adjustListIndent(view: EditorView, outdent: boolean): boolean {
  const lineNumbers = new Set<number>();
  for (const range of view.state.selection.ranges) {
    const first = view.state.doc.lineAt(range.from).number;
    const last = view.state.doc.lineAt(range.to).number;
    for (let number = first; number <= last; number++) lineNumbers.add(number);
  }
  const lines = [...lineNumbers].map((number) => view.state.doc.line(number));
  if (!lines.length || !lines.every((line) => /^\s*(?:[-+*]|\d+[.)])\s+/.test(line.text)))
    return false;
  const changes: { from: number; to?: number; insert: string }[] = [];
  let ordered = 0;
  for (const line of lines) {
    if (outdent) {
      const leading = /^(?: {1,4}|\t)/.exec(line.text)?.[0];
      if (leading)
        changes.push({
          from: line.from,
          to: line.from + leading.length,
          insert: "",
        });
      continue;
    }

    changes.push({ from: line.from, insert: "    " });
    const marker = /^(\s*)(\d+)([.)])(?=[ \t]+)/.exec(line.text);
    if (!marker) {
      ordered = 0;
      continue;
    }
    changes.push({
      from: line.from + marker[1].length,
      to: line.from + marker[1].length + marker[2].length,
      insert: String(++ordered),
    });
  }
  if (!changes.length) return false;
  view.dispatch({ changes, annotations: Transaction.userEvent.of("input.indent") });
  return true;
}

export const latexSuite = [
  tabstopState,
  automaticPlugin,
  /* A native key event preserves the selection. Waiting for the browser's
   * text-input transaction is unreliable in WebKit because it may collapse
   * or replace that selection before visual snippets inspect it. */
  Prec.high(EditorView.domEventHandlers({
    keydown: (event, view) => expandVisualShortcut(view, event),
  })),
  Prec.high(keymap.of([
    { key: "Tab", run: nextTabstop },
    { key: "Tab", run: expandManualSnippet },
    { key: "Tab", run: matrixTab },
    { key: "Tab", run: tabOut },
    { key: "Tab", run: indentList },
    { key: "Tab", run: indentMore },
    { key: "Shift-Tab", run: previousTabstop },
    { key: "Shift-Tab", run: outdentList },
    { key: "Shift-Tab", run: indentLess },
    { key: "Enter", run: continueList },
    { key: "Enter", run: matrixEnter },
    { key: "Shift-Enter", run: matrixShiftEnter },
  ])),
];
