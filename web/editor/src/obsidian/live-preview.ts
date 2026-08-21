import { type EditorState, RangeSetBuilder, StateEffect, StateField } from "@codemirror/state";
import { Decoration, type DecorationSet, EditorView } from "@codemirror/view";
import { parseObsidian, selectionTouches, type ObsidianNode } from "./parser";
import {
  CalloutWidget,
  FootnoteWidget,
  HiddenWidget,
  HtmlPreviewWidget,
  LinkWidget,
  MarkdownLinkWidget,
  MathWidget,
  MermaidWidget,
  PropertiesWidget,
  RawHtmlWidget,
  TagWidget,
  TaskWidget,
} from "../widgets/preview";

const hidden = Decoration.replace({ widget: new HiddenWidget() });
const emphasis = Decoration.mark({ class: "cm-live-emphasis" });
const strong = Decoration.mark({ class: "cm-live-strong" });
const strike = Decoration.mark({ class: "cm-live-strike" });
const highlight = Decoration.mark({ class: "cm-live-highlight" });
const tag = Decoration.mark({ class: "cm-live-tag" });
const blockId = Decoration.mark({ class: "cm-live-block-id" });
export const refreshLivePreview = StateEffect.define<null>();

function active(node: ObsidianNode, state: EditorState): boolean {
  return state.selection.ranges.some((selection) => selectionTouches(node, selection));
}

function blockReplacement(node: ObsidianNode): Decoration | undefined {
  switch (node.kind) {
    case "frontmatter": return Decoration.replace({ widget: new PropertiesWidget(node.text, node.from), block: true });
    case "math": return Decoration.replace({ widget: new MathWidget(node.text, false, node.from) });
    case "display-math": return Decoration.replace({ widget: new MathWidget(node.text, true, node.from), block: true });
    case "wikilink": return Decoration.replace({ widget: new LinkWidget(String(node.meta?.target ?? node.text), String(node.meta?.alias ?? node.text), node.from) });
    case "embed": return Decoration.replace({ widget: new LinkWidget(String(node.meta?.target ?? node.text), String(node.meta?.alias ?? node.text), node.from, true), block: true });
    case "markdown-link": return Decoration.replace({ widget: new MarkdownLinkWidget(String(node.meta?.target ?? ""), String(node.meta?.alias ?? ""), node.from, false) });
    case "markdown-image": return Decoration.replace({ widget: new MarkdownLinkWidget(String(node.meta?.target ?? ""), String(node.meta?.alias ?? ""), node.from, true), block: true });
    case "footnote-reference": return Decoration.replace({ widget: new FootnoteWidget(String(node.meta?.id ?? node.text), node.from, false, Number(node.meta?.definition ?? node.from)) });
    case "inline-footnote": return Decoration.replace({ widget: new FootnoteWidget(node.text, node.from) });
    case "footnote-definition": return Decoration.replace({ widget: new FootnoteWidget(node.text, node.from, true, node.from, String(node.meta?.id ?? "")), block: true });
    case "task": return Decoration.replace({ widget: new TaskWidget(String(node.meta?.status ?? " "), node.from) });
    case "tag": return Decoration.replace({ widget: new TagWidget(node.text, node.from) });
    case "callout": return Decoration.replace({
      widget: new CalloutWidget(node.text, String(node.meta?.type ?? "note"), String(node.meta?.title ?? ""), String(node.meta?.fold ?? ""), node.from),
      block: true,
    });
    case "table": return Decoration.replace({ widget: new HtmlPreviewWidget(node.text, node.from, "table-widget"), block: true });
    case "mermaid": return Decoration.replace({ widget: new MermaidWidget(node.text, node.from), block: true });
    case "html": return Decoration.replace({ widget: new RawHtmlWidget(node.text, node.from), block: node.text.includes("\n") });
    default: return undefined;
  }
}

function addDelimited(
  builder: RangeSetBuilder<Decoration>,
  node: ObsidianNode,
  mark: Decoration,
): void {
  if (node.contentFrom == null || node.contentTo == null) return;
  if (node.from < node.contentFrom) builder.add(node.from, node.contentFrom, hidden);
  builder.add(node.contentFrom, node.contentTo, mark);
  if (node.contentTo < node.to) builder.add(node.contentTo, node.to, hidden);
}

function buildDecorations(state: EditorState): DecorationSet {
  const nodes = parseObsidian(state.doc.toString());
  const builder = new RangeSetBuilder<Decoration>();
  let coveredUntil = -1;
  for (const node of nodes) {
    if (node.from < coveredUntil) continue;
    const isActive = active(node, state);
    if (!isActive) {
      const replacement = blockReplacement(node);
      if (replacement) {
        builder.add(node.from, node.to, replacement);
        coveredUntil = node.to;
        continue;
      }
    }
    switch (node.kind) {
      case "heading": {
        const level = Number(node.meta?.level ?? 1);
        builder.add(node.from, node.from, Decoration.line({ class: `cm-live-heading cm-live-heading-${level}` }));
        if (!isActive && node.contentFrom != null) builder.add(node.from, node.contentFrom, hidden);
        break;
      }
      case "emphasis": if (!isActive) addDelimited(builder, node, emphasis); break;
      case "strong": if (!isActive) addDelimited(builder, node, strong); break;
      case "strike": if (!isActive) addDelimited(builder, node, strike); break;
      case "highlight": if (!isActive) addDelimited(builder, node, highlight); break;
      case "comment": if (!isActive) builder.add(node.from, node.to, hidden); break;
      case "tag": builder.add(node.from, node.to, tag); break;
      case "block-id": builder.add(node.from, node.to, blockId); break;
    }
  }
  return builder.finish();
}

export const livePreview = StateField.define<DecorationSet>({
  create: buildDecorations,
  update(value, transaction) {
    if (transaction.docChanged || transaction.selection ||
        transaction.effects.some((effect) => effect.is(refreshLivePreview)))
      return buildDecorations(transaction.state);
    return value;
  },
  provide: (field) => EditorView.decorations.from(field),
});
