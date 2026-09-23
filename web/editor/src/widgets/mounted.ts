import type { EditorView } from "@codemirror/view";

/** Current start of mounted widget DOM, or null outside this view's content
 * (tooltips, measurement probes, nested table-cell editors, detached DOM).
 *
 * Preview widgets compare equal regardless of absolute document position, and
 * unchanged decorations are mapped through edits, so CodeMirror keeps widget
 * DOM while text above it changes. Positions captured when the DOM was built
 * are therefore stale; interaction code resolves them with this instead. */
export function mountedWidgetFrom(view: EditorView, element: Element): number | null {
  if (!element.isConnected || element.closest(".cm-content") !== view.contentDOM)
    return null;
  return view.posAtDOM(element);
}
