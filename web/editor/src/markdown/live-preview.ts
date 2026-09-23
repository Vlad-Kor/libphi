import {
  type EditorState,
  type Range,
  StateField,
  type Transaction,
} from "@codemirror/state";
import {
  Decoration, type DecorationSet, EditorView, showTooltip, type Tooltip, type ViewUpdate,
} from "@codemirror/view";
import { getMathRevision, mathTypesetStats } from "../math/mathjax";
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
} from "./analysis";
import { measurePerformance } from "../performance";
import {
  clearPreviewGeometry,
  geometryWidgetKey,
  lineGeometryKey,
  measuredPreviewGeometry,
  previewGeometry,
  previewGeometryPreflight,
  withMeasuredGeometry,
} from "./geometry";
import { exportPreviewMode } from "../settings";

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
export const refreshLivePreview = clearPreviewGeometry;

function active(node: MarkdownNode, state: EditorState): boolean {
  return !exportPreviewMode() && previewNodeIsActive(node, state);
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
      widget: new EmptyInlineCodeWidget(sourcePosition - node.from, node.from),
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

function addTableSource(
  builder: DecorationSink,
  state: EditorState,
  node: MarkdownNode,
): void {
  const first = state.doc.lineAt(node.from);
  const last = state.doc.lineAt(Math.max(node.from, node.to - 1));
  for (let number = first.number; number <= last.number; number++) {
    const line = state.doc.line(number);
    builder.add(line.from, line.from, Decoration.line({
      class: "cm-live-table-source",
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
    Decoration.set(buildRanges(state, markdownAnalysis(state).nodes), true));
}

/** Decorations for `nodes`, which must be complete top-level groups: every
 * node overlapping one of their lines is included (see dirtyRegions). All
 * decorations produced for a node lie within the lines it spans. */
function buildRanges(state: EditorState,
                     nodes: readonly MarkdownNode[]): Range<Decoration>[] {
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
        const widget = replacement.spec.widget;
        if (widget) withMeasuredGeometry(widget,
          geometryWidgetKey(node, Boolean(replacement.spec.block),
            state.doc.lineAt(node.from)),
          state.field(previewGeometry, false) ?? new Map());
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
          phiLineGeometry: true,
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
          phiLineGeometry: true,
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
      case "table": if (isActive) addTableSource(
        builder, state, node,
      ); break;
      case "comment": if (!isActive) builder.add(node.from, node.to, hidden); break;
      case "tag": builder.add(node.from, node.to, tag); break;
      case "block-id": builder.add(node.from, node.to, blockId); break;
      case "html": if (isActive) addHtmlSyntax(builder, node); break;
    }
  }
  // Inline formatting can change wrapping even when it does not increase the
  // tallest glyph. Measure the containing line, not just the inline widget.
  const preliminary = Decoration.set(ranges, true);
  const inlineLines = new Set<number>();
  for (const node of nodes) {
    if (active(node, state)) continue;
    const first = state.doc.lineAt(node.from);
    if (node.to <= first.to) inlineLines.add(first.from);
  }
  for (const from of inlineLines) {
    const line = state.doc.lineAt(from);
    let covered = false;
    preliminary.between(line.from, line.to, (a, b, decoration) => {
      if (decoration.spec.block || decoration.spec.phiLineGeometry ||
          a < line.from || b > line.to) covered = true;
    });
    if (!covered) ranges.push(Decoration.widget({
      phiLineGeometry: true,
      widget: new LineHeightEstimateWidget(-1), side: -1,
    }).range(from));
  }
  const heights = state.field(previewGeometry, false);
  if (!heights?.size) return ranges;
  const result = Decoration.set(ranges, true);
  return ranges.map(range => {
    if (!(range.value.spec.widget instanceof LineHeightEstimateWidget)) return range;
    const line = state.doc.lineAt(range.from);
    const height = heights.get(lineGeometryKey(line.text, result, line.from, line.to));
    return height == null ? range : Decoration.widget({
      phiLineGeometry: true,
      widget: new LineHeightEstimateWidget(height), side: -1,
    }).range(range.from);
  });
}

function activeMathNodes(state: EditorState): MarkdownNode[] {
  const analysis = markdownAnalysis(state);
  const seen = new Set<number>();
  const nodes: MarkdownNode[] = [];
  for (const selection of state.selection.ranges) {
    const node = mathNodeAt(analysis, selection);
    if (!node || seen.has(node.from)) continue;
    seen.add(node.from);
    nodes.push(node);
  }
  return nodes;
}

/* A typeset slower than this is deferred until input pauses. Faster ones
 * (most inline and small display equations) still update on every key. */
const LIVE_BUBBLE_TYPESET_MS = 12;
const BUBBLE_QUIET_MS = 180;

/** The live rendering above edited math. CodeMirror reuses a tooltip view
 * while its `create` function is unchanged, so one bubble persists while the
 * caret stays in math and re-renders only the latest source. Recreating it
 * per keystroke used to start a synchronous MathJax typeset for every key
 * (about 100 ms for a multiline environment in WebKit); holding a key queued
 * them, so rendering continued long after the key was released. */
class MathBubble {
  readonly dom = document.createElement("div");
  /** The visible rendering, and a newer one that is still typesetting. */
  private shown: RenderedMath | null = null;
  private pending: RenderedMath | null = null;
  private timer = 0;
  private lastCost = 0;
  private target = "";
  private nodeFrom = -1;

  constructor(private readonly view: EditorView, private readonly slot: number) {
    this.dom.className = "math-preview-bubble";
    const node = activeMathNodes(view.state)[slot];
    if (node) {
      this.target = `${getMathRevision()}\0${node.kind}\0${node.text}`;
      this.nodeFrom = node.from;
    }
    this.render(node);
  }

  update(update: ViewUpdate): void {
    const node = activeMathNodes(update.state)[this.slot];
    if (!node) return;
    /* Compare with the latest requested source, including a deferred one:
     * unrelated transactions must not postpone a scheduled render. */
    const target = `${getMathRevision()}\0${node.kind}\0${node.text}`;
    const sameNode = update.changes.mapPos(this.nodeFrom) === node.from;
    this.nodeFrom = node.from;
    if (target === this.target) return;
    this.target = target;
    window.clearTimeout(this.timer);
    /* Only edits of one equation are deferred; moving to another equation
     * must show that equation immediately. */
    if (!sameNode || this.lastCost <= LIVE_BUBBLE_TYPESET_MS) this.render(node);
    else this.timer = window.setTimeout(() =>
      this.render(activeMathNodes(this.view.state)[this.slot]), BUBBLE_QUIET_MS);
  }

  private render(node: MarkdownNode | undefined): void {
    this.timer = 0;
    if (!node) return;
    if (this.pending) discardMath(this.pending);
    const widget = new MathWidget(
      node.text, node.kind === "display-math", node.from, undefined, true,
    );
    const before = mathTypesetStats().count;
    const math = { widget, dom: widget.toDOM(this.view) };
    const first = !this.shown;
    if (first) {
      this.shown = math;
    } else {
      /* Keep the previous rendering until this one is ready, so the bubble
       * does not flash its source text on every key. */
      this.pending = math;
      math.dom.style.display = "none";
    }
    this.dom.append(math.dom);
    void widgetRendered(math.dom).then(() => {
      if (this.pending !== math && this.shown !== math) return;
      const stats = mathTypesetStats();
      /* Cached renderings do not typeset; only real conversions set cost. */
      if (stats.count !== before) this.lastCost = stats.lastDuration;
      if (first) return;
      if (this.shown) discardMath(this.shown);
      math.dom.style.removeProperty("display");
      this.shown = math;
      this.pending = null;
    });
  }

  destroy(): void {
    window.clearTimeout(this.timer);
    if (this.pending) discardMath(this.pending);
    if (this.shown) discardMath(this.shown);
  }
}

interface RenderedMath { widget: MathWidget; dom: HTMLElement }

function discardMath(math: RenderedMath): void {
  math.widget.destroy(math.dom);
  math.dom.remove();
}

/** Resolves once a MathWidget element has left its loading state. */
function widgetRendered(dom: HTMLElement): Promise<void> {
  return new Promise((resolve) => {
    const check = () => {
      if (!dom.classList.contains("math-loading") || !dom.isConnected) {
        observer.disconnect();
        resolve();
      }
    };
    const observer = new MutationObserver(check);
    observer.observe(dom, { attributes: true, attributeFilter: ["class"] });
    queueMicrotask(check);
  });
}

const mathBubbleCreators: Array<(view: EditorView) => MathBubble> = [];
function mathBubbleCreator(slot: number): (view: EditorView) => MathBubble {
  return mathBubbleCreators[slot] ??= (view) => new MathBubble(view, slot);
}

function buildMathTooltips(state: EditorState): readonly Tooltip[] {
  return activeMathNodes(state).map((node, slot) => ({
    pos: node.from,
    end: node.to,
    above: true,
    strictSide: true,
    create: mathBubbleCreator(slot),
  }));
}

interface Region { from: number; to: number }

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

/* Whether a node is revealed depends only on the selection and pinned
 * source it touches, and its decorations only on its own lines. A document
 * edit therefore affects the analysis's changed lines, and a selection change
 * affects nodes touching the old or new selection or pinned range. Everything
 * else is the previous decoration set, mapped. Rebuilding every decoration on
 * each key (about 12 ms for a 40 KiB note in WebKit) was the largest remaining
 * per-keystroke cost after incremental parsing. */
function updateDecorations(value: DecorationSet,
                           transaction: Transaction): DecorationSet {
  const state = transaction.state;
  const analysis = markdownAnalysis(state);
  if (transaction.docChanged && analysis.updateKind === "full")
    return buildDecorations(state);
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
  if (!ranges.length) return value;
  return measurePerformance("markdown/preview-decorations-region", () => {
    const regions = dirtyRegions(state, analysis.nodes, ranges);
    const covered = regions.reduce((sum, region) => sum + region.to - region.from, 0);
    if (covered > state.doc.length / 2) return buildDecorations(state);
    const nodes = analysis.nodes.filter((node) => regions.some((region) =>
      node.from >= region.from && node.from <= region.to));
    const added = buildRanges(state, nodes);
    const mapped = value.map(changes);
    const outside = (from: number, to: number) => !regions.some((region) =>
      from >= region.from && to <= region.to);
    let stale = false;
    for (const region of regions) {
      mapped.between(region.from, region.to, (from, to) => {
        if (!outside(from, to)) stale = true;
        return stale ? false : undefined;
      });
    }
    if (!stale && !added.length) return mapped;
    return mapped.update({
      filter: outside,
      filterFrom: regions[0].from,
      filterTo: regions.at(-1)!.to,
      add: added,
      sort: true,
    });
  });
}

const livePreviewDecorations = StateField.define<DecorationSet>({
  create: buildDecorations,
  update(value, transaction) {
    const forced = transaction.reconfigured || transaction.effects.some((effect) =>
      effect.is(refreshLivePreview) || effect.is(pinPreviewSource) ||
      effect.is(measuredPreviewGeometry));
    if (forced) return buildDecorations(transaction.state);
    return updateDecorations(value, transaction);
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
  previewGeometry,
  livePreviewDecorations,
  previewGeometryPreflight(livePreviewDecorations),
];
