import { describe, expect, it } from "vitest";
import { parseMarkdownNodes } from "../src/markdown/parser";
import {
  canonicalMarkdownTable,
  escapeTypedCellPipes,
  newMarkdownTable,
  parseMarkdownTable,
  tableCellMarkdownForRender,
} from "../src/markdown/table";

describe("rich Markdown tables", () => {
  it("parses irregular GFM rows and content pipes without false columns", () => {
    const source = [
      "Name | Expression | Link",
      ":--- | :---: | ---:",
      "plain \\| pipe | `a|b` and $x|y$ | [left|right](https://example.test/a|b)",
    ].join("\n");
    const table = parseMarkdownTable(source);
    expect(table?.alignments).toEqual(["left", "center", "right"]);
    expect(table?.cells).toEqual([
      ["Name", "Expression", "Link"],
      ["plain \\| pipe", "`a|b` and $x|y$",
        "[left|right](https://example.test/a|b)"],
    ]);
    const node = parseMarkdownNodes(`${source}\n\nOutside`)
      .find((candidate) => candidate.kind === "table");
    expect(node?.text).toBe(source);
    expect(parseMarkdownTable(
      "Cost $5 | Quantity\n--- | ---\n$10 | 2",
    )?.cells).toEqual([["Cost $5", "Quantity"], ["$10", "2"]]);
  });

  it("canonicalizes spacing while preserving separator alignment", () => {
    const table = parseMarkdownTable(
      "A|Long heading|R\n:---|:---:|---:\nx|y|z",
    );
    expect(table).not.toBeNull();
    const canonical = canonicalMarkdownTable(table!);
    expect(canonical).toBe([
      "| A    | Long heading | R    |",
      "| :--- | :----------: | ---: |",
      "| x    | y            | z    |",
    ].join("\n"));
    const lines = canonical.split("\n");
    expect(lines.map((line) => [...line.matchAll(/\|/g)].map((match) =>
      match.index))).toEqual([
      [0, 7, 22, 29],
      [0, 7, 22, 29],
      [0, 7, 22, 29],
    ]);
  });

  it("escapes newly typed plain pipes but retains supported inline pipes", () => {
    expect(escapeTypedCellPipes(
      "plain | `code|pipe` $math|pipe$ [link|text](target|part)",
    )).toBe(
      "plain \\| `code|pipe` $math|pipe$ [link|text](target|part)",
    );
    expect(tableCellMarkdownForRender(
      "plain \\| `code\\|pipe` $\\|x\\|$",
    )).toBe("plain \\| `code|pipe` $\\|x\\|$");
  });

  it("creates the requested count including the header row", () => {
    const source = canonicalMarkdownTable(newMarkdownTable(3, 4));
    const table = parseMarkdownTable(source);
    expect(table?.cells).toHaveLength(3);
    expect(table?.alignments).toHaveLength(4);
  });

  it("does not absorb blank lines around a table", () => {
    const table = "| A |\n| --- |\n| B |";
    const text = `Before\n\n${table}\n\nAfter`;
    const node = parseMarkdownNodes(text).find((candidate) =>
      candidate.kind === "table");
    expect(node).toMatchObject({
      from: text.indexOf(table),
      to: text.indexOf(table) + table.length,
      text: table,
    });
  });
});
