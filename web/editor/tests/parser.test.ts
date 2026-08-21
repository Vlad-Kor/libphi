import { describe, expect, it } from "vitest";
import { mathModeAt, parseObsidian } from "../src/obsidian/parser";

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
});
