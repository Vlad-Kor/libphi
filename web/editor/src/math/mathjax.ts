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
    const source = `${preamble ? `${preamble}\n` : ""}${latex}`;
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
