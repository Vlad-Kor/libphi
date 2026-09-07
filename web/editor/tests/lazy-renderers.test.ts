// @vitest-environment jsdom
import { EditorState } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { afterEach, describe, expect, it, vi } from "vitest";
import { ensureMathJaxReady, renderMath, wireMathScroll } from "../src/math/mathjax";
import { MathWidget, MermaidWidget } from "../src/widgets/preview";

afterEach(() => {
  document.head.querySelectorAll("script[data-phi-renderer]").forEach((script) => script.remove());
  delete (window as unknown as { MathJax?: unknown }).MathJax;
  delete (window as unknown as { phiMermaid?: unknown }).phiMermaid;
});

describe("lazy preview renderers", () => {
  it("does not typeset equations scrolled out of the viewport while the runtime was loading", async () => {
    let ready!: () => void;
    const startup = new Promise<void>(resolve => { ready = resolve; });
    const convert = vi.fn(() => document.createElement("mjx-container"));
    (window as unknown as { MathJax: unknown }).MathJax = {
      startup: { promise: startup }, tex2svg: convert,
    };
    const measure = vi.fn();
    const view = { requestMeasure: measure } as unknown as EditorView;
    for (let i = 0; i < 20; i++) {
      const widget = new MathWidget(`rapid_scroll_${i}`, true, i);
      const dom = widget.toDOM(view);
      widget.destroy(dom);
    }
    ready();
    await startup;
    await new Promise(resolve => setTimeout(resolve, 0));
    expect(convert).not.toHaveBeenCalled();
    expect(measure).not.toHaveBeenCalled();
  });

  it("abandons a font retry when its equation has been unmounted", async () => {
    let ready!: () => void;
    const font = new Promise<void>(resolve => { ready = resolve; });
    const convert = vi.fn(() => {
      if (convert.mock.calls.length === 1) throw { retry: font };
      return document.createElement("mjx-container");
    });
    (window as unknown as { MathJax: unknown }).MathJax = { tex2svg: convert };
    const measure = vi.fn();
    const widget = new MathWidget("cancel_font_retry", true, 0);
    const dom = widget.toDOM({ requestMeasure: measure } as unknown as EditorView);
    await vi.waitFor(() => expect(convert).toHaveBeenCalledOnce());
    widget.destroy(dom);
    ready();
    await font;
    await new Promise(resolve => setTimeout(resolve, 0));
    expect(convert).toHaveBeenCalledOnce();
    expect(measure).not.toHaveBeenCalled();
  });
  it("keeps horizontal touchpad panning inside an overflowing equation", () => {
    const parent = document.createElement("div");
    const equation = document.createElement("div");
    parent.append(equation);
    Object.defineProperties(equation, {
      clientWidth: { value: 300 },
      scrollWidth: { value: 900 },
      scrollLeft: { value: 100, writable: true },
    });
    wireMathScroll(equation);
    expect(equation.classList.contains("math-overflow")).toBe(true);
    const bubbled = vi.fn();
    parent.addEventListener("wheel", bubbled);

    const pan = new WheelEvent("wheel", {
      bubbles: true,
      cancelable: true,
      deltaX: 48,
      deltaY: 3,
    });
    equation.dispatchEvent(pan);

    expect(equation.scrollLeft).toBe(148);
    expect(pan.defaultPrevented).toBe(true);
    expect(bubbled).not.toHaveBeenCalled();
  });

  it("leaves vertical scrolling and pinch zoom gestures to the editor", () => {
    const parent = document.createElement("div");
    const equation = document.createElement("div");
    parent.append(equation);
    Object.defineProperties(equation, {
      clientWidth: { value: 300 },
      scrollWidth: { value: 900 },
      scrollLeft: { value: 100, writable: true },
    });
    wireMathScroll(equation);
    const bubbled = vi.fn();
    parent.addEventListener("wheel", bubbled);

    const vertical = new WheelEvent("wheel", {
      bubbles: true,
      cancelable: true,
      deltaX: 2,
      deltaY: 40,
    });
    equation.dispatchEvent(vertical);
    const zoom = new WheelEvent("wheel", {
      bubbles: true,
      cancelable: true,
      ctrlKey: true,
      deltaX: 40,
      deltaY: 1,
    });
    equation.dispatchEvent(zoom);

    expect(equation.scrollLeft).toBe(100);
    expect(vertical.defaultPrevented).toBe(false);
    expect(zoom.defaultPrevented).toBe(false);
    expect(bubbled).toHaveBeenCalledTimes(2);
  });

  it("contains horizontal momentum and vertical drift at both equation boundaries", () => {
    const parent = document.createElement("div");
    const equation = document.createElement("div");
    parent.append(equation);
    Object.defineProperties(equation, {
      clientWidth: { value: 300 },
      scrollWidth: { value: 900 },
      scrollLeft: { value: 600, writable: true },
    });
    wireMathScroll(equation);
    const bubbled = vi.fn();
    parent.addEventListener("wheel", bubbled);

    const pan = new WheelEvent("wheel", {
      bubbles: true,
      cancelable: true,
      deltaX: 48,
      deltaY: 3,
    });
    equation.dispatchEvent(pan);

    expect(equation.scrollLeft).toBe(600);
    expect(pan.defaultPrevented).toBe(true);
    expect(bubbled).not.toHaveBeenCalled();
    equation.scrollLeft = 0;
    const reverse = new WheelEvent("wheel", {
      bubbles: true, cancelable: true, deltaX: -48, deltaY: -3,
    });
    equation.dispatchEvent(reverse);
    expect(equation.scrollLeft).toBe(0);
    expect(reverse.defaultPrevented).toBe(true);
    expect(bubbled).not.toHaveBeenCalled();
  });

  it("leaves native math scrollbar presses and clicks alone but reveals equation content", () => {
    (window as unknown as { MathJax: unknown }).MathJax = {
      tex2svg: () => document.createElement("mjx-container"),
    };
    const dispatch = vi.fn();
    const view = {
      dispatch, focus: vi.fn(), state: EditorState.create({ doc: "$$long equation$$" }),
      requestMeasure: vi.fn(),
    } as unknown as EditorView;
    const widget = new MathWidget("scrollbar interaction", true, 0);
    const equation = widget.toDOM(view);
    widget.destroy(equation);
    equation.classList.add("math-overflow");
    Object.defineProperties(equation, {
      clientWidth: { value: 300 }, scrollWidth: { value: 900 },
      offsetWidth: { value: 300 }, offsetHeight: { value: 100 },
      clientHeight: { value: 88 },
    });
    // A transformed widget checks that hit testing uses viewport coordinates.
    equation.getBoundingClientRect = () => ({
      left: 20, top: 30, right: 620, bottom: 230, width: 600, height: 200,
      x: 20, y: 30, toJSON: () => ({}),
    });
    for (const type of ["pointerdown", "click"]) {
      const scrollbar = new MouseEvent(type, {
        bubbles: true, cancelable: true, clientX: 100, clientY: 220,
      });
      equation.dispatchEvent(scrollbar);
      expect(scrollbar.defaultPrevented).toBe(false);
      expect(dispatch).not.toHaveBeenCalled();
    }
    const content = new MouseEvent("pointerdown", {
      bubbles: true, cancelable: true, clientX: 100, clientY: 100,
    });
    equation.dispatchEvent(content);
    expect(content.defaultPrevented).toBe(true);
    expect(dispatch).toHaveBeenCalledOnce();
    expect(dispatch.mock.calls[0][0].selection).toEqual({ anchor: 1 });
  });

  it("does not intercept gestures on equations that fit", () => {
    const parent = document.createElement("div");
    const equation = document.createElement("div");
    parent.append(equation);
    Object.defineProperties(equation, {
      clientWidth: { value: 300 },
      scrollWidth: { value: 300 },
      scrollLeft: { value: 0, writable: true },
    });
    wireMathScroll(equation);
    expect(equation.classList.contains("math-overflow")).toBe(false);
    const bubbled = vi.fn();
    parent.addEventListener("wheel", bubbled);

    const pan = new WheelEvent("wheel", {
      bubbles: true,
      cancelable: true,
      deltaX: 48,
      deltaY: 3,
    });
    equation.dispatchEvent(pan);

    expect(pan.defaultPrevented).toBe(false);
    expect(bubbled).toHaveBeenCalledOnce();
  });

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
