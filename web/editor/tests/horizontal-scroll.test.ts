// @vitest-environment jsdom
import { describe, expect, it, vi } from "vitest";
import { wireHorizontalScroll } from "../src/horizontal-scroll";

function overflowingCodeBlock(): {
  host: HTMLElement; pre: HTMLElement;
} {
  const host = document.createElement("div");
  const pre = document.createElement("pre");
  host.append(pre);
  document.body.append(host);
  Object.defineProperties(pre, {
    clientWidth: { value: 300 },
    scrollWidth: { value: 900 },
    scrollLeft: { value: 100, writable: true },
    offsetWidth: { value: 300 },
    offsetHeight: { value: 100 },
    clientHeight: { value: 88 },
  });
  pre.getBoundingClientRect = () => ({
    left: 20, top: 30, right: 320, bottom: 130, width: 300, height: 100,
    x: 20, y: 30, toJSON: () => ({}),
  });
  return { host, pre };
}

describe("horizontal scrollbar wheel routing", () => {
  it("maps WebKitGTK's vertical scrollbar delta back to horizontal motion", () => {
    const { host, pre } = overflowingCodeBlock();
    wireHorizontalScroll(pre, false);
    const outer = vi.fn();
    host.addEventListener("wheel", outer);
    const pan = new WheelEvent("wheel", {
      bubbles: true, cancelable: true, clientX: 100, clientY: 125,
      deltaX: 0, deltaY: 48,
    });

    pre.dispatchEvent(pan);

    expect(pre.scrollLeft).toBe(148);
    expect(pan.defaultPrevented).toBe(true);
    expect(outer).not.toHaveBeenCalled();
    host.remove();
  });

  it("leaves content-area and vertical gestures on their native paths", () => {
    const { host, pre } = overflowingCodeBlock();
    wireHorizontalScroll(pre, false);
    const bubbled = vi.fn();
    host.addEventListener("wheel", bubbled);
    const content = new WheelEvent("wheel", {
      bubbles: true, cancelable: true, clientX: 100, clientY: 80,
      deltaX: 48, deltaY: 3,
    });
    const vertical = new WheelEvent("wheel", {
      bubbles: true, cancelable: true, clientX: 100, clientY: 80,
      deltaX: 3, deltaY: 48,
    });

    pre.dispatchEvent(content);
    pre.dispatchEvent(vertical);

    expect(pre.scrollLeft).toBe(100);
    expect(content.defaultPrevented).toBe(false);
    expect(vertical.defaultPrevented).toBe(false);
    expect(bubbled).toHaveBeenCalledTimes(2);
    host.remove();
  });

  it("contains remapped scrollbar motion at a horizontal boundary", () => {
    const { host, pre } = overflowingCodeBlock();
    wireHorizontalScroll(pre, false);
    pre.scrollLeft = 600;
    const bubbled = vi.fn();
    host.addEventListener("wheel", bubbled);
    const pan = new WheelEvent("wheel", {
      bubbles: true, cancelable: true, clientX: 100, clientY: 125,
      deltaX: 0, deltaY: 48,
    });

    pre.dispatchEvent(pan);

    expect(pre.scrollLeft).toBe(600);
    expect(pan.defaultPrevented).toBe(true);
    expect(bubbled).not.toHaveBeenCalled();
    host.remove();
  });
});
