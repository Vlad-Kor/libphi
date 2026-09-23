import {
  StateField,
  type ChangeDesc,
  type EditorState,
  type SelectionRange,
  type Transaction,
} from "@codemirror/state";
import { measurePerformance } from "../performance";
import { previewSourceRange } from "./source-edit";
import {
  parseMarkdownNodes,
  selectionTouches,
  type MarkdownNode,
} from "./parser";

export interface MarkdownAnalysis {
  readonly text: string;
  readonly nodes: readonly MarkdownNode[];
  readonly math: readonly MarkdownNode[];
  /** `mapped`: a proven plain-prose edit reused the previous nodes; `region`:
   * only a context-independent window was reparsed (see regionAnalysis). */
  readonly updateKind: "full" | "mapped" | "region";
  /** Whole lines of the new document whose nodes may differ from the previous
   * analysis. Nodes outside it are the previous nodes, mapped. */
  readonly changed: { readonly from: number; readonly to: number };
}

const positionMeta = new Set([
  "contentFrom",
  "definition",
  "markerFrom",
  "markerTo",
  "prefixFrom",
  "prefixTo",
  "taskFrom",
]);

function fullAnalysis(text: string): MarkdownAnalysis {
  return measurePerformance("markdown/analysis-full", () => {
    const nodes = parseMarkdownNodes(text);
    return {
      text,
      nodes,
      math: nodes.filter((node) =>
        node.kind === "math" || node.kind === "display-math"),
      updateKind: "full",
      changed: { from: 0, to: text.length },
    };
  });
}

function mapNode(node: MarkdownNode, changes: ChangeDesc): MarkdownNode {
  const meta = node.meta ? { ...node.meta } : undefined;
  if (meta) {
    for (const [name, value] of Object.entries(meta)) {
      if (typeof value === "number" && positionMeta.has(name))
        meta[name] = changes.mapPos(value, 1);
    }
  }
  return {
    ...node,
    from: changes.mapPos(node.from, 1),
    to: changes.mapPos(node.to, -1),
    contentFrom: node.contentFrom == null
      ? undefined : changes.mapPos(node.contentFrom, 1),
    contentTo: node.contentTo == null
      ? undefined : changes.mapPos(node.contentTo, -1),
    meta,
  };
}

const inlineStructure = /[#*_~`$<>\[\]!^|\\%]/;
const blockStructure = /^\s*(?:[-+*]|\d+[.)])\s/;
/* Horizontal rules and frontmatter delimiters consist of characters that are
 * not otherwise structural; typing `---` used to be mapped as plain prose. */
const lineStructure = /^\s*(?:-[ \t]*){3,}$|^[{}]\s*$/;

function canMapPlainEdit(previous: MarkdownAnalysis,
                         transaction: Transaction): boolean {
  const changes: Array<{
    fromA: number;
    toA: number;
    fromB: number;
    toB: number;
    inserted: string;
  }> = [];
  transaction.changes.iterChanges((fromA, toA, fromB, toB, inserted) => {
    changes.push({ fromA, toA, fromB, toB, inserted: inserted.toString() });
  });
  if (changes.length !== 1) return false;
  const resolved = changes[0];
  const removed = transaction.startState.sliceDoc(resolved.fromA, resolved.toA);
  if (removed.includes("\n") || resolved.inserted.includes("\n")) return false;

  const oldLine = transaction.startState.doc.lineAt(resolved.fromA);
  const newLine = transaction.newDoc.lineAt(resolved.fromB);
  if (inlineStructure.test(oldLine.text) || inlineStructure.test(newLine.text) ||
      blockStructure.test(oldLine.text) || blockStructure.test(newLine.text) ||
      lineStructure.test(oldLine.text) || lineStructure.test(newLine.text))
    return false;

  /* There must be no parsed construct on the edited line. This excludes
   * multiline callouts/tables/HTML as well as ordinary inline syntax. */
  return !previous.nodes.some((node) =>
    node.from <= oldLine.to && node.to >= oldLine.from);
}

/* ---------------------------------------------------------------------------
 * Region reparse.
 *
 * parseMarkdownNodes is a whole-document parser. Typing inside a code block,
 * a multiline equation, or any paragraph containing Markdown syntax used to
 * reparse the entire note on every key. A region reparse parses only a
 * window of whole lines and splices the result between the unchanged nodes.
 *
 * It is used only when the window is provably independent of its context:
 *  1. the old window parsed on its own reproduces exactly the old nodes inside
 *     it, and no old node crosses its boundaries;
 *  2. neither the old nor the new window contains a token whose meaning can
 *     reach past the window: display delimiters, fence lines, `%%` comments,
 *     raw HTML tags, footnote syntax (definitions are global), or frontmatter
 *     delimiters; and the edit itself inserts or removes no `<` or `>`;
 *  3. the window is bounded by empty lines (nothing continues across one), or
 *     is the complete source of a code/display block whose delimiter lines the
 *     edit does not touch.
 * Anything else falls back to one full analysis.
 * ------------------------------------------------------------------------- */

const REGION_LIMIT = 16_384;
const htmlTag = /<[A-Za-z/]/;
const fenceLine = /^ {0,3}(?:`{3,}|~{3,})/m;
const frontmatterOpener = /^(---|\{)\s*\n/;
const frontmatterCloser = /^(?:---|\})[ \t]*$/m;

function longRangeFree(window: string, container: boolean): boolean {
  if (htmlTag.test(window) || window.includes("%%") || window.includes("[^"))
    return false;
  /* A container's own delimiters are balanced inside it and verified by its
   * shape; a paragraph must not contain any. */
  return container || !(window.includes("$$") || window.includes("\\[") ||
    window.includes("\\]") || fenceLine.test(window));
}

function sameNodes(left: readonly MarkdownNode[], right: readonly MarkdownNode[],
                   offset: number): boolean {
  if (left.length !== right.length) return false;
  for (let index = 0; index < left.length; index++) {
    const a = left[index];
    const b = right[index];
    if (a.kind !== b.kind || a.from + offset !== b.from || a.to + offset !== b.to ||
        a.text !== b.text ||
        (a.contentFrom == null ? b.contentFrom != null : a.contentFrom + offset !== b.contentFrom) ||
        (a.contentTo == null ? b.contentTo != null : a.contentTo + offset !== b.contentTo))
      return false;
    const aMeta = a.meta ?? {};
    const bMeta = b.meta ?? {};
    const keys = Object.keys(aMeta);
    if (keys.length !== Object.keys(bMeta).length) return false;
    for (const key of keys) {
      const value = aMeta[key];
      const expected = typeof value === "number" && positionMeta.has(key)
        ? value + offset : value;
      if (bMeta[key] !== expected) return false;
    }
  }
  return true;
}

const positionMetaNames = [...positionMeta];

/** Move a node by `offset`. Positional metadata before `threshold` (a
 * footnote reference's definition elsewhere in the note) stays put. */
function shiftNode(node: MarkdownNode, offset: number,
                   threshold = -Infinity): MarkdownNode {
  if (!offset) return node;
  let meta = node.meta;
  if (meta) {
    for (const name of positionMetaNames) {
      const value = meta[name];
      if (typeof value !== "number" || value < threshold) continue;
      if (meta === node.meta) meta = { ...meta };
      meta[name] = value + offset;
    }
  }
  return {
    ...node,
    from: node.from + offset,
    to: node.to + offset,
    contentFrom: node.contentFrom == null ? undefined : node.contentFrom + offset,
    contentTo: node.contentTo == null ? undefined : node.contentTo + offset,
    meta,
  };
}

interface RegionWindow {
  from: number;
  oldTo: number;
  newTo: number;
  /** The container node whose complete source is the window, if any. */
  container?: MarkdownNode;
}

function changedSpan(transaction: Transaction) {
  let fromA = Infinity, toA = -Infinity, fromB = Infinity, toB = -Infinity;
  let delimiters = false;
  transaction.changes.iterChanges((a, b, c, d, inserted) => {
    fromA = Math.min(fromA, a); toA = Math.max(toA, b);
    fromB = Math.min(fromB, c); toB = Math.max(toB, d);
    if (/[<>]/.test(inserted.toString()) ||
        /[<>]/.test(transaction.startState.sliceDoc(a, b))) delimiters = true;
  });
  return { fromA, toA, fromB, toB, delimiters };
}

const containerKinds = new Set(["code-block", "mermaid", "display-math"]);

function containerWindow(previous: MarkdownAnalysis, transaction: Transaction,
                         span: ReturnType<typeof changedSpan>): RegionWindow | null {
  const doc = transaction.startState.doc;
  /* The innermost container around the edit (nodes are sorted by start). */
  let container: MarkdownNode | undefined;
  for (const node of previous.nodes) {
    if (node.from > span.fromA) break;
    if (containerKinds.has(node.kind) && node.to >= span.toA) container = node;
  }
  if (!container) return null;
  const first = doc.lineAt(container.from);
  const last = doc.lineAt(container.to);
  if (container.from !== first.from || container.to !== last.to ||
      first.number === last.number ||
      span.fromA <= first.to || span.toA >= last.from) return null;
  if (container.kind === "display-math") {
    const open = first.text.trim();
    const close = last.text.trim();
    if (!((open === "$$" && close === "$$") || (open === "\\[" && close === "\\]")))
      return null;
  }
  const delta = transaction.newDoc.length - doc.length;
  return { from: container.from, oldTo: container.to, newTo: container.to + delta, container };
}

function paragraphWindow(transaction: Transaction,
                         span: ReturnType<typeof changedSpan>): RegionWindow | null {
  const doc = transaction.newDoc;
  let first = doc.lineAt(span.fromB);
  while (first.number > 1) {
    const above = doc.line(first.number - 1);
    if (!above.length) break;
    first = above;
    if (span.fromB - first.from > REGION_LIMIT) return null;
  }
  let last = doc.lineAt(span.toB);
  while (last.number < doc.lines) {
    const below = doc.line(last.number + 1);
    if (!below.length) break;
    last = below;
    if (last.to - span.toB > REGION_LIMIT) return null;
  }
  const delta = doc.length - transaction.startState.doc.length;
  /* Everything outside [first.from, last.to] is unchanged text. */
  if (first.from > span.fromA || last.to - delta < span.toA) return null;
  return { from: first.from, oldTo: last.to - delta, newTo: last.to };
}

function regionAnalysis(previous: MarkdownAnalysis, transaction: Transaction,
                        text: string): MarkdownAnalysis | null {
  const span = changedSpan(transaction);
  if (span.delimiters || !Number.isFinite(span.fromA)) return null;
  const window = containerWindow(previous, transaction, span) ??
    paragraphWindow(transaction, span);
  if (!window || window.newTo - window.from > REGION_LIMIT) return null;
  const oldText = previous.text;
  const oldWindow = oldText.slice(window.from, window.oldTo);
  const newWindow = text.slice(window.from, window.newTo);
  const container = Boolean(window.container);
  if (!longRangeFree(oldWindow, container) || !longRangeFree(newWindow, container))
    return null;

  /* A frontmatter block that has not been closed yet could close inside the
   * window, and a window at the very start could open one. */
  const hasFrontmatter = previous.nodes[0]?.kind === "frontmatter";
  if ((window.from === 0 && (frontmatterOpener.test(oldWindow) ||
       frontmatterOpener.test(newWindow))) ||
      (!hasFrontmatter && frontmatterOpener.test(text) &&
       (frontmatterCloser.test(oldWindow) || frontmatterCloser.test(newWindow))))
    return null;

  const before: MarkdownNode[] = [];
  const inside: MarkdownNode[] = [];
  const after: MarkdownNode[] = [];
  for (const node of previous.nodes) {
    if (node.to <= window.from && node.from < window.from) before.push(node);
    else if (node.from >= window.oldTo && node.from > window.from) after.push(node);
    else if (node.from >= window.from && node.to <= window.oldTo) inside.push(node);
    else return null;
  }
  if (!sameNodes(parseMarkdownNodes(oldWindow), inside, window.from)) return null;

  const parsed = parseMarkdownNodes(newWindow);
  if (window.container) {
    const kind = window.container.kind;
    const shape = parsed.find((node) => node.from === 0 &&
      node.to === newWindow.length && node.kind === kind);
    if (!shape || (kind !== "display-math" &&
        shape.meta?.incomplete !== window.container.meta?.incomplete)) return null;
  }

  const changes = transaction.changes;
  const nodes = [
    /* Footnote references keep their definition's position in `meta`. */
    ...before.map((node) => typeof node.meta?.definition === "number" &&
      node.meta.definition >= window.from ? mapNode(node, changes) : node),
    ...parsed.map((node) => shiftNode(node, window.from)),
    /* Every change lies inside the window: later text moved uniformly. */
    ...after.map((node) => shiftNode(node, window.newTo - window.oldTo, window.oldTo)),
  ];
  return {
    text,
    nodes,
    math: nodes.filter((node) =>
      node.kind === "math" || node.kind === "display-math"),
    updateKind: "region",
    changed: { from: window.from, to: window.newTo },
  };
}

function updateAnalysis(previous: MarkdownAnalysis,
                        transaction: Transaction): MarkdownAnalysis {
  const text = transaction.newDoc.toString();
  if (!canMapPlainEdit(previous, transaction)) {
    return measurePerformance("markdown/analysis-region", () =>
      regionAnalysis(previous, transaction, text)) ?? fullAnalysis(text);
  }
  return measurePerformance("markdown/analysis-map", () => {
    const nodes = previous.nodes.map((node) => mapNode(node, transaction.changes));
    let changedFrom = transaction.newDoc.length;
    let changedTo = 0;
    transaction.changes.iterChangedRanges((_fromA, _toA, fromB, toB) => {
      changedFrom = Math.min(changedFrom, transaction.newDoc.lineAt(fromB).from);
      changedTo = Math.max(changedTo, transaction.newDoc.lineAt(toB).to);
    });
    return {
      text,
      nodes,
      math: nodes.filter((node) =>
        node.kind === "math" || node.kind === "display-math"),
      updateKind: "mapped",
      changed: { from: changedFrom, to: changedTo },
    };
  });
}

export const markdownAnalysisField = StateField.define<MarkdownAnalysis>({
  create: (state) => fullAnalysis(state.doc.toString()),
  update(previous, transaction) {
    return transaction.docChanged
      ? updateAnalysis(previous, transaction)
      : previous;
  },
});

export function markdownAnalysis(state: EditorState): MarkdownAnalysis {
  return state.field(markdownAnalysisField);
}

export function mathNodeAt(
  analysis: MarkdownAnalysis,
  range: Pick<SelectionRange, "from" | "to">,
): MarkdownNode | undefined {
  /* Math nodes are sorted and non-overlapping. Binary-search the last node
   * starting at or before the selection, then check its actual interval. */
  let low = 0;
  let high = analysis.math.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (analysis.math[middle].from <= range.from) low = middle + 1;
    else high = middle;
  }
  const candidate = analysis.math[Math.max(0, low - 1)];
  if (candidate && (selectionTouches(candidate, range) ||
      (range.from === range.to && range.from === candidate.to))) return candidate;
  return analysis.math.slice(low).find((node) =>
    node.from < range.to && selectionTouches(node, range));
}

export interface Region { from: number; to: number }

/** Grow each range to whole lines and whole top-level node groups, then
 * merge overlapping regions. Nodes are sorted by start. */
function dirtyRegions(state: EditorState, nodes: readonly MarkdownNode[],
                      ranges: readonly Region[]): Region[] {
  const regions: Region[] = [];
  for (const range of ranges) {
    let from = state.doc.lineAt(range.from).from;
    let to = state.doc.lineAt(range.to).to;
    for (let grown = true; grown;) {
      grown = false;
      for (const node of nodes) {
        if (node.from > to) break;
        if (node.to < from || (node.from >= from && node.to <= to)) continue;
        from = Math.min(from, state.doc.lineAt(node.from).from);
        to = Math.max(to, state.doc.lineAt(node.to).to);
        grown = true;
      }
    }
    regions.push({ from, to });
  }
  regions.sort((a, b) => a.from - b.from);
  const merged: Region[] = [];
  for (const region of regions) {
    const last = merged.at(-1);
    if (last && region.from <= last.to + 1) last.to = Math.max(last.to, region.to);
    else merged.push({ ...region });
  }
  return merged;
}

/** Regions of the new document whose preview state may differ after this
 * transaction, or "all". Whether a node is revealed depends only on the
 * selection and pinned source that touch it, and everything derived from a
 * node lies within its own lines. A document edit therefore affects the lines
 * the analysis reparsed, and a selection change the node groups touching the
 * old or new selection or pinned range. Consumers map their previous value
 * and recompute only these regions. */
const dirtyRegionCache = new WeakMap<Transaction, Region[] | "all">();

export function previewDirtyRegions(transaction: Transaction): Region[] | "all" {
  let regions = dirtyRegionCache.get(transaction);
  if (!regions) {
    regions = computeDirtyRegions(transaction);
    dirtyRegionCache.set(transaction, regions);
  }
  return regions;
}

function computeDirtyRegions(transaction: Transaction): Region[] | "all" {
  const state = transaction.state;
  const analysis = markdownAnalysis(state);
  if (transaction.docChanged && analysis.updateKind === "full") return "all";
  const changes = transaction.changes;
  const ranges: Region[] = [];
  if (transaction.docChanged) ranges.push(analysis.changed);
  const oldPin = transaction.startState.field(previewSourceRange, false);
  const newPin = state.field(previewSourceRange, false);
  if (transaction.selection || transaction.docChanged || oldPin !== newPin) {
    for (const range of transaction.startState.selection.ranges)
      ranges.push({ from: changes.mapPos(range.from, -1), to: changes.mapPos(range.to, 1) });
    for (const range of state.selection.ranges) ranges.push(range);
    if (oldPin) ranges.push({ from: changes.mapPos(oldPin.from, -1), to: changes.mapPos(oldPin.to, 1) });
    if (newPin) ranges.push(newPin);
  }
  if (!ranges.length) return [];
  const regions = dirtyRegions(state, analysis.nodes, ranges);
  const covered = regions.reduce((sum, region) => sum + region.to - region.from, 0);
  return covered > state.doc.length / 2 ? "all" : regions;
}

/** Nodes starting in one of the regions (which contain them completely). */
export function nodesInRegions(nodes: readonly MarkdownNode[],
                               regions: readonly Region[]): MarkdownNode[] {
  return nodes.filter((node) => regions.some((region) =>
    node.from >= region.from && node.from <= region.to));
}
