import { StateEffect, StateField } from "@codemirror/state";

export interface PreviewSourceRange {
  from: number;
  to: number;
}

export const pinPreviewSource = StateEffect.define<PreviewSourceRange>();

export const previewSourceRange = StateField.define<PreviewSourceRange | null>({
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
    const staysInSource = selection.empty
      ? selection.head >= current.from && selection.head <= current.to
      : selection.from >= current.from && selection.to <= current.to;
    return staysInSource ? current : null;
  },
});
