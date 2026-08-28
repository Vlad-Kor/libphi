const GESTURE_GAP_MS = 80;
const PRECISION_GESTURE_ACTIVATION_PX = 3;
const PIXEL_WHEEL_GESTURE_THRESHOLD = 32;

/**
 * Keep precision scrolling out of WebKit's occasionally stuck kinetic path.
 *
 * The wheel stream already contains the device's pixel deltas. Apply those
 * deltas directly, just as GtkScrolledWindow updates its adjustment while a
 * scroll gesture is active. In particular, do not smooth input again or
 * synthesize another momentum phase after the input stream stops.
 *
 * A new precision gesture must move a few pixels before it takes ownership of
 * the scroller. This is the same practical dead zone users get from native GTK
 * scrolling and prevents tiny post-gesture finger movements from nudging the
 * document. Discrete mouse wheels, large pixel-mode wheel steps, touch input,
 * pinch zoom, and horizontal child scrolling stay on WebKit's native path.
 */
export function installKineticScroll(scroller: HTMLElement): () => void {
  let lastWheelAt: number | null = null;
  let nativeWheelGesture = false;
  let precisionGestureActive = false;
  let heldDelta = 0;
  let nativeTouchActive = false;
  let nativeTouchUntil = 0;

  const resetPrecisionGesture = () => {
    lastWheelAt = null;
    nativeWheelGesture = false;
    precisionGestureActive = false;
    heldDelta = 0;
  };

  const onWheel = (event: WheelEvent) => {
    /* Pinch zoom is represented as Ctrl+wheel. Horizontal-dominant gestures
     * may belong to an overflowing equation and remain on the native path. */
    const now = performance.now();
    if (nativeTouchActive || now < nativeTouchUntil ||
        event.ctrlKey || event.defaultPrevented ||
        event.deltaMode !== WheelEvent.DOM_DELTA_PIXEL ||
        Math.abs(event.deltaY) <= Math.abs(event.deltaX)) return;

    const gap = lastWheelAt === null ? Number.POSITIVE_INFINITY
      : now - lastWheelAt;
    const newGesture = lastWheelAt === null || gap <= 0 ||
      gap > GESTURE_GAP_MS;
    if (newGesture) {
      nativeWheelGesture = Math.abs(event.deltaY) >=
        PIXEL_WHEEL_GESTURE_THRESHOLD;
      precisionGestureActive = false;
      heldDelta = 0;
    }
    lastWheelAt = now;
    if (nativeWheelGesture) return;

    event.preventDefault();
    if (!precisionGestureActive) {
      heldDelta += event.deltaY;
      if (Math.abs(heldDelta) < PRECISION_GESTURE_ACTIVATION_PX) return;
      precisionGestureActive = true;
      scroller.scrollTop += heldDelta;
      heldDelta = 0;
      return;
    }
    scroller.scrollTop += event.deltaY;
  };

  const onPointerDown = () => resetPrecisionGesture();
  const onKeyDown = () => resetPrecisionGesture();
  const onTouchStart = () => {
    resetPrecisionGesture();
    nativeTouchActive = true;
  };
  const onTouchFinish = () => {
    nativeTouchActive = false;
    nativeTouchUntil = performance.now() + 500;
  };
  scroller.addEventListener("wheel", onWheel, { passive: false });
  scroller.addEventListener("pointerdown", onPointerDown, { passive: true });
  scroller.addEventListener("keydown", onKeyDown, { passive: true });
  scroller.addEventListener("touchstart", onTouchStart, { passive: true });
  scroller.addEventListener("touchend", onTouchFinish, { passive: true });
  scroller.addEventListener("touchcancel", onTouchFinish, { passive: true });
  return () => {
    resetPrecisionGesture();
    scroller.removeEventListener("wheel", onWheel);
    scroller.removeEventListener("pointerdown", onPointerDown);
    scroller.removeEventListener("keydown", onKeyDown);
    scroller.removeEventListener("touchstart", onTouchStart);
    scroller.removeEventListener("touchend", onTouchFinish);
    scroller.removeEventListener("touchcancel", onTouchFinish);
  };
}
