import { describe, expect, it } from "vitest";
import { expandReplacement, parseSnippetFile } from "../src/latex-suite/snippet-parser";

describe("LaTeX Suite snippet format", () => {
  it("parses JSON5-style snippet files and priorities", () => {
    const snippets = parseSnippetFile(`[
      // comments and unquoted keys match LaTeX Suite files
      {trigger: "sq", replacement: "\\\\sqrt{$0}$1", options: "mA"},
      {trigger: "x", replacement: "y", options: "m", priority: 3},
    ]`);
    expect(snippets).toHaveLength(2);
    expect(snippets[0].trigger).toBe("x");
  });

  it("expands captures, visual selections, placeholders, and ordered tabstops", () => {
    const result = expandReplacement("\\frac{[[0]]}{${1:x}}$2 ${VISUAL}", ["a+b"], "chosen");
    expect(result.text).toBe("\\frac{a+b}{x} chosen");
    expect(result.tabstops.map((stop) => stop.index)).toEqual([1, 2]);
    expect(result.text.slice(result.tabstops[0].from, result.tabstops[0].to)).toBe("x");
  });
});
