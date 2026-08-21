import { reportError } from "../bridge";

interface MathJaxApi {
  startup?: { promise?: Promise<unknown> };
  typesetPromise?: (elements?: Element[]) => Promise<unknown>;
  typesetClear?: (elements?: Element[]) => void;
  texReset?: () => void;
}

const cache = new Map<string, string>();
let preamble = "";
let preambleRevision = 0;

function api(): MathJaxApi | undefined {
  return (window as unknown as { MathJax?: MathJaxApi }).MathJax;
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
  const mathjax = api();
  try {
    await mathjax?.startup?.promise;
    if (!mathjax?.typesetPromise) throw new Error("MathJax did not initialize");
    target.replaceChildren();
    const source = `${preamble ? `${preamble}\n` : ""}${latex}`;
    target.textContent = display ? `$$${source}$$` : `$${source}$`;
    await mathjax.typesetPromise([target]);
    cache.set(key, target.innerHTML);
    if (cache.size > 256) cache.delete(cache.keys().next().value as string);
  } catch (error) {
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
