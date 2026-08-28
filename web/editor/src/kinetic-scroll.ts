const GESTURE_GAP_MS = 80;
const PIXEL_WHEEL_GESTURE_THRESHOLD = 32;
const GTK_DECELERATION_FRICTION = 4;
const GTK_MIN_VELOCITY = 0.1;

export interface KineticScrollHandle {
  nativeBegin(generation: number): void;
  nativeDecelerate(generation: number, velocityY: number): void;
  dispose(): void;
}

/**
 * Route precision deltas around WebKit's occasionally stuck scrolling path.
 *
 * The native host supplies GTK's real scroll-begin/decelerate boundary and
 * velocity. Input deltas are applied immediately and the post-release curve is
 * the same y'' = -4y' curve used by GtkScrolledWindow. There is no timeout that
 * guesses when a gesture ended and no extra low-pass filter over active input.
 */
export function installKineticScroll(scroller: HTMLElement): KineticScrollHandle {
  let lastWheelAt: number | null = null;
  let nativeWheelGesture = false;
  let precisionGestureActive = false;
  let precisionGestureMoved = false;
  let fractionalDelta = 0;
  let nativeTouchActive = false;
  let nativeTouchUntil = 0;
  let nativeGeneration = 0;
  let kineticFrame = 0;

  const cancelKinetic = () => {
    if (kineticFrame) window.cancelAnimationFrame(kineticFrame);
    kineticFrame = 0;
  };

  const resetPrecisionGesture = () => {
    lastWheelAt = null;
    nativeWheelGesture = false;
    precisionGestureActive = false;
    precisionGestureMoved = false;
    fractionalDelta = 0;
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
      cancelKinetic();
      nativeWheelGesture = Math.abs(event.deltaY) >=
        PIXEL_WHEEL_GESTURE_THRESHOLD;
      precisionGestureActive = false;
      precisionGestureMoved = false;
      fractionalDelta = 0;
    }
    lastWheelAt = now;
    if (nativeWheelGesture) return;

    event.preventDefault();
    precisionGestureActive = true;
    fractionalDelta += event.deltaY;
    /* DOM scroll positions retain fractions even though GTK ultimately paints
     * on device pixels. Keep a signed remainder so a tiny back-and-forth
     * finger wiggle cancels instead of visibly nudging the document. */
    const delta = Math.trunc(fractionalDelta);
    if (!delta) return;
    fractionalDelta -= delta;
    const previous = scroller.scrollTop;
    scroller.scrollTop += delta;
    precisionGestureMoved ||= scroller.scrollTop !== previous;
  };

  const onPointerDown = () => {
    cancelKinetic();
    resetPrecisionGesture();
  };
  const onKeyDown = () => {
    cancelKinetic();
    resetPrecisionGesture();
  };
  const onTouchStart = () => {
    cancelKinetic();
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

  return {
    nativeBegin(generation: number): void {
      nativeGeneration = generation;
      cancelKinetic();
    },

    nativeDecelerate(generation: number, velocityY: number): void {
      if (generation !== nativeGeneration || !precisionGestureActive) return;
      const shouldDecelerate = precisionGestureMoved &&
        Number.isFinite(velocityY);
      precisionGestureActive = false;
      precisionGestureMoved = false;
      fractionalDelta = 0;
      cancelKinetic();
      if (!shouldDecelerate || Math.abs(velocityY) < GTK_MIN_VELOCITY) return;

      const initialPosition = scroller.scrollTop;
      const initialVelocity = velocityY;
      const startedAt = performance.now();
      const tick = (now: number) => {
        const elapsed = Math.max(0, now - startedAt) / 1000;
        const decay = Math.exp(-GTK_DECELERATION_FRICTION * elapsed);
        const position = initialPosition +
          initialVelocity / GTK_DECELERATION_FRICTION * (1 - decay);
        const previous = scroller.scrollTop;
        scroller.scrollTop = position;
        const velocity = initialVelocity * decay;
        if ((scroller.scrollTop === previous && position !== previous) ||
            Math.abs(velocity) < GTK_MIN_VELOCITY) {
          kineticFrame = 0;
          return;
        }
        kineticFrame = window.requestAnimationFrame(tick);
      };
      kineticFrame = window.requestAnimationFrame(tick);
    },

    dispose(): void {
      cancelKinetic();
      resetPrecisionGesture();
      scroller.removeEventListener("wheel", onWheel);
      scroller.removeEventListener("pointerdown", onPointerDown);
      scroller.removeEventListener("keydown", onKeyDown);
      scroller.removeEventListener("touchstart", onTouchStart);
      scroller.removeEventListener("touchend", onTouchFinish);
      scroller.removeEventListener("touchcancel", onTouchFinish);
    },
  };
}
