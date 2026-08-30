export type TableAlignment = "none" | "left" | "center" | "right";

export interface MarkdownTable {
  /** The first row is always the Markdown header row. */
  cells: string[][];
  alignments: TableAlignment[];
}

interface RowParts {
  cells: string[];
  separators: number;
}

function escapedAt(source: string, position: number): boolean {
  let slashes = 0;
  for (let at = position - 1; at >= 0 && source[at] === "\\"; at--)
    slashes++;
  return slashes % 2 === 1;
}

function markerRun(source: string, position: number, marker: string): number {
  let end = position;
  while (source[end] === marker) end++;
  return end - position;
}

function unescapedIndexOf(source: string, token: string, from: number): number {
  for (let at = source.indexOf(token, from); at >= 0;
       at = source.indexOf(token, at + token.length)) {
    if (!escapedAt(source, at)) return at;
  }
  return -1;
}

/**
 * Locate pipes which are table separators rather than inline content.
 *
 * Markdown table implementations commonly split a row with one regular
 * expression. That loses escaped pipes and pipes in authoring constructs.
 * This small lexer deliberately understands the inline forms Phi supports in
 * rich cells: code spans, dollar/parenthesized math, links/wikilinks, and
 * emphasis. It retains the exact cell source for later inline rendering.
 */
function structuralPipes(source: string): number[] {
  const pipes: number[] = [];
  let codeTicks = 0;
  let math: "$" | "$$" | "\\(" | "\\[" | null = null;
  let brackets = 0;
  let linkParens = 0;
  let emphasis: string | null = null;

  for (let at = 0; at < source.length;) {
    const character = source[at];
    if (character === "`" && !escapedAt(source, at)) {
      const run = markerRun(source, at, "`");
      if (!codeTicks) codeTicks = run;
      else if (codeTicks === run) codeTicks = 0;
      at += run;
      continue;
    }
    if (codeTicks) { at++; continue; }

    if (math) {
      const closer = math === "$" ? "$"
        : math === "$$" ? "$$"
          : math === "\\(" ? "\\)" : "\\]";
      if (source.startsWith(closer, at) && !escapedAt(source, at)) {
        math = null;
        at += closer.length;
      } else at++;
      continue;
    }
    if (!escapedAt(source, at)) {
      if (source.startsWith("$$", at) &&
          unescapedIndexOf(source, "$$", at + 2) >= 0) {
        math = "$$";
        at += 2;
        continue;
      }
      if ((source.startsWith("\\(", at) &&
           unescapedIndexOf(source, "\\)", at + 2) >= 0) ||
          (source.startsWith("\\[", at) &&
           unescapedIndexOf(source, "\\]", at + 2) >= 0)) {
        math = source.slice(at, at + 2) as "\\(" | "\\[";
        at += 2;
        continue;
      }
      if (character === "$" &&
          unescapedIndexOf(source, "$", at + 1) >= 0) {
        math = "$";
        at++;
        continue;
      }
    }

    if (!escapedAt(source, at)) {
      if (character === "[") brackets++;
      else if (character === "]" && brackets) brackets--;
      else if (character === "(" && at > 0 && source[at - 1] === "]")
        linkParens++;
      else if (character === "(" && linkParens) linkParens++;
      else if (character === ")" && linkParens) linkParens--;
    }
    if (brackets || linkParens) { at++; continue; }

    if (!escapedAt(source, at) && (character === "*" || character === "_" ||
        (character === "~" && source[at + 1] === "~"))) {
      const length = character === "~" ? 2 : Math.min(2,
        markerRun(source, at, character));
      const marker = source.slice(at, at + length);
      if (emphasis === marker) emphasis = null;
      else if (!emphasis && source.indexOf(marker, at + length) >= 0)
        emphasis = marker;
      at += length;
      continue;
    }
    if (emphasis) { at++; continue; }

    if (character === "|" && !escapedAt(source, at)) pipes.push(at);
    at++;
  }
  return pipes;
}

function rowParts(line: string): RowParts {
  const first = line.search(/\S/);
  if (first < 0) return { cells: [""], separators: 0 };
  const last = line.search(/\s*$/) - 1;
  let pipes = structuralPipes(line);
  const leading = pipes[0] === first;
  const trailing = pipes.at(-1) === last;
  if (leading) pipes = pipes.slice(1);
  if (trailing) pipes = pipes.slice(0, -1);

  const from = leading ? first + 1 : first;
  const to = trailing ? last : last + 1;
  const cells: string[] = [];
  let cursor = from;
  for (const pipe of pipes) {
    cells.push(line.slice(cursor, pipe).trim());
    cursor = pipe + 1;
  }
  cells.push(line.slice(cursor, to).trim());
  return {
    cells,
    separators: pipes.length + Number(leading) + Number(trailing),
  };
}

export function splitMarkdownTableRow(line: string): string[] | null {
  const parts = rowParts(line);
  return parts.separators ? parts.cells : null;
}

function separatorAlignment(value: string): TableAlignment | null {
  const match = /^(:)?(-{3,})(:)?$/.exec(value.trim());
  if (!match) return null;
  if (match[1] && match[3]) return "center";
  if (match[1]) return "left";
  if (match[3]) return "right";
  return "none";
}

export function parseMarkdownTable(source: string): MarkdownTable | null {
  const lines = source.replace(/\r\n?/g, "\n").split("\n");
  if (lines.length < 2) return null;
  const header = rowParts(lines[0]);
  const separator = rowParts(lines[1]);
  if (!header.separators || !separator.separators ||
      header.cells.length !== separator.cells.length) return null;
  const alignments = separator.cells.map(separatorAlignment);
  if (alignments.some((alignment) => alignment == null)) return null;

  const columns = header.cells.length;
  const cells = [header.cells];
  for (const line of lines.slice(2)) {
    if (!line.trim()) return null;
    const row = rowParts(line);
    if (!row.separators) return null;
    cells.push(Array.from({ length: columns }, (_, column) =>
      row.cells[column] ?? ""));
  }
  return { cells, alignments: alignments as TableAlignment[] };
}

function minimumSeparatorWidth(alignment: TableAlignment): number {
  return alignment === "center" ? 5
    : alignment === "left" || alignment === "right" ? 4 : 3;
}

function separatorSource(alignment: TableAlignment, width: number): string {
  width = Math.max(width, minimumSeparatorWidth(alignment));
  if (alignment === "left") return `:${"-".repeat(width - 1)}`;
  if (alignment === "right") return `${"-".repeat(width - 1)}:`;
  if (alignment === "center") return `:${"-".repeat(width - 2)}:`;
  return "-".repeat(width);
}

function monospaceWidth(source: string): number {
  let width = 0;
  for (const character of source) {
    const point = character.codePointAt(0)!;
    if (/\p{Mark}/u.test(character) || point === 0x200d ||
        point === 0xfe0e || point === 0xfe0f) continue;
    const wide = point >= 0x1100 && (
      point <= 0x115f || point === 0x2329 || point === 0x232a ||
      (point >= 0x2e80 && point <= 0xa4cf && point !== 0x303f) ||
      (point >= 0xac00 && point <= 0xd7a3) ||
      (point >= 0xf900 && point <= 0xfaff) ||
      (point >= 0xfe10 && point <= 0xfe19) ||
      (point >= 0xfe30 && point <= 0xfe6f) ||
      (point >= 0xff00 && point <= 0xff60) ||
      (point >= 0xffe0 && point <= 0xffe6) ||
      (point >= 0x1f300 && point <= 0x1faff) ||
      (point >= 0x20000 && point <= 0x3fffd)
    );
    width += wide ? 2 : 1;
  }
  return width;
}

export function canonicalMarkdownTable(table: MarkdownTable): string {
  const columns = Math.max(1, table.alignments.length,
    ...table.cells.map((row) => row.length));
  const rows = table.cells.length ? table.cells : [[]];
  const alignments = Array.from({ length: columns }, (_, column) =>
    table.alignments[column] ?? "none" as TableAlignment);
  const normalized = rows.map((row) => Array.from(
    { length: columns }, (_, column) => (row[column] ?? "").trim(),
  ));
  const widths = Array.from({ length: columns }, (_, column) => Math.max(
    minimumSeparatorWidth(alignments[column]),
    ...normalized.map((row) => monospaceWidth(row[column])),
  ));
  const format = (row: string[]) => `| ${row.map((cell, column) =>
    `${cell}${" ".repeat(Math.max(0,
      widths[column] - monospaceWidth(cell)))}`).join(" | ")} |`;
  const separator = alignments.map((alignment, column) =>
    separatorSource(alignment, widths[column]));
  return [format(normalized[0]), format(separator),
    ...normalized.slice(1).map(format)].join("\n");
}

export function newMarkdownTable(rows: number, columns: number): MarkdownTable {
  rows = Math.max(1, Math.floor(rows));
  columns = Math.max(1, Math.floor(columns));
  return {
    cells: Array.from({ length: rows }, () =>
      Array.from({ length: columns }, () => "")),
    alignments: Array.from({ length: columns }, () => "none"),
  };
}

export function escapeTypedCellPipes(source: string): string {
  const structural = new Set(structuralPipes(source));
  let value = "";
  for (let at = 0; at < source.length; at++) {
    if (source[at] === "|" && structural.has(at)) value += "\\";
    value += source[at];
  }
  return value;
}

/** Markdown-it's table rule removes the table escape before parsing a code
 * span, whereas renderInline (used by the rich cell) intentionally does not.
 * Mirror that one table-specific step without touching LaTeX `\|` commands. */
export function tableCellMarkdownForRender(source: string): string {
  let value = "";
  for (let at = 0; at < source.length;) {
    if (source[at] !== "`" || escapedAt(source, at)) {
      value += source[at++];
      continue;
    }
    const length = markerRun(source, at, "`");
    const marker = "`".repeat(length);
    const close = source.indexOf(marker, at + length);
    if (close < 0) {
      value += source.slice(at);
      break;
    }
    value += marker + source.slice(at + length, close)
      .replace(/\\\|/g, "|") + marker;
    at = close + length;
  }
  return value;
}

export function cloneMarkdownTable(table: MarkdownTable): MarkdownTable {
  return {
    cells: table.cells.map((row) => [...row]),
    alignments: [...table.alignments],
  };
}
