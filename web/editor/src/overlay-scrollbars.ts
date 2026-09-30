/* GTK shows overlay scrollbars: hidden while nothing happens, a thin
 * indicator while scrolling, and a wider slider under the pointer. WebKitGTK
 * draws them itself; engines that draw their own platform's scrollbars
 * instead (WebView2 on Windows) get the same behaviour from editor.css. */

/** Width of the scrollbar area, which must match editor.css. */
const SCROLLBAR_WIDTH = 14;
/** How long the indicator stays after the last scroll or movement, in ms. */
const HIDE_DELAY = 1000;

export function toolkitScrollbarsDrawnByEngine(): boolean {
  return window.phiHost?.toolkitScrollbars !== false;
}

export function installOverlayScrollbars(scroller: HTMLElement): void {
  if (toolkitScrollbarsDrawnByEngine()) return;
  document.documentElement.classList.add("phi-overlay-scrollbars");

  let hideTimer = 0;
  let hovering = false;
  const setClass = (name: string, enabled: boolean) =>
    scroller.classList.toggle(name, enabled);
  const show = () => {
    setClass("phi-scrollbar-visible", true);
    window.clearTimeout(hideTimer);
    hideTimer = window.setTimeout(() => {
      if (!hovering) setClass("phi-scrollbar-visible", false);
    }, HIDE_DELAY);
  };
  const setHovering = (value: boolean) => {
    hovering = value;
    setClass("phi-scrollbar-hover", value);
    show();
  };

  scroller.addEventListener("scroll", show, { passive: true });
  scroller.addEventListener("pointermove", (event) => {
    const rect = scroller.getBoundingClientRect();
    const scrollable = scroller.scrollHeight > scroller.clientHeight;
    const over = scrollable && event.clientX >= rect.right - SCROLLBAR_WIDTH;
    if (over !== hovering) setHovering(over);
    else if (scrollable) show();
  }, { passive: true });
  scroller.addEventListener("pointerleave", () => setHovering(false));
}
