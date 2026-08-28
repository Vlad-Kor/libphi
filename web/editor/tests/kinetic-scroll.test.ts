// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";
import { installKineticScroll } from "../src/kinetic-scroll";

afterEach(() => {
  vi.restoreAllMocks();
  document.body.replaceChildren();
});

function wheel(
  target: HTMLElement,
  deltaY: number,
  options: WheelEventInit = {},
): WheelEvent {
  const event = new WheelEvent("wheel", {
    bubbles: true,
    cancelable: true,
    deltaY,
    deltaMode: WheelEvent.DOM_DELTA_PIXEL,
    ...options,
  });
  target.dispatchEvent(event);
  return event;
}

function touch(
  target: HTMLElement,
  type: string,
  y: number,
  active = true,
): Event {
  const point = {
    identifier: 7,
    clientX: 20,
    clientY: y,
    target,
  } as unknown as Touch;
  const event = new Event(type, { bubbles: true, cancelable: true });
  Object.defineProperties(event, {
    touches: { value: active ? [point] : [] },
    changedTouches: { value: [point] },
  });
  target.dispatchEvent(event);
  return event;
}

describe("WebKitGTK precision scrolling workaround", () => {
  it("applies active precision deltas directly without delayed motion", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    const requestFrame = vi.spyOn(window, "requestAnimationFrame");
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(26);
    installKineticScroll(scroller);

    const first = wheel(scroller, 12);
    const second = wheel(scroller, 7);
    expect(first.defaultPrevented).toBe(true);
    expect(second.defaultPrevented).toBe(true);
    expect(scroller.scrollTop).toBe(19);
    expect(requestFrame).not.toHaveBeenCalled();
  });

  it("cancels sub-pixel finger wiggles without blocking normal scrolling", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(20)
      .mockReturnValueOnce(30)
      .mockReturnValueOnce(120);
    installKineticScroll(scroller);

    expect(wheel(scroller, 0.4).defaultPrevented).toBe(true);
    wheel(scroller, -0.35);
    wheel(scroller, 0.2);
    expect(scroller.scrollTop).toBe(0);

    /* A pause starts another gesture, so its movement is not combined with
     * the old sub-pixel remainder. */
    wheel(scroller, 1.8);
    expect(scroller.scrollTop).toBe(1);
  });

  it("uses GTK's native velocity and deceleration curve after release", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    let frame: FrameRequestCallback | undefined;
    vi.spyOn(window, "requestAnimationFrame").mockImplementation((callback) => {
      frame = callback;
      return 1;
    });
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(20);
    const kinetic = installKineticScroll(scroller);

    kinetic.nativeBegin(7);
    wheel(scroller, 12);
    kinetic.nativeDecelerate(7, 400);
    expect(scroller.scrollTop).toBe(12);
    frame?.(270);
    expect(scroller.scrollTop).toBeCloseTo(
      12 + 100 * (1 - Math.exp(-1)),
    );
  });

  it("does not invent momentum for an abrupt zero-velocity stop", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    const requestFrame = vi.spyOn(window, "requestAnimationFrame")
      .mockImplementation(() => 1);
    vi.spyOn(performance, "now").mockReturnValue(10);
    const kinetic = installKineticScroll(scroller);

    kinetic.nativeBegin(3);
    wheel(scroller, 12);
    kinetic.nativeDecelerate(3, 0);
    expect(scroller.scrollTop).toBe(12);
    expect(requestFrame).not.toHaveBeenCalled();
  });

  it("leaves mouse wheels, horizontal gestures, and pinch zoom native", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    const kinetic = installKineticScroll(scroller);

    const mouse = wheel(scroller, 3, { deltaMode: WheelEvent.DOM_DELTA_LINE });
    const pixelWheel = wheel(scroller, 53);
    const horizontal = wheel(scroller, 2, { deltaX: 8 });
    const pinch = wheel(scroller, 8, { ctrlKey: true });
    expect(mouse.defaultPrevented).toBe(false);
    expect(pixelWheel.defaultPrevented).toBe(false);
    expect(horizontal.defaultPrevented).toBe(false);
    expect(pinch.defaultPrevented).toBe(false);
    expect(scroller.scrollTop).toBe(0);
    kinetic.nativeBegin(2);
    kinetic.nativeDecelerate(2, 800);
    expect(scroller.scrollTop).toBe(0);
  });

  it("keeps an entire pixel-mode wheel burst on the native path", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(35);
    installKineticScroll(scroller);

    const first = wheel(scroller, 53);
    const second = wheel(scroller, 12);
    expect(first.defaultPrevented).toBe(false);
    expect(second.defaultPrevented).toBe(false);
  });

  it("does not carry a partial gesture across keyboard input", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(20);
    installKineticScroll(scroller);

    wheel(scroller, 0.6);
    scroller.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true }));
    wheel(scroller, 0.6);
    expect(scroller.scrollTop).toBe(0);
  });

  it("leaves touchscreen gestures and their synthesized wheels native", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    vi.spyOn(performance, "now").mockReturnValue(100);
    installKineticScroll(scroller);

    touch(scroller, "touchstart", 100);
    const duringTouch = wheel(scroller, 20);
    touch(scroller, "touchend", 140, false);
    const touchMomentum = wheel(scroller, 20);
    expect(duringTouch.defaultPrevented).toBe(false);
    expect(touchMomentum.defaultPrevented).toBe(false);
  });
});
