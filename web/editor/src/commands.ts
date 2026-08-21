import { EditorSelection } from "@codemirror/state";
import type { Command } from "@codemirror/view";
import { undo, redo } from "@codemirror/commands";
import { foldCode, unfoldCode } from "@codemirror/language";

function surround(open: string, close = open, placeholder = ""): Command {
  return (view) => {
    const transaction = view.state.changeByRange((range) => {
      const selected = view.state.sliceDoc(range.from, range.to);
      const body = selected || placeholder;
      const insert = `${open}${body}${close}`;
      const anchor = range.from + open.length;
      return {
        changes: { from: range.from, to: range.to, insert },
        range: selected
          ? EditorSelection.range(anchor, anchor + selected.length)
          : EditorSelection.range(anchor, anchor + placeholder.length),
      };
    });
    view.dispatch({ ...transaction, userEvent: "input" });
    view.focus();
    return true;
  };
}

function prefixLines(prefix: string, ordered = false): Command {
  return (view) => {
    const changes: { from: number; insert: string }[] = [];
    for (const range of view.state.selection.ranges) {
      const first = view.state.doc.lineAt(range.from).number;
      const last = view.state.doc.lineAt(range.to).number;
      for (let number = first; number <= last; number++) {
        const line = view.state.doc.line(number);
        changes.push({ from: line.from, insert: ordered ? `${number - first + 1}. ` : prefix });
      }
    }
    view.dispatch({ changes, userEvent: "input" });
    view.focus();
    return true;
  };
}

function heading(level: number): Command {
  return (view) => {
    const changes = view.state.selection.ranges.map((range) => {
      const line = view.state.doc.lineAt(range.head);
      const current = /^(#{1,6})\s+/.exec(line.text);
      return {
        from: line.from,
        to: line.from + (current?.[0].length ?? 0),
        insert: `${"#".repeat(level)} `,
      };
    });
    view.dispatch({ changes, userEvent: "input" });
    return true;
  };
}

const commands: Record<string, Command> = {
  "editor.undo": undo,
  "editor.redo": redo,
  "editor.bold": surround("**"),
  "editor.italic": surround("*"),
  "editor.strikethrough": surround("~~"),
  "editor.highlight": surround("=="),
  "editor.inlineCode": surround("`"),
  "editor.blockquote": prefixLines("> "),
  "editor.bulletList": prefixLines("- "),
  "editor.numberedList": prefixLines("", true),
  "editor.taskList": prefixLines("- [ ] "),
  "editor.link": surround("[", "]()", "link text"),
  "editor.wikilink": surround("[[", "]]"),
  "editor.callout": prefixLines("> "),
  "editor.inlineMath": surround("$"),
  "editor.displayMath": surround("$$\n", "\n$$"),
  "editor.codeBlock": surround("```\n", "\n```"),
  "editor.horizontalRule": (view) => {
    view.dispatch(view.state.replaceSelection("\n---\n"));
    return true;
  },
  "editor.fold": foldCode,
  "editor.unfold": unfoldCode,
};

for (let level = 1; level <= 6; level++) commands[`editor.heading${level}`] = heading(level);

export function runEditingCommand(id: string, view: Parameters<Command>[0]): boolean {
  return commands[id]?.(view) ?? false;
}

export const formattingKeymap = [
  { key: "Mod-b", run: commands["editor.bold"] },
  { key: "Mod-i", run: commands["editor.italic"] },
  { key: "Mod-k", run: commands["editor.link"] },
];
