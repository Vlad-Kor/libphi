// @vitest-environment jsdom
import { EditorState } from "@codemirror/state";
import type { EditorView } from "@codemirror/view";
type MeasureRequest = Parameters<EditorView["requestMeasure"]>[0] & {};
import { afterEach, expect, it, vi } from "vitest";
import { ConcealPointerGuard } from "../src/latex-suite/pointer-conceal";

const guards: ConcealPointerGuard[] = [];
afterEach(() => {
  guards.splice(0).forEach(guard => guard.destroy());
  document.body.replaceChildren();
  vi.restoreAllMocks();
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
  vi.spyOn(document, "createRange").mockReturnValue({
    setStart() {}, setEnd() {},
    getClientRects: () => top === 10
      ? [new DOMRect(75, top, 30, 10)]
      : [new DOMRect(75, 10, 5, 10), new DOMRect(5, top, 30, 10)],
  } as unknown as Range);
  const view = {
    dom, contentDOM: dom, posAtDOM: () => 4,
    domAtPos: () => ({ node: span, offset: 0 }),
    state: EditorState.create({ doc: "  $G^*" }),
    coordsAtPos: (pos: number) => {
      const left = (top === 10 ? 75 : 5) + (pos - 2) * 7.5;
      return { left, right: left, top, bottom: top + 10 };
    },
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
  for (const x of [80, 81, 79, 80]) {
    move(x);
    expect(guard.reveal(spec, false)).toBe(true);
  }
  move(70);
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


it("stays revealed across the line gap and while moving over the new base position", () => {
  const { guard, spec, measure, move, view } = setup();
  guard.reveal(spec, true);
  measure(30);
  for (const [x, y] of [[80, 15], [50, 25], [13, 35], [14, 35], [12, 35], [80, 15]]) {
    move(x, y);
    expect(guard.reveal(spec, false)).toBe(true);
  }
  expect(view.dispatch).not.toHaveBeenCalled();
  move(50, 50);
  expect(guard.reveal(spec, false)).toBe(false);
});
