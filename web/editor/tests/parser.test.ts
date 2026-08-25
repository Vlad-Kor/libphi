import { describe, expect, it } from "vitest";
import { CompletionContext } from "@codemirror/autocomplete";
import { EditorState } from "@codemirror/state";
import { markdownCompletion } from "../src/markdown/completion";
import { mathModeAt, parseMarkdownNodes, selectionTouches } from "../src/markdown/parser";

describe("Markdown extension parser precedence", () => {
  it("does not parse links or math inside code", () => {
    const source = "`$not math$ [[not a link]]`\n\n```text\n[[no]]\n$x$\n```";
    const nodes = parseMarkdownNodes(source);
    expect(nodes.some((node) => node.kind === "math")).toBe(false);
    expect(nodes.some((node) => node.kind === "wikilink")).toBe(false);
  });

  it("does not parse Markdown inside raw HTML", () => {
    const nodes = parseMarkdownNodes('<span style="color:red">**not bold** $50</span>');
    expect(nodes.map((node) => node.kind)).toEqual(["html"]);
  });

  it("treats an iframe as one raw HTML block", () => {
    const source = '<iframe src="https://example.com">$not-math$ [[not-a-link]]</iframe>';
    const nodes = parseMarkdownNodes(source);
    expect(nodes.map((node) => node.kind)).toEqual(["html"]);
    expect(nodes[0].text).toBe(source);
  });

  it("recognizes extended Markdown syntax without changing its ranges", () => {
    const source = "==mark== %%secret%% [[Note#Heading|Alias]] ![[image.png|300]] ^block-id";
    const nodes = parseMarkdownNodes(source);
    for (const node of nodes) expect(source.slice(node.from, node.to)).not.toHaveLength(0);
    expect(nodes.some((node) => node.kind === "highlight")).toBe(true);
    expect(nodes.some((node) => node.kind === "comment")).toBe(true);
    expect(nodes.some((node) => node.kind === "wikilink")).toBe(true);
    expect(nodes.some((node) => node.kind === "embed")).toBe(true);
    expect(nodes.some((node) => node.kind === "block-id")).toBe(true);
  });

  it("recognizes frontmatter, callouts, tables, Mermaid, and both math modes", () => {
    const source = `---\ntitle: Test\n---\n# Heading\n\n> [!warning]- Careful\n> $x$\n\n| A | B |\n| --- | ---: |\n| 1 | 2 |\n\n$y$\n\n$$\nz^2\n$$\n\n\`\`\`mermaid\ngraph TD\nA-->B\n\`\`\``;
    const kinds = new Set(parseMarkdownNodes(source).map((node) => node.kind));
    for (const kind of ["frontmatter", "heading", "callout", "table", "math", "display-math", "mermaid"])
      expect(kinds.has(kind as never), kind).toBe(true);
  });

  it("handles escaped table aliases, Markdown links, images, and math ambiguity", () => {
    const source = "| Link |\n| --- |\n| [[Note\\|Alias]] |\n\n[Local](../Note.md) ![Plot|300](images/plot.png) ref[^a] ^[inline]\n\n[^a]: definition";
    const nodes = parseMarkdownNodes(source);
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

  it("recognizes horizontal rules without treating frontmatter fences as rules", () => {
    const nodes = parseMarkdownNodes("---\ntitle: Test\n---\n\nBefore\n\n---\n\nAfter");
    expect(nodes.filter((node) => node.kind === "frontmatter")).toHaveLength(1);
    expect(nodes.filter((node) => node.kind === "horizontal-rule")).toHaveLength(1);
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
    const nodes = parseMarkdownNodes(source);
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
    const math = parseMarkdownNodes(source).find((node) => node.kind === "display-math");
    expect(math).toMatchObject({ from: 0, to: source.length, text: "x = 0" });
    expect(selectionTouches(math!, { from: 4, to: 4 })).toBe(true);
    expect(selectionTouches(math!, { from: source.length, to: source.length })).toBe(false);
  });

  it("limits callouts to contiguous quoted lines and removes the marker from their body", () => {
    const source = "> [!info] test\n> test\n\n\nplain";
    const callout = parseMarkdownNodes(source).find((node) => node.kind === "callout");
    expect(callout).toMatchObject({
      text: "test",
      meta: { type: "info", title: "test" },
    });
    expect(source.slice(callout!.from, callout!.to)).toBe("> [!info] test\n> test");
  });

  it("parses unordered, task, and ordered list marker geometry", () => {
    const nodes = parseMarkdownNodes(
      "- bullet\n- [ ] task\n10. ordered\n    1) nested\n- [x]compact",
    ).filter((node) => node.kind === "list-item");
    expect(nodes).toHaveLength(5);
    expect(nodes[0].meta).toMatchObject({
      marker: "-", markerFrom: 0, contentFrom: 2, ordered: false,
    });
    expect(nodes[1].meta).toMatchObject({ task: true, contentFrom: 15 });
    expect(nodes[2].meta).toMatchObject({
      marker: "10.", contentFrom: 24, ordered: true, number: 10,
    });
    expect(nodes[3].meta).toMatchObject({
      marker: "1)", indentColumns: 4, ordered: true, number: 1,
    });
    expect(nodes[4].meta).toMatchObject({ task: true, contentFrom: 51 });
  });

  it("offers callout types without the generic keyword icon", async () => {
    const source = "> [!wa";
    const result = await markdownCompletion(new CompletionContext(
      EditorState.create({ doc: source }), source.length, false,
    ));
    expect(result?.options.some((option) => option.label === "warning")).toBe(true);
    expect(result?.options.every((option) => option.type == null)).toBe(true);
  });

  it("parses linked HTML images, linked Markdown images, and fenced code", () => {
    const source = `[<img src="https://example.com/button.png" height="100">](https://example.com/extension)
[![GitHub Sponsors](https://img.shields.io/example)](https://example.com/sponsor)

\`\`\`c
test();
\`\`\``;
    const nodes = parseMarkdownNodes(source);
    const links = nodes.filter((node) => node.kind === "markdown-link");
    expect(links).toHaveLength(2);
    expect(links[0].meta).toMatchObject({ target: "https://example.com/extension" });
    expect(links[1].meta).toMatchObject({ target: "https://example.com/sponsor" });
    expect(String(links[1].meta?.alias)).toContain("![GitHub Sponsors]");
    expect(nodes.some((node) => node.kind === "code-block" && node.meta?.language === "c"))
      .toBe(true);
  });
});
