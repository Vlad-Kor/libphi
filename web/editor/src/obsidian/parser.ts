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
  | "code-block"
  | "html"
  | "horizontal-rule"
  | "list-item"
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

function isMarkdownEscape(text: string, position: number): boolean {
  let slashes = 0;
  for (let at = position - 1; at >= 0 && text[at] === "\\"; at--) slashes++;
  return slashes % 2 === 1;
}

function markdownDestination(raw: string): string {
  const value = raw.trim();
  if (value.startsWith("<")) {
    const close = value.indexOf(">");
    if (close > 0) return value.slice(1, close);
  }
  let escaped = false;
  for (let at = 0; at < value.length; at++) {
    const character = value[at];
    if (escaped) { escaped = false; continue; }
    if (character === "\\") { escaped = true; continue; }
    if (/\s/.test(character)) return value.slice(0, at);
  }
  return value;
}

function collectMarkdownLinks(text: string, protectedRanges: Range[]): ObsidianNode[] {
  const nodes: ObsidianNode[] = [];
  for (let open = 0; open < text.length; open++) {
    if (text[open] !== "[" || isMarkdownEscape(text, open) ||
        text[open + 1] === "[" || inside(open, protectedRanges)) continue;
    const image = open > 0 && text[open - 1] === "!" &&
      !isMarkdownEscape(text, open - 1);
    const from = image ? open - 1 : open;
    if (inside(from, protectedRanges)) continue;

    let depth = 1;
    let close = open + 1;
    for (; close < text.length && text[close] !== "\n"; close++) {
      if (isMarkdownEscape(text, close)) continue;
      if (text[close] === "[") depth++;
      else if (text[close] === "]" && --depth === 0) break;
    }
    if (depth !== 0 || text[close + 1] !== "(") continue;

    let parentheses = 1;
    let end = close + 2;
    let angle = false;
    for (; end < text.length && text[end] !== "\n"; end++) {
      if (isMarkdownEscape(text, end)) continue;
      if (text[end] === "<" && parentheses === 1) angle = true;
      else if (text[end] === ">" && angle) angle = false;
      else if (!angle && text[end] === "(") parentheses++;
      else if (!angle && text[end] === ")" && --parentheses === 0) break;
    }
    if (parentheses !== 0) continue;

    const label = text.slice(open + 1, close);
    const target = markdownDestination(text.slice(close + 2, end));
    if (!target) continue;
    nodes.push({
      kind: image ? "markdown-image" : "markdown-link",
      from,
      to: end + 1,
      text: text.slice(from, end + 1),
      meta: { target, alias: label },
    });
  }
  return nodes;
}

export function parseObsidian(text: string): ObsidianNode[] {
  const nodes: ObsidianNode[] = [];
  const codeRanges = collectCodeRanges(text);
  const htmlRanges: Range[] = [];
  const rawHtml = /<(span|div|kbd|details|summary|sup|sub|small|mark|table|thead|tbody|tr|th|td|iframe)(?:\s[^>]*)?>[\s\S]*?<\/\1\s*>/gi;
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
      protectedRanges.push({ from: 0, to });
    }
  }

  const fenced = /^( {0,3})(`{3,}|~{3,})([^\n]*)\n([\s\S]*?)^\1\2[ \t]*$/gm;
  for (const match of text.matchAll(fenced)) {
    const language = match[3].trim().split(/\s+/, 1)[0].toLowerCase();
    nodes.push({
      kind: language === "mermaid" ? "mermaid" : "code-block",
      from: match.index,
      to: match.index + match[0].length,
      text: language === "mermaid" ? match[4] : match[0],
      meta: { language },
    });
  }

  const displayOpener = /^( {0,3})(\$\$|\\\[)[ \t]*(.*)$/gm;
  let displayMatch: RegExpExecArray | null;
  while ((displayMatch = displayOpener.exec(text))) {
    const from = displayMatch.index;
    if (inside(from, protectedRanges)) continue;
    const opener = displayMatch[2];
    const closer = opener === "$$" ? "$$" : "\\]";
    const openingLineEnd = lineEnd(text, from);
    const sameLine = displayMatch[3];
    if (sameLine.endsWith(closer) && sameLine.length > closer.length) {
      const content = sameLine.slice(0, -closer.length).trim();
      nodes.push({
        kind: "display-math",
        from,
        to: openingLineEnd,
        contentFrom: from + displayMatch[1].length + opener.length,
        contentTo: openingLineEnd - closer.length,
        text: content,
      });
      displayOpener.lastIndex = openingLineEnd;
      continue;
    }
    const closingPattern = new RegExp(
      `^ {0,3}${closer === "$$" ? "\\$\\$" : "\\\\\\]"}[ \\t]*$`, "gm",
    );
    closingPattern.lastIndex = openingLineEnd < text.length ? openingLineEnd + 1 : openingLineEnd;
    const closing = closingPattern.exec(text);
    if (!closing) continue;
    const contentFrom = Math.min(openingLineEnd + 1, text.length);
    const contentTo = closing.index;
    const to = lineEnd(text, closing.index);
    nodes.push({
      kind: "display-math",
      from,
      to,
      contentFrom,
      contentTo,
      text: text.slice(contentFrom, contentTo).trim(),
    });
    displayOpener.lastIndex = to;
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
    let target = (separator >= 0 ? raw.slice(0, separator) : raw).trim();
    if (separator >= 0 && target.endsWith("\\")) target = target.slice(0, -1);
    const alias = separator >= 0 ? raw.slice(separator + 1).trim() : target.split(/[#^]/)[0];
    nodes.push({
      kind: match[1] ? "embed" : "wikilink",
      from: match.index,
      to: match.index + match[0].length,
      text: raw,
      meta: { target, alias },
    });
  }

  nodes.push(...collectMarkdownLinks(text, protectedRanges));

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

  const callout = /^ {0,3}>[ \t]*\[!([^\]\s]+)\]([+-])?([^\n]*)$/gm;
  for (const match of text.matchAll(callout)) {
    if (inside(match.index, protectedRanges)) continue;
    let to = lineEnd(text, match.index);
    const bodyLines: string[] = [];
    while (to < text.length) {
      const nextFrom = to + 1;
      const nextTo = lineEnd(text, nextFrom);
      const line = text.slice(nextFrom, nextTo);
      if (!/^ {0,3}>/.test(line)) break;
      bodyLines.push(line.replace(/^ {0,3}>[ \t]?/, ""));
      to = nextTo;
    }
    nodes.push({
      kind: "callout",
      from: match.index,
      to,
      text: bodyLines.join("\n"),
      meta: {
        type: match[1].toLowerCase(),
        fold: match[2] ?? "",
        title: match[3].trim(),
      },
    });
  }

  const table = /^(\s*\|?.+\|.+\n\s*\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?\s*\n(?:.*\|.*(?:\n|$))+)/gm;
  for (const match of text.matchAll(table)) {
    if (!inside(match.index, protectedRanges))
      nodes.push({ kind: "table", from: match.index, to: match.index + match[1].trimEnd().length, text: match[1].trimEnd() });
  }

  for (const match of text.matchAll(/^ {0,3}(?:-[ \t]*){3,}$/gm)) {
    if (!inside(match.index, protectedRanges))
      nodes.push({ kind: "horizontal-rule", from: match.index,
        to: match.index + match[0].length, text: match[0] });
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

  for (const match of text.matchAll(/^([ \t]*)([-+*])(?=[ \t]+)/gm)) {
    if (inside(match.index, protectedRanges)) continue;
    const markerFrom = match.index + match[1].length;
    const to = lineEnd(text, markerFrom);
    nodes.push({
      kind: "list-item",
      from: match.index,
      to,
      text: match[2],
      meta: {
        markerFrom,
        indentColumns: [...match[1]].reduce(
          (columns, character) => character === "\t"
            ? columns + (4 - columns % 4) : columns + 1,
          0,
        ),
        task: /^\s*\[[^\]]\]/.test(text.slice(markerFrom + 1, to)),
      },
    });
  }

  for (const match of text.matchAll(/^[ \t]*[-*+][ \t]+\[([^\]])\]/gm)) {
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
  if (selection.from === selection.to)
    return selection.from >= node.from && selection.from < node.to;
  return selection.from < node.to && selection.to > node.from;
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
