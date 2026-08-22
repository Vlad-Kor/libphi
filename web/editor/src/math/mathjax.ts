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
    if (!mathjax.tex2svgPromise && !mathjax.tex2svg)
      throw new Error("MathJax SVG renderer is unavailable");
    // MathJax can discover that an extension or font needs asynchronous work
    // while converting the expression. Its synchronous API reports that case
    // as a visible "MathJax retry" error; the promise API waits and retries it.
    const rendered = mathjax.tex2svgPromise
      ? await mathjax.tex2svgPromise(source, { display })
      : mathjax.tex2svg!(source, { display });
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
