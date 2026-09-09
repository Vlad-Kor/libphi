const wired = new WeakSet<HTMLElement>();

/** Apply one nested horizontal-scroll wheel event to a scroller. */
export function consumeHorizontalWheel(
  target: HTMLElement,
  event: WheelEvent,
  includeContent = true,
): boolean {
  // WebKit exposes pinch zoom as a Ctrl-modified wheel gesture. Ordinary
  // vertical gestures must remain owned by CodeMirror's outer scroller.
  if (event.ctrlKey || event.defaultPrevented ||
      target.scrollWidth <= target.clientWidth)
    return false;

  const scrollbar = isHorizontalScrollbarEvent(target, event);
  if (!scrollbar && (!includeContent ||
      Math.abs(event.deltaX) <= Math.abs(event.deltaY)))
    return false;
  /* WebKitGTK remaps a physical horizontal touchpad gesture to deltaY while
   * the pointer is over horizontal scrollbar chrome. Use the dominant axis in
   * that gutter; over content, preserve the ordinary deltaX behavior. */
  const delta = scrollbar && Math.abs(event.deltaY) > Math.abs(event.deltaX)
    ? event.deltaY : event.deltaX;
  if (!delta) return false;

  const scale = event.deltaMode === WheelEvent.DOM_DELTA_LINE ? 16 :
    event.deltaMode === WheelEvent.DOM_DELTA_PAGE ? target.clientWidth : 1;
  const maximum = target.scrollWidth - target.clientWidth;
  target.scrollLeft = Math.max(0, Math.min(
    maximum, target.scrollLeft + delta * scale,
  ));
  /* Contain horizontal momentum at both edges. Otherwise its small vertical
   * component can escape into the document after scrollLeft is clamped. */
  event.preventDefault();
  event.stopPropagation();
  return true;
}

/** Whether a pointer-coordinate event lies in an element's bottom scrollbar. */
export function isHorizontalScrollbarEvent(
  target: HTMLElement,
  event: MouseEvent,
): boolean {
  if (target.scrollWidth <= target.clientWidth ||
      !target.offsetHeight || !target.offsetWidth)
    return false;
  const rect = target.getBoundingClientRect();
  const scaleX = rect.width / target.offsetWidth;
  const scaleY = rect.height / target.offsetHeight;
  const left = rect.left + target.clientLeft * scaleX;
  const top = rect.top + target.clientTop * scaleY;
  const scrollbarTop = top + target.clientHeight * scaleY;
  return scrollbarTop < rect.bottom &&
    event.clientX >= left &&
    event.clientX < left + target.clientWidth * scaleX &&
    event.clientY >= scrollbarTop && event.clientY < rect.bottom;
}

/** Install the direct nested-scroller policy without involving the editor. */
export function wireHorizontalScroll(
  target: HTMLElement,
  includeContent = true,
): void {
  if (wired.has(target)) return;
  wired.add(target);
  target.addEventListener("wheel", (event) => {
    consumeHorizontalWheel(target, event, includeContent);
  }, { passive: false });
}
