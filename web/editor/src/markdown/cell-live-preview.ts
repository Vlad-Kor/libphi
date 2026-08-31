import { type EditorState, type Range, StateField } from "@codemirror/state";
import {
  Decoration,
  type DecorationSet,
  EditorView,
  WidgetType,
} from "@codemirror/view";
import { renderMath } from "../math/mathjax";
import { markdownAnalysis, markdownAnalysisField } from "./analysis";
import { selectionTouches, type MarkdownNode } from "./parser";

const hidden = Decoration.replace({});
const emphasis = Decoration.mark({ class: "cm-live-emphasis" });
const strong = Decoration.mark({ class: "cm-live-strong" });
const strike = Decoration.mark({ class: "cm-live-strike" });
const highlight = Decoration.mark({ class: "cm-live-highlight" });
const inlineCode = Decoration.mark({ class: "cm-live-inline-code" });

class CellMathWidget extends WidgetType {
  constructor(readonly latex: string) { super(); }

  eq(other: CellMathWidget): boolean {
    return other.latex === this.latex;
  }

  toDOM(): HTMLElement {
    const element = document.createElement("span");
    element.className = "math-widget math-inline";
    element.setAttribute("aria-label", `LaTeX: ${this.latex}`);
    void renderMath(this.latex, false, element);
    return element;
  }

  ignoreEvent(): boolean { return true; }
}

function active(node: MarkdownNode, state: EditorState): boolean {
  return state.selection.ranges.some((selection) =>
    selectionTouches(node, selection));
}

function addDelimited(
  ranges: Range<Decoration>[],
  node: MarkdownNode,
  mark: Decoration,
): void {
  if (node.contentFrom == null || node.contentTo == null) return;
  if (node.from < node.contentFrom)
    ranges.push(hidden.range(node.from, node.contentFrom));
  if (node.contentFrom < node.contentTo)
    ranges.push(mark.range(node.contentFrom, node.contentTo));
  if (node.contentTo < node.to)
    ranges.push(hidden.range(node.contentTo, node.to));
}

function buildCellDecorations(state: EditorState): DecorationSet {
  const ranges: Range<Decoration>[] = [];
  let coveredUntil = -1;
  for (const node of markdownAnalysis(state).nodes) {
    if (node.from < coveredUntil) continue;
    const isActive = active(node, state);
    if (node.kind === "math" && !isActive) {
      ranges.push(Decoration.replace({
        widget: new CellMathWidget(node.text),
      }).range(node.from, node.to));
      coveredUntil = node.to;
      continue;
    }
    if (isActive) continue;
    switch (node.kind) {
      case "emphasis": addDelimited(ranges, node, emphasis); break;
      case "strong": addDelimited(ranges, node, strong); break;
      case "strike": addDelimited(ranges, node, strike); break;
      case "highlight": addDelimited(ranges, node, highlight); break;
      case "inline-code": addDelimited(ranges, node, inlineCode); break;
    }
  }
  return Decoration.set(ranges, true);
}

const cellLivePreviewDecorations = StateField.define<DecorationSet>({
  create: buildCellDecorations,
  update(value, transaction) {
    if (transaction.docChanged || transaction.selection)
      return buildCellDecorations(transaction.state);
    return value;
  },
  provide: (field) => EditorView.decorations.from(field),
});

export const cellLivePreview = [
  markdownAnalysisField,
  cellLivePreviewDecorations,
];
