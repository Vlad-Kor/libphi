// @vitest-environment jsdom
import type { EditorView } from "@codemirror/view";
type MeasureRequest = Parameters<EditorView["requestMeasure"]>[0] & {};
import { afterEach, expect, it, vi } from "vitest";
import { ConcealPointerGuard } from "../src/latex-suite/pointer-conceal";

const guards: ConcealPointerGuard[] = [];
afterEach(() => {
  guards.splice(0).forEach(guard => guard.destroy());
  document.body.replaceChildren();
});

function setup() {
  const dom = document.createElement("div");
  const span = document.createElement("sup");
  span.className = "cm-latex-conceal";
  span.getBoundingClientRect = () => new DOMRect(90, 10, 10, 10);
  dom.getBoundingClientRect = () => new DOMRect(0, 0, 110, 100);
  dom.append(span);
  document.body.append(dom);
  let top = 10;
  let request: MeasureRequest;
  const view = {
    dom, contentDOM: dom, posAtDOM: () => 4,
    coordsAtPos: () => ({ left: 90, right: 90, top, bottom: top + 10 }),
    requestMeasure: (value: MeasureRequest) => { request = value; },
    dispatch: vi.fn(),
  } as unknown as EditorView;
  const guard = new ConcealPointerGuard(view);
  guards.push(guard);
  span.dispatchEvent(new MouseEvent("mousedown", { bubbles: true, button: 0, buttons: 1, clientX: 95, clientY: 15 }));
  return {
    guard, view,
    measure: (nextTop: number) => { top = nextTop; request.write!(request.read(view), view); },
    spec: [{ from: 4, to: 6, text: "*", source: "^*" }],
    move: (x: number, y = 15) => dom.dispatchEvent(new MouseEvent("mousemove", { bubbles: true, buttons: 1, clientX: x, clientY: y })),
  };
}

it("keeps a wrapped fragment revealed over its former position, then releases when moving back", () => {
  const { guard, spec, measure, move } = setup();
  expect(guard.reveal(spec, true)).toBe(true);
  measure(30);
  expect(guard.reveal(spec, false)).toBe(true);
  move(95);
  expect(guard.reveal(spec, false)).toBe(true);
  move(80);
  expect(guard.reveal(spec, false)).toBe(false);
});

it("does not retain a same-row expansion", async () => {
  const { guard, spec, measure } = setup();
  expect(guard.reveal(spec, true)).toBe(true);
  measure(10);
  await Promise.resolve();
  expect(guard.reveal(spec, false)).toBe(false);
});

it.each(["mouseup", "blur"])("clears wrap hysteresis on %s", event => {
  const { guard, spec, measure } = setup();
  guard.reveal(spec, true);
  measure(30);
  window.dispatchEvent(new Event(event));
  expect(guard.reveal(spec, false)).toBe(false);
});
