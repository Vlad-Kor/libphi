// @vitest-environment jsdom
import { afterEach, describe, expect, it } from "vitest";
import { renderMarkdown, renderMarkdownInline, sanitizeHtml, wireRenderedContent } from "../src/obsidian/markdown";
import { defaultSettings, updateRuntimeSettings } from "../src/settings";

afterEach(() => updateRuntimeSettings(defaultSettings));

describe("rendering security", () => {
  it("removes executable HTML while preserving useful formatting", () => {
    const value = sanitizeHtml('<span class="foo" style="color:red" onclick="alert(1)">ok</span><script>alert(2)</script><img src="x" onerror="alert(3)"><a href="javascript:alert(4)">bad</a><iframe src="https://example.com"></iframe>');
    expect(value).toContain("class=\"foo\"");
    expect(value).toContain("style=\"color:red\"");
    expect(value).not.toContain("onclick");
    expect(value).not.toContain("onerror");
    expect(value).not.toContain("script");
    expect(value).not.toContain("javascript:");
    expect(value).not.toContain("iframe");
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

  it("keeps safe linked-image presentation attributes", () => {
    const value = renderMarkdownInline(
      '<img src="https://example.com/button.png" height="100" align="right">',
    );
    const template = document.createElement("template");
    template.innerHTML = value;
    const image = template.content.querySelector("img");
    expect(image?.getAttribute("height")).toBe("100");
    expect(image?.getAttribute("align")).toBe("right");
    expect(image?.dataset.remoteSrc).toContain("button.png");
  });

  it("sandboxes HTTPS iframes and preserves only their dimensions", () => {
    updateRuntimeSettings({ ...defaultSettings, allowRemoteImages: true });
    const value = renderMarkdown(
      '<iframe src="https://example.com/tool" srcdoc="<script>x</script>" ' +
      'sandbox="allow-top-navigation" onload="x()" ' +
      'style="height:400px;width:100%;position:fixed"></iframe>',
    );
    const template = document.createElement("template");
    template.innerHTML = value;
    const frame = template.content.querySelector("iframe");
    expect(frame?.src).toBe("https://example.com/tool");
    expect(frame?.getAttribute("sandbox"))
      .toBe("allow-forms allow-popups allow-same-origin allow-scripts");
    expect(frame?.getAttribute("loading")).toBe("lazy");
    expect(frame?.getAttribute("referrerpolicy")).toBe("no-referrer");
    expect(frame?.style.height).toBe("400px");
    expect(frame?.style.width).toBe("100%");
    expect(frame?.style.position).toBe("");
    expect(value).not.toContain("srcdoc");
    expect(value).not.toContain("onload");
  });

  it("shows a click-to-load placeholder for remote iframes", () => {
    const root = document.createElement("div");
    root.innerHTML = renderMarkdown(
      '<iframe src="[https://example.com/tool](https://example.com/tool)" ' +
      'style="height:400px;width:100%"></iframe>',
    );
    wireRenderedContent(root);
    const frame = root.querySelector("iframe");
    expect(frame?.hasAttribute("src")).toBe(false);
    expect(frame?.dataset.remoteSrc).toBe("https://example.com/tool");
    const load = root.querySelector<HTMLButtonElement>(".iframe-embed-load");
    expect(load).not.toBeNull();
    load?.click();
    expect(frame?.src).toBe("https://example.com/tool");
    expect(root.querySelector(".iframe-embed-placeholder")).toBeNull();
  });

  it("rejects non-HTTPS iframe sources", () => {
    const value = renderMarkdown(
      '<iframe src="javascript:alert(1)"></iframe>' +
      '<iframe src="http://example.com"></iframe>',
    );
    expect(value).not.toContain("<iframe");
    expect(value.match(/Embedded page blocked/g)).toHaveLength(2);
  });

  it("syntax-highlights known fenced-code languages", () => {
    const value = renderMarkdown("```c\nint main(void) { return 0; }\n```");
    expect(value).toContain('class="language-c"');
    expect(value).toContain('class="token keyword"');
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
