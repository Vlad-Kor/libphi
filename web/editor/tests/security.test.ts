// @vitest-environment jsdom
import { describe, expect, it } from "vitest";
import { renderMarkdown, sanitizeHtml, wireRenderedContent } from "../src/obsidian/markdown";

describe("rendering security", () => {
  it("removes executable HTML while preserving useful formatting", () => {
    const value = sanitizeHtml('<span class="foo" style="color:red" onclick="alert(1)">ok</span><script>alert(2)</script><img src="x" onerror="alert(3)"><a href="javascript:alert(4)">bad</a>');
    expect(value).toContain("class=\"foo\"");
    expect(value).toContain("style=\"color:red\"");
    expect(value).not.toContain("onclick");
    expect(value).not.toContain("onerror");
    expect(value).not.toContain("script");
    expect(value).not.toContain("javascript:");
  });

  it("keeps Markdown literal inside raw HTML", () => {
    const value = renderMarkdown("<span>**not bold**</span>");
    expect(value).toContain("**not bold**");
    expect(value).not.toContain("<strong>");
  });

  it("preserves escaped wikilink aliases in rendered tables", () => {
    const value = renderMarkdown("| Link |\n| --- |\n| [[Note\\|Alias]] |");
    expect(value).toContain("data-wikilink=\"Note\"");
    expect(value).toContain("Alias");
  });

  it("blocks remote images until the user explicitly loads them", () => {
    const value = renderMarkdown("![remote](https://example.com/image.png)");
    expect(value).toContain("data-remote-src");
    const template = document.createElement("template");
    template.innerHTML = value;
    expect(template.content.querySelector("img")?.hasAttribute("src")).toBe(false);
  });

  it("renders nested callouts and math while respecting raw HTML boundaries", () => {
    const root = document.createElement("div");
    root.innerHTML = renderMarkdown(
      "> [!question] Outer\n> $x$\n>\n> > [!tip]- Inner\n> > nested\n\n<span>$not-math$</span>",
    );
    wireRenderedContent(root);
    expect(root.querySelectorAll("details.callout")).toHaveLength(2);
    expect(root.querySelector('[data-callout="tip"]')?.hasAttribute("open")).toBe(false);
    expect(root.querySelectorAll(".math-widget")).toHaveLength(1);
    expect(root.querySelector("[data-phi-raw-html]")?.textContent).toBe("$not-math$");
  });
});
