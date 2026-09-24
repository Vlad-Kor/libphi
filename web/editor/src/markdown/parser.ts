import {
  parseMarkdownTable,
  splitMarkdownTableRow,
} from "./table";

export type MarkdownNodeKind =
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
  | "inline-code"
  | "task"
  | "callout"
  | "blockquote"
  | "table"
  | "mermaid"
  | "code-block"
  | "html"
  | "horizontal-rule"
  | "list-item"
  | "tag"
  | "block-id";

export interface MarkdownNode {
  kind: MarkdownNodeKind;
  from: number;
  to: number;
  contentFrom?: number;
  contentTo?: number;
  text: string;
  meta?: Record<string, string | boolean | number>;
}

interface Range { from: number; to: number }

/** Membership index for protected source ranges. Parsing checks thousands of
 * candidate positions; a linear scan per check made large notes quadratic. */
class RangeIndex {
  private readonly starts: number[] = [];
  private readonly ends: number[] = [];

  constructor(ranges: readonly Range[]) {
    const sorted = ranges.filter((range) => range.from < range.to)
      .sort((left, right) => left.from - right.from);
    for (const range of sorted) {
      const last = this.ends.length - 1;
      if (last >= 0 && range.from <= this.ends[last])
        this.ends[last] = Math.max(this.ends[last], range.to);
      else {
        this.starts.push(range.from);
        this.ends.push(range.to);
      }
    }
  }

  has(position: number): boolean {
    let low = 0;
    let high = this.starts.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (this.starts[middle] <= position) low = middle + 1;
      else high = middle;
    }
    return low > 0 && position < this.ends[low - 1];
  }
}

const inside = (position: number, ranges: RangeIndex) => ranges.has(position);

function collectFencedCodeNodes(text: string): MarkdownNode[] {
  const nodes: MarkdownNode[] = [];
  const opener = /^( {0,3})(`{3,}|~{3,})([^\n]*)$/gm;
  let match: RegExpExecArray | null;
  while ((match = opener.exec(text))) {
    const from = match.index;
    const openingLineEnd = lineEnd(text, from);
    const marker = match[2][0];
    const markerLength = match[2].length;
    const contentFrom = openingLineEnd < text.length
      ? openingLineEnd + 1
      : openingLineEnd;
    let contentTo = text.length;
    let to = text.length;
    let complete = false;

    for (let lineFrom = contentFrom; lineFrom <= text.length;) {
      const lineTo = lineEnd(text, lineFrom);
      const closing = /^( {0,3})(`+|~+)[ \t]*$/.exec(
        text.slice(lineFrom, lineTo),
      );
      if (closing && closing[2][0] === marker &&
          closing[2].length >= markerLength) {
        contentTo = lineFrom;
        to = lineTo;
        complete = true;
        break;
      }
      if (lineTo >= text.length) break;
      lineFrom = lineTo + 1;
    }

    const language = match[3].trim().split(/\s+/, 1)[0].toLowerCase();
    nodes.push({
      kind: language === "mermaid" ? "mermaid" : "code-block",
      from,
      to,
      contentFrom,
      contentTo,
      text: language === "mermaid"
        ? text.slice(contentFrom, contentTo)
        : text.slice(from, to),
      meta: { language, incomplete: !complete, markerLength },
    });

    /* A fence owns everything through its closer or the end of the document,
     * so none of those lines can begin another fenced block. */
    opener.lastIndex = to < text.length ? to + 1 : text.length;
  }
  return nodes;
}

function collectInlineCodeNodes(
  text: string,
  fencedRanges: Range[],
): MarkdownNode[] {
  const nodes: MarkdownNode[] = [];
  /* Fenced ranges are produced in source order and never overlap. */
  let fence = 0;
  for (let lineFrom = 0; lineFrom <= text.length;) {
    const lineTo = lineEnd(text, lineFrom);
    while (fence < fencedRanges.length && fencedRanges[fence].to <= lineFrom)
      fence++;
    if (!(fence < fencedRanges.length &&
          lineTo >= fencedRanges[fence].from)) {
      let at = lineFrom;
      while (at < lineTo) {
        const open = text.indexOf("`", at);
        if (open < 0 || open >= lineTo) break;
        if (isMarkdownEscape(text, open)) {
          at = open + 1;
          continue;
        }

        let afterOpen = open + 1;
        while (afterOpen < lineTo && text[afterOpen] === "`") afterOpen++;
        const markerLength = afterOpen - open;
        let close = -1;
        let afterClose = -1;
        for (let candidate = afterOpen; candidate < lineTo;) {
          candidate = text.indexOf("`", candidate);
          if (candidate < 0 || candidate >= lineTo) break;
          if (isMarkdownEscape(text, candidate)) {
            candidate++;
            continue;
          }
          let runEnd = candidate + 1;
          while (runEnd < lineTo && text[runEnd] === "`") runEnd++;
          if (runEnd - candidate === markerLength) {
            close = candidate;
            afterClose = runEnd;
            break;
          }
          candidate = runEnd;
        }

        if (close >= 0) {
          nodes.push({
            kind: "inline-code",
            from: open,
            to: afterClose,
            contentFrom: afterOpen,
            contentTo: close,
            text: text.slice(afterOpen, close),
            meta: { incomplete: false, markerLength },
          });
          at = afterClose;
          continue;
        }

        /* Two adjacent markers are useful while authoring an empty code span.
         * Treat a marker-only even run at EOL as equal opening/closing halves;
         * otherwise an unmatched run previews through the end of its line. */
        if (afterOpen === lineTo && markerLength % 2 === 0) {
          const half = markerLength / 2;
          nodes.push({
            kind: "inline-code",
            from: open,
            to: lineTo,
            contentFrom: open + half,
            contentTo: open + half,
            text: "",
            meta: { incomplete: false, markerLength: half },
          });
        } else {
          nodes.push({
            kind: "inline-code",
            from: open,
            to: lineTo,
            contentFrom: afterOpen,
            contentTo: lineTo,
            text: text.slice(afterOpen, lineTo),
            meta: { incomplete: true, markerLength },
          });
        }
        break;
      }
    }
    if (lineTo >= text.length) break;
    lineFrom = lineTo + 1;
  }
  return nodes;
}

/* JavaScriptCore's regular-expression JIT does not compile lookbehind; such
 * patterns fall back to its interpreter and scanned whole notes about ten times
 * more slowly in WebKit than under V8. A leading `(?<!c)` is therefore checked
 * by hand: on rejection the search resumes one character later, exactly where
 * the engine would have tried next, so the matches are identical. */
function pushInline(
  nodes: MarkdownNode[],
  text: string,
  ranges: RangeIndex,
  pattern: RegExp,
  notAfter: string,
  kind: MarkdownNodeKind,
  openLength: number,
  closeLength = openLength,
): void {
  pattern.lastIndex = 0;
  let match: RegExpExecArray | null;
  while ((match = pattern.exec(text))) {
    const from = match.index;
    if (from > 0 && text[from - 1] === notAfter) {
      pattern.lastIndex = from + 1;
      continue;
    }
    const to = from + match[0].length;
    if (inside(from, ranges) || isMarkdownEscape(text, from) ||
        isMarkdownEscape(text, to - closeLength)) continue;
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

function tableSeparatorCandidate(text: string, from: number, to: number): boolean {
  let pipe = false;
  for (let at = from; at < to; at++) {
    const character = text[at];
    if (character === "|") pipe = true;
    else if (character !== " " && character !== "\t" && character !== "\r" &&
             character !== ":" && character !== "-") return false;
  }
  return pipe;
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

function collectMarkdownLinks(text: string, protectedIndex: RangeIndex): MarkdownNode[] {
  const nodes: MarkdownNode[] = [];
  for (let open = 0; open < text.length; open++) {
    if (text[open] !== "[" || isMarkdownEscape(text, open) ||
        text[open + 1] === "[" || inside(open, protectedIndex)) continue;
    const image = open > 0 && text[open - 1] === "!" &&
      !isMarkdownEscape(text, open - 1);
    const from = image ? open - 1 : open;
    if (inside(from, protectedIndex)) continue;

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

function collectDisplayMathNodes(
  text: string,
  protectedRanges: Range[],
  protectedIndex: RangeIndex,
): MarkdownNode[] {
  const nodes: MarkdownNode[] = [];
  const ranges = [...protectedRanges].sort((left, right) =>
    left.from - right.from || right.to - left.to);
  let rangeIndex = 0;
  let inlineDollar = false;
  let inlineParen = false;
  const interesting = /[\n$\\]/g;
  for (let delimiterFrom = 0; delimiterFrom < text.length;) {
    /* Only newlines, dollars, and backslashes change scanner state; every
     * other character merely advances. Landing inside a protected range is
     * handled below exactly as when stepping one character at a time. */
    const character = text.charCodeAt(delimiterFrom);
    if (character !== 10 && character !== 36 && character !== 92) {
      interesting.lastIndex = delimiterFrom;
      const next = interesting.exec(text);
      if (!next) break;
      delimiterFrom = next.index;
    }
    if (text[delimiterFrom] === "\n") {
      inlineDollar = false;
      inlineParen = false;
      delimiterFrom++;
      continue;
    }
    while (rangeIndex < ranges.length && ranges[rangeIndex].to <= delimiterFrom)
      rangeIndex++;
    const protectedRange = ranges[rangeIndex];
    if (protectedRange && delimiterFrom >= protectedRange.from) {
      const protectedNewline = text.indexOf("\n", delimiterFrom);
      if (protectedNewline >= 0 && protectedNewline < protectedRange.to) {
        inlineDollar = false;
        inlineParen = false;
      }
      delimiterFrom = protectedRange.to;
      continue;
    }
    if (isMarkdownEscape(text, delimiterFrom)) {
      delimiterFrom++;
      continue;
    }

    /* When two inline expressions touch, their close/open boundary is `$$`.
     * Consume the first dollar as the current inline closer, then reconsider
     * the second as the next inline opener instead of creating one display
     * node that can swallow source through a much later display delimiter. */
    if (inlineDollar) {
      if (text[delimiterFrom] === "$") inlineDollar = false;
      delimiterFrom++;
      continue;
    }
    if (inlineParen) {
      if (text.startsWith("\\)", delimiterFrom)) {
        inlineParen = false;
        delimiterFrom += 2;
      } else {
        delimiterFrom++;
      }
      continue;
    }

    if (text.startsWith("\\(", delimiterFrom)) {
      inlineParen = true;
      delimiterFrom += 2;
      continue;
    }

    const dollarDisplay = text.startsWith("$$", delimiterFrom) &&
      text[delimiterFrom - 1] !== "$" && text[delimiterFrom + 2] !== "$";
    const bracketDisplay = text.startsWith("\\[", delimiterFrom);
    if (!dollarDisplay && !bracketDisplay) {
      if (text[delimiterFrom] === "$") inlineDollar = true;
      delimiterFrom++;
      continue;
    }

    const opener = dollarDisplay ? "$$" : "\\[";
    const closer = dollarDisplay ? "$$" : "\\]";
    let close = text.indexOf(closer, delimiterFrom + opener.length);
    while (close >= 0) {
      const validDollarPair = closer !== "$$" ||
        (text[close - 1] !== "$" && text[close + 2] !== "$");
      if (validDollarPair && !isMarkdownEscape(text, close) &&
          !inside(close, protectedIndex)) break;
      close = text.indexOf(closer, close + closer.length);
    }
    if (close < 0) {
      delimiterFrom += opener.length;
      continue;
    }

    const lineFrom = text.lastIndexOf("\n", delimiterFrom - 1) + 1;
    const prefix = text.slice(lineFrom, delimiterFrom);
    const from = /^ {0,3}$/.test(prefix) ? lineFrom : delimiterFrom;
    let to = close + closer.length;
    const closingLineEnd = lineEnd(text, to);
    if (from === lineFrom && /^\s*$/.test(text.slice(to, closingLineEnd)))
      to = closingLineEnd;
    const contentFrom = delimiterFrom + opener.length;
    nodes.push({
      kind: "display-math",
      from,
      to,
      contentFrom,
      contentTo: close,
      text: text.slice(contentFrom, close).trim(),
    });
    delimiterFrom = to;
  }
  return nodes;
}

export function parseMarkdownNodes(text: string): MarkdownNode[] {
  const fencedCodeNodes = collectFencedCodeNodes(text);
  const fencedRanges = fencedCodeNodes.map(({ from, to }) => ({ from, to }));
  const inlineCodeNodes = collectInlineCodeNodes(text, fencedRanges);
  const nodes: MarkdownNode[] = [...fencedCodeNodes, ...inlineCodeNodes];
  const codeRanges = [...fencedCodeNodes, ...inlineCodeNodes]
    .map(({ from, to }) => ({ from, to }));
  const codeIndex = new RangeIndex(codeRanges);
  const htmlRanges: Range[] = [];
  const rawHtml = /<(span|div|kbd|details|summary|sup|sub|small|mark|table|thead|tbody|tr|th|td|iframe)(?:\s[^>]*)?>[\s\S]*?<\/\1\s*>/gi;
  for (const match of text.matchAll(rawHtml)) {
    if (!inside(match.index, codeIndex)) {
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

  const protectedIndex = new RangeIndex(protectedRanges);
  nodes.push(...collectDisplayMathNodes(text, protectedRanges, protectedIndex));
  const occupiedMath = nodes.filter((node) => node.kind === "display-math");
  /* Equivalent to `(?<!\\)(\$|\\\()([^\n]+?)(?<!\\)(\$|\\\))` without
   * lookbehind (see pushInline): the closer's condition is that the nonempty
   * content does not end in a backslash, and the opener's is checked here. */
  const inlineMath = /(\$|\\\()([^\n]*?[^\n\\])(\$|\\\))/g;
  let inlineMatch: RegExpExecArray | null;
  while ((inlineMatch = inlineMath.exec(text))) {
    const match = inlineMatch;
    const from = match.index;
    if (from > 0 && text[from - 1] === "\\") {
      inlineMath.lastIndex = from + 1;
      continue;
    }
    if (inside(from, protectedIndex)) continue;
    /* Display nodes are sorted and disjoint: the first one ending after
     * `from` is the only candidate that can overlap this match. */
    let low = 0;
    let high = occupiedMath.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (occupiedMath[middle].to <= from) low = middle + 1;
      else high = middle;
    }
    const candidate = occupiedMath[low];
    const display = candidate && from + match[0].length > candidate.from
      ? candidate : undefined;
    if (display) {
      inlineMath.lastIndex = Math.max(inlineMath.lastIndex, display.to);
      continue;
    }
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
    if (!inside(match.index, protectedIndex))
      nodes.push({ kind: "comment", from: match.index, to: match.index + match[0].length, text: match[0] });
  }

  const wiki = /(!)?\[\[([^\]\n]+)\]\]/g;
  for (const match of text.matchAll(wiki)) {
    if (inside(match.index, protectedIndex)) continue;
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

  nodes.push(...collectMarkdownLinks(text, protectedIndex));

  const footnoteDefinitions = new Map<string, number>();
  const definitionPattern = /^\[\^([^\]\n]+)\]:[ \t]*([^\n]*(?:\n(?: {4}|\t)[^\n]*)*)/gm;
  for (const match of text.matchAll(definitionPattern)) {
    if (inside(match.index, protectedIndex)) continue;
    footnoteDefinitions.set(match[1], match.index);
    nodes.push({
      kind: "footnote-definition",
      from: match.index,
      to: match.index + match[0].length,
      text: match[2].replace(/\n(?: {4}|\t)/g, "\n"),
      meta: { id: match[1] },
    });
  }
  const reference = /\[\^([^\]\n]+)\]/g;
  for (let match; (match = reference.exec(text));) {
    if (match.index > 0 && text[match.index - 1] === "!") {
      reference.lastIndex = match.index + 1;
      continue;
    }
    if (inside(match.index, protectedIndex)) continue;
    nodes.push({
      kind: "footnote-reference",
      from: match.index,
      to: match.index + match[0].length,
      text: match[1],
      meta: { id: match[1], definition: footnoteDefinitions.get(match[1]) ?? match.index },
    });
  }
  for (const match of text.matchAll(/\^\[([^\]\n]+)\]/g)) {
    if (inside(match.index, protectedIndex)) continue;
    nodes.push({
      kind: "inline-footnote",
      from: match.index,
      to: match.index + match[0].length,
      text: match[1],
    });
  }

  const calloutRanges: Range[] = [];
  const callout = /^ {0,3}>[ \t]*\[!([^\]\s]+)\]([+-])?([^\n]*)$/gm;
  for (const match of text.matchAll(callout)) {
    if (inside(match.index, protectedIndex)) continue;
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
    calloutRanges.push({ from: match.index, to });
  }

  const calloutIndex = new RangeIndex(calloutRanges);

  /* Callouts are the conspicuous special case of Markdown blockquotes and
   * retain priority above. Render every other contiguous run of quoted lines
   * through MarkdownIt so nested quotes and inline Markdown keep their normal
   * semantics. */
  const blockquote = /^ {0,3}>.*$/gm;
  let blockquoteMatch: RegExpExecArray | null;
  while ((blockquoteMatch = blockquote.exec(text))) {
    const from = blockquoteMatch.index;
    if (inside(from, protectedIndex) || inside(from, calloutIndex)) continue;
    let to = lineEnd(text, from);
    while (to < text.length) {
      const nextFrom = to + 1;
      const nextTo = lineEnd(text, nextFrom);
      if (inside(nextFrom, calloutIndex) ||
          !/^ {0,3}>/.test(text.slice(nextFrom, nextTo))) break;
      to = nextTo;
    }
    nodes.push({
      kind: "blockquote",
      from,
      to,
      text: text.slice(from, to),
    });
    blockquote.lastIndex = to;
  }

  /* Tables need a line-wise parser rather than a regular expression. Besides
   * accepting outer-pipe-free GFM rows, this keeps escaped/content pipes from
   * becoming false columns and leaves adjacent blank lines outside the hard
   * replacement range. */
  for (let from = 0; from < text.length;) {
    const headerTo = lineEnd(text, from);
    if (headerTo >= text.length || inside(from, protectedIndex)) {
      from = headerTo + 1;
      continue;
    }
    const separatorFrom = headerTo + 1;
    const separatorTo = lineEnd(text, separatorFrom);
    /* A delimiter row consists only of pipes, colons, dashes, and blanks,
     * and the header needs a pipe. Reject other line pairs before running the
     * full row lexer, which previously dominated parsing of every note. */
    if (!tableSeparatorCandidate(text, separatorFrom, separatorTo) ||
        text.lastIndexOf("|", headerTo) < from) {
      from = headerTo + 1;
      continue;
    }
    const firstTwo = text.slice(from, separatorTo);
    const initial = parseMarkdownTable(firstTwo);
    if (!initial || inside(separatorFrom, protectedIndex)) {
      from = headerTo + 1;
      continue;
    }
    let to = separatorTo;
    while (to < text.length) {
      const rowFrom = to + 1;
      const rowTo = lineEnd(text, rowFrom);
      const line = text.slice(rowFrom, rowTo);
      if (!line.trim() || inside(rowFrom, protectedIndex) ||
          !splitMarkdownTableRow(line)) break;
      to = rowTo;
    }
    const source = text.slice(from, to);
    if (parseMarkdownTable(source))
      nodes.push({ kind: "table", from, to, text: source });
    from = to < text.length ? to + 1 : text.length;
  }

  for (const match of text.matchAll(/^ {0,3}(?:-[ \t]*){3,}$/gm)) {
    if (!inside(match.index, protectedIndex))
      nodes.push({ kind: "horizontal-rule", from: match.index,
        to: match.index + match[0].length, text: match[0] });
  }

  for (const match of text.matchAll(/^(#{1,6})[ \t]+(.+)$/gm)) {
    if (!inside(match.index, protectedIndex)) {
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

  for (const match of text.matchAll(
    /^([ \t]*)(?:([-+*])|(\d+)([.)]))([ \t]+)/gm,
  )) {
    if (inside(match.index, protectedIndex)) continue;
    const markerFrom = match.index + match[1].length;
    const marker = match[2] ?? `${match[3]}${match[4]}`;
    const to = lineEnd(text, markerFrom);
    const markerContentFrom = match.index + match[0].length;
    const taskMatch = match[2]
      ? /^\[([^\]])\]([ \t]*)/.exec(text.slice(markerContentFrom, to))
      : null;
    const contentFrom = markerContentFrom + (taskMatch
      ? 3 + Math.min(1, taskMatch[2].length) : 0);
    nodes.push({
      kind: "list-item",
      from: match.index,
      to,
      text: marker,
      meta: {
        marker,
        markerFrom,
        markerTo: markerFrom + marker.length,
        contentFrom,
        indentColumns: [...match[1]].reduce(
          (columns, character) => character === "\t"
            ? columns + (4 - columns % 4) : columns + 1,
          0,
        ),
        ordered: Boolean(match[3]),
        number: Number(match[3] ?? 0),
        task: Boolean(taskMatch),
        taskFrom: markerContentFrom,
      },
    });
  }

  for (const match of text.matchAll(
    /^[ \t]*[-*+][ \t]+\[([^\]])\]([ \t]*)/gm,
  )) {
    if (!inside(match.index, protectedIndex)) {
      const marker = match[0].lastIndexOf("[");
      const from = match.index + marker;
      // The checkbox supplies one separator visually; extra source whitespace
      // remains editable and visible, including for checked tasks.
      const to = from + 3 + Math.min(1, match[2].length);
      nodes.push({
        kind: "task",
        from,
        to,
        text: match[1],
        meta: {
          status: match[1],
          prefixFrom: match.index,
          prefixTo: to,
        },
      });
    }
  }

  pushInline(nodes, text, protectedIndex, /\*\*([^\n*]|\*(?!\*))+?\*\*(?!\*)/g, "*", "strong", 2);
  pushInline(nodes, text, protectedIndex, /__([^\n_]|_(?!_))+?__(?!_)/g, "_", "strong", 2);
  pushInline(nodes, text, protectedIndex, /~~[^\n~]+~~(?!~)/g, "~", "strike", 2);
  pushInline(nodes, text, protectedIndex, /==[^\n=]+==(?!\=)/g, "=", "highlight", 2);
  pushInline(nodes, text, protectedIndex, /\*[^\n*]+\*(?!\*)/g, "*", "emphasis", 1);
  pushInline(nodes, text, protectedIndex, /_[^\n_]+_(?!_)/g, "_", "emphasis", 1);

  for (const match of text.matchAll(/(^|[\s(])#([\p{L}\p{N}_-]+(?:\/[\p{L}\p{N}_-]+)*)/gmu)) {
    const from = match.index + match[1].length;
    if (!inside(from, protectedIndex))
      nodes.push({ kind: "tag", from, to: from + match[0].length - match[1].length, text: match[2] });
  }
  for (const match of text.matchAll(/(?:^|\s)\^([A-Za-z0-9-]+)(?=\s*$)/gm)) {
    const from = match.index + (match[0].startsWith("^") ? 0 : 1);
    nodes.push({ kind: "block-id", from, to: from + match[1].length + 1, text: match[1] });
  }

  return nodes.sort((left, right) => left.from - right.from || right.to - left.to);
}

export function selectionTouches(
  node: Pick<MarkdownNode, "from" | "to">,
  selection: { from: number; to: number },
): boolean {
  if (selection.from === selection.to)
    return selection.from >= node.from && selection.from < node.to;
  return selection.from < node.to && selection.to > node.from;
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

export interface OpenMath {
  mode: MathMode;
  /** Offset of the delimiter that opened the math at `position`; null when
   * `mode` is "none". */
  from: number | null;
  delimiter: "$" | "$$" | "\\(" | "\\[" | null;
}

export function mathModeAt(text: string, position: number,
                           knownCodeMode?: CodeMode): MathMode {
  return openMathAt(text, position, knownCodeMode).mode;
}

/** The snippet engine's view of math at `position`: whether it is inside math
 * and which delimiter opened it. Unlike the Markdown parser, this counts a
 * delimiter that has not been closed yet. */
export function openMathAt(text: string, position: number,
                           knownCodeMode?: CodeMode): OpenMath {
  position = Math.max(0, Math.min(position, text.length));
  const start = Math.max(0, position - 32768);
  if ((knownCodeMode ?? codeModeAt(text, position)) !== "none")
    return { mode: "none", from: null, delimiter: null };

  let display: "dollar" | "bracket" | null = null;
  let inlineDollar = false;
  let inlineParen = false;
  let openedAt = 0;
  /* Only newlines, dollars, and backslashes change the state; skip everything
   * else. This scan runs for automatic snippets on every key in math and
   * cost milliseconds per 32 KiB in WebKit when stepping every character. */
  const interesting = /[\n$\\]/g;
  for (let at = start; at < position; at++) {
    const code = text.charCodeAt(at);
    if (code !== 10 && code !== 36 && code !== 92) {
      interesting.lastIndex = at;
      const next = interesting.exec(text);
      if (!next || next.index >= position) break;
      at = next.index;
    }
    /* Inline delimiters cannot span source lines. Check this before escape
     * handling so even a Markdown hard-break backslash ends inline state. */
    if (!display && text[at] === "\n") {
      inlineDollar = false;
      inlineParen = false;
      continue;
    }
    if (isEscaped(text, at)) continue;

    if (display === "dollar") {
      if (text.startsWith("$$", at)) {
        display = null;
        at++;
      }
      continue;
    }
    if (display === "bracket") {
      if (text.startsWith("\\]", at)) {
        display = null;
        at++;
      }
      continue;
    }

    if (inlineDollar) {
      /* In `...}$${...`, the first dollar closes one inline expression and
       * the second opens the next. Do not reinterpret that boundary as a
       * display-math delimiter. */
      if (text[at] === "$") inlineDollar = false;
      continue;
    }
    if (inlineParen) {
      if (text.startsWith("\\)", at)) {
        inlineParen = false;
        at++;
      }
      continue;
    }

    openedAt = at;
    if (text.startsWith("$$", at)) {
      display = "dollar";
      at++;
    } else if (text.startsWith("\\[", at)) {
      display = "bracket";
      at++;
    } else if (text[at] === "$") {
      inlineDollar = true;
    } else if (text.startsWith("\\(", at)) {
      inlineParen = true;
      at++;
    }
  }
  if (display)
    return { mode: "display", from: openedAt, delimiter: display === "dollar" ? "$$" : "\\[" };
  if (inlineDollar) return { mode: "inline", from: openedAt, delimiter: "$" };
  if (inlineParen) return { mode: "inline", from: openedAt, delimiter: "\\(" };
  return { mode: "none", from: null, delimiter: null };
}
