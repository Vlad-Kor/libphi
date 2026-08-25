import type { Completion, CompletionContext, CompletionResult } from "@codemirror/autocomplete";
import { requestNative } from "../bridge";

interface NativeCompletion {
  label?: string;
  name?: string;
  path?: string;
  detail?: string;
  target?: string;
}

const callouts = [
  "note", "abstract", "summary", "tldr", "info", "todo", "tip", "hint",
  "important", "success", "check", "done", "question", "help", "faq",
  "warning", "caution", "attention", "failure", "fail", "missing",
  "danger", "error", "bug", "example", "quote", "cite",
];

function asCompletion(value: NativeCompletion | string, type: string): Completion {
  const raw = typeof value === "string"
    ? value
    : value.target ?? value.label ?? value.name ?? value.path ?? "";
  const applied = type === "completion/files" ? raw.replace(/\.md$/i, "") : raw;
  const basename = applied.split("/").pop() ?? applied;
  const label = typeof value === "string"
    ? basename
    : value.label ?? value.name ?? basename;
  const detail = typeof value === "string"
    ? (applied === basename ? undefined : applied)
    : value.detail ?? value.path;
  return {
    label,
    detail,
    apply(view, _completion, from, to) {
      const suffix = view.state.sliceDoc(to, Math.min(to + 2, view.state.doc.length));
      const closing = suffix.startsWith("]]" ) ? "" : "]]";
      const insert = `${applied}${closing}`;
      view.dispatch({
        changes: { from, to, insert },
        selection: { anchor: from + insert.length },
        userEvent: "input.complete",
      });
    },
  };
}

export async function markdownCompletion(
  context: CompletionContext,
): Promise<CompletionResult | null> {
  const callout = context.matchBefore(/>\s*\[![\w-]*/);
  if (callout) {
    const typed = callout.text.split("!").pop() ?? "";
    return {
      from: callout.to - typed.length,
      options: callouts.map((label) => ({ label })),
      validFor: /^[\w-]*$/,
    };
  }

  const wiki = context.matchBefore(/!?\[\[[^\]\n]*/);
  if (!wiki || (!context.explicit && wiki.text.length < 2)) return null;
  const query = wiki.text.replace(/^!?\[\[/, "");
  const blockAt = query.lastIndexOf("#^");
  const headingAt = query.lastIndexOf("#");
  let type = "completion/files";
  let target = "";
  let search = query;
  let from = wiki.from + wiki.text.length - query.length;
  if (blockAt >= 0) {
    type = "completion/blocks";
    target = query.slice(0, blockAt);
    search = query.slice(blockAt + 2);
    from += blockAt + 2;
  } else if (headingAt >= 0) {
    type = "completion/headings";
    target = query.slice(0, headingAt);
    search = query.slice(headingAt + 1);
    from += headingAt + 1;
  }
  try {
    const result = await requestNative<(NativeCompletion | string)[]>(type, { query: search, target });
    return {
      from,
      options: (result ?? []).map((value) => asCompletion(value, type)),
      validFor: /^[^\]\n]*$/,
    };
  } catch {
    return { from, options: [] };
  }
}
