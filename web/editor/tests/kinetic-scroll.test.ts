// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";
import { installKineticScroll } from "../src/kinetic-scroll";

afterEach(() => {
  vi.useRealTimers();
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

describe("WebKitGTK kinetic scrolling workaround", () => {
  it("eases pixel-precision vertical gestures on animation frames", () => {
    const scroller = document.createElement("div");
    document.body.append(scroller);
    let frame: FrameRequestCallback | undefined;
    vi.spyOn(window, "requestAnimationFrame").mockImplementation((callback) => {
      frame = callback;
      return 1;
    });
    vi.spyOn(performance, "now").mockReturnValue(10);
    installKineticScroll(scroller);

    const event = wheel(scroller, 23);
    expect(event.defaultPrevented).toBe(true);
    expect(scroller.scrollTop).toBe(0);
    frame?.(26);
    expect(scroller.scrollTop).toBeGreaterThan(0);
    expect(scroller.scrollTop).toBeLessThan(23);
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

  it("continues a multi-event gesture with frame-driven momentum", () => {
    vi.useFakeTimers();
    const scroller = document.createElement("div");
    document.body.append(scroller);
    let frame: FrameRequestCallback | undefined;
    vi.spyOn(window, "requestAnimationFrame").mockImplementation((callback) => {
      frame = callback;
      return 1;
    });
    vi.spyOn(window, "cancelAnimationFrame").mockImplementation(() => {});
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(26)
      .mockReturnValueOnce(60);
    installKineticScroll(scroller);

    wheel(scroller, 12);
    wheel(scroller, 12);
    expect(scroller.scrollTop).toBe(0);
    frame?.(76);
    const afterInput = scroller.scrollTop;
    expect(afterInput).toBeGreaterThan(0);
    vi.advanceTimersByTime(34);
    expect(frame).toBeTypeOf("function");
    frame?.(108);
    expect(scroller.scrollTop).toBeGreaterThan(afterInput);
  });

  it("never adds animation or momentum to a pixel-mode wheel burst", () => {
    vi.useFakeTimers();
    const scroller = document.createElement("div");
    document.body.append(scroller);
    const requestFrame = vi.spyOn(window, "requestAnimationFrame")
      .mockImplementation(() => 1);
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(35);
    installKineticScroll(scroller);

    const first = wheel(scroller, 53);
    const second = wheel(scroller, 53);
    vi.advanceTimersByTime(500);
    expect(first.defaultPrevented).toBe(false);
    expect(second.defaultPrevented).toBe(false);
    expect(requestFrame).not.toHaveBeenCalled();
  });

  it("cancels pending momentum when the user presses a key", () => {
    vi.useFakeTimers();
    const scroller = document.createElement("div");
    document.body.append(scroller);
    const requestFrame = vi.spyOn(window, "requestAnimationFrame")
      .mockImplementation(() => 1);
    const cancelFrame = vi.spyOn(window, "cancelAnimationFrame")
      .mockImplementation(() => {});
    vi.spyOn(performance, "now")
      .mockReturnValueOnce(10)
      .mockReturnValueOnce(26);
    installKineticScroll(scroller);

    wheel(scroller, 12);
    wheel(scroller, 12);
    scroller.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true }));
    vi.advanceTimersByTime(100);
    expect(requestFrame).toHaveBeenCalledTimes(1);
    expect(cancelFrame).toHaveBeenCalledWith(1);
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
