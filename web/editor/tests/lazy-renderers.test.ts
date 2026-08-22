// @vitest-environment jsdom
import { EditorState } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { afterEach, describe, expect, it, vi } from "vitest";
import { ensureMathJaxReady, renderMath } from "../src/math/mathjax";
import { MermaidWidget } from "../src/widgets/preview";

afterEach(() => {
  document.head.querySelectorAll("script[data-phi-renderer]").forEach((script) => script.remove());
  delete (window as unknown as { MathJax?: unknown }).MathJax;
  delete (window as unknown as { phiMermaid?: unknown }).phiMermaid;
});

describe("lazy preview renderers", () => {
  it("loads the offline MathJax runtime only when requested", async () => {
    expect(document.querySelector('script[data-phi-renderer="mathjax"]')).toBeNull();
    const ready = ensureMathJaxReady();
    const script = document.querySelector<HTMLScriptElement>(
      'script[data-phi-renderer="mathjax"]',
    );
    expect(script?.src).toBe("app://editor/mathjax/tex-svg.js");
    let resolveRetry: (() => void) | undefined;
    const retry = new Promise<void>((resolve) => { resolveRetry = resolve; });
    const rendered = document.createElement("mjx-container");
    const tex2svg = vi.fn()
      .mockImplementationOnce(() => {
        const error = Object.assign(new Error("MathJax retry"), { retry });
        throw error;
      })
      .mockImplementationOnce(() => rendered);
    const tex2svgPromise = vi.fn(() => new Promise<Element>(() => undefined));
    (window as unknown as { MathJax: unknown }).MathJax = {
      startup: { promise: Promise.resolve() },
      tex2svg,
      tex2svgPromise,
    };
    script?.dispatchEvent(new Event("load"));
    await ready;

    const target = document.createElement("span");
    const rendering = renderMath("x = 0", false, target);
    await vi.waitFor(() => expect(tex2svg).toHaveBeenCalledWith(
      "x = 0",
      { display: false },
    ));
    expect(target.classList.contains("math-loading")).toBe(true);
    expect(tex2svgPromise).not.toHaveBeenCalled();
    resolveRetry?.();
    await rendering;
    expect(tex2svg).toHaveBeenCalledTimes(2);
    expect(target.querySelector("mjx-container")).not.toBeNull();
    expect(target.classList.contains("math-loading")).toBe(false);
  });

  it("loads and initializes Mermaid only for a Mermaid widget", async () => {
    const parent = document.createElement("div");
    document.body.append(parent);
    const view = new EditorView({
      parent,
      state: EditorState.create({ doc: "graph TD\nA-->B" }),
    });
    const initialize = vi.fn();
    const render = vi.fn().mockResolvedValue({ svg: "<svg><title>Diagram</title></svg>" });
    const widget = new MermaidWidget("graph TD\nA-->B", 0).toDOM(view);
    const script = document.querySelector<HTMLScriptElement>(
      'script[data-phi-renderer="mermaid"]',
    );
    expect(script?.src).toBe("app://editor/mermaid.js");
    (window as unknown as { phiMermaid: unknown }).phiMermaid = { initialize, render };
    script?.dispatchEvent(new Event("load"));
    await vi.waitFor(() => expect(render).toHaveBeenCalledWith(
      expect.stringMatching(/^phi-mermaid-/),
      "graph TD\nA-->B",
    ));
    expect(initialize).toHaveBeenCalledOnce();
    expect(widget.querySelector("svg title")?.textContent).toBe("Diagram");
    view.destroy();
  });
});
