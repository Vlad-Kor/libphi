import { type EditorState, type Range, StateEffect, StateField } from "@codemirror/state";
import { Decoration, type DecorationSet, EditorView, showTooltip, type Tooltip } from "@codemirror/view";
import { parseMarkdownNodes, selectionTouches, type MarkdownNode } from "./parser";
import {
  CalloutWidget,
  BulletWidget,
  FootnoteWidget,
  HiddenWidget,
  HorizontalRuleWidget,
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
import { rawHtmlIsBlock } from "./render";

const hidden = Decoration.replace({ widget: new HiddenWidget() });
const emphasis = Decoration.mark({ class: "cm-live-emphasis" });
const strong = Decoration.mark({ class: "cm-live-strong" });
const strike = Decoration.mark({ class: "cm-live-strike" });
const highlight = Decoration.mark({ class: "cm-live-highlight" });
const tag = Decoration.mark({ class: "cm-live-tag" });
const blockId = Decoration.mark({ class: "cm-live-block-id" });
const inlineCode = Decoration.mark({ class: "cm-live-inline-code" });
const htmlTag = Decoration.mark({ class: "cm-html-tag" });
const htmlAttribute = Decoration.mark({ class: "cm-html-attribute" });
const htmlValue = Decoration.mark({ class: "cm-html-value" });
const htmlPunctuation = Decoration.mark({ class: "cm-html-punctuation" });
export const refreshLivePreview = StateEffect.define<null>();

function active(node: MarkdownNode, state: EditorState): boolean {
  return state.selection.ranges.some((selection) => {
    if (node.kind === "task") {
      const from = Number(node.meta?.prefixFrom ?? node.from);
      const to = Number(node.meta?.prefixTo ?? node.to);
      return selection.from === selection.to
        ? selection.from >= from && selection.from < to
        : selection.from < to && selection.to > from;
    }
    /* A click in the empty area after the final callout line maps to node.to.
     * Keep that endpoint editable; the next document line starts at to + 1. */
    if ((node.kind === "callout" || node.kind === "code-block") &&
        selection.from === selection.to)
      return selection.from >= node.from && selection.from <= node.to;
    return selectionTouches(node, selection);
  });
}

function imageIsInsideListItem(state: EditorState, node: MarkdownNode): boolean {
  const line = state.doc.lineAt(node.from);
  const prefix = state.sliceDoc(line.from, node.from);
  return /^\s*[-+*]\s+(?:\[[^\]]\]\s+)?$/.test(prefix);
}

function targetIsImage(target: string): boolean {
  return /\.(?:png|jpe?g|gif|webp|svg|avif)(?:$|[?#])/i.test(
    target.split("|")[0],
  );
}

function blockReplacement(node: MarkdownNode, state: EditorState): Decoration | undefined {
  switch (node.kind) {
    case "frontmatter": return Decoration.replace({ widget: new PropertiesWidget(node.text, node.from), block: true });
    case "horizontal-rule": return Decoration.replace({ widget: new HorizontalRuleWidget(), block: true });
    case "math": return Decoration.replace({ widget: new MathWidget(node.text, false, node.from) });
    case "display-math": return Decoration.replace({ widget: new MathWidget(node.text, true, node.from), block: true });
    case "wikilink": return Decoration.replace({ widget: new LinkWidget(String(node.meta?.target ?? node.text), String(node.meta?.alias ?? node.text), node.from, node.to) });
    case "embed": {
      const target = String(node.meta?.target ?? node.text);
      const standalone = !imageIsInsideListItem(state, node);
      return Decoration.replace({
        widget: new LinkWidget(
          target,
          String(node.meta?.alias ?? node.text),
          node.from,
          node.to,
          true,
          standalone,
        ),
        block: standalone && !targetIsImage(target),
      });
    }
    case "markdown-link": return Decoration.replace({ widget: new MarkdownLinkWidget(String(node.meta?.target ?? ""), String(node.meta?.alias ?? ""), node.from, false) });
    case "markdown-image": {
      const standalone = !imageIsInsideListItem(state, node);
      return Decoration.replace({
        widget: new MarkdownLinkWidget(
          String(node.meta?.target ?? ""),
          String(node.meta?.alias ?? ""),
          node.from,
          true,
          node.to,
          standalone,
        ),
        block: false,
      });
    }
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
    case "code-block": return Decoration.replace({ widget: new HtmlPreviewWidget(node.text, node.from, "code-block-widget"), block: true });
    case "html": return Decoration.replace({ widget: new RawHtmlWidget(node.text, node.from), block: rawHtmlIsBlock(node.text) });
    default: return undefined;
  }
}

interface DecorationSink {
  add(from: number, to: number, decoration: Decoration): void;
}

function addDelimited(
  builder: DecorationSink,
  node: MarkdownNode,
  mark: Decoration,
): void {
  if (node.contentFrom == null || node.contentTo == null) return;
  if (node.from < node.contentFrom) builder.add(node.from, node.contentFrom, hidden);
  builder.add(node.contentFrom, node.contentTo, mark);
  if (node.contentTo < node.to) builder.add(node.contentTo, node.to, hidden);
}

function addHtmlSyntax(builder: DecorationSink, node: MarkdownNode): void {
  for (const tagMatch of node.text.matchAll(/<\/?([A-Za-z][\w:-]*)([^<>]*)>/g)) {
    const tagFrom = node.from + tagMatch.index;
    const nameAt = tagMatch[0].indexOf(tagMatch[1]);
    builder.add(tagFrom, tagFrom + nameAt, htmlPunctuation);
    builder.add(tagFrom + nameAt, tagFrom + nameAt + tagMatch[1].length,
      htmlTag);
    builder.add(tagFrom + tagMatch[0].length - 1,
      tagFrom + tagMatch[0].length, htmlPunctuation);

    const attributes = tagMatch[2];
    const attributesFrom = tagFrom + nameAt + tagMatch[1].length;
    const pattern = /([:@A-Za-z_][\w:.-]*)(\s*=\s*)(?:"[^"]*"|'[^']*'|[^\s"'=<>`]+)/g;
    for (const attribute of attributes.matchAll(pattern)) {
      const from = attributesFrom + attribute.index;
      builder.add(from, from + attribute[1].length, htmlAttribute);
      builder.add(from + attribute[1].length,
        from + attribute[1].length + attribute[2].length, htmlPunctuation);
      builder.add(from + attribute[1].length + attribute[2].length,
        from + attribute[0].length, htmlValue);
    }
  }
}

function buildDecorations(state: EditorState): DecorationSet {
  const nodes = parseMarkdownNodes(state.doc.toString());
  const ranges: Range<Decoration>[] = [];
  const builder: DecorationSink = {
    add(from, to, decoration) {
      ranges.push(decoration.range(from, to));
    },
  };
  let coveredUntil = -1;
  for (const node of nodes) {
    if (node.from < coveredUntil) continue;
    const isActive = active(node, state);
    if (!isActive) {
      const replacement = blockReplacement(node, state);
      if (replacement) {
        let to = node.to;
        if (replacement.spec.block) {
          const lastLine = state.doc.lineAt(node.to);
          if (node.to === lastLine.to && node.to < state.doc.length)
            to++;
        }
        builder.add(node.from, to, replacement);
        coveredUntil = to;
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
      case "list-item": {
        const indentColumns = Number(node.meta?.indentColumns ?? 0);
        const marker = String(node.meta?.marker ?? node.text);
        const ordered = Boolean(node.meta?.ordered);
        const markerIndent = ordered
          ? 0.62 * marker.length + 0.35
          : 1.1;
        const contentIndent = node.meta?.task
          ? `calc(${indentColumns ? `${indentColumns * 0.25}em + ` : ""}18px + 0.4em)`
          : `${Number((indentColumns * 0.25 + markerIndent).toFixed(2))}em`;
        builder.add(node.from, node.from, Decoration.line({
          attributes: {
            class: `cm-live-list-item${ordered ? " cm-live-ordered-list-item" : ""}`,
            style: `--phi-list-content-indent:${contentIndent}`,
          },
        }));
        if (!isActive) {
          const markerFrom = Number(node.meta?.markerFrom ?? node.from);
          if (node.meta?.task) {
            builder.add(markerFrom, Number(node.meta?.markerTo ?? markerFrom + 1),
              hidden);
          } else {
            builder.add(node.from, Number(node.meta?.contentFrom ?? markerFrom + marker.length),
              Decoration.replace({
                widget: new BulletWidget(ordered ? marker : "•", ordered),
              }));
          }
        } else if (ordered) {
          builder.add(Number(node.meta?.markerFrom ?? node.from),
            Number(node.meta?.markerTo ?? node.from + marker.length),
            Decoration.mark({ class: "cm-live-list-number" }));
        }
        if (node.meta?.task) {
          const prefixTo = Number(node.meta?.contentFrom ?? node.to);
          const prefixActive = state.selection.ranges.some((selection) =>
            selection.from === selection.to
              ? selection.from >= node.from && selection.from < prefixTo
              : selection.from < prefixTo && selection.to > node.from);
          if (isActive && !prefixActive) {
            builder.add(Number(node.meta?.markerFrom ?? node.from),
              Number(node.meta?.markerTo ?? node.from + marker.length),
              hidden);
          }
        }
        break;
      }
      case "emphasis": if (!isActive) addDelimited(builder, node, emphasis); break;
      case "strong": if (!isActive) addDelimited(builder, node, strong); break;
      case "strike": if (!isActive) addDelimited(builder, node, strike); break;
      case "highlight": if (!isActive) addDelimited(builder, node, highlight); break;
      case "inline-code": if (!isActive) addDelimited(builder, node, inlineCode); break;
      case "comment": if (!isActive) builder.add(node.from, node.to, hidden); break;
      case "tag": builder.add(node.from, node.to, tag); break;
      case "block-id": builder.add(node.from, node.to, blockId); break;
      case "html": if (isActive) addHtmlSyntax(builder, node); break;
    }
  }
  return Decoration.set(ranges, true);
}

function buildMathTooltips(state: EditorState): readonly Tooltip[] {
  const nodes = parseMarkdownNodes(state.doc.toString()).filter(
    (node) => node.kind === "math" || node.kind === "display-math",
  );
  const seen = new Set<number>();
  const tooltips: Tooltip[] = [];
  for (const selection of state.selection.ranges) {
    const node = nodes.find((candidate) => selectionTouches(candidate, selection));
    if (!node || seen.has(node.from)) continue;
    seen.add(node.from);
    tooltips.push({
      pos: node.from,
      end: node.to,
      above: true,
      strictSide: true,
      create(view) {
        const dom = document.createElement("div");
        dom.className = "math-preview-bubble";
        dom.append(new MathWidget(
          node.text,
          node.kind === "display-math",
          node.from,
          undefined,
          true,
        ).toDOM(view));
        return { dom };
      },
    });
  }
  return tooltips;
}

export const livePreview = StateField.define<DecorationSet>({
  create: buildDecorations,
  update(value, transaction) {
    if (transaction.docChanged || transaction.selection ||
        transaction.effects.some((effect) => effect.is(refreshLivePreview)))
      return buildDecorations(transaction.state);
    return value;
  },
  provide: (field) => [
    EditorView.decorations.from(field),
    showTooltip.computeN([field], (state) => buildMathTooltips(state)),
  ],
});
