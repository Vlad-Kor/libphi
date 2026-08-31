import { closeBrackets } from "@codemirror/autocomplete";
import { redo, undo } from "@codemirror/commands";
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { EditorSelection, EditorState, Prec } from "@codemirror/state";
import { drawSelection, EditorView, keymap, WidgetType } from "@codemirror/view";
import {
  handleLatexTab,
  latexSnippetsEnabled,
  latexSuite,
} from "../latex-suite/engine";
import { latexEnhancements } from "../latex-suite/enhancements";
import { cellLivePreview } from "../markdown/cell-live-preview";
import { smartPairs } from "../markdown/pairs";
import {
  canonicalMarkdownTable,
  cloneMarkdownTable,
  escapeTypedCellPipes,
  parseMarkdownTable,
  tableCellMarkdownForRender,
  type MarkdownTable,
} from "../markdown/table";
import { renderMarkdownInline, wireRenderedContent } from "../markdown/render";
import { pinPreviewSource, previewSourceRange } from "../markdown/source-edit";
import { currentEditorSettings } from "../settings";

type CellEdge = "start" | "end" | "preserve";
type HandleKind = "row" | "column";

const tableController = Symbol("phi-rich-table-controller");
type ControlledTable = HTMLElement & {
  [tableController]?: RichTableController;
};

function sourceIcon(): SVGElement {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 16 16");
  svg.setAttribute("aria-hidden", "true");
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", "M5.5 3.5 1.5 8l4 4.5M10.5 3.5l4 4.5-4 4.5M9.25 2 6.75 14");
  svg.append(path);
  return svg;
}

function dots(): HTMLSpanElement {
  const span = document.createElement("span");
  span.className = "rich-table-handle-dots";
  span.setAttribute("aria-hidden", "true");
  for (let index = 0; index < 6; index++) span.append(document.createElement("i"));
  return span;
}

interface ActiveCell {
  element: HTMLElement;
  editor: EditorView;
  row: number;
  column: number;
}

interface DragState {
  kind: HandleKind;
  from: number;
  slot: number;
  moved: boolean;
  handle: HTMLButtonElement;
  origin: DOMRect;
  grabOffset: number | null;
  pointerCoordinate: number | null;
}

interface RichTableGeometryContext {
  documentPath: string;
  textWidth: number;
  fontScale: number;
  key: string;
}

const TABLE_HEIGHT_CACHE_LIMIT = 256;
const tableHeightCache = new Map<string, number>();
let richTableGeometryContext: RichTableGeometryContext = {
  documentPath: "",
  textWidth: 780,
  fontScale: 1,
  key: "\u0000w780:s100",
};

export function setRichTableGeometryContext(
  documentPath: string,
  textWidth: number,
  fontScale: number,
): void {
  const width = Math.max(160, Math.round((Number.isFinite(textWidth)
    ? textWidth : 780) / 16) * 16);
  const scale = Math.max(0.5, Math.min(3, Number.isFinite(fontScale)
    ? fontScale : 1));
  richTableGeometryContext = {
    documentPath,
    textWidth: width,
    fontScale: scale,
    key: `${documentPath}\u0000w${width}:s${Math.round(scale * 100)}`,
  };
}

export function resetRichTableGeometryCache(): void {
  tableHeightCache.clear();
}

function tableHeightCacheKey(
  context: RichTableGeometryContext,
  geometryKey: string,
): string {
  return `${context.key}\u0000${geometryKey}`;
}

function cachedTableHeight(
  context: RichTableGeometryContext,
  geometryKey: string,
): number | undefined {
  const key = tableHeightCacheKey(context, geometryKey);
  const value = tableHeightCache.get(key);
  if (value == null) return undefined;
  tableHeightCache.delete(key);
  tableHeightCache.set(key, value);
  return value;
}

function rememberTableHeight(
  context: RichTableGeometryContext,
  geometryKey: string,
  height: number,
): void {
  if (!Number.isFinite(height) || height < 1) return;
  const key = tableHeightCacheKey(context, geometryKey);
  tableHeightCache.delete(key);
  tableHeightCache.set(key, Math.round(height * 2) / 2);
  while (tableHeightCache.size > TABLE_HEIGHT_CACHE_LIMIT)
    tableHeightCache.delete(tableHeightCache.keys().next().value!);
}

function estimatedTableHeight(
  model: MarkdownTable,
  context: RichTableGeometryContext,
): number {
  const fontSize = 16 * context.fontScale;
  const lineHeight = fontSize * 1.62;
  const columns = Math.max(1, model.alignments.length);
  const contentWidth = Math.max(
    44,
    (context.textWidth - 36 - 20) / columns - fontSize * 1.3,
  );
  const rows = model.cells.reduce((height, row) => {
    const visualLines = Math.max(1, ...row.map((cell) => {
      const approximateText = cell.replace(/[*_~=`$\[\]\\]/g, "");
      const width = Math.max(fontSize, approximateText.length * fontSize * 0.54);
      return Math.max(1, Math.ceil(width / contentWidth));
    }));
    return height + visualLines * lineHeight + fontSize * 0.7 + 1;
  }, 0);
  /* Root padding, the handle rail, cell padding, and collapsed borders all
   * contribute to the mounted block even before any cell text wraps. */
  return Math.ceil(36 + 20 + rows);
}

function measureTableHeight(
  view: EditorView,
  root: HTMLElement,
  context: RichTableGeometryContext,
  geometryKey: string,
): void {
  view.requestMeasure({
    key: root,
    read: () => root.isConnected ? root.getBoundingClientRect().height : 0,
    write: (height) => rememberTableHeight(context, geometryKey, height),
  });
}

class RichTableController {
  private active: ActiveCell | null = null;
  private drag: DragState | null = null;
  private syncingCell = false;
  private overflowObserver: ResizeObserver | null = null;
  private pointerDragCleanup: (() => void) | null = null;

  constructor(
    readonly root: ControlledTable,
    private view: EditorView,
    private from: number,
    private to: number,
    private model: MarkdownTable,
  ) {
    root[tableController] = this;
    root.addEventListener("pointerdown", (event) => {
      if (event.button === 0) this.clearPinnedSource();
      if (event.target === root ||
          (event.target instanceof Element &&
            event.target.classList.contains("rich-table-scroll")))
        event.preventDefault();
    });
    this.render();
  }

  destroy(): void {
    this.cancelDrag();
    this.overflowObserver?.disconnect();
    this.overflowObserver = null;
    this.active?.editor.destroy();
    this.active = null;
    this.clearDragVisual();
    delete this.root[tableController];
  }

  update(view: EditorView, from: number, to: number, model: MarkdownTable): void {
    const dimensionsChanged = model.cells.length !== this.model.cells.length ||
      model.alignments.length !== this.model.alignments.length;
    this.view = view;
    this.from = from;
    this.to = to;
    this.model = model;
    this.markRoot();
    if (dimensionsChanged) {
      this.discardActive();
      this.render();
      return;
    }

    this.root.querySelectorAll<HTMLElement>(".rich-table-cell").forEach((cell) => {
      const row = Number(cell.dataset.row);
      const column = Number(cell.dataset.column);
      const alignment = this.model.alignments[column];
      if (cell.parentElement)
        cell.parentElement.style.textAlign = alignment === "none" ? "" : alignment;
      if (this.active?.element === cell) {
        this.syncActiveSource(this.model.cells[row]?.[column] ?? "");
      } else {
        this.renderCell(cell, this.model.cells[row]?.[column] ?? "");
      }
    });
  }

  private markRoot(): void {
    this.root.className = "table-widget rich-table-widget cm-hard-rendered-item";
    this.root.dataset.hardPreviewFrom = String(this.from);
    this.root.dataset.hardPreviewTo = String(this.to);
    this.root.setAttribute("aria-selected", "false");
  }

  private render(): void {
    this.cancelDrag();
    this.overflowObserver?.disconnect();
    this.overflowObserver = null;
    this.discardActive();
    this.clearDragVisual();
    this.markRoot();
    this.root.replaceChildren();

    const source = document.createElement("button");
    source.type = "button";
    source.className = "image-source-button rich-table-source-button";
    source.title = "Edit table source";
    source.setAttribute("aria-label", "Edit table source");
    source.append(sourceIcon());
    source.addEventListener("pointerdown", (event) => event.stopPropagation());
    source.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      this.commitActive();
      this.view.dispatch({
        selection: EditorSelection.cursor(Math.min(this.from + 1, this.to)),
        effects: pinPreviewSource.of({ from: this.from, to: this.to }),
        scrollIntoView: true,
        userEvent: "select",
      });
      this.view.focus();
    });

    const table = document.createElement("table");
    table.className = "rich-table-grid";
    const thead = document.createElement("thead");
    const handles = document.createElement("tr");
    handles.className = "rich-table-column-handles";
    const corner = document.createElement("th");
    corner.className = "rich-table-handle-corner";
    handles.append(corner);
    for (let column = 0; column < this.model.alignments.length; column++) {
      const holder = document.createElement("th");
      holder.className = "rich-table-column-handle-cell";
      holder.dataset.column = String(column);
      holder.append(this.handle("column", column));
      handles.append(holder);
    }
    thead.append(handles, this.rowElement(0, true));
    table.append(thead);

    const tbody = document.createElement("tbody");
    for (let row = 1; row < this.model.cells.length; row++)
      tbody.append(this.rowElement(row, false));
    table.append(tbody);

    const scroller = document.createElement("div");
    scroller.className = "rich-table-scroll";
    scroller.append(table);

    const frame = document.createElement("div");
    frame.className = "rich-table-frame";
    frame.append(scroller, source);

    const addColumn = this.addButton("column", "Add column to the right", () => {
      this.commitActive();
      const next = cloneMarkdownTable(this.model);
      next.alignments.push("none");
      for (const row of next.cells) row.push("");
      this.commitModel(next, "input.table-add-column");
      queueMicrotask(() => this.focusCell(0, next.alignments.length - 1, "start"));
    });
    const addRow = this.addButton("row", "Add row below", () => {
      this.commitActive();
      const next = cloneMarkdownTable(this.model);
      next.cells.push(Array.from({ length: next.alignments.length }, () => ""));
      this.commitModel(next, "input.table-add-row");
      queueMicrotask(() => this.focusCell(next.cells.length - 1, 0, "start"));
    });

    this.root.append(frame, addColumn, addRow);
    const updateOverflow = () => scroller.classList.toggle(
      "rich-table-overflowing",
      scroller.scrollWidth > scroller.clientWidth + 1,
    );
    queueMicrotask(updateOverflow);
    if (typeof ResizeObserver !== "undefined") {
      this.overflowObserver = new ResizeObserver(updateOverflow);
      this.overflowObserver.observe(scroller);
      this.overflowObserver.observe(table);
    }
  }

  private addButton(
    kind: HandleKind,
    label: string,
    action: () => void,
  ): HTMLButtonElement {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `rich-table-add rich-table-add-${kind}`;
    button.title = label;
    button.setAttribute("aria-label", label);
    const plus = document.createElement("span");
    plus.textContent = "+";
    plus.setAttribute("aria-hidden", "true");
    button.append(plus);
    button.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      action();
    });
    return button;
  }

  private rowElement(row: number, header: boolean): HTMLTableRowElement {
    const tr = document.createElement("tr");
    tr.dataset.row = String(row);
    const holder = document.createElement("th");
    holder.className = "rich-table-row-handle-cell";
    holder.append(this.handle("row", row));
    tr.append(holder);
    for (let column = 0; column < this.model.alignments.length; column++) {
      const cell = document.createElement(header ? "th" : "td");
      const alignment = this.model.alignments[column];
      cell.style.textAlign = alignment === "none" ? "" : alignment;
      cell.dataset.column = String(column);
      const editor = document.createElement("div");
      editor.className = "rich-table-cell";
      editor.dataset.row = String(row);
      editor.dataset.column = String(column);
      editor.tabIndex = -1;
      editor.setAttribute("role", "textbox");
      editor.setAttribute("aria-label", `Row ${row + 1}, column ${column + 1}`);
      this.renderCell(editor, this.model.cells[row]?.[column] ?? "");
      editor.addEventListener("pointerdown", (event) => {
        if (event.button !== 0) return;
        event.preventDefault();
        event.stopPropagation();
        this.activateCell(editor, row, column, {
          x: event.clientX,
          y: event.clientY,
        });
      }, true);
      editor.addEventListener("focus", () => {
        if (!this.active || this.active.element !== editor)
          this.activateCell(editor, row, column);
      });
      editor.addEventListener("focusout", () => queueMicrotask(() => {
        if (this.active?.element === editor &&
            !editor.contains(document.activeElement)) this.commitActive();
      }));
      cell.append(editor);
      tr.append(cell);
    }
    return tr;
  }

  private renderCell(element: HTMLElement, source: string): void {
    element.classList.remove("rich-table-cell-active");
    element.parentElement?.classList.remove("rich-table-cell-active-parent");
    element.innerHTML = renderMarkdownInline(tableCellMarkdownForRender(source));
    if (!element.textContent) element.append(document.createElement("br"));
    wireRenderedContent(element);
  }

  private cellState(
    source: string,
    row: number,
    column: number,
  ): EditorState {
    const settings = currentEditorSettings();
    return EditorState.create({
      doc: source,
      extensions: [
        EditorState.lineSeparator.of("\n"),
        markdown({ base: markdownLanguage }),
        closeBrackets(),
        smartPairs,
        Prec.highest(keymap.of([
          { key: "Mod-a", run: () => this.selectDocument() },
          { key: "Mod-z", run: () => this.undoCell(row, column, false) },
          { key: "Mod-Shift-z", run: () => this.undoCell(row, column, true) },
          { key: "Mod-y", run: () => this.undoCell(row, column, true) },
          { key: "Tab", run: (view) => this.tabCell(view, row, column, false) },
          { key: "Shift-Tab", run: (view) => this.tabCell(view, row, column, true) },
          { key: "Enter", run: () => this.enterCell(row, column) },
          { key: "ArrowLeft", run: (view) => this.horizontalCell(view, row, column, false) },
          { key: "ArrowRight", run: (view) => this.horizontalCell(view, row, column, true) },
          { key: "ArrowUp", run: (view) => this.verticalCell(view, row, column, false) },
          { key: "ArrowDown", run: (view) => this.verticalCell(view, row, column, true) },
        ])),
        ...latexSuite,
        latexSnippetsEnabled.of(settings.executableSnippets),
        latexEnhancements(settings.executableSnippets && settings.latexConceal),
        cellLivePreview,
        drawSelection(),
        EditorView.lineWrapping,
        EditorView.domEventHandlers({
          paste: (event, view) => {
            event.preventDefault();
            const text = (event.clipboardData?.getData("text/plain") ?? "")
              .replace(/\s*\n+\s*/g, " ");
            const selection = view.state.selection.main;
            view.dispatch({
              changes: { from: selection.from, to: selection.to, insert: text },
              selection: { anchor: selection.from + text.length },
              userEvent: "input.paste",
            });
            return true;
          },
        }),
        EditorView.updateListener.of((update) => {
          if (update.docChanged && !this.syncingCell)
            this.commitCellInput(update.view, row, column);
        }),
      ],
    });
  }

  private activateCell(
    element: HTMLElement,
    row: number,
    column: number,
    coordinates?: { x: number; y: number },
  ): void {
    if (this.active?.element === element) {
      this.active.editor.focus();
      return;
    }
    this.clearPinnedSource();
    this.commitActive();
    element.replaceChildren();
    element.classList.add("rich-table-cell-active");
    element.parentElement?.classList.add("rich-table-cell-active-parent");
    const editor = new EditorView({
      parent: element,
      state: this.cellState(this.model.cells[row]?.[column] ?? "", row, column),
    });
    /* The inner keymap still handles the event at this element. Stopping DOM
     * propagation here prevents that same arrow from subsequently reaching
     * the outer CodeMirror and re-entering the table from its boundary. */
    editor.dom.addEventListener("keydown", (event) => event.stopPropagation());
    this.active = { element, editor, row, column };
    let position = editor.state.doc.length;
    if (coordinates) {
      try {
        position = editor.posAtCoords(coordinates) ?? position;
      } catch {
        /* Geometry is unavailable while WebKit is completing the click. */
      }
    }
    editor.dispatch({ selection: EditorSelection.cursor(position) });
    editor.focus();
  }

  private clearPinnedSource(): void {
    if (this.view.state.field(previewSourceRange, false))
      this.view.dispatch({ effects: pinPreviewSource.of(null) });
  }

  focusCell(
    row: number,
    column: number,
    edge: CellEdge = "start",
    offset?: number,
  ): boolean {
    row = Math.max(0, Math.min(this.model.cells.length - 1, row));
    column = Math.max(0, Math.min(this.model.alignments.length - 1, column));
    const cell = this.root.querySelector<HTMLElement>(
      `.rich-table-cell[data-row="${row}"][data-column="${column}"]`,
    );
    if (!cell) return false;
    const previous = this.active?.element === cell
      ? this.active.editor.state.selection.main.head : offset;
    this.activateCell(cell, row, column);
    const editor = this.active?.editor;
    if (!editor) return false;
    const length = editor.state.doc.length;
    const at = edge === "start" ? 0 : edge === "end" ? length
      : Math.max(0, Math.min(length, previous ?? length));
    editor.dispatch({ selection: EditorSelection.cursor(at) });
    editor.focus();
    this.revealCell(cell);
    return true;
  }

  private revealCell(cell: HTMLElement): void {
    const scroller = this.root.querySelector<HTMLElement>(".rich-table-scroll");
    if (!scroller) return;
    const cellBounds = cell.getBoundingClientRect();
    const bounds = scroller.getBoundingClientRect();
    if (cellBounds.left < bounds.left)
      scroller.scrollLeft -= bounds.left - cellBounds.left;
    else if (cellBounds.right > bounds.right)
      scroller.scrollLeft += cellBounds.right - bounds.right;
  }

  private syncActiveSource(source: string): void {
    const active = this.active;
    if (!active) return;
    const raw = active.editor.state.doc.toString();
    if (escapeTypedCellPipes(raw) === source) return;
    const head = Math.min(active.editor.state.selection.main.head, source.length);
    this.syncingCell = true;
    active.editor.dispatch({
      changes: { from: 0, to: active.editor.state.doc.length, insert: source },
      selection: EditorSelection.cursor(head),
    });
    this.syncingCell = false;
  }

  private discardActive(): void {
    if (!this.active) return;
    this.active.editor.destroy();
    this.active = null;
  }

  private commitActive(render = true): boolean {
    const active = this.active;
    if (!active) return false;
    const raw = active.editor.state.doc.toString().replace(/\s*\n+\s*/g, " ");
    const value = escapeTypedCellPipes(raw);
    const current = this.model.cells[active.row]?.[active.column] ?? "";
    active.editor.destroy();
    active.element.classList.remove("rich-table-cell-active");
    active.element.parentElement?.classList.remove("rich-table-cell-active-parent");
    this.active = null;
    if (value !== current) {
      const next = cloneMarkdownTable(this.model);
      next.cells[active.row][active.column] = value;
      this.commitModel(next, "input.table-cell");
    }
    if (render && active.element.isConnected)
      this.renderCell(active.element, value);
    return value !== current;
  }

  private commitCellInput(editor: EditorView, row: number, column: number): boolean {
    const raw = editor.state.doc.toString().replace(/\s*\n+\s*/g, " ");
    const value = escapeTypedCellPipes(raw);
    if (value === (this.model.cells[row]?.[column] ?? "")) return false;
    const next = cloneMarkdownTable(this.model);
    next.cells[row][column] = value;
    this.commitModel(next, "input.type.table-cell");
    return true;
  }

  private commitModel(model: MarkdownTable, userEvent: string): void {
    const source = canonicalMarkdownTable(model);
    const current = this.view.state.sliceDoc(this.from, this.to);
    if (source === current) return;
    this.view.dispatch({
      changes: { from: this.from, to: this.to, insert: source },
      userEvent,
    });
  }

  private selectDocument(): boolean {
    this.commitActive();
    this.view.dispatch({
      selection: EditorSelection.range(0, this.view.state.doc.length),
      scrollIntoView: true,
      userEvent: "select",
    });
    this.view.focus();
    return true;
  }

  private undoCell(row: number, column: number, redoRequested: boolean): boolean {
    (redoRequested ? redo : undo)(this.view);
    queueMicrotask(() => this.focusCell(row, column, "preserve"));
    return true;
  }

  private tabCell(
    editor: EditorView,
    row: number,
    column: number,
    backwards: boolean,
  ): boolean {
    if (handleLatexTab(editor, backwards)) return true;
    this.moveLinear(row, column, !backwards);
    return true;
  }

  private enterCell(row: number, column: number): boolean {
    if (row + 1 < this.model.cells.length)
      this.moveVertical(row, column, true);
    return true;
  }

  private horizontalCell(
    editor: EditorView,
    row: number,
    column: number,
    forward: boolean,
  ): boolean {
    const selection = editor.state.selection.main;
    if (!selection.empty) return false;
    const boundary = forward ? editor.state.doc.length : 0;
    if (selection.head !== boundary) return false;
    this.moveLinear(row, column, forward);
    return true;
  }

  private verticalCell(
    editor: EditorView,
    row: number,
    column: number,
    forward: boolean,
  ): boolean {
    const selection = editor.state.selection.main;
    if (!selection.empty) return false;
    try {
      const moved = editor.moveVertically(selection, forward);
      if (moved.head !== selection.head) {
        const current = editor.coordsAtPos(selection.head);
        const target = editor.coordsAtPos(moved.head);
        /* Live-preview replacements can make CodeMirror return a different
         * source position on the same visual row, or even one whose geometry
         * points in the opposite direction. Neither is vertical progress. */
        const progressesOnVisualRows = current && target && (forward
          ? target.top > current.top + 2
          : target.top < current.top - 2);
        if (!current || !target || progressesOnVisualRows)
          return false;
      }
    } catch {
      /* Empty cells and DOM-only tests have no measurable visual line. */
    }
    this.moveVertical(row, column, forward);
    return true;
  }

  private leave(forward: boolean): void {
    this.commitActive();
    const first = this.view.state.doc.lineAt(this.from);
    const last = this.view.state.doc.lineAt(Math.max(this.from, this.to - 1));
    const number = forward ? last.number + 1 : first.number - 1;
    if (number < 1 || number > this.view.state.doc.lines) return;
    const line = this.view.state.doc.line(number);
    this.view.dispatch({
      selection: EditorSelection.cursor(forward ? line.from : line.to),
      scrollIntoView: true,
      userEvent: "select",
    });
    this.view.focus();
  }

  private moveLinear(row: number, column: number, forward: boolean): void {
    const index = row * this.model.alignments.length + column + (forward ? 1 : -1);
    const count = this.model.cells.length * this.model.alignments.length;
    if (index < 0 || index >= count) {
      this.leave(forward);
      return;
    }
    this.commitActive();
    const targetRow = Math.floor(index / this.model.alignments.length);
    const targetColumn = index % this.model.alignments.length;
    queueMicrotask(() => this.focusCell(
      targetRow, targetColumn, forward ? "start" : "end",
    ));
  }

  private moveVertical(row: number, column: number, forward: boolean): void {
    const target = row + (forward ? 1 : -1);
    if (target < 0 || target >= this.model.cells.length) {
      this.leave(forward);
      return;
    }
    const offset = this.active?.editor.state.selection.main.head ?? 0;
    this.commitActive();
    queueMicrotask(() => this.focusCell(target, column, "preserve", offset));
  }

  private handle(kind: HandleKind, index: number): HTMLButtonElement {
    const handle = document.createElement("button");
    handle.type = "button";
    handle.className = `rich-table-handle rich-table-${kind}-handle`;
    handle.dataset.tableHandle = kind;
    handle.dataset.index = String(index);
    handle.dataset.removable = String((kind === "row"
      ? this.model.cells.length : this.model.alignments.length) > 1);
    /* Pointer dragging lets the real handle slide on its table axis without
     * also showing the browser's freely moving native drag ghost. */
    handle.draggable = false;
    handle.title = `Drag to reorder ${kind}`;
    handle.setAttribute("aria-label", `${kind === "row" ? "Row" : "Column"} ${index + 1} handle`);
    handle.append(dots());
    handle.addEventListener("dragstart", (event) => {
      this.startDrag(kind, index, handle);
      event.dataTransfer?.setData("text/plain", `${kind}:${index}`);
      if (event.dataTransfer) event.dataTransfer.effectAllowed = "move";
    });
    handle.addEventListener("dragover", (event) => {
      if (!this.drag || this.drag.kind !== kind) return;
      event.preventDefault();
      this.setDragTarget(index);
    });
    handle.addEventListener("drop", (event) => {
      event.preventDefault();
      this.setDragTarget(index);
      this.finishDrag();
    });
    handle.addEventListener("dragend", () => this.finishDrag());
    this.wirePointerDrag(handle, kind, index);
    return handle;
  }

  private startDrag(
    kind: HandleKind,
    index: number,
    handle: HTMLButtonElement,
    pointerCoordinate?: number,
  ): void {
    const origin = handle.getBoundingClientRect();
    const originStart = kind === "column" ? origin.left : origin.top;
    this.drag = {
      kind,
      from: index,
      slot: index,
      moved: false,
      handle,
      origin,
      grabOffset: pointerCoordinate == null
        ? null : pointerCoordinate - originStart,
      pointerCoordinate: pointerCoordinate ?? null,
    };
    this.root.classList.add("rich-table-is-dragging");
    handle.classList.add("rich-table-handle-dragging");
    this.updateDragVisual();
  }

  private wirePointerDrag(
    handle: HTMLButtonElement,
    kind: HandleKind,
    index: number,
  ): void {
    handle.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      this.cancelDrag();
      const startX = event.clientX;
      const startY = event.clientY;
      const pointerId = event.pointerId;
      let started = false;
      let cleaned = false;
      if (typeof handle.setPointerCapture === "function") {
        try {
          handle.setPointerCapture(pointerId);
        } catch {
          /* WebKit may have cancelled the pointer before capture is set. */
        }
      }
      const move = (moveEvent: PointerEvent) => {
        if (!started && Math.hypot(
          moveEvent.clientX - startX, moveEvent.clientY - startY,
        ) < 5) return;
        if (!started) {
          started = true;
          this.startDrag(
            kind,
            index,
            handle,
            kind === "column" ? startX : startY,
          );
        }
        moveEvent.preventDefault();
        if (this.drag) this.drag.pointerCoordinate = kind === "column"
          ? moveEvent.clientX : moveEvent.clientY;
        this.setDragSlot(this.slotAtPoint(kind, moveEvent.clientX, moveEvent.clientY));
      };
      const cleanup = () => {
        if (cleaned) return;
        cleaned = true;
        window.removeEventListener("pointermove", move, true);
        window.removeEventListener("pointerup", up, true);
        window.removeEventListener("pointercancel", cancel, true);
        window.removeEventListener("blur", cancel);
        handle.removeEventListener("lostpointercapture", cancel);
        if (typeof handle.hasPointerCapture === "function") {
          try {
            if (handle.hasPointerCapture(pointerId))
              handle.releasePointerCapture(pointerId);
          } catch {
            /* Pointer capture was already released by the platform. */
          }
        }
        if (this.pointerDragCleanup === cleanup)
          this.pointerDragCleanup = null;
      };
      const up = () => {
        cleanup();
        if (started) this.finishDrag();
      };
      const cancel = () => {
        cleanup();
        this.drag = null;
        this.clearDragVisual();
      };
      this.pointerDragCleanup = cleanup;
      window.addEventListener("pointermove", move, true);
      window.addEventListener("pointerup", up, true);
      window.addEventListener("pointercancel", cancel, true);
      window.addEventListener("blur", cancel);
      handle.addEventListener("lostpointercapture", cancel);
    });
  }

  private dragSlotElements(kind: HandleKind): HTMLElement[] {
    return kind === "column"
      ? [...this.root.querySelectorAll<HTMLElement>(
        ".rich-table-column-handle-cell",
      )]
      : [...this.root.querySelectorAll<HTMLElement>(
        ".rich-table-grid tr[data-row]",
      )];
  }

  private slotAtPoint(kind: HandleKind, x: number, y: number): number {
    const candidates = this.dragSlotElements(kind);
    const coordinate = kind === "column" ? x : y;
    for (let index = 0; index < candidates.length; index++) {
      const bounds = candidates[index].getBoundingClientRect();
      const midpoint = kind === "column"
        ? (bounds.left + bounds.right) / 2
        : (bounds.top + bounds.bottom) / 2;
      if (coordinate < midpoint) return index;
    }
    return candidates.length;
  }

  private setDragTarget(index: number): void {
    if (!this.drag || !Number.isFinite(index)) return;
    const slot = index > this.drag.from ? index + 1 : index;
    this.setDragSlot(slot);
  }

  private setDragSlot(slot: number): void {
    const drag = this.drag;
    if (!drag || !Number.isFinite(slot)) return;
    const count = drag.kind === "column"
      ? this.model.alignments.length : this.model.cells.length;
    drag.slot = Math.max(0, Math.min(count, slot));
    drag.moved = this.dragDestination(drag) !== drag.from;
    this.updateDragVisual();
  }

  private dragDestination(drag: DragState): number {
    return drag.slot > drag.from ? drag.slot - 1 : drag.slot;
  }

  private updateDragVisual(): void {
    const drag = this.drag;
    if (!drag) return;
    this.positionDragSelection(drag);

    /* Handles are transformed during the drag, so using their client rects as
     * bounds makes an edge handle move its own min/max and become clamped
     * there. Row/column slot elements never move and provide stable bounds. */
    const candidates = this.dragSlotElements(drag.kind);
    let distance: number | null = null;
    if (drag.pointerCoordinate != null && drag.grabOffset != null &&
        candidates.length) {
      const first = candidates[0].getBoundingClientRect();
      const last = candidates.at(-1)!.getBoundingClientRect();
      const originStart = drag.kind === "column"
        ? drag.origin.left : drag.origin.top;
      const size = drag.kind === "column"
        ? drag.origin.width : drag.origin.height;
      const minimum = drag.kind === "column" ? first.left : first.top;
      const maximum = (drag.kind === "column" ? last.right : last.bottom) - size;
      const desired = drag.pointerCoordinate - drag.grabOffset;
      distance = Math.max(minimum, Math.min(maximum, desired)) - originStart;
    } else {
      const target = candidates[this.dragDestination(drag)];
      if (target) {
        const bounds = target.getBoundingClientRect();
        distance = drag.kind === "column"
          ? bounds.left - drag.origin.left
          : bounds.top - drag.origin.top;
      }
    }
    if (distance != null) drag.handle.style.transform = drag.kind === "column"
      ? `translateX(${distance}px)` : `translateY(${distance}px)`;
    this.positionDropIndicator(drag);
  }

  private positionDragSelection(drag: DragState): void {
    let selection = this.root.querySelector<HTMLElement>(
      ".rich-table-drag-selection",
    );
    if (!selection) {
      selection = document.createElement("div");
      selection.className = "rich-table-drag-selection";
      this.root.append(selection);
    }
    const rootBounds = this.root.getBoundingClientRect();
    const grid = this.root.querySelector<HTMLElement>(".rich-table-grid");
    const target = drag.kind === "column"
      ? this.root.querySelector<HTMLElement>(
        `.rich-table-column-handle-cell[data-column="${drag.from}"]`,
      )
      : this.root.querySelector<HTMLElement>(
        `.rich-table-grid tr[data-row="${drag.from}"]`,
      );
    if (!grid || !target) return;
    const gridBounds = grid.getBoundingClientRect();
    const bounds = target.getBoundingClientRect();
    if (drag.kind === "column") {
      selection.style.left = `${bounds.left - rootBounds.left}px`;
      selection.style.top = `${gridBounds.top - rootBounds.top}px`;
      selection.style.width = `${bounds.width}px`;
      selection.style.height = `${gridBounds.height}px`;
    } else {
      selection.style.left = `${gridBounds.left - rootBounds.left}px`;
      selection.style.top = `${bounds.top - rootBounds.top}px`;
      selection.style.width = `${gridBounds.width}px`;
      selection.style.height = `${bounds.height}px`;
    }
  }

  private positionDropIndicator(drag: DragState): void {
    let indicator = this.root.querySelector<HTMLElement>(
      ".rich-table-drop-indicator",
    );
    if (!indicator) {
      indicator = document.createElement("div");
      indicator.className = "rich-table-drop-indicator";
      this.root.append(indicator);
    }
    indicator.classList.toggle("rich-table-drop-indicator-column",
      drag.kind === "column");
    indicator.classList.toggle("rich-table-drop-indicator-row",
      drag.kind === "row");

    const rootBounds = this.root.getBoundingClientRect();
    const grid = this.root.querySelector<HTMLElement>(".rich-table-grid");
    if (!grid) return;
    const gridBounds = grid.getBoundingClientRect();
    const candidates = this.dragSlotElements(drag.kind);
    const before = candidates[drag.slot];
    const after = candidates[drag.slot - 1];
    const boundary = before
      ? (drag.kind === "column" ? before.getBoundingClientRect().left
        : before.getBoundingClientRect().top)
      : after
        ? (drag.kind === "column" ? after.getBoundingClientRect().right
          : after.getBoundingClientRect().bottom)
        : (drag.kind === "column" ? gridBounds.left : gridBounds.top);
    if (drag.kind === "column") {
      indicator.style.left = `${boundary - rootBounds.left}px`;
      indicator.style.top = `${gridBounds.top - rootBounds.top}px`;
      indicator.style.height = `${gridBounds.height}px`;
      indicator.style.width = "2px";
    } else {
      indicator.style.left = `${gridBounds.left - rootBounds.left}px`;
      indicator.style.top = `${boundary - rootBounds.top}px`;
      indicator.style.width = `${gridBounds.width}px`;
      indicator.style.height = "2px";
    }
  }

  private clearDragVisual(): void {
    this.root.classList.remove("rich-table-is-dragging");
    this.root.querySelectorAll(".rich-table-handle-dragging")
      .forEach((element) => element.classList.remove(
        "rich-table-handle-dragging",
      ));
    this.root.querySelectorAll<HTMLElement>(".rich-table-handle")
      .forEach((handle) => handle.style.removeProperty("transform"));
    this.root.querySelector(".rich-table-drop-indicator")?.remove();
    this.root.querySelector(".rich-table-drag-selection")?.remove();
  }

  private cancelDrag(): void {
    const cleanup = this.pointerDragCleanup;
    this.pointerDragCleanup = null;
    cleanup?.();
    this.drag = null;
    this.clearDragVisual();
  }

  private finishDrag(): void {
    const cleanup = this.pointerDragCleanup;
    this.pointerDragCleanup = null;
    cleanup?.();
    const drag = this.drag;
    this.drag = null;
    this.clearDragVisual();
    if (!drag?.moved) return;
    const destination = this.dragDestination(drag);
    if (destination === drag.from) return;
    this.commitActive();
    const next = cloneMarkdownTable(this.model);
    if (drag.kind === "row") {
      const [row] = next.cells.splice(drag.from, 1);
      next.cells.splice(destination, 0, row);
    } else {
      for (const row of next.cells) {
        const [cell] = row.splice(drag.from, 1);
        row.splice(destination, 0, cell);
      }
      const [alignment] = next.alignments.splice(drag.from, 1);
      next.alignments.splice(destination, 0, alignment);
    }
    this.commitModel(next, `input.table-reorder-${drag.kind}`);
    /* A synchronous widget update may reuse the same handle DOM. Clear once
     * more after that update so no translated handle can remain at an edge. */
    queueMicrotask(() => this.clearDragVisual());
  }

  remove(kind: HandleKind, index: number): void {
    const count = kind === "row"
      ? this.model.cells.length : this.model.alignments.length;
    if (count <= 1) return;
    this.commitActive();
    const next = cloneMarkdownTable(this.model);
    if (kind === "row") next.cells.splice(index, 1);
    else {
      next.alignments.splice(index, 1);
      for (const row of next.cells) row.splice(index, 1);
    }
    this.commitModel(next, `input.table-remove-${kind}`);
  }
}

export class RichTableWidget extends WidgetType {
  readonly geometryContext = richTableGeometryContext;

  constructor(
    readonly source: string,
    readonly from: number,
    readonly to: number,
  ) { super(); }

  eq(other: RichTableWidget): boolean {
    return other.source === this.source && other.from === this.from &&
      other.to === this.to &&
      other.geometryContext.key === this.geometryContext.key;
  }

  private get geometryKey(): string {
    return `table:${this.source}`;
  }

  get estimatedHeight(): number {
    const model = parseMarkdownTable(this.source);
    if (!model) return -1;
    return cachedTableHeight(this.geometryContext, this.geometryKey) ??
      estimatedTableHeight(model, this.geometryContext);
  }

  toDOM(view: EditorView): HTMLElement {
    const root: ControlledTable = document.createElement("div");
    const model = parseMarkdownTable(this.source);
    if (!model) {
      root.className = "table-widget render-error";
      root.textContent = "Invalid Markdown table";
      return root;
    }
    new RichTableController(root, view, this.from, this.to, model);
    measureTableHeight(
      view, root, this.geometryContext, this.geometryKey,
    );
    return root;
  }

  updateDOM(dom: HTMLElement, view: EditorView): boolean {
    const controller = (dom as ControlledTable)[tableController];
    const model = parseMarkdownTable(this.source);
    if (!controller || !model) return false;
    controller.update(view, this.from, this.to, model);
    measureTableHeight(
      view, dom, this.geometryContext, this.geometryKey,
    );
    return true;
  }

  destroy(dom: HTMLElement): void {
    (dom as ControlledTable)[tableController]?.destroy();
  }

  ignoreEvent(): boolean { return true; }
}

function controllerFor(view: EditorView, from: number): RichTableController | null {
  const root = [...view.dom.querySelectorAll<ControlledTable>(
    ".rich-table-widget",
  )].find((candidate) => Number(candidate.dataset.hardPreviewFrom) === from);
  return root?.[tableController] ?? null;
}

export function removeRichTablePart(
  view: EditorView,
  from: number,
  kind: string,
  index: number,
): boolean {
  if ((kind !== "row" && kind !== "column") || !Number.isInteger(index))
    return false;
  const controller = controllerFor(view, from);
  if (!controller) return false;
  controller.remove(kind, index);
  return true;
}

export function focusRichTableCell(
  view: EditorView,
  from: number,
  row: number,
  column: number,
  edge: CellEdge = "start",
): boolean {
  const controller = controllerFor(view, from);
  if (controller) return controller.focusCell(row, column, edge);
  view.dispatch({ effects: EditorView.scrollIntoView(from, { y: "nearest" }) });
  const retry = (remaining: number) => {
    if (controllerFor(view, from)?.focusCell(row, column, edge) || !remaining)
      return;
    window.requestAnimationFrame(() => retry(remaining - 1));
  };
  queueMicrotask(() => retry(2));
  return true;
}

export function focusRichTableBoundary(
  view: EditorView,
  from: number,
  to: number,
  forward: boolean,
  vertical: boolean,
): boolean {
  const model = parseMarkdownTable(view.state.sliceDoc(from, to));
  if (!model) return false;
  const row = forward ? 0 : model.cells.length - 1;
  const column = vertical ? 0 : forward ? 0 : model.alignments.length - 1;
  return focusRichTableCell(
    view, from, row, column, forward ? "start" : "end",
  );
}
