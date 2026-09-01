import { StateEffect, StateField } from "@codemirror/state";

export interface PreviewSourceRange {
  from: number;
  to: number;
}

export const pinPreviewSource = StateEffect.define<PreviewSourceRange | null>();

export const previewSourceRange = StateField.define<PreviewSourceRange | null>({
  create: () => null,
  update(value, transaction) {
    let lineBreakInsertedAtEnd = false;
    if (value && transaction.docChanged) {
      transaction.changes.iterChanges((fromA, toA, _fromB, _toB, inserted) => {
        if (fromA === value.to && toA === value.to && inserted.lines > 1)
          lineBreakInsertedAtEnd = true;
      });
    }
    let current = value && {
      from: transaction.changes.mapPos(value.from, -1),
      /* Typing at the closing boundary normally remains part of the pinned
       * edit. Enter is different: its new line belongs after the source, so
       * keep the old endpoint before that insertion and let the caret leave. */
      to: transaction.changes.mapPos(
        value.to, lineBreakInsertedAtEnd ? -1 : 1,
      ),
    };
    for (const effect of transaction.effects) {
      if (effect.is(pinPreviewSource)) current = effect.value;
    }
    if (!current) return null;

    const selection = transaction.state.selection.main;
    const staysInSource = selection.empty
      ? selection.head >= current.from && selection.head <= current.to
      : selection.from >= current.from && selection.to <= current.to;
    return staysInSource ? current : null;
  },
});
