import { reportError } from "../bridge";

interface MathJaxApi {
  startup?: { promise?: Promise<unknown> };
  tex2svg?: (latex: string, options?: { display?: boolean }) => Element;
  tex2svgPromise?: (
    latex: string,
    options?: { display?: boolean },
  ) => Promise<Element>;
  texReset?: () => void;
}

interface MathJaxWindow extends Window {
  MathJax?: MathJaxApi;
  __phiMathJaxError?: string;
}

const cache = new Map<string, string>();
const scrollWired = new WeakSet<HTMLElement>();
let preamble = "";
let preambleRevision = 0;
let mathJaxScript: Promise<void> | undefined;

function startMathJax(): Promise<void> {
  if (api()?.tex2svgPromise || api()?.tex2svg) return Promise.resolve();
  if (mathJaxScript) return mathJaxScript;
  mathJaxScript = new Promise<void>((resolve, reject) => {
    const script = document.createElement("script");
    script.src = "app://editor/mathjax/tex-svg.js";
    script.async = true;
    script.dataset.phiRenderer = "mathjax";
    script.addEventListener("load", () => resolve(), { once: true });
    script.addEventListener("error", () => {
      const message = "MathJax runtime could not be loaded";
      (window as MathJaxWindow).__phiMathJaxError = message;
      reject(new Error(message));
    }, { once: true });
    document.head.append(script);
  });
  return mathJaxScript;
}

async function waitForMathJax(timeout = 10_000): Promise<MathJaxApi> {
  void startMathJax().catch(() => undefined);
  const started = performance.now();
  while (performance.now() - started < timeout) {
    const failure = (window as MathJaxWindow).__phiMathJaxError;
    if (failure) throw new Error(`MathJax could not load: ${failure}`);
    const mathjax = api();
    if (mathjax?.tex2svgPromise || mathjax?.tex2svg) {
      if (mathjax.startup?.promise) await mathjax.startup.promise;
      return mathjax;
    }
    await new Promise((resolve) => window.setTimeout(resolve, 25));
  }
  throw new Error("MathJax did not initialize");
}

export async function ensureMathJaxReady(): Promise<void> {
  await waitForMathJax();
}

function api(): MathJaxApi | undefined {
  return (window as MathJaxWindow).MathJax;
}

function normalizeInlineEnvironments(latex: string): string {
  if (!/\\begin\{align\*?\}/.test(latex)) return latex;
  return latex
    .replace(/\\begin\{align\*?\}/g, "\\begin{aligned}")
    .replace(/\\end\{align\*?\}/g, "\\end{aligned}");
}

function retryPromise(error: unknown): Promise<unknown> | undefined {
  if (!error || typeof error !== "object" || !("retry" in error)) return undefined;
  const retry = (error as { retry?: unknown }).retry;
  if (!retry || typeof (retry as PromiseLike<unknown>).then !== "function")
    return undefined;
  return Promise.resolve(retry as PromiseLike<unknown>);
}

async function convertMath(
  mathjax: MathJaxApi,
  source: string,
  display: boolean,
): Promise<Element> {
  if (!mathjax.tex2svg) {
    if (mathjax.tex2svgPromise)
      return mathjax.tex2svgPromise(source, { display });
    throw new Error("MathJax SVG renderer is unavailable");
  }

  // MathJax's synchronous converter throws an Error with a `retry` promise
  // when an extension or dynamic font must be loaded. Await precisely that
  // work and retry instead of displaying MathJax's internal exception. The
  // browser-wide tex2svgPromise() queue can remain pending in WebKitGTK.
  for (let attempt = 0; attempt < 32; attempt++) {
    try {
      return mathjax.tex2svg(source, { display });
    } catch (error) {
      const retry = retryPromise(error);
      if (!retry) throw error;
      await retry;
    }
  }
  throw new Error("MathJax could not finish loading this expression");
}

export function updatePreamble(value: string): void {
  if (value === preamble) return;
  preamble = value;
  preambleRevision++;
  cache.clear();
}

export function getMathRevision(): number { return preambleRevision; }

/**
 * Keep a horizontal touchpad gesture inside an overflowing equation. WebKitGTK
 * otherwise applies the gesture's small vertical component to CodeMirror's
 * outer scroller, which makes the document jump while the formula moves.
 */
export function wireMathScroll(target: HTMLElement): void {
  /* A non-passive wheel listener makes WebKitGTK send the gesture through the
   * web process instead of keeping it entirely on the asynchronous scrolling
   * path.  Only install one when the equation really needs horizontal
   * scrolling; ordinary equations should never affect document momentum. */
  if (scrollWired.has(target)) return;
  /* `overflow-x: auto` also makes the element a nested asynchronous scroll
   * container in WebKit, even when its contents fit. Touchpad momentum can
   * then stop when it crosses an ordinary display equation. Only create that
   * nested scroller after measuring a genuinely overflowing expression. */
  if (target.scrollWidth <= target.clientWidth) return;
  target.classList.add("math-overflow");
  scrollWired.add(target);
  target.addEventListener("wheel", (event) => {
    // Pinch zoom is exposed as a Ctrl-modified wheel gesture by WebKit. Leave it
    // to the browser, as well as ordinary vertical document scrolling.
    if (event.ctrlKey || event.defaultPrevented ||
        Math.abs(event.deltaX) <= Math.abs(event.deltaY) ||
        target.scrollWidth <= target.clientWidth)
      return;

    const scale = event.deltaMode === WheelEvent.DOM_DELTA_LINE ? 16 :
      event.deltaMode === WheelEvent.DOM_DELTA_PAGE ? target.clientWidth : 1;
    const maximum = target.scrollWidth - target.clientWidth;
    const next = Math.max(0, Math.min(maximum,
      target.scrollLeft + event.deltaX * scale));
    /* Do not trap the rest of a kinetic gesture at either edge.  Let WebKit
     * hand it back to CodeMirror's outer scroller instead. */
    if (next === target.scrollLeft) return;
    target.scrollLeft = next;
    event.preventDefault();
    event.stopPropagation();
  }, { passive: false });
}

export async function renderMath(
  latex: string,
  display: boolean,
  target: HTMLElement,
): Promise<void> {
  const key = `${preambleRevision}\0${display ? "display" : "inline"}\0${latex}`;
  const cached = cache.get(key);
  if (cached) {
    target.innerHTML = cached;
    return;
  }
  try {
    target.classList.add("math-loading");
    target.textContent = latex;
    const mathjax = await waitForMathJax();
    const normalized = display ? latex : normalizeInlineEnvironments(latex);
    const source = `${preamble ? `${preamble}\n` : ""}${normalized}`;
    const rendered = await convertMath(mathjax, source, display);
    target.replaceChildren(rendered);
    target.classList.remove("math-loading");
    cache.set(key, target.innerHTML);
    if (cache.size > 256) cache.delete(cache.keys().next().value as string);
  } catch (error) {
    target.classList.remove("math-loading");
    target.replaceChildren();
    const message = document.createElement("span");
    message.className = "render-error";
    message.textContent = error instanceof Error ? error.message : String(error);
    target.append(message);
    reportError(error, "mathjax");
  }
}

export function invalidateMath(): void {
  cache.clear();
  api()?.texReset?.();
}
