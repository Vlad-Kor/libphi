import { describe, expect, it } from "vitest";
import { mathModeAt, parseObsidian, selectionTouches } from "../src/obsidian/parser";

describe("Obsidian parser precedence", () => {
  it("does not parse links or math inside code", () => {
    const source = "`$not math$ [[not a link]]`\n\n```text\n[[no]]\n$x$\n```";
    const nodes = parseObsidian(source);
    expect(nodes.some((node) => node.kind === "math")).toBe(false);
    expect(nodes.some((node) => node.kind === "wikilink")).toBe(false);
  });

  it("does not parse Markdown inside raw HTML", () => {
    const nodes = parseObsidian('<span style="color:red">**not bold** $50</span>');
    expect(nodes.map((node) => node.kind)).toEqual(["html"]);
  });

  it("recognizes Obsidian extensions without changing their ranges", () => {
    const source = "==mark== %%secret%% [[Note#Heading|Alias]] ![[image.png|300]] ^block-id";
    const nodes = parseObsidian(source);
    for (const node of nodes) expect(source.slice(node.from, node.to)).not.toHaveLength(0);
    expect(nodes.some((node) => node.kind === "highlight")).toBe(true);
    expect(nodes.some((node) => node.kind === "comment")).toBe(true);
    expect(nodes.some((node) => node.kind === "wikilink")).toBe(true);
    expect(nodes.some((node) => node.kind === "embed")).toBe(true);
    expect(nodes.some((node) => node.kind === "block-id")).toBe(true);
  });

  it("recognizes frontmatter, callouts, tables, Mermaid, and both math modes", () => {
    const source = `---\ntitle: Test\n---\n# Heading\n\n> [!warning]- Careful\n> $x$\n\n| A | B |\n| --- | ---: |\n| 1 | 2 |\n\n$y$\n\n$$\nz^2\n$$\n\n\`\`\`mermaid\ngraph TD\nA-->B\n\`\`\``;
    const kinds = new Set(parseObsidian(source).map((node) => node.kind));
    for (const kind of ["frontmatter", "heading", "callout", "table", "math", "display-math", "mermaid"])
      expect(kinds.has(kind as never), kind).toBe(true);
  });

  it("handles escaped table aliases, Markdown links, images, and math ambiguity", () => {
    const source = "| Link |\n| --- |\n| [[Note\\|Alias]] |\n\n[Local](../Note.md) ![Plot|300](images/plot.png) ref[^a] ^[inline]\n\n[^a]: definition";
    const nodes = parseObsidian(source);
    const wiki = nodes.find((node) => node.kind === "wikilink");
    expect(wiki?.meta).toMatchObject({ target: "Note", alias: "Alias" });
    expect(nodes.some((node) => node.kind === "markdown-link")).toBe(true);
    expect(nodes.some((node) => node.kind === "markdown-image")).toBe(true);
    expect(nodes.some((node) => node.kind === "footnote-reference")).toBe(true);
    expect(nodes.some((node) => node.kind === "inline-footnote")).toBe(true);
    expect(nodes.some((node) => node.kind === "footnote-definition")).toBe(true);
    expect(mathModeAt("`$not math$`", 5)).toBe("none");
    expect(mathModeAt("$x + y$", 4)).toBe("inline");
    expect(mathModeAt("$$\nx + y\n$$", 6)).toBe("display");
  });

  it("parses spaced image sizes, list items, and multiline boxed math", () => {
    const source = `# Metrics
- first item

![[Pasted image.png | 400]]

$$
\\boxed{
mAP
=
\\frac{1}{C}
\\sum_{c=1}^{C}AP_c
}
$$`;
    const nodes = parseObsidian(source);
    const image = nodes.find((node) => node.kind === "embed");
    expect(image?.meta).toMatchObject({
      target: "Pasted image.png",
      alias: "400",
    });
    expect(nodes.some((node) => node.kind === "list-item")).toBe(true);
    expect(nodes.find((node) => node.kind === "display-math")?.text)
      .toContain("\\boxed{");
  });

  it("parses newly typed display math and leaves its closing boundary inactive", () => {
    const source = "$$\nx = 0\n$$";
    const math = parseObsidian(source).find((node) => node.kind === "display-math");
    expect(math).toMatchObject({ from: 0, to: source.length, text: "x = 0" });
    expect(selectionTouches(math!, { from: 4, to: 4 })).toBe(true);
    expect(selectionTouches(math!, { from: source.length, to: source.length })).toBe(false);
  });

  it("limits callouts to contiguous quoted lines and removes the marker from their body", () => {
    const source = "> [!info] test\n> test\n\n\nplain";
    const callout = parseObsidian(source).find((node) => node.kind === "callout");
    expect(callout).toMatchObject({
      text: "test",
      meta: { type: "info", title: "test" },
    });
    expect(source.slice(callout!.from, callout!.to)).toBe("> [!info] test\n> test");
  });

  it("parses linked HTML images, linked Markdown images, and fenced code", () => {
    const source = `[<img src="https://example.com/button.png" height="100">](https://example.com/extension)
[![GitHub Sponsors](https://img.shields.io/example)](https://example.com/sponsor)

\`\`\`c
test();
\`\`\``;
    const nodes = parseObsidian(source);
    const links = nodes.filter((node) => node.kind === "markdown-link");
    expect(links).toHaveLength(2);
    expect(links[0].meta).toMatchObject({ target: "https://example.com/extension" });
    expect(links[1].meta).toMatchObject({ target: "https://example.com/sponsor" });
    expect(String(links[1].meta?.alias)).toContain("![GitHub Sponsors]");
    expect(nodes.some((node) => node.kind === "code-block" && node.meta?.language === "c"))
      .toBe(true);
  });
});
