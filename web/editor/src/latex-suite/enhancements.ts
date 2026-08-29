/* LaTeX syntax presentation and optional concealment.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import {
  RangeSet,
  RangeSetBuilder,
  RangeValue,
  type EditorState,
  type Extension,
  type Range,
} from "@codemirror/state";
import {
  Decoration,
  EditorView,
  ViewPlugin,
  WidgetType,
  type DecorationSet,
  type ViewUpdate,
} from "@codemirror/view";
import {
  markdownAnalysis,
  markdownAnalysisField,
  mathNodeAt,
  type MarkdownAnalysis,
} from "../markdown/analysis";
import { measurePerformance } from "../performance";
import {
  brackets,
  cmd_symbols,
  fractions,
  greek,
  leftrightBrackets,
  mathbb,
  mathscrcal,
  not_remap,
  operators,
} from "./conceal-maps";

interface LatexGroup {
  open: number;
  contentFrom: number;
  contentTo: number;
  close: number;
  end: number;
}

export interface LatexReplacement {
  from: number;
  to: number;
  text: string;
  className?: string;
  elementType?: "span" | "sup" | "sub";
  source: string;
}

export type LatexConcealSpec = LatexReplacement[];

interface LatexCommand {
  from: number;
  to: number;
  name: string;
}

const allSymbols: Record<string, string> = { ...greek, ...cmd_symbols };
const operatorNames = new Set(operators);
const fractionCommands = new Set(["frac", "dfrac", "tfrac", "gfrac"]);
const accents: Record<string, string> = {
  hat: "\u0302",
  dot: "\u0307",
  ddot: "\u0308",
  overline: "\u0304",
  bar: "\u0304",
  tilde: "\u0303",
  vec: "\u20d7",
};
const textModifiers: Record<string, string | undefined> = {
  mathbf: "cm-latex-conceal-bold",
  boldsymbol: "cm-latex-conceal-bold",
  underline: "cm-latex-conceal-underline",
  mathrm: "cm-latex-conceal-roman",
  mathbb: undefined,
};

function commandAt(source: string, from: number): LatexCommand | null {
  if (source[from] !== "\\" || from + 1 >= source.length) return null;
  let to = from + 1;
  if (/[A-Za-z]/.test(source[to])) {
    while (to < source.length && /[A-Za-z]/.test(source[to])) to++;
  } else {
    to++;
  }
  return { from, to, name: source.slice(from + 1, to) };
}

function groupAt(source: string, open: number): LatexGroup | null {
  if (source[open] !== "{") return null;
  let depth = 0;
  for (let at = open; at < source.length; at++) {
    if (source[at] === "\\") {
      at++;
      continue;
    }
    if (source[at] === "{") depth++;
    else if (source[at] === "}" && --depth === 0) {
      return {
        open,
        contentFrom: open + 1,
        contentTo: at,
        close: at,
        end: at + 1,
      };
    }
  }
  return null;
}

function argumentAfter(source: string, from: number): LatexGroup | null {
  let open = from;
  while (open < source.length && /[ \t]/.test(source[open])) open++;
  return groupAt(source, open);
}

function replacement(
  source: string,
  offset: number,
  from: number,
  to: number,
  text: string,
  className?: string,
  elementType?: LatexReplacement["elementType"],
): LatexReplacement {
  return {
    from: offset + from,
    to: offset + to,
    text,
    className,
    elementType,
    source: source.slice(from, to),
  };
}

function sourceReplacement(
  source: string,
  offset: number,
  from: number,
  to: number,
  text: string,
  className?: string,
  elementType?: LatexReplacement["elementType"],
): LatexConcealSpec {
  return [replacement(
    source, offset, from, to, text, className, elementType,
  )];
}

function mappedCharacters(value: string, mapping: Record<string, string | undefined>): string | null {
  const mapped = [...value].map((character) => mapping[character]);
  return mapped.some((character) => character == null) ? null : mapped.join("");
}

function plainSymbol(value: string): string | null {
  if (/^[A-Za-z]$/.test(value)) return value;
  const command = commandAt(value, 0);
  if (!command || command.to !== value.length) return null;
  return greek[command.name] ?? null;
}

function applyVisualReplacements(source: string, specs: LatexConcealSpec[]): string | null {
  const replacements = specs.flat();
  if (replacements.some((replacement) => replacement.elementType)) return null;
  let rendered = source;
  for (const replacement of replacements.sort((left, right) => right.from - left.from)) {
    if (replacement.from < 0 || replacement.to > source.length) return null;
    rendered = rendered.slice(0, replacement.from) + replacement.text + rendered.slice(replacement.to);
  }
  return rendered;
}

function visualFragment(source: string): string {
  return applyVisualReplacements(source, concealLatex(source)) ?? source;
}

function limitsEnd(source: string, from: number): number {
  let start = from;
  while (start < source.length && /[ \t]/.test(source[start])) start++;
  const limits = commandAt(source, start);
  return limits?.name === "limits" ? limits.to : from;
}

function scriptOperand(source: string, from: number): { content: string; end: number } | null {
  const group = groupAt(source, from);
  if (group) {
    return {
      content: source.slice(group.contentFrom, group.contentTo),
      end: group.end,
    };
  }
  const command = commandAt(source, from);
  if (command)
    return { content: source.slice(command.from, command.to), end: command.to };
  if (from < source.length && !/\s/.test(source[from]))
    return { content: source[from], end: from + 1 };
  return null;
}

/** Build upstream-compatible conceal replacements for one LaTeX fragment. */
export function concealLatex(source: string, offset = 0): LatexConcealSpec[] {
  const specs: LatexConcealSpec[] = [];
  for (let at = 0; at < source.length;) {
    if (source[at] === "^" || source[at] === "_") {
      const operand = scriptOperand(source, at + 1);
      if (operand) {
        const visual = applyVisualReplacements(
          operand.content, concealLatex(operand.content),
        );
        /* A sup/sub widget can only contain text.  If its operand contains a
         * nested sup/sub widget, leave the outer script source visible and
         * continue walking its contents.  Otherwise the nested source (for
         * example `\\mathbb{R}^{2}`) would be shown literally inside the
         * outer widget and its own concealments would never be reached. */
        if (visual == null) {
          at++;
          continue;
        }
        specs.push(sourceReplacement(
          source, offset, at, operand.end, visual, "cm-latex-conceal-script",
          source[at] === "^" ? "sup" : "sub",
        ));
        at = operand.end;
        continue;
      }
      at++;
      continue;
    }

    const command = commandAt(source, at);
    if (!command) {
      at++;
      continue;
    }

    if (fractionCommands.has(command.name)) {
      const numerator = argumentAfter(source, command.to);
      const denominator = numerator ? argumentAfter(source, numerator.end) : null;
      if (numerator && denominator) {
        const fractionKey = source.slice(numerator.open, denominator.end);
        if (fractions[fractionKey]) {
          specs.push(sourceReplacement(
            source, offset, command.from, denominator.end,
            fractions[fractionKey], "cm-latex-conceal-fraction",
          ));
          at = denominator.end;
          continue;
        }
        specs.push([
          replacement(source, offset, command.from, command.to, ""),
          replacement(source, offset, numerator.open, numerator.contentFrom,
                      "(", "cm-latex-conceal-bracket"),
          replacement(source, offset, numerator.contentTo, numerator.end,
                      ")", "cm-latex-conceal-bracket"),
          replacement(source, offset, numerator.end, numerator.end,
                      "/", "cm-latex-conceal-bracket"),
          replacement(source, offset, denominator.open, denominator.contentFrom,
                      "(", "cm-latex-conceal-bracket"),
          replacement(source, offset, denominator.contentTo, denominator.end,
                      ")", "cm-latex-conceal-bracket"),
        ]);
        /* Continue through both arguments so nested commands and scripts get
         * their own independent concealments, as in upstream LaTeX Suite. */
        at = command.to;
        continue;
      }
    }

    if (command.name in accents) {
      const argument = argumentAfter(source, command.to);
      if (argument) {
        const raw = source.slice(argument.contentFrom, argument.contentTo);
        const symbol = plainSymbol(raw);
        if (symbol) {
          specs.push(sourceReplacement(
            source, offset, command.from, argument.end,
            symbol + accents[command.name], "cm-latex-conceal-unicode",
          ));
          at = argument.end;
          continue;
        }
      }
    }

    if (command.name in textModifiers) {
      const argument = argumentAfter(source, command.to);
      if (argument) {
        let content = source.slice(argument.contentFrom, argument.contentTo);
        if (/[^A-Za-z0-9 ]/.test(content)) {
          const allowedGreek = (command.name === "underline" || command.name === "boldsymbol")
            ? plainSymbol(content) : null;
          if (allowedGreek) content = allowedGreek;
          else {
            at = command.to;
            continue;
          }
        }
        if (command.name === "mathbb") {
          const mapped = mappedCharacters(content, mathbb);
          if (!mapped) {
            at = command.to;
            continue;
          }
          content = mapped;
        }
        specs.push(sourceReplacement(
          source, offset, command.from, argument.end, content,
          textModifiers[command.name],
        ));
        at = argument.end;
        continue;
      }
    }

    if (command.name === "mathcal") {
      const argument = argumentAfter(source, command.to);
      if (argument) {
        const mapped = mappedCharacters(
          source.slice(argument.contentFrom, argument.contentTo), mathscrcal,
        );
        if (mapped) {
          specs.push(sourceReplacement(
            source, offset, command.from, argument.end, mapped,
            "cm-latex-conceal-unicode",
          ));
          at = argument.end;
          continue;
        }
      }
    }

    if (command.name === "operatorname" || command.name === "text" ||
        command.name === "set") {
      const argument = argumentAfter(source, command.to);
      if (argument) {
        const content = source.slice(argument.contentFrom, argument.contentTo);
        const allowed = command.name === "text"
          /* Keep nested LaTeX editable, but do not limit ordinary text to
           * ASCII: accented text, non-Latin scripts, and emoji are all valid. */
          ? !/[\\{}\r\n]/u.test(content)
          : command.name === "set" || /^[A-Za-z]+$/.test(content);
        if (allowed) {
          specs.push(sourceReplacement(
            source, offset, command.from, argument.end,
            command.name === "set" ? `{${visualFragment(content)}}` : content,
            command.name === "set" ? undefined : "cm-latex-conceal-roman",
          ));
          at = argument.end;
          continue;
        }
      }
    }

    if (command.name === "bra" || command.name === "ket" ||
        command.name === "braket") {
      const argument = argumentAfter(source, command.to);
      if (argument) {
        const left = command.name === "ket" ? "|" : "⟨";
        const right = command.name === "bra" ? "|" : "⟩";
        specs.push([
          replacement(source, offset, command.from, argument.open, ""),
          replacement(source, offset, argument.open, argument.contentFrom, left,
                      "cm-latex-conceal-bracket"),
          replacement(source, offset, argument.contentTo, argument.end, right,
                      "cm-latex-conceal-bracket"),
        ]);
        at = command.to;
        continue;
      }
    }

    if (command.name === "not") {
      let nextFrom = command.to;
      while (nextFrom < source.length && /[ \t]/.test(source[nextFrom])) nextFrom++;
      const next = commandAt(source, nextFrom);
      const symbol = next ? not_remap[next.name] : undefined;
      if (next && symbol) {
        specs.push(sourceReplacement(
          source, offset, command.from, next.to, symbol,
          "cm-latex-conceal-unicode",
        ));
        at = next.to;
        continue;
      }
    }

    if (command.name === "left" || command.name === "right") {
      let nextFrom = command.to;
      while (nextFrom < source.length && /[ \t]/.test(source[nextFrom])) nextFrom++;
      const nextCommand = commandAt(source, nextFrom);
      const raw = nextCommand
        ? source.slice(nextCommand.from, nextCommand.to)
        : source[nextFrom] ?? "";
      const symbol = leftrightBrackets[raw] ??
        (nextCommand ? brackets[nextCommand.name] : undefined);
      if (symbol != null) {
        const to = nextCommand?.to ?? nextFrom + 1;
        specs.push(sourceReplacement(
          source, offset, command.from, to, symbol, "cm-latex-conceal-bracket",
        ));
        at = to;
        continue;
      }
    }

    if (command.name in brackets) {
      specs.push(sourceReplacement(
        source, offset, command.from, command.to, brackets[command.name],
        "cm-latex-conceal-bracket",
      ));
      at = command.to;
      continue;
    }

    if (operatorNames.has(command.name)) {
      const to = limitsEnd(source, command.to);
      specs.push(sourceReplacement(
        source, offset, command.from, to, command.name,
        "cm-latex-conceal-roman",
      ));
      at = to;
      continue;
    }

    if (command.name in allSymbols) {
      const to = limitsEnd(source, command.to);
      specs.push(sourceReplacement(
        source, offset, command.from, to, allSymbols[command.name],
        "cm-latex-conceal-unicode",
      ));
      at = to;
      continue;
    }

    at = command.to;
  }
  return specs;
}

function escapedAt(source: string, position: number): boolean {
  let slashes = 0;
  for (let at = position - 1; at >= 0 && source[at] === "\\"; at--) slashes++;
  return slashes % 2 === 1;
}

interface BracketPair {
  open: number;
  close: number;
  depth: number;
}

interface RelativeSyntaxRange {
  from: number;
  to: number;
  className: string;
  pairOpen?: number;
}

interface SyntaxFragment {
  ranges: RelativeSyntaxRange[];
  pairs: BracketPair[];
}

const syntaxFragmentCache = new Map<string, SyntaxFragment>();
const concealFragmentCache = new Map<string, LatexConcealSpec[]>();
const fragmentCacheLimit = 512;

function retainCacheEntry<T>(cache: Map<string, T>, key: string, value: T): T {
  cache.delete(key);
  cache.set(key, value);
  while (cache.size > fragmentCacheLimit)
    cache.delete(cache.keys().next().value!);
  return value;
}

function analyzeSyntaxFragment(source: string): SyntaxFragment {
  const cached = syntaxFragmentCache.get(source);
  if (cached) return retainCacheEntry(syntaxFragmentCache, source, cached);

  const bracketsByPosition = new Map<number, {
    className: string;
    pairOpen?: number;
  }>();
  const stack: Array<{ character: string; position: number; depth: number }> = [];
  const matched: BracketPair[] = [];
  const pairs: Record<string, string> = { "(": ")", "[": "]", "{": "}" };
  for (let at = 0; at < source.length; at++) {
    const character = source[at];
    if (escapedAt(source, at)) continue;
    if (character in pairs) {
      stack.push({ character, position: at, depth: stack.length });
      continue;
    }
    if (!["}", "]", ")"].includes(character)) continue;
    const opening = stack.at(-1);
    if (!opening || pairs[opening.character] !== character) {
      bracketsByPosition.set(at, {
        className: "cm-latex-bracket cm-latex-bracket-mismatch",
      });
      continue;
    }
    stack.pop();
    matched.push({ open: opening.position, close: at, depth: opening.depth });
  }
  for (const opening of stack)
    bracketsByPosition.set(opening.position, {
      className: "cm-latex-bracket cm-latex-bracket-mismatch",
    });
  for (const pair of matched) {
    const className =
      `cm-latex-bracket cm-latex-bracket-depth-${pair.depth % 3}`;
    bracketsByPosition.set(pair.open, { className, pairOpen: pair.open });
    bracketsByPosition.set(pair.close, { className, pairOpen: pair.open });
  }

  const ranges: RelativeSyntaxRange[] = [];
  const add = (from: number, to: number, className: string,
               pairOpen?: number) => {
    if (from < to) ranges.push({ from, to, className, pairOpen });
  };
  for (let at = 0; at < source.length;) {
    const bracket = bracketsByPosition.get(at);
    if (bracket) {
      add(at, at + 1, bracket.className, bracket.pairOpen);
      at++;
      continue;
    }
    if (source[at] === "%" && !escapedAt(source, at)) {
      const end = source.indexOf("\n", at);
      const to = end < 0 ? source.length : end;
      add(at, to, "cm-latex-comment");
      at = to;
      continue;
    }
    const command = commandAt(source, at);
    if (command) {
      add(command.from, command.to, "cm-latex-command");
      at = command.to;
      continue;
    }
    if (source[at] === "^" || source[at] === "_") {
      add(at, at + 1, "cm-latex-script-operator");
      at++;
      continue;
    }
    if (/\d/.test(source[at])) {
      let to = at + 1;
      while (to < source.length && /[\d.]/.test(source[to])) to++;
      add(at, to, "cm-latex-number");
      at = to;
      continue;
    }
    if (/[+\-=/<>|&]/.test(source[at]))
      add(at, at + 1, "cm-latex-operator");
    at++;
  }
  return retainCacheEntry(syntaxFragmentCache, source, { ranges, pairs: matched });
}

function activeBracketOpens(fragment: SyntaxFragment, offset: number,
                            state: EditorState): Set<number> {
  const active = new Set<number>();
  for (const selection of state.selection.ranges) {
    if (!selection.empty) {
      const enclosed = fragment.pairs.filter((pair) =>
        selection.from <= offset + pair.open &&
        selection.to >= offset + pair.close + 1);
      const closest = enclosed.sort((left, right) =>
        (right.open - left.open) || (left.close - right.close))[0];
      if (closest) active.add(closest.open);
      continue;
    }
    const position = selection.from - offset;
    const adjacent = fragment.pairs.find((pair) =>
      position === pair.open || position === pair.open + 1 ||
      position === pair.close || position === pair.close + 1);
    if (adjacent) {
      active.add(adjacent.open);
      continue;
    }
    const enclosing = fragment.pairs.filter((pair) =>
      position > pair.open && position < pair.close)
      .sort((left, right) =>
        (left.close - left.open) - (right.close - right.open))[0];
    if (enclosing) active.add(enclosing.open);
  }
  return active;
}

function syntaxRanges(view: EditorView): Range<Decoration>[] {
  return measurePerformance("latex/syntax-decorations", () =>
    syntaxRangesNow(view, markdownAnalysis(view.state)));
}

function syntaxRangesNow(view: EditorView,
                         analysis: MarkdownAnalysis): Range<Decoration>[] {
  const document = analysis.text;
  const ranges: Range<Decoration>[] = [];
  const mark = (from: number, to: number, className: string) => {
    if (from < to) ranges.push(Decoration.mark({ class: className }).range(from, to));
  };
  for (const node of analysis.math) {
    if (node.contentFrom == null || node.contentTo == null) continue;
    const openingEnd = Math.min(node.contentFrom, view.state.doc.lineAt(node.from).to);
    mark(node.from, openingEnd, "cm-latex-delimiter");
    mark(node.contentTo, node.to, "cm-latex-delimiter");
    const source = document.slice(node.contentFrom, node.contentTo);
    const fragment = analyzeSyntaxFragment(source);
    const active = activeBracketOpens(fragment, node.contentFrom, view.state);
    for (const range of fragment.ranges) {
      mark(node.contentFrom + range.from, node.contentFrom + range.to,
        range.className + (range.pairOpen != null && active.has(range.pairOpen)
          ? " cm-latex-bracket-active" : ""));
    }
  }
  return ranges;
}

class LatexConcealWidget extends WidgetType {
  constructor(readonly replacement: LatexReplacement) { super(); }

  eq(other: LatexConcealWidget): boolean {
    return other.replacement.text === this.replacement.text &&
      other.replacement.className === this.replacement.className &&
      other.replacement.elementType === this.replacement.elementType &&
      other.replacement.source === this.replacement.source;
  }

  toDOM(): HTMLElement {
    const element = document.createElement(this.replacement.elementType ?? "span");
    element.className = `cm-math cm-latex-conceal ${this.replacement.className ?? ""}`.trim();
    element.textContent = this.replacement.text;
    element.title = this.replacement.source;
    return element;
  }

  ignoreEvent(): boolean { return false; }
}

function selectionTouchesSpec(view: EditorView, spec: LatexConcealSpec): boolean {
  return view.state.selection.ranges.some((selection) => spec.some((replacement) => {
    if (selection.empty)
      return selection.from >= replacement.from && selection.from <= replacement.to;
    return selection.from <= replacement.to && selection.to >= replacement.from;
  }));
}

function concealedFragment(source: string, offset: number): LatexConcealSpec[] {
  let relative = concealFragmentCache.get(source);
  if (relative) {
    retainCacheEntry(concealFragmentCache, source, relative);
  } else {
    relative = retainCacheEntry(
      concealFragmentCache, source, concealLatex(source),
    );
  }
  return relative.map((spec) => spec.map((replacement) => ({
    ...replacement,
    from: replacement.from + offset,
    to: replacement.to + offset,
  })));
}

class AtomicRange extends RangeValue {}
const atomicRange = new AtomicRange();

function concealPresentation(view: EditorView): {
  decorations: DecorationSet;
  atomicRanges: RangeSet<RangeValue>;
} {
  return measurePerformance("latex/conceal-decorations", () => {
    const analysis = markdownAnalysis(view.state);
    const specs = analysis.math.flatMap((node) =>
      node.contentFrom == null || node.contentTo == null ? [] :
        concealedFragment(
          analysis.text.slice(node.contentFrom, node.contentTo),
          node.contentFrom,
        ));
    const enabled = specs.filter((spec) =>
      !selectionTouchesSpec(view, spec)).flat();
    const decorations = Decoration.set(enabled.map((replacement) =>
      replacement.from === replacement.to
        ? Decoration.widget({
          widget: new LatexConcealWidget(replacement),
          side: -1,
        }).range(replacement.from)
        : Decoration.replace({
          widget: new LatexConcealWidget(replacement),
          inclusive: false,
        }).range(replacement.from, replacement.to)), true);
    const builder = new RangeSetBuilder<RangeValue>();
    for (const replacement of enabled.sort((left, right) =>
      left.from - right.from)) {
      if (replacement.from < replacement.to)
        builder.add(replacement.from, replacement.to, atomicRange);
    }
    return { decorations, atomicRanges: builder.finish() };
  });
}

function changedMath(update: ViewUpdate): boolean {
  const previous = markdownAnalysis(update.startState);
  const current = markdownAnalysis(update.state);
  let touched = false;
  update.changes.iterChangedRanges((fromA, toA, fromB, toB) => {
    const intersects = (node: { from: number; to: number },
                        from: number, to: number) =>
      from === to
        ? from >= node.from && from <= node.to
        : from < node.to && to > node.from;
    touched ||= previous.math.some((node) => intersects(node, fromA, toA)) ||
      current.math.some((node) => intersects(node, fromB, toB));
  });
  return touched;
}

function selectionInMath(analysis: MarkdownAnalysis,
                         state: EditorState): boolean {
  return state.selection.ranges.some((range) => mathNodeAt(analysis, range));
}

const latexSyntaxPlugin = ViewPlugin.fromClass(class {
  decorations: DecorationSet;

  constructor(view: EditorView) {
    this.decorations = Decoration.set(syntaxRanges(view), true);
  }

  update(update: ViewUpdate): void {
    /* These ranges cover the document and do not depend on which lines are
     * currently visible. Rebuilding them on every kinetic-scroll viewport
     * update reparses the entire note and stalls WebKit's scroll animation. */
    if (update.docChanged && !changedMath(update) &&
        !selectionInMath(markdownAnalysis(update.startState), update.startState) &&
        !selectionInMath(markdownAnalysis(update.state), update.state)) {
      this.decorations = this.decorations.map(update.changes);
    } else if (update.docChanged || (update.selectionSet &&
        (selectionInMath(markdownAnalysis(update.startState), update.startState) ||
         selectionInMath(markdownAnalysis(update.state), update.state)))) {
      this.decorations = Decoration.set(syntaxRanges(update.view), true);
    }
  }
}, { decorations: (plugin) => plugin.decorations });

const latexConcealPlugin = ViewPlugin.fromClass(class {
  decorations: DecorationSet;
  atomicRanges: RangeSet<RangeValue>;

  constructor(view: EditorView) {
    ({ decorations: this.decorations, atomicRanges: this.atomicRanges } =
      concealPresentation(view));
  }

  update(update: ViewUpdate): void {
    if (update.docChanged && !changedMath(update) &&
        !selectionInMath(markdownAnalysis(update.startState), update.startState) &&
        !selectionInMath(markdownAnalysis(update.state), update.state)) {
      this.decorations = this.decorations.map(update.changes);
      this.atomicRanges = this.atomicRanges.map(update.changes);
    } else if (update.docChanged || (update.selectionSet &&
        (selectionInMath(markdownAnalysis(update.startState), update.startState) ||
         selectionInMath(markdownAnalysis(update.state), update.state)))) {
      ({ decorations: this.decorations, atomicRanges: this.atomicRanges } =
        concealPresentation(update.view));
    }
  }
}, {
  decorations: (plugin) => plugin.decorations,
  provide: (plugin) => EditorView.atomicRanges.of((view) =>
    view.plugin(plugin)?.atomicRanges ?? RangeSet.empty),
});

export function latexEnhancements(conceal: boolean): Extension {
  return [
    markdownAnalysisField,
    latexSyntaxPlugin,
    conceal ? latexConcealPlugin : [],
  ];
}
