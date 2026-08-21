import { reportError } from "../bridge";

interface MathJaxApi {
  startup?: { promise?: Promise<unknown> };
  typesetPromise?: (elements?: Element[]) => Promise<unknown>;
  typesetClear?: (elements?: Element[]) => void;
  texReset?: () => void;
}

interface MathJaxWindow extends Window {
  MathJax?: MathJaxApi;
  __phiMathJaxError?: string;
}

const cache = new Map<string, string>();
let preamble = "";
let preambleRevision = 0;

async function waitForMathJax(timeout = 10_000): Promise<MathJaxApi> {
  const started = performance.now();
  while (performance.now() - started < timeout) {
    const failure = (window as MathJaxWindow).__phiMathJaxError;
    if (failure) throw new Error(`MathJax could not load: ${failure}`);
    const mathjax = api();
    if (mathjax?.typesetPromise) {
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
    target.replaceChildren();
    const source = `${preamble ? `${preamble}\n` : ""}${latex}`;
    target.textContent = display ? `$$${source}$$` : `$${source}$`;
    await mathjax.typesetPromise!([target]);
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
