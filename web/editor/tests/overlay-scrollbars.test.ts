// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";
import { installOverlayScrollbars } from "../src/overlay-scrollbars";

function scrollableElement(): HTMLElement {
  const scroller = document.createElement("div");
  document.body.append(scroller);
  Object.defineProperties(scroller, {
    scrollHeight: { value: 2000 },
    clientHeight: { value: 500 },
  });
  scroller.getBoundingClientRect = () => new DOMRect(0, 0, 800, 500);
  return scroller;
}

function setHost(host: Window["phiHost"]): void {
  window.phiHost = host;
}

afterEach(() => {
  vi.useRealTimers();
  delete window.phiHost;
  document.documentElement.classList.remove("phi-overlay-scrollbars");
  document.body.replaceChildren();
});

describe("overlay scrollbars", () => {
  it("leaves scrollbars to engines that draw the toolkit's", () => {
    for (const host of [
      undefined,
      { postMessage: () => {} },
      { postMessage: () => {}, toolkitScrollbars: true },
    ]) {
      setHost(host);
      const scroller = scrollableElement();
      installOverlayScrollbars(scroller);
      scroller.dispatchEvent(new Event("scroll"));
      expect(document.documentElement.classList
        .contains("phi-overlay-scrollbars")).toBe(false);
      expect(scroller.className).toBe("");
    }
  });

  it("shows an indicator while scrolling and hides it afterwards", () => {
    vi.useFakeTimers();
    setHost({ postMessage: () => {}, toolkitScrollbars: false });
    const scroller = scrollableElement();
    installOverlayScrollbars(scroller);
    expect(document.documentElement.classList
      .contains("phi-overlay-scrollbars")).toBe(true);

    scroller.dispatchEvent(new Event("scroll"));
    expect(scroller.classList.contains("phi-scrollbar-visible")).toBe(true);
    vi.advanceTimersByTime(1000);
    expect(scroller.classList.contains("phi-scrollbar-visible")).toBe(false);
  });

  it("widens the slider under the pointer and keeps it shown", () => {
    vi.useFakeTimers();
    setHost({ postMessage: () => {}, toolkitScrollbars: false });
    const scroller = scrollableElement();
    installOverlayScrollbars(scroller);

    scroller.dispatchEvent(new MouseEvent("pointermove", { clientX: 795 }));
    expect(scroller.classList.contains("phi-scrollbar-hover")).toBe(true);
    vi.advanceTimersByTime(5000);
    expect(scroller.classList.contains("phi-scrollbar-visible")).toBe(true);

    scroller.dispatchEvent(new MouseEvent("pointermove", { clientX: 400 }));
    expect(scroller.classList.contains("phi-scrollbar-hover")).toBe(false);
    vi.advanceTimersByTime(1000);
    expect(scroller.classList.contains("phi-scrollbar-visible")).toBe(false);
  });
});
