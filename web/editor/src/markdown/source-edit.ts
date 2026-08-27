import { StateEffect, StateField } from "@codemirror/state";

export interface PreviewSourceLine {
  from: number;
  to: number;
}

export const pinPreviewSource = StateEffect.define<PreviewSourceLine>();

export const previewSourceLine = StateField.define<PreviewSourceLine | null>({
  create: () => null,
  update(value, transaction) {
    let current = value && {
      from: transaction.changes.mapPos(value.from, -1),
      to: transaction.changes.mapPos(value.to, 1),
    };
    for (const effect of transaction.effects) {
      if (effect.is(pinPreviewSource)) current = effect.value;
    }
    if (!current) return null;

    const selection = transaction.state.selection.main;
    const line = transaction.state.doc.lineAt(
      Math.min(current.from, transaction.state.doc.length),
    );
    const staysOnLine = selection.empty
      ? transaction.state.doc.lineAt(selection.head).number === line.number
      : selection.from >= line.from &&
        selection.to <= Math.min(transaction.state.doc.length, line.to + 1);
    return staysOnLine ? { from: line.from, to: line.to } : null;
  },
});
