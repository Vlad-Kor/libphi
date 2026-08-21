export type ObsidianNodeKind =
  | "frontmatter"
  | "heading"
  | "emphasis"
  | "strong"
  | "strike"
  | "highlight"
  | "comment"
  | "wikilink"
  | "embed"
  | "markdown-link"
  | "markdown-image"
  | "footnote-reference"
  | "inline-footnote"
  | "footnote-definition"
  | "math"
  | "display-math"
  | "task"
  | "callout"
  | "table"
  | "mermaid"
  | "html"
  | "tag"
  | "block-id";

export interface ObsidianNode {
  kind: ObsidianNodeKind;
  from: number;
  to: number;
  contentFrom?: number;
  contentTo?: number;
  text: string;
  meta?: Record<string, string | boolean | number>;
}

interface Range { from: number; to: number }

const inside = (position: number, ranges: Range[]) =>
  ranges.some((range) => position >= range.from && position < range.to);

function collectCodeRanges(text: string): Range[] {
  const ranges: Range[] = [];
  const fence = /^( {0,3})(`{3,}|~{3,})[^\n]*\n[\s\S]*?^\1\2[ \t]*$/gm;
  for (const match of text.matchAll(fence))
    ranges.push({ from: match.index, to: match.index + match[0].length });
  const inline = /(`+)([^\n]*?)\1/g;
  for (const match of text.matchAll(inline)) {
    if (!inside(match.index, ranges))
      ranges.push({ from: match.index, to: match.index + match[0].length });
  }
  return ranges;
}

function pushInline(
  nodes: ObsidianNode[],
  text: string,
  ranges: Range[],
  pattern: RegExp,
  kind: ObsidianNodeKind,
  openLength: number,
  closeLength = openLength,
): void {
  for (const match of text.matchAll(pattern)) {
    const from = match.index;
    const to = from + match[0].length;
    if (inside(from, ranges)) continue;
    nodes.push({
      kind,
      from,
      to,
      contentFrom: from + openLength,
      contentTo: to - closeLength,
      text: match[0],
    });
  }
}

function lineEnd(text: string, from: number): number {
  const newline = text.indexOf("\n", from);
  return newline < 0 ? text.length : newline;
}

export function parseObsidian(text: string): ObsidianNode[] {
  const nodes: ObsidianNode[] = [];
  const codeRanges = collectCodeRanges(text);
  const htmlRanges: Range[] = [];
  const rawHtml = /<(span|div|kbd|details|summary|sup|sub|small|mark|table|thead|tbody|tr|th|td)(?:\s[^>]*)?>[\s\S]*?<\/\1\s*>/gi;
  for (const match of text.matchAll(rawHtml)) {
    if (!inside(match.index, codeRanges)) {
      htmlRanges.push({ from: match.index, to: match.index + match[0].length });
      nodes.push({ kind: "html", from: match.index, to: match.index + match[0].length, text: match[0] });
    }
  }
  const protectedRanges = [...codeRanges, ...htmlRanges];

  const frontmatter = /^(---|\{)\s*\n/.exec(text);
  if (frontmatter) {
    const pattern = frontmatter[1] === "---" ? /^---[ \t]*$/gm : /^\}[ \t]*$/gm;
    pattern.lastIndex = frontmatter[0].length;
    const end = pattern.exec(text);
    if (end) {
      const to = lineEnd(text, end.index);
      nodes.push({ kind: "frontmatter", from: 0, to, text: text.slice(0, to) });
    }
  }

  const fenced = /^( {0,3})(`{3,}|~{3,})([^\n]*)\n([\s\S]*?)^\1\2[ \t]*$/gm;
  for (const match of text.matchAll(fenced)) {
    if (match[3].trim().toLowerCase() === "mermaid") {
      nodes.push({
        kind: "mermaid",
        from: match.index,
        to: match.index + match[0].length,
        text: match[4],
      });
    }
  }

  const displayMath = /(^|\n)(\$\$|\\\[)\s*\n?([\s\S]*?)\n?(\$\$|\\\])(?=\n|$)/g;
  for (const match of text.matchAll(displayMath)) {
    const from = match.index + match[1].length;
    if (inside(from, protectedRanges)) continue;
    const to = match.index + match[0].length;
    nodes.push({
      kind: "display-math",
      from,
      to,
      contentFrom: from + match[2].length,
      contentTo: to - match[4].length,
      text: match[3].trim(),
    });
  }
  const occupiedMath = nodes.filter((node) => node.kind === "display-math");
  const inlineMath = /(?<!\\)(\$|\\\()([^\n]+?)(?<!\\)(\$|\\\))/g;
  for (const match of text.matchAll(inlineMath)) {
    const from = match.index;
    if (inside(from, protectedRanges) || inside(from, occupiedMath)) continue;
    if ((match[1] === "$" && match[3] !== "$") ||
        (match[1] === "\\(" && match[3] !== "\\)")) continue;
    nodes.push({
      kind: "math",
      from,
      to: from + match[0].length,
      contentFrom: from + match[1].length,
      contentTo: from + match[0].length - match[3].length,
      text: match[2],
    });
  }

  const comments = /%%[\s\S]*?%%/g;
  for (const match of text.matchAll(comments)) {
    if (!inside(match.index, protectedRanges))
      nodes.push({ kind: "comment", from: match.index, to: match.index + match[0].length, text: match[0] });
  }

  const wiki = /(!)?\[\[([^\]\n]+)\]\]/g;
  for (const match of text.matchAll(wiki)) {
    if (inside(match.index, protectedRanges)) continue;
    const raw = match[2];
    const separator = raw.lastIndexOf("|");
    let target = separator >= 0 ? raw.slice(0, separator) : raw;
    if (separator >= 0 && target.endsWith("\\")) target = target.slice(0, -1);
    const alias = separator >= 0 ? raw.slice(separator + 1) : target.split(/[#^]/)[0];
    nodes.push({
      kind: match[1] ? "embed" : "wikilink",
      from: match.index,
      to: match.index + match[0].length,
      text: raw,
      meta: { target, alias },
    });
  }

  const markdownLinks = /(!)?\[([^\]\n]*)\]\((<?[^\s)>]+>?)(?:\s+["'][^\n]*?["'])?\)/g;
  for (const match of text.matchAll(markdownLinks)) {
    if (inside(match.index, protectedRanges)) continue;
    const target = match[3].replace(/^<|>$/g, "");
    nodes.push({
      kind: match[1] ? "markdown-image" : "markdown-link",
      from: match.index,
      to: match.index + match[0].length,
      text: match[0],
      meta: { target, alias: match[2] },
    });
  }

  const footnoteDefinitions = new Map<string, number>();
  const definitionPattern = /^\[\^([^\]\n]+)\]:[ \t]*([^\n]*(?:\n(?: {4}|\t)[^\n]*)*)/gm;
  for (const match of text.matchAll(definitionPattern)) {
    if (inside(match.index, protectedRanges)) continue;
    footnoteDefinitions.set(match[1], match.index);
    nodes.push({
      kind: "footnote-definition",
      from: match.index,
      to: match.index + match[0].length,
      text: match[2].replace(/\n(?: {4}|\t)/g, "\n"),
      meta: { id: match[1] },
    });
  }
  for (const match of text.matchAll(/(?<!!)\[\^([^\]\n]+)\]/g)) {
    if (inside(match.index, protectedRanges)) continue;
    nodes.push({
      kind: "footnote-reference",
      from: match.index,
      to: match.index + match[0].length,
      text: match[1],
      meta: { id: match[1], definition: footnoteDefinitions.get(match[1]) ?? match.index },
    });
  }
  for (const match of text.matchAll(/\^\[([^\]\n]+)\]/g)) {
    if (inside(match.index, protectedRanges)) continue;
    nodes.push({
      kind: "inline-footnote",
      from: match.index,
      to: match.index + match[0].length,
      text: match[1],
    });
  }

  const callout = /^(\s*>\s*\[!([^\]\s]+)\]([+-])?([^\n]*)(?:\n(?:\s*>.*|\s*)*)?)/gm;
  for (const match of text.matchAll(callout)) {
    if (inside(match.index, protectedRanges)) continue;
    const body = match[1].split("\n").slice(1).map((line) => line.replace(/^\s*>\s?/, "")).join("\n");
    nodes.push({
      kind: "callout",
      from: match.index,
      to: match.index + match[1].length,
      text: body,
      meta: {
        type: match[2].toLowerCase(),
        fold: match[3] ?? "",
        title: match[4].trim(),
      },
    });
  }

  const table = /^(\s*\|?.+\|.+\n\s*\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?\s*\n(?:.*\|.*(?:\n|$))+)/gm;
  for (const match of text.matchAll(table)) {
    if (!inside(match.index, protectedRanges))
      nodes.push({ kind: "table", from: match.index, to: match.index + match[1].trimEnd().length, text: match[1].trimEnd() });
  }

  for (const match of text.matchAll(/^(#{1,6})[ \t]+(.+)$/gm)) {
    if (!inside(match.index, protectedRanges)) {
      nodes.push({
        kind: "heading",
        from: match.index,
        to: match.index + match[0].length,
        contentFrom: match.index + match[1].length + 1,
        contentTo: match.index + match[0].length,
        text: match[2],
        meta: { level: match[1].length },
      });
    }
  }

  for (const match of text.matchAll(/^\s*[-*+]\s+\[([^\]])\]/gm)) {
    if (!inside(match.index, protectedRanges)) {
      const marker = match[0].lastIndexOf("[");
      nodes.push({
        kind: "task",
        from: match.index + marker,
        to: match.index + marker + 3,
        text: match[1],
        meta: { status: match[1] },
      });
    }
  }

  pushInline(nodes, text, protectedRanges, /(?<!\*)\*\*([^\n*]|\*(?!\*))+?\*\*(?!\*)/g, "strong", 2);
  pushInline(nodes, text, protectedRanges, /(?<!_)__([^\n_]|_(?!_))+?__(?!_)/g, "strong", 2);
  pushInline(nodes, text, protectedRanges, /(?<!~)~~[^\n~]+~~(?!~)/g, "strike", 2);
  pushInline(nodes, text, protectedRanges, /(?<!=)==[^\n=]+==(?!\=)/g, "highlight", 2);
  pushInline(nodes, text, protectedRanges, /(?<!\*)\*[^\n*]+\*(?!\*)/g, "emphasis", 1);
  pushInline(nodes, text, protectedRanges, /(?<!_)_[^\n_]+_(?!_)/g, "emphasis", 1);

  for (const match of text.matchAll(/(^|[\s(])#([\p{L}\p{N}_-]+(?:\/[\p{L}\p{N}_-]+)*)/gmu)) {
    const from = match.index + match[1].length;
    if (!inside(from, protectedRanges))
      nodes.push({ kind: "tag", from, to: from + match[0].length - match[1].length, text: match[2] });
  }
  for (const match of text.matchAll(/(?:^|\s)\^([A-Za-z0-9-]+)(?=\s*$)/gm)) {
    const from = match.index + (match[0].startsWith("^") ? 0 : 1);
    nodes.push({ kind: "block-id", from, to: from + match[1].length + 1, text: match[1] });
  }

  return nodes.sort((left, right) => left.from - right.from || right.to - left.to);
}

export function selectionTouches(
  node: Pick<ObsidianNode, "from" | "to">,
  selection: { from: number; to: number },
): boolean {
  return selection.from <= node.to && selection.to >= node.from;
}

export function inMath(text: string, position: number): boolean {
  return mathModeAt(text, position) !== "none";
}

export type MathMode = "none" | "inline" | "display";

function isEscaped(text: string, position: number): boolean {
  let slashes = 0;
  for (let at = position - 1; at >= 0 && text[at] === "\\"; at--) slashes++;
  return slashes % 2 === 1;
}

export type CodeMode = "none" | "inline" | "block";

export function codeModeAt(text: string, position: number): CodeMode {
  const start = Math.max(0, position - 32768);
  const prefix = text.slice(start, position);
  let fence: { marker: string; length: number } | null = null;
  for (const line of prefix.split("\n")) {
    const match = /^ {0,3}(`{3,}|~{3,})/.exec(line);
    if (!match) continue;
    if (!fence) fence = { marker: match[1][0], length: match[1].length };
    else if (match[1][0] === fence.marker && match[1].length >= fence.length) fence = null;
  }
  if (fence) return "block";

  const lineStart = text.lastIndexOf("\n", position - 1) + 1;
  const line = text.slice(lineStart, position);
  let delimiter = 0;
  for (let at = 0; at < line.length;) {
    if (line[at] !== "`") { at++; continue; }
    let end = at + 1;
    while (line[end] === "`") end++;
    const length = end - at;
    delimiter = delimiter === length ? 0 : delimiter || length;
    at = end;
  }
  return delimiter !== 0 ? "inline" : "none";
}

export function mathModeAt(text: string, position: number): MathMode {
  position = Math.max(0, Math.min(position, text.length));
  const start = Math.max(0, position - 32768);
  if (codeModeAt(text, position) !== "none") return "none";

  let display: "dollar" | "bracket" | null = null;
  for (let at = start; at < position; at++) {
    if (isEscaped(text, at)) continue;
    if (text.startsWith("$$", at)) {
      if (!display) display = "dollar";
      else if (display === "dollar") display = null;
      at++;
    } else if (text.startsWith("\\[", at) && !display) {
      display = "bracket";
      at++;
    } else if (text.startsWith("\\]", at) && display === "bracket") {
      display = null;
      at++;
    }
  }
  if (display) return "display";

  const lineStart = text.lastIndexOf("\n", position - 1) + 1;
  let inlineDollar = false;
  let inlineParen = false;
  for (let at = lineStart; at < position; at++) {
    if (isEscaped(text, at)) continue;
    if (text.startsWith("$$", at)) { at++; continue; }
    if (text[at] === "$") inlineDollar = !inlineDollar;
    else if (text.startsWith("\\(", at)) { inlineParen = true; at++; }
    else if (text.startsWith("\\)", at)) { inlineParen = false; at++; }
  }
  return inlineDollar || inlineParen ? "inline" : "none";
}
