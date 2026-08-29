import {
  EditorSelection,
  Prec,
  StateEffect,
  StateField,
  type EditorState,
  type Range,
} from "@codemirror/state";
import {
  Decoration,
  EditorView,
  keymap,
  ViewPlugin,
  type DecorationSet,
  type KeyBinding,
} from "@codemirror/view";
import { markdownAnalysis } from "./analysis";
import {
  pinPreviewSource,
  previewSourceRange,
} from "./source-edit";
import { selectionTouches, type MarkdownNode } from "./parser";

export interface HardPreviewSelection {
  from: number;
  to: number;
}

export const selectHardPreview =
  StateEffect.define<HardPreviewSelection | null>();

/* Keep the interaction boundary centralized. In particular, the planned
 * graphical table editor should replace the edit action, not make tables
 * selection-active like soft preview blocks. */
export function isHardRenderedNode(node: MarkdownNode): boolean {
  return node.kind === "embed" || node.kind === "markdown-image" ||
    node.kind === "table" || node.kind === "frontmatter" ||
    (node.kind === "html" && /<(?:iframe|table)\b/i.test(node.text)) ||
    (node.kind === "markdown-link" &&
      /^(?:!\[|<img\b)/i.test(String(node.meta?.alias ?? "").trim()));
}

export function previewNodeIsActive(
  node: MarkdownNode,
  state: EditorState,
): boolean {
  const pinned = state.field(previewSourceRange, false);
  if (pinned && node.from < pinned.to && node.to > pinned.from) return true;
  /* A caret inside hard source is only an implementation detail of clicking
   * or traversing its replacement, so it must not reveal the source. A real
   * text selection is different: dragging across a hard item (or Select All)
   * must expose the selected source and preserve normal text operations. */
  if (isHardRenderedNode(node)) {
    return state.selection.ranges.some((selection) =>
      !selection.empty && selectionTouches(node, selection)
    );
  }
  return state.selection.ranges.some((selection) => {
    if (node.kind === "task") {
      const from = Number(node.meta?.prefixFrom ?? node.from);
      const to = Number(node.meta?.prefixTo ?? node.to);
      return selection.from === selection.to
        ? selection.from >= from && selection.from < to
        : selection.from < to && selection.to > from;
    }
    if (softBlockEndpointBelongsToNode(node) && selection.empty)
      return selection.from >= node.from && selection.from <= node.to;
    return selectionTouches(node, selection);
  });
}

function softBlockEndpointBelongsToNode(node: MarkdownNode): boolean {
  return node.kind === "heading" || node.kind === "horizontal-rule" ||
    node.kind === "footnote-definition" ||
    node.kind === "callout" || node.kind === "blockquote" ||
    node.kind === "mermaid" || node.kind === "code-block";
}

function verticalMotionCanEnterBlockEndpoint(node: MarkdownNode): boolean {
  return softBlockEndpointBelongsToNode(node) ||
    node.kind === "display-math";
}

function positionTouchesPreviewNode(
  node: MarkdownNode,
  position: number,
): boolean {
  return selectionTouches(node, { from: position, to: position }) ||
    (position === node.to && verticalMotionCanEnterBlockEndpoint(node));
}

function hasWholePreviewReplacement(node: MarkdownNode): boolean {
  return node.kind === "frontmatter" || node.kind === "horizontal-rule" ||
    node.kind === "math" || node.kind === "display-math" ||
    node.kind === "wikilink" || node.kind === "embed" ||
    node.kind === "markdown-link" || node.kind === "markdown-image" ||
    node.kind === "footnote-reference" || node.kind === "inline-footnote" ||
    node.kind === "footnote-definition" || node.kind === "task" ||
    node.kind === "tag" || node.kind === "callout" ||
    node.kind === "blockquote" || node.kind === "table" ||
    node.kind === "mermaid" || node.kind === "code-block" ||
    node.kind === "html";
}

function hardNodes(state: EditorState): MarkdownNode[] {
  const hard: MarkdownNode[] = [];
  let coveredUntil = -1;
  /* Match Live Preview's outer-replacement precedence so an image nested in
   * a rendered callout/table is not exposed as an invisible atomic range. */
  for (const node of markdownAnalysis(state).nodes) {
    if (node.from < coveredUntil) continue;
    if (previewNodeIsActive(node, state) || !hasWholePreviewReplacement(node))
      continue;
    if (isHardRenderedNode(node)) hard.push(node);
    coveredUntil = node.to;
  }
  return hard;
}

export const hardPreviewSelection =
  StateField.define<HardPreviewSelection | null>({
    create: () => null,
    update(value, transaction) {
      let explicitlySet = false;
      for (const effect of transaction.effects) {
        if (!effect.is(selectHardPreview)) continue;
        value = effect.value;
        explicitlySet = true;
      }
      if (explicitlySet) return value;
      if (transaction.docChanged || transaction.selection) return null;
      return value;
    },
  });

export function selectedHardPreview(
  state: EditorState,
): HardPreviewSelection | null {
  return state.field(hardPreviewSelection, false) ?? null;
}

function buildHardAtomicRanges(state: EditorState): DecorationSet {
  const ranges: Range<Decoration>[] = hardNodes(state).map((node) =>
    Decoration.mark({}).range(node.from, node.to));
  return Decoration.set(ranges, true);
}

const hardPreviewAtomicRanges = StateField.define<DecorationSet>({
  create: buildHardAtomicRanges,
  update(value, transaction) {
    return transaction.docChanged || transaction.selection ||
      transaction.effects.some((effect) => effect.is(pinPreviewSource))
      ? buildHardAtomicRanges(transaction.state)
      : value;
  },
  provide: (field) => EditorView.atomicRanges.of((view) =>
    view.state.field(field)),
});

function syncHardSelectionDOM(view: EditorView): void {
  const selected = selectedHardPreview(view.state);
  view.dom.classList.toggle("cm-hard-preview-selection", Boolean(selected));
  for (const element of view.dom.querySelectorAll<HTMLElement>(
    ".cm-hard-rendered-item",
  )) {
    const from = Number(element.dataset.hardPreviewFrom);
    const to = Number(element.dataset.hardPreviewTo);
    const active = selected?.from === from && selected.to === to;
    element.classList.toggle("cm-hard-selected", active);
    element.setAttribute("aria-selected", String(active));
  }
}

const hardPreviewDOM = ViewPlugin.fromClass(class {
  constructor(readonly view: EditorView) {
    syncHardSelectionDOM(view);
  }

  update(): void {
    syncHardSelectionDOM(this.view);
  }
});

export function chooseHardPreview(
  view: EditorView,
  node: Pick<MarkdownNode, "from" | "to">,
): void {
  view.dispatch({
    effects: [
      selectHardPreview.of({ from: node.from, to: node.to }),
      EditorView.scrollIntoView(node.from, { y: "nearest" }),
    ],
  });
  view.focus();
}

function sameHardNode(
  node: MarkdownNode,
  selected: HardPreviewSelection,
): boolean {
  return node.from === selected.from && node.to === selected.to;
}

function adjacentHardNode(
  state: EditorState,
  selected: HardPreviewSelection,
  forward: boolean,
  vertical: boolean,
): MarkdownNode | undefined {
  const nodes = hardNodes(state);
  const index = nodes.findIndex((node) => sameHardNode(node, selected));
  if (index < 0) return undefined;
  const candidate = nodes[index + (forward ? 1 : -1)];
  if (!candidate) return undefined;
  const gap = forward
    ? state.sliceDoc(selected.to, candidate.from)
    : state.sliceDoc(candidate.to, selected.from);
  if (!/^\s*$/.test(gap)) return undefined;
  return gap.includes("\n") === vertical ? candidate : undefined;
}

function leaveHardPreview(
  view: EditorView,
  selected: HardPreviewSelection,
  forward: boolean,
  vertical: boolean,
): boolean {
  const adjacent = adjacentHardNode(
    view.state, selected, forward, vertical,
  );
  if (adjacent) {
    chooseHardPreview(view, adjacent);
    return true;
  }
  const anchor = forward ? selected.to : selected.from;
  view.dispatch({
    selection: EditorSelection.cursor(anchor),
    effects: selectHardPreview.of(null),
    scrollIntoView: true,
    userEvent: "select",
  });
  return true;
}

function hardNodeCrossed(
  state: EditorState,
  from: number,
  to: number,
  forward: boolean,
): MarkdownNode | undefined {
  if (forward) {
    return hardNodes(state).find((node) =>
      node.from >= from && node.from < to && node.to <= to);
  }
  const nodes = hardNodes(state);
  for (let index = nodes.length - 1; index >= 0; index--) {
    const node = nodes[index];
    if (node.to <= from && node.to > to && node.from >= to) return node;
  }
  return undefined;
}

function hardNodeAtCursor(
  state: EditorState,
  position: number,
  forward?: boolean,
): MarkdownNode | undefined {
  return hardNodes(state).find((node) =>
    (position > node.from && position < node.to) ||
    (forward === true && position === node.from) ||
    (forward === false && position === node.to));
}

function previewNodeAt(
  state: EditorState,
  position: number,
): MarkdownNode | undefined {
  return hardNodes(state).find((node) =>
    positionTouchesPreviewNode(node, position)) ??
    markdownAnalysis(state).nodes.find((node) =>
      positionTouchesPreviewNode(node, position));
}

function moveHorizontally(view: EditorView, forward: boolean): boolean {
  const selected = selectedHardPreview(view.state);
  if (selected) return leaveHardPreview(view, selected, forward, false);
  const selection = view.state.selection.main;
  if (!selection.empty) return false;
  const containing = hardNodeAtCursor(
    view.state, selection.head, forward,
  );
  if (containing) {
    chooseHardPreview(view, containing);
    return true;
  }
  const moved = view.moveByChar(selection, forward);
  if (moved.head === selection.head) return false;
  const hard = hardNodeCrossed(
    view.state, selection.head, moved.head, forward,
  );
  if (!hard) return false;
  chooseHardPreview(view, hard);
  return true;
}

function sourceColumnPosition(
  state: EditorState,
  lineNumber: number,
  column: number,
): number {
  const line = state.doc.line(lineNumber);
  return line.from + Math.min(column, line.length);
}

function moveVerticallyThroughPreview(
  view: EditorView,
  forward: boolean,
): boolean {
  const selected = selectedHardPreview(view.state);
  if (selected) return leaveHardPreview(view, selected, forward, true);
  const selection = view.state.selection.main;
  if (!selection.empty) return false;
  const containing = hardNodeAtCursor(
    view.state, selection.head, forward,
  );
  if (containing) {
    chooseHardPreview(view, containing);
    return true;
  }
  const currentLine = view.state.doc.lineAt(selection.head);
  const moved = view.moveVertically(selection, forward);
  if (moved.head === selection.head) return false;
  const movedLine = view.state.doc.lineAt(moved.head);
  if (movedLine.number === currentLine.number) return false;

  const step = forward ? 1 : -1;
  const column = selection.head - currentLine.from;
  for (let number = currentLine.number + step;
       forward ? number <= movedLine.number : number >= movedLine.number;
       number += step) {
    const position = sourceColumnPosition(view.state, number, column);
    const node = previewNodeAt(view.state, position);
    if (!node) continue;
    if (isHardRenderedNode(node)) {
      chooseHardPreview(view, node);
      return true;
    }
    const target = position === node.to &&
      !softBlockEndpointBelongsToNode(node)
      ? Math.max(node.from, position - 1)
      : position;
    view.dispatch({
      selection: EditorSelection.cursor(target),
      scrollIntoView: true,
      userEvent: "select",
    });
    return true;
  }
  return false;
}

function deleteHardPreview(view: EditorView): boolean {
  const selected = selectedHardPreview(view.state);
  if (!selected) return false;
  view.dispatch({
    changes: { from: selected.from, to: selected.to },
    selection: EditorSelection.cursor(selected.from),
    effects: selectHardPreview.of(null),
    scrollIntoView: true,
    userEvent: "delete",
  });
  return true;
}

function editHardPreview(view: EditorView): boolean {
  const selected = selectedHardPreview(view.state);
  if (!selected) return false;
  const anchor = Math.min(selected.from + 1, selected.to);
  view.dispatch({
    selection: EditorSelection.cursor(anchor),
    effects: [
      selectHardPreview.of(null),
      pinPreviewSource.of({ from: selected.from, to: selected.to }),
    ],
    scrollIntoView: true,
    userEvent: "select",
  });
  view.focus();
  return true;
}

function clearHardPreview(view: EditorView): boolean {
  if (!selectedHardPreview(view.state)) return false;
  view.dispatch({ effects: selectHardPreview.of(null) });
  return true;
}

const hardPreviewKeymap: readonly KeyBinding[] = [
  { key: "ArrowLeft", run: (view) => moveHorizontally(view, false) },
  { key: "ArrowRight", run: (view) => moveHorizontally(view, true) },
  { key: "ArrowUp", run: (view) => moveVerticallyThroughPreview(view, false) },
  { key: "ArrowDown", run: (view) => moveVerticallyThroughPreview(view, true) },
  { key: "Backspace", run: deleteHardPreview },
  { key: "Delete", run: deleteHardPreview },
  { key: "Enter", run: editHardPreview },
  { key: "Escape", run: clearHardPreview },
];

export const previewInteraction = [
  hardPreviewSelection,
  hardPreviewAtomicRanges,
  hardPreviewDOM,
  Prec.highest(keymap.of(hardPreviewKeymap)),
];
