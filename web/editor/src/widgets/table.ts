import { EditorSelection } from "@codemirror/state";
import { redo, undo } from "@codemirror/commands";
import { EditorView, WidgetType } from "@codemirror/view";
import {
  canonicalMarkdownTable,
  cloneMarkdownTable,
  escapeTypedCellPipes,
  parseMarkdownTable,
  tableCellMarkdownForRender,
  type MarkdownTable,
} from "../markdown/table";
import { renderMarkdownInline, wireRenderedContent } from "../markdown/render";
import { pinPreviewSource } from "../markdown/source-edit";

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

function putCaret(element: HTMLElement, edge: CellEdge, offset?: number): void {
  const selection = window.getSelection();
  if (!selection) return;
  const text = element.firstChild ?? element.appendChild(document.createTextNode(""));
  const length = text.textContent?.length ?? 0;
  const at = edge === "start" ? 0 : edge === "end" ? length
    : Math.max(0, Math.min(length, offset ?? length));
  const range = document.createRange();
  range.setStart(text, at);
  range.collapse(true);
  selection.removeAllRanges();
  selection.addRange(range);
}

function caretOffset(element: HTMLElement): number {
  const selection = window.getSelection();
  if (!selection?.focusNode || !element.contains(selection.focusNode)) return 0;
  const range = document.createRange();
  range.selectNodeContents(element);
  range.setEnd(selection.focusNode, selection.focusOffset);
  return range.toString().length;
}

function caretIsOnEdgeLine(element: HTMLElement, top: boolean): boolean {
  const selection = window.getSelection();
  if (!selection?.focusNode || !element.contains(selection.focusNode)) return true;
  const range = document.createRange();
  range.setStart(selection.focusNode, selection.focusOffset);
  range.collapse(true);
  const caret = typeof range.getClientRects === "function"
    ? range.getClientRects()[0] ?? range.getBoundingClientRect()
    : null;
  const bounds = element.getBoundingClientRect();
  if (!caret || (!caret.height && !bounds.height)) return true;
  const lineHeight = Number.parseFloat(getComputedStyle(element).lineHeight) ||
    Math.max(16, caret.height);
  return top
    ? caret.top <= bounds.top + lineHeight * 0.7
    : caret.bottom >= bounds.bottom - lineHeight * 0.7;
}

function insertPlainText(value: string): void {
  const selection = window.getSelection();
  if (!selection?.rangeCount) return;
  const range = selection.getRangeAt(0);
  range.deleteContents();
  const node = document.createTextNode(value);
  range.insertNode(node);
  range.setStartAfter(node);
  range.collapse(true);
  selection.removeAllRanges();
  selection.addRange(range);
}

interface ActiveCell {
  element: HTMLElement;
  row: number;
  column: number;
}

interface DragState {
  kind: HandleKind;
  from: number;
  to: number;
  moved: boolean;
}

class RichTableController {
  private active: ActiveCell | null = null;
  private drag: DragState | null = null;

  constructor(
    readonly root: ControlledTable,
    private view: EditorView,
    private from: number,
    private to: number,
    private model: MarkdownTable,
  ) {
    root[tableController] = this;
    this.render();
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
      this.active = null;
      this.render();
      return;
    }
    this.root.querySelectorAll<HTMLElement>(".rich-table-cell").forEach((cell) => {
      const row = Number(cell.dataset.row);
      const column = Number(cell.dataset.column);
      const alignment = this.model.alignments[column];
      if (cell.parentElement)
        cell.parentElement.style.textAlign = alignment === "none" ? "" : alignment;
      if (this.active?.element === cell) return;
      this.renderCell(cell, this.model.cells[row]?.[column] ?? "");
    });
  }

  private markRoot(): void {
    this.root.className = "table-widget rich-table-widget cm-hard-rendered-item";
    this.root.dataset.hardPreviewFrom = String(this.from);
    this.root.dataset.hardPreviewTo = String(this.to);
    this.root.setAttribute("aria-selected", "false");
  }

  private render(): void {
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
      holder.append(this.handle("column", column));
      handles.append(holder);
    }
    thead.append(handles, this.rowElement(0, true));
    table.append(thead);

    const tbody = document.createElement("tbody");
    for (let row = 1; row < this.model.cells.length; row++)
      tbody.append(this.rowElement(row, false));
    table.append(tbody);

    const addColumn = document.createElement("button");
    addColumn.type = "button";
    addColumn.className = "rich-table-add rich-table-add-column";
    addColumn.textContent = "+";
    addColumn.title = "Add column to the right";
    addColumn.setAttribute("aria-label", "Add column to the right");
    addColumn.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      this.commitActive();
      const next = cloneMarkdownTable(this.model);
      next.alignments.push("none");
      for (const row of next.cells) row.push("");
      this.commitModel(next, "input.table-add-column");
      queueMicrotask(() => this.focusCell(0, next.alignments.length - 1, "start"));
    });

    const addRow = document.createElement("button");
    addRow.type = "button";
    addRow.className = "rich-table-add rich-table-add-row";
    addRow.textContent = "+";
    addRow.title = "Add row below";
    addRow.setAttribute("aria-label", "Add row below");
    addRow.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      this.commitActive();
      const next = cloneMarkdownTable(this.model);
      next.cells.push(Array.from({ length: next.alignments.length }, () => ""));
      this.commitModel(next, "input.table-add-row");
      queueMicrotask(() => this.focusCell(next.cells.length - 1, 0, "start"));
    });

    this.root.append(table, source, addColumn, addRow);
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
      const editor = document.createElement("div");
      editor.className = "rich-table-cell";
      editor.dataset.row = String(row);
      editor.dataset.column = String(column);
      editor.tabIndex = -1;
      editor.setAttribute("role", "textbox");
      editor.setAttribute("aria-label", `Row ${row + 1}, column ${column + 1}`);
      editor.spellcheck = true;
      this.renderCell(editor, this.model.cells[row]?.[column] ?? "");
      editor.addEventListener("pointerdown", (event) => {
        if ((event as PointerEvent).button !== 0) return;
        this.activateCell(editor, row, column);
      }, true);
      editor.addEventListener("focus", () =>
        this.activateCell(editor, row, column));
      editor.addEventListener("blur", () => {
        if (this.active?.element === editor) this.commitActive(true);
      });
      editor.addEventListener("keydown", (event) =>
        this.cellKeyDown(event, editor, row, column));
      editor.addEventListener("input", () =>
        this.commitCellInput(editor, row, column));
      editor.addEventListener("paste", (event) => {
        event.preventDefault();
        insertPlainText((event.clipboardData?.getData("text/plain") ?? "")
          .replace(/\s*\n+\s*/g, " "));
        this.commitCellInput(editor, row, column);
      });
      cell.append(editor);
      tr.append(cell);
    }
    return tr;
  }

  private renderCell(element: HTMLElement, source: string): void {
    element.contentEditable = "false";
    element.innerHTML = renderMarkdownInline(tableCellMarkdownForRender(source));
    if (!element.textContent) element.append(document.createElement("br"));
    wireRenderedContent(element);
  }

  private activateCell(
    element: HTMLElement,
    row: number,
    column: number,
  ): void {
    if (this.active?.element === element) return;
    if (this.active && this.commitActive()) {
      queueMicrotask(() => this.focusCell(row, column, "preserve"));
      return;
    }
    this.active = { element, row, column };
    element.contentEditable = "plaintext-only";
    element.textContent = this.model.cells[row]?.[column] ?? "";
    element.classList.add("rich-table-cell-active");
  }

  focusCell(row: number, column: number, edge: CellEdge = "start"): boolean {
    row = Math.max(0, Math.min(this.model.cells.length - 1, row));
    column = Math.max(0, Math.min(this.model.alignments.length - 1, column));
    const cell = this.root.querySelector<HTMLElement>(
      `.rich-table-cell[data-row="${row}"][data-column="${column}"]`,
    );
    if (!cell) return false;
    const previous = this.active?.element === cell ? caretOffset(cell) : undefined;
    this.activateCell(cell, row, column);
    cell.focus({ preventScroll: true });
    putCaret(cell, edge, previous);
    cell.scrollIntoView?.({ block: "nearest", inline: "nearest" });
    return true;
  }

  private commitActive(render = true): boolean {
    const active = this.active;
    if (!active) return false;
    const raw = (active.element.textContent ?? "").replace(/\s*\n+\s*/g, " ");
    const value = escapeTypedCellPipes(raw);
    const current = this.model.cells[active.row]?.[active.column] ?? "";
    active.element.classList.remove("rich-table-cell-active");
    this.active = null;
    if (value === current) {
      if (render) this.renderCell(active.element, current);
      return false;
    }
    const next = cloneMarkdownTable(this.model);
    next.cells[active.row][active.column] = value;
    this.commitModel(next, "input.table-cell");
    return true;
  }

  private commitCellInput(
    element: HTMLElement,
    row: number,
    column: number,
  ): boolean {
    const raw = (element.textContent ?? "").replace(/\s*\n+\s*/g, " ");
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
    let index = row * this.model.alignments.length + column + (forward ? 1 : -1);
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
    const offset = this.active ? caretOffset(this.active.element) : 0;
    this.commitActive();
    queueMicrotask(() => {
      if (!this.focusCell(target, column, "start")) return;
      const cell = this.active?.element;
      if (cell) putCaret(cell, "preserve", offset);
    });
  }

  private cellKeyDown(
    event: KeyboardEvent,
    element: HTMLElement,
    row: number,
    column: number,
  ): void {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
      event.preventDefault();
      event.stopPropagation();
      this.commitActive();
      this.view.dispatch({
        selection: EditorSelection.range(0, this.view.state.doc.length),
        scrollIntoView: true,
        userEvent: "select",
      });
      this.view.focus();
      return;
    }
    if ((event.ctrlKey || event.metaKey) &&
        (event.key.toLowerCase() === "z" || event.key.toLowerCase() === "y")) {
      event.preventDefault();
      event.stopPropagation();
      this.commitCellInput(element, row, column);
      this.commitActive();
      const redoRequested = event.key.toLowerCase() === "y" || event.shiftKey;
      (redoRequested ? redo : undo)(this.view);
      queueMicrotask(() => this.focusCell(row, column, "end"));
      return;
    }
    if (event.key === "Tab") {
      event.preventDefault();
      event.stopPropagation();
      this.moveLinear(row, column, !event.shiftKey);
      return;
    }
    if (event.key === "Enter") {
      event.preventDefault();
      event.stopPropagation();
      if (row + 1 < this.model.cells.length)
        this.moveVertical(row, column, true);
      return;
    }
    const offset = caretOffset(element);
    const length = element.textContent?.length ?? 0;
    if (event.key === "ArrowLeft" && offset === 0) {
      event.preventDefault();
      event.stopPropagation();
      this.moveLinear(row, column, false);
    } else if (event.key === "ArrowRight" && offset === length) {
      event.preventDefault();
      event.stopPropagation();
      this.moveLinear(row, column, true);
    } else if (event.key === "ArrowUp" && caretIsOnEdgeLine(element, true)) {
      event.preventDefault();
      event.stopPropagation();
      this.moveVertical(row, column, false);
    } else if (event.key === "ArrowDown" && caretIsOnEdgeLine(element, false)) {
      event.preventDefault();
      event.stopPropagation();
      this.moveVertical(row, column, true);
    }
  }

  private handle(kind: HandleKind, index: number): HTMLButtonElement {
    const handle = document.createElement("button");
    handle.type = "button";
    handle.className = `rich-table-handle rich-table-${kind}-handle`;
    handle.dataset.tableHandle = kind;
    handle.dataset.index = String(index);
    handle.draggable = true;
    handle.title = `Drag to reorder ${kind}; right-click to remove`;
    handle.setAttribute("aria-label", `${kind === "row" ? "Row" : "Column"} ${index + 1} handle`);
    handle.append(dots());
    handle.addEventListener("contextmenu", (event) => {
      event.preventDefault();
      event.stopPropagation();
      this.remove(kind, index);
    });
    handle.addEventListener("dragstart", (event) => {
      this.drag = { kind, from: index, to: index, moved: false };
      event.dataTransfer?.setData("text/plain", `${kind}:${index}`);
      if (event.dataTransfer) event.dataTransfer.effectAllowed = "move";
      handle.classList.add("rich-table-handle-dragging");
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

  private wirePointerDrag(
    handle: HTMLButtonElement,
    kind: HandleKind,
    index: number,
  ): void {
    handle.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      const startX = event.clientX;
      const startY = event.clientY;
      this.drag = { kind, from: index, to: index, moved: false };
      const move = (moveEvent: PointerEvent) => {
        if (!this.drag) return;
        if (!this.drag.moved && Math.hypot(
          moveEvent.clientX - startX, moveEvent.clientY - startY,
        ) < 5) return;
        this.drag.moved = true;
        moveEvent.preventDefault();
        const target = document.elementFromPoint?.(
          moveEvent.clientX, moveEvent.clientY,
        )?.closest<HTMLElement>(`.rich-table-${kind}-handle`);
        if (target) this.setDragTarget(Number(target.dataset.index));
      };
      const up = () => {
        document.removeEventListener("pointermove", move);
        document.removeEventListener("pointerup", up);
        document.removeEventListener("pointercancel", cancel);
        this.finishDrag();
      };
      const cancel = () => {
        this.drag = null;
        this.clearDragTargets();
        document.removeEventListener("pointermove", move);
        document.removeEventListener("pointerup", up);
        document.removeEventListener("pointercancel", cancel);
      };
      document.addEventListener("pointermove", move);
      document.addEventListener("pointerup", up);
      document.addEventListener("pointercancel", cancel);
    });
  }

  private setDragTarget(index: number): void {
    if (!this.drag || !Number.isFinite(index)) return;
    this.drag.to = index;
    if (index !== this.drag.from) this.drag.moved = true;
    this.clearDragTargets();
    this.root.querySelector<HTMLElement>(
      `.rich-table-${this.drag.kind}-handle[data-index="${index}"]`,
    )?.classList.add("rich-table-handle-target");
  }

  private clearDragTargets(): void {
    this.root.querySelectorAll(".rich-table-handle-target, .rich-table-handle-dragging")
      .forEach((element) => element.classList.remove(
        "rich-table-handle-target", "rich-table-handle-dragging",
      ));
  }

  private finishDrag(): void {
    const drag = this.drag;
    this.drag = null;
    this.clearDragTargets();
    if (!drag?.moved || drag.from === drag.to) return;
    this.commitActive();
    const next = cloneMarkdownTable(this.model);
    if (drag.kind === "row") {
      const [row] = next.cells.splice(drag.from, 1);
      next.cells.splice(drag.to, 0, row);
    } else {
      for (const row of next.cells) {
        const [cell] = row.splice(drag.from, 1);
        row.splice(drag.to, 0, cell);
      }
      const [alignment] = next.alignments.splice(drag.from, 1);
      next.alignments.splice(drag.to, 0, alignment);
    }
    this.commitModel(next, `input.table-reorder-${drag.kind}`);
  }

  private remove(kind: HandleKind, index: number): void {
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
  constructor(
    readonly source: string,
    readonly from: number,
    readonly to: number,
  ) { super(); }

  eq(other: RichTableWidget): boolean {
    return other.source === this.source && other.from === this.from &&
      other.to === this.to;
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
    return root;
  }

  updateDOM(dom: HTMLElement, view: EditorView): boolean {
    const controller = (dom as ControlledTable)[tableController];
    const model = parseMarkdownTable(this.source);
    if (!controller || !model) return false;
    controller.update(view, this.from, this.to, model);
    return true;
  }

  ignoreEvent(): boolean { return true; }
}

function controllerFor(view: EditorView, from: number): RichTableController | null {
  const root = [...view.dom.querySelectorAll<ControlledTable>(
    ".rich-table-widget",
  )].find((candidate) => Number(candidate.dataset.hardPreviewFrom) === from);
  return root?.[tableController] ?? null;
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
