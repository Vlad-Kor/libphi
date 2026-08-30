import {
  type EditorState,
  type Range,
  StateEffect,
  StateField,
  type Transaction,
} from "@codemirror/state";
import { Decoration, type DecorationSet, EditorView, showTooltip, type Tooltip } from "@codemirror/view";
import type { MarkdownNode } from "./parser";
import {
  CalloutWidget,
  BulletWidget,
  EmptyInlineCodeWidget,
  FootnoteWidget,
  HiddenWidget,
  HorizontalRuleWidget,
  HtmlPreviewWidget,
  LineHeightEstimateWidget,
  LinkWidget,
  MarkdownLinkWidget,
  MathWidget,
  MermaidWidget,
  PropertiesWidget,
  RawHtmlWidget,
  TagWidget,
  TaskWidget,
  estimatedHeadingHeight,
  estimatedListLineHeight,
} from "../widgets/preview";
import { rawHtmlIsBlock } from "./render";
import { RichTableWidget } from "../widgets/table";
import { pinPreviewSource, previewSourceRange } from "./source-edit";
import { previewInteraction, previewNodeIsActive } from "./preview-interaction";
import {
  markdownAnalysis,
  markdownAnalysisField,
  mathNodeAt,
  type MarkdownAnalysis,
} from "./analysis";
import { measurePerformance } from "../performance";

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
  return previewNodeIsActive(node, state);
}

function imageIsInsideListItem(state: EditorState, node: MarkdownNode): boolean {
  const line = state.doc.lineAt(node.from);
  const prefix = state.sliceDoc(line.from, node.from);
  return /^\s*[-+*]\s+(?:\[[^\]]\]\s+)?$/.test(prefix);
}

function imageIsStandaloneLine(state: EditorState, node: MarkdownNode): boolean {
  if (imageIsInsideListItem(state, node)) return false;
  const line = state.doc.lineAt(node.from);
  return state.sliceDoc(line.from, node.from).trim() === "" &&
    state.sliceDoc(node.to, line.to).trim() === "";
}

function targetIsImage(target: string): boolean {
  return /\.(?:png|jpe?g|gif|webp|svg|avif)(?:$|[?#])/i.test(
    target.split("|")[0],
  );
}

function blockReplacement(node: MarkdownNode, state: EditorState): Decoration | undefined {
  switch (node.kind) {
    case "frontmatter": return Decoration.replace({ widget: new PropertiesWidget(node.text, node.from, node.to), block: true });
    case "horizontal-rule": return Decoration.replace({ widget: new HorizontalRuleWidget(node.from, node.to), block: true });
    case "math": return Decoration.replace({ widget: new MathWidget(node.text, false, node.from) });
    case "display-math": return Decoration.replace({ widget: new MathWidget(node.text, true, node.from), block: true });
    case "wikilink": return Decoration.replace({ widget: new LinkWidget(String(node.meta?.target ?? node.text), String(node.meta?.alias ?? node.text), node.from, node.to) });
    case "embed": {
      const target = String(node.meta?.target ?? node.text);
      const standalone = targetIsImage(target)
        ? imageIsStandaloneLine(state, node)
        : !imageIsInsideListItem(state, node);
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
    case "markdown-link": return Decoration.replace({ widget: new MarkdownLinkWidget(String(node.meta?.target ?? ""), String(node.meta?.alias ?? ""), node.from, false, node.to) });
    case "markdown-image": {
      const standalone = imageIsStandaloneLine(state, node);
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
    case "blockquote": return Decoration.replace({
      widget: new HtmlPreviewWidget(node.text, node.from, "blockquote-widget"),
      block: true,
    });
    case "table": return Decoration.replace({
      widget: new RichTableWidget(node.text, node.from, node.to),
      block: true,
    });
    case "mermaid": return Decoration.replace({ widget: new MermaidWidget(node.text, node.from), block: true });
    case "code-block": return Decoration.replace({ widget: new HtmlPreviewWidget(node.text, node.from, "code-block-widget"), block: true });
    case "html": return Decoration.replace({ widget: new RawHtmlWidget(node.text, node.from, node.to), block: rawHtmlIsBlock(node.text) });
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
  /* CodeMirror mark decorations may not be empty. Incomplete authoring
   * delimiters are valid editor input, so simply omit their content mark. */
  if (node.contentFrom < node.contentTo)
    builder.add(node.contentFrom, node.contentTo, mark);
  if (node.contentTo < node.to) builder.add(node.contentTo, node.to, hidden);
}

function addInlineCode(
  builder: DecorationSink,
  node: MarkdownNode,
  isActive: boolean,
): void {
  if (isActive) {
    builder.add(node.from, node.to, inlineCode);
    return;
  }
  if (node.contentFrom === node.contentTo) {
    const sourcePosition = node.meta?.incomplete
      ? node.to
      : Number(node.contentFrom ?? node.from);
    builder.add(node.from, node.to, Decoration.replace({
      widget: new EmptyInlineCodeWidget(sourcePosition),
    }));
    return;
  }
  addDelimited(builder, node, inlineCode);
}

function addCodeBlockSource(
  builder: DecorationSink,
  state: EditorState,
  node: MarkdownNode,
): void {
  const first = state.doc.lineAt(node.from);
  const last = state.doc.lineAt(node.to);
  for (let number = first.number; number <= last.number; number++) {
    const classes = ["cm-live-code-block-source"];
    if (number === first.number) classes.push("cm-live-code-block-source-first");
    if (number === last.number) classes.push("cm-live-code-block-source-last");
    const line = state.doc.line(number);
    builder.add(line.from, line.from, Decoration.line({
      class: classes.join(" "),
    }));
  }
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
  return measurePerformance("markdown/preview-decorations", () =>
    buildDecorationsNow(state, markdownAnalysis(state)));
}

function buildDecorationsNow(state: EditorState,
                             analysis: MarkdownAnalysis): DecorationSet {
  const nodes = analysis.nodes;
  const inactiveDisplayMathByLine = new Map<number, MarkdownNode[]>();
  for (const node of nodes) {
    if (node.kind !== "display-math" || active(node, state)) continue;
    const lineFrom = state.doc.lineAt(node.from).from;
    const lineMath = inactiveDisplayMathByLine.get(lineFrom);
    if (lineMath) lineMath.push(node);
    else inactiveDisplayMathByLine.set(lineFrom, [node]);
  }
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
        /* Keep the terminating newline outside block replacements. It belongs
         * to the following source line, whose geometry must not appear or
         * disappear as the caret moves between adjacent blank lines. */
        builder.add(node.from, node.to, replacement);
        coveredUntil = node.to;
        continue;
      }
    }
    switch (node.kind) {
      case "heading": {
        const level = Number(node.meta?.level ?? 1);
        builder.add(node.from, node.from, Decoration.line({
          class: `cm-live-heading cm-live-heading-${level}`,
        }));
        const headingText = state.sliceDoc(
          Number(node.contentFrom ?? node.from), node.to,
        );
        builder.add(node.from, node.from, Decoration.widget({
          widget: new LineHeightEstimateWidget(
            estimatedHeadingHeight(level, headingText),
          ),
          side: -1,
        }));
        if (!isActive) {
          if (node.contentFrom != null)
            builder.add(node.from, node.contentFrom, hidden);
        }
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
          ? `calc(${indentColumns ? `${indentColumns * 0.375}em + ` : ""}18px + 0.4em)`
          : `${Number((indentColumns * 0.375 + markerIndent).toFixed(2))}em`;
        const listAttributes = {
          class: `cm-live-list-item${ordered ? " cm-live-ordered-list-item" : ""}`,
          style: `--phi-list-content-indent:${contentIndent}`,
        };
        builder.add(node.from, node.from, Decoration.line({
          attributes: {
            ...listAttributes,
          },
        }));
        /* A block replacement in the middle of a source line makes
         * CodeMirror start a fresh .cm-line after the widget. Line
         * decorations from the physical line's start do not carry over to
         * that visual continuation. Wrap the remainder after the first
         * rendered display equation so it retains the list content indent,
         * including when that remainder wraps again. */
        const firstDisplay = (inactiveDisplayMathByLine.get(node.from) ?? [])
          .find((display) => display.from >= node.from && display.to < node.to);
        if (firstDisplay) {
          builder.add(firstDisplay.to, node.to, Decoration.mark({
            class: "cm-live-list-continuation",
            attributes: { style: listAttributes.style },
          }));
        }
        const listLine = state.doc.lineAt(node.from);
        const listText = state.sliceDoc(
          Number(node.meta?.contentFrom ?? node.from), listLine.to,
        );
        builder.add(node.from, node.from, Decoration.widget({
          widget: new LineHeightEstimateWidget(estimatedListLineHeight(
            listText,
            indentColumns * 0.375 + (node.meta?.task ? 1.5 : markerIndent),
          )),
          side: -1,
        }));
        if (!isActive) {
          const markerFrom = Number(node.meta?.markerFrom ?? node.from);
          if (node.meta?.task) {
            builder.add(markerFrom, Number(node.meta?.taskFrom ??
              node.meta?.markerTo ?? markerFrom + 1),
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
              Number(node.meta?.taskFrom ?? node.meta?.markerTo ??
                node.from + marker.length),
              hidden);
          }
        }
        break;
      }
      case "emphasis": if (!isActive) addDelimited(builder, node, emphasis); break;
      case "strong": if (!isActive) addDelimited(builder, node, strong); break;
      case "strike": if (!isActive) addDelimited(builder, node, strike); break;
      case "highlight": if (!isActive) addDelimited(builder, node, highlight); break;
      case "inline-code": addInlineCode(builder, node, isActive); break;
      case "mermaid":
      case "code-block": if (isActive) addCodeBlockSource(
        builder, state, node,
      ); break;
      case "comment": if (!isActive) builder.add(node.from, node.to, hidden); break;
      case "tag": builder.add(node.from, node.to, tag); break;
      case "block-id": builder.add(node.from, node.to, blockId); break;
      case "html": if (isActive) addHtmlSyntax(builder, node); break;
    }
  }
  return Decoration.set(ranges, true);
}

function buildMathTooltips(state: EditorState): readonly Tooltip[] {
  const analysis = markdownAnalysis(state);
  const seen = new Set<number>();
  const tooltips: Tooltip[] = [];
  for (const selection of state.selection.ranges) {
    const node = mathNodeAt(analysis, selection);
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

function selectionActivatesNode(analysis: MarkdownAnalysis,
                                state: EditorState): boolean {
  return analysis.nodes.some((node) => active(node, state));
}

function changesFollowAllNodes(transaction: Transaction): boolean {
  let earliest = transaction.startState.doc.length;
  transaction.changes.iterChanges((fromA) => { earliest = Math.min(earliest, fromA); });
  return markdownAnalysis(transaction.startState).nodes.every((node) =>
    node.to <= earliest);
}

const livePreviewDecorations = StateField.define<DecorationSet>({
  create: buildDecorations,
  update(value, transaction) {
    const forced = transaction.effects.some((effect) =>
      effect.is(refreshLivePreview) || effect.is(pinPreviewSource));
    if (forced) return buildDecorations(transaction.state);
    if (transaction.docChanged) {
      const previous = markdownAnalysis(transaction.startState);
      const current = markdownAnalysis(transaction.state);
      /* A proven plain-prose edit after every preview node cannot affect any
       * decoration or position-aware widget. Keep the exact set and DOM. */
      if (current.updateKind === "mapped" && changesFollowAllNodes(transaction) &&
          !selectionActivatesNode(previous, transaction.startState) &&
          !selectionActivatesNode(current, transaction.state))
        return value;
      return buildDecorations(transaction.state);
    }
    if (transaction.selection) {
      const analysis = markdownAnalysis(transaction.state);
      if (!selectionActivatesNode(analysis, transaction.startState) &&
          !selectionActivatesNode(analysis, transaction.state))
        return value;
      return buildDecorations(transaction.state);
    }
    return value;
  },
  provide: (field) => [
    EditorView.decorations.from(field),
    showTooltip.computeN([field, markdownAnalysisField],
      (state) => buildMathTooltips(state)),
  ],
});

export const livePreview = [
  markdownAnalysisField,
  previewSourceRange,
  ...previewInteraction,
  livePreviewDecorations,
];
