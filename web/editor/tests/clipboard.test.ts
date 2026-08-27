// @vitest-environment jsdom
import { describe, expect, it } from "vitest";
import {
  clipboardHtmlToMarkdown,
  clipboardImageFile,
  clipboardMayContainNativeImage,
  markdownClipboardHtml,
  pastedImageMarkdown,
} from "../src/clipboard";

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

  it("accepts WebKit-style non-iterable clipboard image lists", () => {
    const file = new File(["image"], "paste.png", { type: "image/png" });
    const item = { type: "image/png", getAsFile: () => file };
    const items = { 0: item, length: 1 } as unknown as DataTransferItemList;
    expect(clipboardImageFile({ items, files: [] } as unknown as DataTransfer))
      .toBe(file);
  });

  it("falls back to clipboard files when no image item is exposed", () => {
    const file = new File(["image"], "paste.webp", { type: "image/webp" });
    const files = { 0: file, length: 1 } as unknown as FileList;
    expect(clipboardImageFile({ items: [], files } as unknown as DataTransfer))
      .toBe(file);
  });

  it("uses the native fallback for WebKitGTK's empty image paste event", () => {
    const empty = { items: [], files: [], types: [] } as unknown as DataTransfer;
    expect(clipboardMayContainNativeImage(empty)).toBe(true);
    const text = {
      items: [], files: [], types: ["text/plain"],
    } as unknown as DataTransfer;
    expect(clipboardMayContainNativeImage(text)).toBe(false);
  });

  it("uses filename embeds only inside workspaces", () => {
    const image = {
      name: "Pasted image 20260827183304.png",
      path: "../../../~Images/Pasted image 20260827183304.png",
    };
    expect(pastedImageMarkdown(image, "wiki-embed", true))
      .toBe("![[Pasted image 20260827183304.png]]");
    expect(pastedImageMarkdown(image, "markdown-link", true))
      .toBe("![Pasted image 20260827183304](<../../../~Images/Pasted image 20260827183304.png>)");
    expect(pastedImageMarkdown(image, "wiki-embed", false))
      .toBe("![Pasted image 20260827183304](<../../../~Images/Pasted image 20260827183304.png>)");
  });
});
