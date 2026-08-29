import {
  StateField,
  type ChangeDesc,
  type EditorState,
  type SelectionRange,
  type Transaction,
} from "@codemirror/state";
import { measurePerformance } from "../performance";
import {
  parseMarkdownNodes,
  selectionTouches,
  type MarkdownNode,
} from "./parser";

export interface MarkdownAnalysis {
  readonly text: string;
  readonly nodes: readonly MarkdownNode[];
  readonly math: readonly MarkdownNode[];
  /** `mapped` means a proven plain-prose edit reused the previous analysis. */
  readonly updateKind: "full" | "mapped";
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
      blockStructure.test(oldLine.text) || blockStructure.test(newLine.text))
    return false;

  /* There must be no parsed construct on the edited line. This excludes
   * multiline callouts/tables/HTML as well as ordinary inline syntax. */
  return !previous.nodes.some((node) =>
    node.from <= oldLine.to && node.to >= oldLine.from);
}

function updateAnalysis(previous: MarkdownAnalysis,
                        transaction: Transaction): MarkdownAnalysis {
  const text = transaction.newDoc.toString();
  if (!canMapPlainEdit(previous, transaction)) return fullAnalysis(text);
  return measurePerformance("markdown/analysis-map", () => {
    const nodes = previous.nodes.map((node) => mapNode(node, transaction.changes));
    return {
      text,
      nodes,
      math: nodes.filter((node) =>
        node.kind === "math" || node.kind === "display-math"),
      updateKind: "mapped",
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
  if (candidate && selectionTouches(candidate, range)) return candidate;
  return analysis.math.slice(low).find((node) =>
    node.from < range.to && selectionTouches(node, range));
}
