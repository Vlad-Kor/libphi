import { insertBracket } from "@codemirror/autocomplete";
import { EditorSelection, Prec, type EditorState, type Transaction } from "@codemirror/state";
import { EditorView } from "@codemirror/view";

const closingFor: Record<string, string> = {
  "(": ")",
  "[": "]",
  "{": "}",
};

function unescapedQuotes(value: string): number {
  let count = 0;
  for (let index = 0; index < value.length; index++) {
    if (value[index] !== '"') continue;
    let escapes = 0;
    for (let at = index - 1; at >= 0 && value[at] === "\\"; at--)
      escapes++;
    if (escapes % 2 === 0) count++;
  }
  return count;
}

/** Handles the cases where the language-agnostic close-brackets extension
 * lacks enough Markdown context. Returning null delegates to CodeMirror's
 * normal tracked-pair behavior. */
export function smartPairTransaction(
  state: EditorState,
  from: number,
  to: number,
  insert: string,
): Transaction | null {
  if (state.readOnly || from !== to ||
      state.selection.ranges.length !== 1 || insert.length !== 1)
    return null;

  const close = closingFor[insert];
  if (close && state.sliceDoc(from, from + close.length) === close) {
    /* Reuse an authored closer. A closer recorded by closeBrackets belongs to
     * an outer pair, so leave that case to CodeMirror to preserve nesting. */
    if (insertBracket(state, close)) return null;
    return state.update({
      changes: { from, insert },
      selection: EditorSelection.cursor(from + insert.length),
      scrollIntoView: true,
      userEvent: "input.type",
    });
  }

  if (insert === '"' && state.sliceDoc(from, from + 1) !== '"') {
    const line = state.doc.lineAt(from);
    if (unescapedQuotes(state.sliceDoc(line.from, from)) % 2 === 1) {
      return state.update({
        changes: { from, insert },
        selection: EditorSelection.cursor(from + 1),
        scrollIntoView: true,
        userEvent: "input.type",
      });
    }
  }
  return null;
}

export const smartPairs = Prec.high(EditorView.inputHandler.of(
  (view, from, to, insert) => {
    const transaction = smartPairTransaction(view.state, from, to, insert);
    if (!transaction) return false;
    view.dispatch(transaction);
    return true;
  },
));
