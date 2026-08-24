// @vitest-environment jsdom
import { describe, expect, it } from "vitest";
import { clipboardHtmlToMarkdown, markdownClipboardHtml } from "../src/clipboard";

describe("clipboard interoperability", () => {
  it("exports Markdown as safe rich HTML", () => {
    const html = markdownClipboardHtml("A **bold** and *emphasized* selection");
    expect(html).toContain("<strong>bold</strong>");
    expect(html).toContain("<em>emphasized</em>");
  });

  it("converts common rich clipboard content back to Markdown", () => {
    expect(clipboardHtmlToMarkdown(
      "<h2>Notes</h2><p>A <strong>bold</strong> <a href='https://example.com'>link</a>.</p>" +
      "<ul><li>First</li><li>Second<ul><li>Nested</li></ul></li></ul>",
    )).toBe("## Notes\n\nA **bold** [link](https://example.com).\n\n- First\n- Second\n  - Nested");
  });

  it("does not carry executable destinations into Markdown", () => {
    expect(clipboardHtmlToMarkdown(
      '<a href="javascript:alert(1)">safe label</a><img src="data:text/html,bad" alt="image">',
    )).toBe("safe labelimage");
  });
});
