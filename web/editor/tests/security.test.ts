// @vitest-environment jsdom
import { afterEach, describe, expect, it } from "vitest";
import { acceptNativeResponse } from "../src/bridge";
import { renderMarkdown, renderMarkdownInline, sanitizeHtml, wireRenderedContent } from "../src/markdown/render";
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

  it("keeps renderer syntax literal inside inline and fenced code", () => {
    const fenced = [
      "```text",
      String.raw`$x$ \[ y \]`,
      "<span>**literal**</span>",
      "%%comment%% ==highlight== [[Note]]",
      "```",
    ].join("\n");
    const root = document.createElement("div");
    root.innerHTML = renderMarkdown(`${fenced}\n\nInline \`$z$\``);
    wireRenderedContent(root);

    expect(root.innerHTML).not.toContain("phi-math-source");
    expect(root.innerHTML).not.toContain("phi-raw-html");
    expect(root.querySelector("pre code")?.textContent).toBe(
      [
        String.raw`$x$ \[ y \]`,
        "<span>**literal**</span>",
        "%%comment%% ==highlight== [[Note]]",
        "",
      ].join("\n"),
    );
    expect(root.querySelectorAll(".math-widget")).toHaveLength(0);
    expect(root.querySelector("code:not(pre code)")?.textContent).toBe("$z$");
    expect(root.querySelector(".internal-link")).toBeNull();
    expect(root.querySelector("mark")).toBeNull();
  });

  it("renders nested callouts and math while respecting raw HTML boundaries", () => {
    const root = document.createElement("div");
    root.innerHTML = renderMarkdown(
      "> [!question] Outer\n> $x$\n>\n> > [!tip]- Inner\n> > nested\n\n<span>$not-math$</span>",
    );
    wireRenderedContent(root);
    expect(root.querySelectorAll("details.callout")).toHaveLength(2);
    expect(root.querySelector('[data-callout="question"] [data-callout-icon="question"]')).not.toBeNull();
    expect(root.querySelector('[data-callout="tip"] [data-callout-icon="tip"]')).not.toBeNull();
    expect(root.querySelector('[data-callout="tip"]')?.hasAttribute("open")).toBe(false);
    expect(root.querySelectorAll(".math-widget")).toHaveLength(1);
    expect(root.querySelector("[data-phi-raw-html]")?.textContent).toBe("$not-math$");
  });

  it("protects rendered LaTeX from Markdown emphasis parsing", () => {
    const latex = String.raw`$\displaystyle S^{-1} A_{n} = E_{\lambda}$`;
    const value = renderMarkdown(`Formula ${latex}`);
    expect(value).toContain(latex);
    expect(value).not.toContain("<em>");
    const root = document.createElement("div");
    root.innerHTML = value;
    wireRenderedContent(root);
    expect(root.querySelector(".math-widget")?.getAttribute("aria-label"))
      .toBe(String.raw`LaTeX: \displaystyle S^{-1} A_{n} = E_{\lambda}`);
  });

  it("resolves rendered images relative to the embedded note", async () => {
    const root = document.createElement("div");
    root.innerHTML = renderMarkdown(
      "![Diagram|197](<../~Images/My diagram.png>)",
    );
    let request: { id?: string; payload?: Record<string, unknown> } | undefined;
    const respond = (event: Event) => {
      request = (event as CustomEvent).detail;
      acceptNativeResponse({
        protocol: 1,
        type: "request/response",
        id: request?.id,
        payload: { result: { path: "~Images/My diagram.png" } },
      });
    };
    window.addEventListener("phi-native-message", respond, { once: true });
    wireRenderedContent(root, "Nested/Embedded note.md");
    await Promise.resolve();
    await Promise.resolve();
    expect(request?.payload).toMatchObject({
      target: "../~Images/My%20diagram.png",
      relative: true,
      sourcePath: "Nested/Embedded note.md",
    });
    const image = root.querySelector<HTMLImageElement>("img");
    expect(image?.getAttribute("src")).toBe("vault:///~Images/My%20diagram.png");
    expect(image?.style.width).toBe("197px");
    expect(image?.alt).toBe("Diagram");

    Object.defineProperties(image!, {
      naturalWidth: { value: 900, configurable: true },
      naturalHeight: { value: 600, configurable: true },
    });
    image?.dispatchEvent(new Event("load"));
    let repeatedRequests = 0;
    const countRequest = (event: Event) => {
      if ((event as CustomEvent).detail?.type === "attachment/resolve")
        repeatedRequests++;
    };
    window.addEventListener("phi-native-message", countRequest);
    const remounted = document.createElement("div");
    remounted.innerHTML = renderMarkdown(
      "![Diagram](<../~Images/My diagram.png>)",
    );
    wireRenderedContent(remounted, "Nested/Embedded note.md");
    window.removeEventListener("phi-native-message", countRequest);
    const remountedImage = remounted.querySelector<HTMLImageElement>("img");
    expect(repeatedRequests).toBe(0);
    expect(remountedImage?.getAttribute("src"))
      .toBe("vault:///~Images/My%20diagram.png");
    expect(remountedImage?.width).toBe(900);
    expect(remountedImage?.height).toBe(600);
  });
});
