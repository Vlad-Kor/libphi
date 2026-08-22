import { describe, expect, it } from "vitest";
import { concealLatex } from "../src/latex-suite/enhancements";
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

  it("matches LaTeX Suite conceal forms without changing the source", () => {
    const source = String.raw`\dot{x}^{2} + \sqrt{ 1-\beta^{2} } + \frac{1}{2} + \mathbb{R} + \not\in`;
    const replacements = concealLatex(source).flat();

    expect(replacements.map((replacement) => replacement.text)).toEqual([
      "x\u0307", "2", "√", "β", "2", "½", "ℝ", "∉",
    ]);
    expect(replacements.filter((replacement) => replacement.elementType === "sup"))
      .toHaveLength(2);
    expect(source).toContain(String.raw`\dot{x}^{2}`);
  });

  it("conceals general fractions, operators, scripts, and bra-ket notation", () => {
    const source = String.raw`\frac{x}{y} + \sin\limits x_i + \braket{\alpha}`;
    const replacements = concealLatex(source).flat();
    expect(replacements.map((replacement) => replacement.text)).toEqual(
      expect.arrayContaining(["(", ")", "/", "sin", "i", "⟨", "α", "⟩"]),
    );
    expect(replacements.find((replacement) => replacement.text === "i")?.elementType)
      .toBe("sub");
    expect(replacements.some((replacement) => replacement.source === "x")).toBe(false);
  });
});
