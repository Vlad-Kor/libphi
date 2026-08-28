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

  it("holds tiny finger wiggles below the gesture activation distance", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(20)
      .mockReturnValueOnce(30)
      .mockReturnValueOnce(120);
    installKineticScroll(scroller);

    expect(wheel(scroller, 0.8).defaultPrevented).toBe(true);
    wheel(scroller, -0.4);
    wheel(scroller, 1.1);
    expect(scroller.scrollTop).toBe(0);

    /* A pause starts another gesture, so held sub-pixel input cannot leak
     * into it and unexpectedly cross the threshold. */
    wheel(scroller, 1.8);
    expect(scroller.scrollTop).toBe(0);
  });

  it("stops exactly when the precision event stream stops", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(26);
    installKineticScroll(scroller);

    wheel(scroller, 12);
    wheel(scroller, 12);
    expect(scroller.scrollTop).toBe(24);
    expect(scroller.scrollTop).toBe(24);
  });

  it("leaves mouse wheels, horizontal gestures, and pinch zoom native", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    installKineticScroll(scroller);

    const mouse = wheel(scroller, 3, { deltaMode: WheelEvent.DOM_DELTA_LINE });
    const pixelWheel = wheel(scroller, 53);
    const horizontal = wheel(scroller, 2, { deltaX: 8 });
    const pinch = wheel(scroller, 8, { ctrlKey: true });
    expect(mouse.defaultPrevented).toBe(false);
    expect(pixelWheel.defaultPrevented).toBe(false);
    expect(horizontal.defaultPrevented).toBe(false);
    expect(pinch.defaultPrevented).toBe(false);
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

    wheel(scroller, 2);
    scroller.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true }));
    wheel(scroller, 2);
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
