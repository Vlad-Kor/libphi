const GESTURE_GAP_MS = 80;
const MOMENTUM_DELAY_MS = 34;
const MOMENTUM_DECAY_PER_MS = 0.0035;
const INPUT_FOLLOW_MS = 12;
const MIN_MOMENTUM_SPEED = 0.025;
const MAX_MOMENTUM_SPEED = 3;
const PIXEL_WHEEL_GESTURE_THRESHOLD = 32;

/**
 * Work around WebKitGTK occasionally terminating touchpad momentum early.
 *
 * GTK touchpads arrive as pixel-precision wheel events. Applying those deltas
 * ourselves prevents WebKit's flaky kinetic animator from taking ownership of
 * the gesture; a short rAF animation supplies the momentum after the input
 * stream ends. Discrete mouse wheels stay on WebKit's native path.
 */
export function installKineticScroll(scroller: HTMLElement): () => void {
  let lastWheelAt = 0;
  let velocity = 0;
  let samples = 0;
  let pendingDelta = 0;
  let momentumActive = false;
  let momentumTimer = 0;
  let animationFrame = 0;
  let lastFrameAt = 0;
  let nativeTouchActive = false;
  let nativeTouchUntil = 0;
  let nativeWheelGesture = false;

  const cancelMomentum = () => {
    window.clearTimeout(momentumTimer);
    momentumTimer = 0;
    if (animationFrame) window.cancelAnimationFrame(animationFrame);
    animationFrame = 0;
    lastFrameAt = 0;
    pendingDelta = 0;
    momentumActive = false;
    velocity = 0;
    samples = 0;
  };

  const animate = (now: number) => {
    const elapsed = Math.min(32, Math.max(1, now - lastFrameAt));
    lastFrameAt = now;
    const previous = scroller.scrollTop;
    if (Math.abs(pendingDelta) > 0.05) {
      const inputStep = pendingDelta *
        (1 - Math.exp(-elapsed / INPUT_FOLLOW_MS));
      scroller.scrollTop += inputStep;
      pendingDelta -= inputStep;
    } else {
      scroller.scrollTop += pendingDelta;
      pendingDelta = 0;
    }
    if (momentumActive) {
      scroller.scrollTop += velocity * elapsed;
      velocity *= Math.exp(-MOMENTUM_DECAY_PER_MS * elapsed);
      if (Math.abs(velocity) < MIN_MOMENTUM_SPEED) momentumActive = false;
    }
    if (scroller.scrollTop === previous &&
        (pendingDelta || momentumActive)) {
      pendingDelta = 0;
      momentumActive = false;
    }
    if (!pendingDelta && !momentumActive) {
      animationFrame = 0;
      return;
    }
    animationFrame = window.requestAnimationFrame(animate);
  };

  const ensureAnimation = () => {
    if (animationFrame) return;
    lastFrameAt = performance.now();
    animationFrame = window.requestAnimationFrame(animate);
  };

  const startMomentum = () => {
    momentumTimer = 0;
    if (samples < 2 || Math.abs(velocity) < MIN_MOMENTUM_SPEED) return;
    momentumActive = true;
    ensureAnimation();
  };

  const onWheel = (event: WheelEvent) => {
    /* Pinch zoom is represented as Ctrl+wheel. Horizontal-dominant gestures
     * may belong to an overflowing equation and remain on the native path. */
    const now = performance.now();
    if (nativeTouchActive || now < nativeTouchUntil ||
        event.ctrlKey || event.defaultPrevented ||
        event.deltaMode !== WheelEvent.DOM_DELTA_PIXEL ||
        Math.abs(event.deltaY) <= Math.abs(event.deltaX)) return;

    const gap = now - lastWheelAt;
    const newGesture = lastWheelAt === 0 || gap <= 0 ||
      gap > GESTURE_GAP_MS;
    if (newGesture)
      nativeWheelGesture = Math.abs(event.deltaY) >=
        PIXEL_WHEEL_GESTURE_THRESHOLD;
    lastWheelAt = now;
    if (nativeWheelGesture) return;

    window.clearTimeout(momentumTimer);
    momentumTimer = 0;
    momentumActive = false;
    if (newGesture) {
      pendingDelta = 0;
      velocity = 0;
      samples = 0;
    }

    event.preventDefault();
    pendingDelta += event.deltaY;
    ensureAnimation();

    const elapsed = Math.min(40, Math.max(8, gap || 16));
    const instantaneous = Math.max(-MAX_MOMENTUM_SPEED,
      Math.min(MAX_MOMENTUM_SPEED, event.deltaY / elapsed));
    velocity = samples && Math.sign(velocity) === Math.sign(instantaneous)
      ? velocity * 0.55 + instantaneous * 0.45
      : instantaneous;
    samples++;
    momentumTimer = window.setTimeout(startMomentum, MOMENTUM_DELAY_MS);
  };

  const onPointerDown = () => cancelMomentum();
  const onKeyDown = () => cancelMomentum();
  const onTouchStart = () => {
    cancelMomentum();
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
    cancelMomentum();
    scroller.removeEventListener("wheel", onWheel);
    scroller.removeEventListener("pointerdown", onPointerDown);
    scroller.removeEventListener("keydown", onKeyDown);
    scroller.removeEventListener("touchstart", onTouchStart);
    scroller.removeEventListener("touchend", onTouchFinish);
    scroller.removeEventListener("touchcancel", onTouchFinish);
  };
}
