import { EditorView, WidgetType } from "@codemirror/view";
import { parseDocument } from "yaml";
import { requestNative, reportError, sendNative } from "../bridge";
import { getMathRevision, renderMath, wireMathScroll } from "../math/mathjax";
import {
  rawHtmlIsBlock,
  renderMarkdown,
  renderMarkdownInline,
  renderRawHtml,
  sanitizeHtml,
  wireRenderedContent,
} from "../markdown/render";
import { remoteImagesAllowed } from "../settings";
import { calloutIcon } from "../markdown/callout-icons";
import { pinPreviewSource } from "../markdown/source-edit";
import {
  chooseHardPreview,
  selectHardPreview,
  selectedHardPreview,
} from "../markdown/preview-interaction";

let mermaidSequence = 0;
interface MermaidApi {
  initialize(config: Record<string, unknown>): void;
  render(id: string, source: string): Promise<{ svg: string }>;
}

interface MermaidWindow extends Window {
  phiMermaid?: MermaidApi;
}

let mermaidReady: Promise<MermaidApi> | undefined;

function ensureMermaidReady(): Promise<MermaidApi> {
  const existing = (window as MermaidWindow).phiMermaid;
  if (existing) return Promise.resolve(existing);
  if (!mermaidReady) {
    mermaidReady = new Promise<MermaidApi>((resolve, reject) => {
      const script = document.createElement("script");
      script.src = "app://editor/mermaid.js";
      script.async = true;
      script.dataset.phiRenderer = "mermaid";
      script.addEventListener("load", () => {
        const mermaid = (window as MermaidWindow).phiMermaid;
        if (!mermaid) {
          reject(new Error("Mermaid runtime did not initialize"));
          return;
        }
        mermaid.initialize({
          startOnLoad: false,
          securityLevel: "strict",
          theme: "base",
          suppressErrorRendering: true,
        });
        resolve(mermaid);
      }, { once: true });
      script.addEventListener("error", () =>
        reject(new Error("Mermaid runtime could not be loaded")), { once: true });
      document.head.append(script);
    });
  }
  return mermaidReady;
}

function reveal(
  view: EditorView,
  position: number,
  pinnedSource?: { from: number; to: number },
): void {
  view.dispatch({
    selection: { anchor: Math.min(position + 1, view.state.doc.length) },
    effects: pinnedSource ? pinPreviewSource.of(pinnedSource) : [],
    scrollIntoView: true,
  });
  view.focus();
}

function revealAt(view: EditorView, position: number): void {
  view.dispatch({
    selection: { anchor: Math.max(0, Math.min(position, view.state.doc.length)) },
    scrollIntoView: true,
  });
  view.focus();
}

function selectedSourceOffset(root: Node, source: string): number | null {
  const selection = window.getSelection();
  if (!selection || selection.isCollapsed || !selection.anchorNode ||
      !root.contains(selection.anchorNode)) return null;
  const selected = selection.toString();
  if (!selected) return null;
  const offset = source.indexOf(selected);
  return offset >= 0 ? offset : null;
}

function clickedSourceOffset(
  root: Node,
  source: string,
  event: MouseEvent,
): number | null {
  const selected = selectedSourceOffset(root, source);
  if (selected != null) return selected;
  const caretDocument = document as Document & {
    caretRangeFromPoint?(x: number, y: number): Range | null;
    caretPositionFromPoint?(x: number, y: number): {
      offsetNode: Node;
      offset: number;
    } | null;
  };
  const range = caretDocument.caretRangeFromPoint?.(
    event.clientX, event.clientY,
  );
  const position = range
    ? { node: range.startContainer, offset: range.startOffset }
    : (() => {
      const caret = caretDocument.caretPositionFromPoint?.(
        event.clientX, event.clientY,
      );
      return caret ? { node: caret.offsetNode, offset: caret.offset } : null;
    })();
  if (!position || !root.contains(position.node)) return null;
  const text = position.node.nodeType === Node.TEXT_NODE
    ? position.node.textContent ?? ""
    : "";
  if (!text) return null;
  const at = source.indexOf(text);
  return at < 0 ? null : at + Math.min(position.offset, text.length);
}

function markHardRenderedItem(
  view: EditorView,
  element: HTMLElement,
  from: number,
  to: number,
  clickSelects: boolean,
): HTMLElement {
  element.classList.add("cm-hard-rendered-item");
  element.dataset.hardPreviewFrom = String(from);
  element.dataset.hardPreviewTo = String(to);
  element.setAttribute("aria-selected", "false");
  if (clickSelects) {
    element.addEventListener("pointerdown", (event) => {
      event.preventDefault();
      event.stopPropagation();
      chooseHardPreview(view, { from, to });
    });
  } else {
    element.addEventListener("pointerdown", () => {
      if (selectedHardPreview(view.state))
        view.dispatch({ effects: selectHardPreview.of(null) });
    });
  }
  return element;
}

function calloutBodyPosition(
  view: EditorView,
  from: number,
  body: string,
  bodyOffset: number,
): number {
  const before = body.slice(0, bodyOffset);
  const lines = before.split("\n");
  const header = view.state.doc.lineAt(from);
  const number = Math.min(header.number + lines.length, view.state.doc.lines);
  const line = view.state.doc.line(number);
  const quote = /^ {0,3}>[ \t]?/.exec(line.text)?.[0].length ?? 0;
  return line.from + quote + lines.at(-1)!.length;
}

function vaultUri(path: string): string {
  return `vault:///${path.split("/").map(encodeURIComponent).join("/")}`;
}

function isExternal(target: string): boolean {
  return /^(?:https?|mailto|obsidian):/i.test(target);
}

const FALLBACK_BLOCK_IMAGE_HEIGHT = 360;
const FALLBACK_NOTE_EMBED_HEIGHT = 360;
const PREVIEW_IMAGE_CACHE_LIMIT = 256;
const PREVIEW_HEIGHT_CACHE_LIMIT = 512;

interface PreviewGeometryContext {
  documentPath: string;
  textWidth: number;
  fontScale: number;
  key: string;
}

let previewGeometryContext: PreviewGeometryContext = {
  documentPath: "",
  textWidth: 780,
  fontScale: 1,
  key: "\u0000w780:s100",
};

/* Geometry is deliberately bucketed. Tiny allocation differences should not
 * invalidate otherwise useful measurements, and a 16 px width difference is
 * smaller than the uncertainty in CodeMirror's off-screen line estimates. */
export function setPreviewGeometryContext(
  documentPath: string,
  textWidth: number,
  fontScale: number,
): void {
  const width = Math.max(160, Math.round((Number.isFinite(textWidth)
    ? textWidth : 780) / 16) * 16);
  const scale = Math.max(0.5, Math.min(3, Number.isFinite(fontScale)
    ? fontScale : 1));
  previewGeometryContext = {
    documentPath,
    textWidth: width,
    fontScale: scale,
    key: `${documentPath}\u0000w${width}:s${Math.round(scale * 100)}`,
  };
}

function lruGet<T>(cache: Map<string, T>, key: string): T | undefined {
  const value = cache.get(key);
  if (value === undefined) return undefined;
  cache.delete(key);
  cache.set(key, value);
  return value;
}

function lruSet<T>(
  cache: Map<string, T>,
  key: string,
  value: T,
  limit: number,
): void {
  cache.delete(key);
  cache.set(key, value);
  while (cache.size > limit)
    cache.delete(cache.keys().next().value!);
}

const previewHeightCache = new Map<string, number>();

function measuredHeight(context: PreviewGeometryContext, key: string): number | undefined {
  return lruGet(previewHeightCache, `${context.key}\u0000${key}`);
}

function rememberHeight(
  context: PreviewGeometryContext,
  key: string,
  height: number,
): void {
  if (!Number.isFinite(height) || height < 1) return;
  lruSet(
    previewHeightCache,
    `${context.key}\u0000${key}`,
    Math.max(1, Math.round(height * 2) / 2),
    PREVIEW_HEIGHT_CACHE_LIMIT,
  );
}

function estimateWrappedLineHeight(
  text: string,
  fontSize: number,
  lineHeight: number,
  availableWidth: number,
  paddingTop = 0,
): number {
  /* Adwaita Sans averages a little over half an em for prose. This is only an
   * off-screen estimate—the real DOM measurement replaces it on first view. */
  const textWidth = Math.max(fontSize, text.length * fontSize * 0.54);
  const lines = Math.max(1, Math.ceil(textWidth / Math.max(80, availableWidth)));
  return lines * lineHeight + paddingTop;
}

export function estimatedHeadingHeight(level: number, text: string): number {
  const factors = [2.25, 1.75, 1.42, 1.18, 1.08, 1];
  const factor = factors[Math.max(0, Math.min(5, level - 1))];
  const fontSize = 16 * previewGeometryContext.fontScale * factor;
  return estimateWrappedLineHeight(
    text,
    fontSize,
    fontSize * 1.25,
    previewGeometryContext.textWidth,
    fontSize * 0.28,
  );
}

export function estimatedListLineHeight(text: string, indentEm: number): number {
  const fontSize = 16 * previewGeometryContext.fontScale;
  return estimateWrappedLineHeight(
    text,
    fontSize,
    fontSize * 1.62,
    previewGeometryContext.textWidth - indentEm * fontSize,
  );
}

/* A zero-width widget lets CodeMirror's height map use a realistic cold
 * estimate while leaving the actual Markdown line and all editing semantics
 * untouched. Once the line is mounted, CodeMirror measures the real DOM. */
export class LineHeightEstimateWidget extends WidgetType {
  constructor(readonly height: number) { super(); }
  eq(other: LineHeightEstimateWidget): boolean {
    return Math.abs(other.height - this.height) < 0.5;
  }
  get estimatedHeight(): number { return this.height; }
  toDOM(): HTMLElement {
    const marker = document.createElement("span");
    marker.className = "cm-height-estimate";
    marker.setAttribute("aria-hidden", "true");
    return marker;
  }
}

interface PreviewImageCacheEntry {
  uri?: string;
  resolving?: Promise<string>;
  naturalWidth?: number;
  naturalHeight?: number;
}

/* CodeMirror intentionally unmounts widgets well outside the viewport. Keep a
 * small process-local cache across document switches and reopenings. Keys are
 * scoped to the source note because the same relative image target can resolve
 * to different files in different folders. */
const previewImageCaches = new Map<string, PreviewImageCacheEntry>();

function previewImageCache(
  context: PreviewGeometryContext,
  key: string,
): PreviewImageCacheEntry {
  const scopedKey = `${context.documentPath}\u0000${key}`;
  let entry = lruGet(previewImageCaches, scopedKey);
  if (!entry) {
    entry = {};
    lruSet(previewImageCaches, scopedKey, entry, PREVIEW_IMAGE_CACHE_LIMIT);
  }
  return entry;
}

export function resetPreviewGeometryCaches(): void {
  previewImageCaches.clear();
  previewHeightCache.clear();
}

export interface PreviewImageGeometry {
  target: string;
  path: string;
  width: number;
  height: number;
}

export function seedPreviewImageGeometry(
  entries: readonly PreviewImageGeometry[],
): void {
  for (const geometry of entries.slice(0, 64)) {
    if (!geometry.target || !geometry.path || geometry.width <= 0 ||
        geometry.height <= 0) continue;
    const key = `${previewGeometryContext.documentPath}\u0000local:${geometry.target}`;
    lruSet(previewImageCaches, key, {
      uri: vaultUri(geometry.path),
      naturalWidth: geometry.width,
      naturalHeight: geometry.height,
    }, PREVIEW_IMAGE_CACHE_LIMIT);
  }
}

function resolveLocalImage(
  image: HTMLImageElement,
  target: string,
  cacheKey: string,
  context: PreviewGeometryContext,
): void {
  const entry = previewImageCache(context, cacheKey);
  const apply = (uri: string) => {
    if (entry.naturalWidth && entry.naturalHeight &&
        !image.width && !image.height) {
      image.width = entry.naturalWidth;
      image.height = entry.naturalHeight;
    }
    image.src = uri;
    image.classList.remove("dimmed");
  };
  if (entry.uri) {
    apply(entry.uri);
    return;
  }

  image.classList.add("dimmed");
  if (!entry.resolving) {
    const resolving = requestNative<{
      path?: string;
      width?: number;
      height?: number;
    }>(
      "attachment/resolve", {
        target,
        relative: true,
        sourcePath: context.documentPath,
      },
    ).then((result) => {
      if (!result.path) throw new Error("Image was not found in the vault");
      if ((result.width ?? 0) > 0 && (result.height ?? 0) > 0) {
        entry.naturalWidth = result.width;
        entry.naturalHeight = result.height;
      }
      const uri = vaultUri(result.path);
      entry.uri = uri;
      return uri;
    });
    entry.resolving = resolving;
    void resolving.then(
      () => { if (entry.resolving === resolving) entry.resolving = undefined; },
      () => { if (entry.resolving === resolving) entry.resolving = undefined; },
    );
  }
  void entry.resolving.then(apply).catch((error) => {
    reportError(error, "image");
    image.replaceWith(imageError(error));
  });
}

function targetIsImage(target: string): boolean {
  return /\.(?:png|jpe?g|gif|webp|svg|avif)(?:$|[|?#])/i.test(target);
}

function estimatedBlockImageHeight(...sources: string[]): number {
  for (const source of sources) {
    const size = source.match(/\|(\d+)(?:x(\d+))?(?:$|[?#])/);
    if (!size) continue;
    if (size[2]) return Math.max(40, Math.min(900, Number(size[2])));
    return Math.max(80, Math.min(600, Number(size[1]) * 0.6));
  }
  return FALLBACK_BLOCK_IMAGE_HEIGHT;
}

function estimatedCachedImageHeight(
  context: PreviewGeometryContext,
  cacheKey: string,
  ...sources: string[]
): number {
  const fallback = estimatedBlockImageHeight(...sources);
  const scopedKey = `${context.documentPath}\u0000${cacheKey}`;
  const entry = lruGet(previewImageCaches, scopedKey);
  if (!entry?.naturalWidth || !entry.naturalHeight) return fallback;
  for (const source of sources) {
    const size = source.match(/\|(\d+)(?:x(\d+))?(?:$|[?#])/);
    if (size?.[2]) return fallback;
    if (size?.[1]) {
      const width = Math.min(context.textWidth, Number(size[1]));
      return Math.max(1, width * entry.naturalHeight / entry.naturalWidth);
    }
  }
  const width = Math.min(context.textWidth, entry.naturalWidth);
  return Math.max(1, width * entry.naturalHeight / entry.naturalWidth);
}

const widgetResizeObservers = new WeakMap<HTMLElement, ResizeObserver>();

function observeNoteEmbedResize(
  view: EditorView,
  element: HTMLElement,
  context: PreviewGeometryContext,
  geometryKey: string,
): void {
  const measure = () => view.requestMeasure({
    key: element,
    read: () => element.isConnected
      ? element.getBoundingClientRect().height
      : 0,
    write: (height) => rememberHeight(context, geometryKey, height),
  });
  measure();
  if (typeof ResizeObserver === "undefined") return;
  const observer = new ResizeObserver(measure);
  observer.observe(element);
  widgetResizeObservers.set(element, observer);
}

function stopObservingWidgetResize(element: HTMLElement): void {
  widgetResizeObservers.get(element)?.disconnect();
  widgetResizeObservers.delete(element);
}

export class HiddenWidget extends WidgetType {
  toDOM(): HTMLElement {
    const span = document.createElement("span");
    span.className = "cm-concealed";
    span.setAttribute("aria-hidden", "true");
    return span;
  }
}

export class BulletWidget extends WidgetType {
  constructor(readonly label = "•", readonly ordered = false) { super(); }

  toDOM(): HTMLElement {
    const bullet = document.createElement("span");
    bullet.className = this.ordered
      ? "list-marker list-number"
      : "list-marker list-bullet";
    bullet.textContent = this.label;
    bullet.setAttribute("aria-hidden", "true");
    return bullet;
  }
}

export class HorizontalRuleWidget extends WidgetType {
  constructor(readonly from: number, readonly to: number) { super(); }
  eq(other: HorizontalRuleWidget): boolean {
    return other.from === this.from && other.to === this.to;
  }
  toDOM(view: EditorView): HTMLElement {
    const rule = document.createElement("hr");
    rule.className = "horizontal-rule-widget";
    rule.setAttribute("aria-label", "Horizontal rule");
    const revealRule = (event: MouseEvent) => {
      event.preventDefault();
      event.stopPropagation();
      revealAt(view, this.from);
    };
    rule.addEventListener("pointerdown", revealRule);
    rule.addEventListener("click", revealRule);
    return rule;
  }
}

export class MathWidget extends WidgetType {
  readonly geometryContext = previewGeometryContext;

  constructor(
    readonly latex: string,
    readonly display: boolean,
    readonly from: number,
    readonly mathRevision = getMathRevision(),
    readonly editing = false,
  ) { super(); }

  eq(other: MathWidget): boolean {
    return other.latex === this.latex && other.display === this.display &&
      other.from === this.from && other.mathRevision === this.mathRevision &&
      other.editing === this.editing &&
      other.geometryContext.key === this.geometryContext.key;
  }

  private get geometryKey(): string {
    return `math:${this.mathRevision}:${this.latex}`;
  }

  get estimatedHeight(): number {
    if (!this.display) return -1;
    const cached = measuredHeight(this.geometryContext, this.geometryKey);
    if (cached != null) return cached;
    const rows = Math.max(
      1,
      this.latex.split(/(?:\\\\|\n|\\begin\{(?:align\*?|gather\*?|matrix|pmatrix|bmatrix)\})/).length,
    );
    return Math.max(64, Math.min(720,
      (42 + (rows - 1) * 24) * this.geometryContext.fontScale));
  }

  toDOM(view: EditorView): HTMLElement {
    const element = document.createElement(this.display ? "div" : "span");
    element.className = this.display ? "math-widget math-display" : "math-widget math-inline";
    if (this.editing) element.classList.add("math-edit-preview");
    element.tabIndex = 0;
    element.setAttribute("aria-label", `LaTeX: ${this.latex}`);
    const revealMath = (event: Event) => {
      event.preventDefault();
      event.stopPropagation();
      reveal(view, this.from);
    };
    element.addEventListener("pointerdown", revealMath);
    element.addEventListener("click", revealMath);
    const rendered = renderMath(this.latex, this.display, element);
    if (this.display) void rendered.then(() => {
      wireMathScroll(element);
      view.requestMeasure({
        key: element,
        read: () => element.isConnected
          ? element.getBoundingClientRect().height
          : 0,
        write: (height) => rememberHeight(
          this.geometryContext, this.geometryKey, height,
        ),
      });
    });
    return element;
  }

  ignoreEvent(): boolean { return true; }
}

export function resizeImageMarkdown(source: string, width: number): string {
  const size = String(Math.max(40, Math.round(width)));
  const wiki = /^!\[\[([\s\S]*)\]\]$/.exec(source);
  if (wiki) {
    const pipe = wiki[1].lastIndexOf("|");
    const target = (pipe >= 0 ? wiki[1].slice(0, pipe) : wiki[1]).trimEnd();
    return `![[${target}|${size}]]`;
  }
  const markdown = /^!\[([\s\S]*?)\](\([\s\S]*\))$/.exec(source);
  if (markdown) {
    const alt = markdown[1].replace(/\|\d+(?:x\d+)?\s*$/, "");
    return `![${alt}|${size}]${markdown[2]}`;
  }
  return source;
}

function imageCodeIcon(): SVGElement {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 16 16");
  svg.setAttribute("aria-hidden", "true");
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", "M5.5 3.5 1.5 8l4 4.5M10.5 3.5l4 4.5-4 4.5M9.25 2 6.75 14");
  svg.append(path);
  return svg;
}

function sourceEditButton(
  view: EditorView,
  from: number,
  to: number,
  label: string,
): HTMLButtonElement {
  const source = document.createElement("button");
  source.type = "button";
  source.className = "image-source-button";
  source.title = label;
  source.setAttribute("aria-label", label);
  source.append(imageCodeIcon());
  source.addEventListener("pointerdown", (event) => event.stopPropagation());
  source.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    reveal(view, from, { from, to });
  });
  return source;
}

function interactiveImage(
  view: EditorView,
  image: HTMLImageElement,
  from: number,
  to: number,
  block: boolean,
  cacheKey: string,
  estimatedHeight: number,
  context: PreviewGeometryContext,
  geometryKey: string,
): HTMLElement {
  const container = document.createElement("span");
  container.className = `image-widget image-widget-${block ? "block" : "inline"}`;
  const cache = previewImageCache(context, cacheKey);
  if (cache.naturalWidth && cache.naturalHeight) {
    if (image.width && !image.height) {
      image.height = Math.max(1, Math.round(
        image.width * cache.naturalHeight / cache.naturalWidth,
      ));
    } else if (!image.width && !image.height) {
      image.width = cache.naturalWidth;
      image.height = cache.naturalHeight;
    }
  } else if (block && !image.height) {
    container.style.minHeight = `${estimatedHeight}px`;
  }
  image.decoding = "async";
  let measured = false;
  const imageLoaded = () => {
    if (image.naturalWidth > 0 && image.naturalHeight > 0) {
      cache.naturalWidth = image.naturalWidth;
      cache.naturalHeight = image.naturalHeight;
    }
    container.style.removeProperty("min-height");
    if (measured) return;
    measured = true;
    view.requestMeasure({
      key: container,
      read: () => container.isConnected
        ? container.getBoundingClientRect().height
        : 0,
      write: (height) => rememberHeight(context, geometryKey, height),
    });
  };
  image.addEventListener("load", imageLoaded);
  if (image.complete && image.naturalWidth > 0) queueMicrotask(imageLoaded);
  const source = sourceEditButton(view, from, to, "Edit image source");

  const handle = document.createElement("span");
  handle.className = "image-resize-handle";
  handle.tabIndex = 0;
  handle.setAttribute("role", "separator");
  handle.setAttribute("aria-label", "Resize image");
  handle.title = "Drag to resize";
  const commit = (width: number) => {
    const current = view.state.sliceDoc(from, to);
    const replacement = resizeImageMarkdown(current, width);
    if (replacement !== current)
      view.dispatch({ changes: { from, to, insert: replacement }, userEvent: "input.resize-image" });
  };
  handle.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const startX = event.clientX;
    const startWidth = Math.max(40, image.getBoundingClientRect().width || image.naturalWidth || 300);
    let width = startWidth;
    handle.setPointerCapture?.(event.pointerId);
    const move = (moveEvent: PointerEvent) => {
      width = Math.max(40, Math.min(1600, startWidth + moveEvent.clientX - startX));
      image.style.width = `${Math.round(width)}px`;
      image.style.height = "auto";
    };
    const finish = (finishEvent: PointerEvent) => {
      handle.removeEventListener("pointermove", move);
      handle.removeEventListener("pointerup", finish);
      handle.removeEventListener("pointercancel", cancel);
      if (finishEvent.type === "pointerup") commit(width);
    };
    const cancel = (cancelEvent: PointerEvent) => finish(cancelEvent);
    handle.addEventListener("pointermove", move);
    handle.addEventListener("pointerup", finish);
    handle.addEventListener("pointercancel", cancel);
  });
  handle.addEventListener("keydown", (event) => {
    if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") return;
    event.preventDefault();
    event.stopPropagation();
    const current = image.getBoundingClientRect().width || image.width || 300;
    commit(current + (event.key === "ArrowRight" ? 10 : -10));
  });
  container.append(image, source, handle);
  return markHardRenderedItem(view, container, from, to, true);
}

export class LinkWidget extends WidgetType {
  readonly geometryContext = previewGeometryContext;

  constructor(
    readonly target: string,
    readonly label: string,
    readonly from: number,
    readonly to: number,
    readonly embed = false,
    readonly block = true,
  ) { super(); }

  eq(other: LinkWidget): boolean {
    return other.target === this.target && other.label === this.label &&
      other.from === this.from && other.to === this.to &&
      other.embed === this.embed && other.block === this.block &&
      other.geometryContext.key === this.geometryContext.key;
  }

  private get geometryKey(): string {
    return `embed:${this.target}:${this.label}`;
  }

  get estimatedHeight(): number {
    if (!this.embed || !this.block) return -1;
    const cached = measuredHeight(this.geometryContext, this.geometryKey);
    if (cached != null) return cached;
    const path = this.target.split(/[|#]/)[0];
    return targetIsImage(this.target)
      ? estimatedCachedImageHeight(
        this.geometryContext, `local:${path}`, this.target,
      )
      : FALLBACK_NOTE_EMBED_HEIGHT;
  }

  toDOM(view: EditorView): HTMLElement {
    if (this.embed) return this.embedDOM(view);
    const link = document.createElement("a");
    link.className = "internal-link";
    link.href = "#";
    link.textContent = this.label;
    link.addEventListener("click", (event) => {
      event.preventDefault();
      sendNative("link/open", {
        target: this.target,
        ctrl: event.ctrlKey,
        shift: event.shiftKey,
        middle: event.button === 1,
      });
    });
    link.addEventListener("dblclick", () => reveal(view, this.from));
    return link;
  }

  private embedDOM(view: EditorView): HTMLElement {
    const path = this.target.split(/[|#]/)[0];
    const extension = path.split(".").pop()?.toLowerCase();
    if (["png", "jpg", "jpeg", "gif", "webp", "svg", "avif"].includes(extension ?? "")) {
      const image = document.createElement("img");
      const cacheKey = `local:${path}`;
      image.className = "image-embed";
      image.alt = this.label;
      image.addEventListener("error", () => {
        const error = new Error(`Could not load image: ${path}`);
        reportError(error, "image");
        image.replaceWith(imageError(error));
      }, { once: true });
      const size = this.label.match(/^(\d+)(?:x(\d+))?$/);
      if (size) {
        image.width = Number(size[1]);
        if (size[2]) image.height = Number(size[2]);
      }
      resolveLocalImage(
        image, path, cacheKey, this.geometryContext,
      );
      return interactiveImage(
        view, image, this.from, this.to, this.block, cacheKey,
        this.estimatedHeight, this.geometryContext, this.geometryKey,
      );
    }
    if (["mp3", "ogg", "wav", "flac", "m4a"].includes(extension ?? "")) {
      const audio = document.createElement("audio");
      audio.className = "media-embed";
      audio.controls = true;
      audio.src = vaultUri(path);
      return this.attachmentDOM(view, audio, "Edit audio embed source");
    }
    if (["mp4", "webm", "ogv", "mov"].includes(extension ?? "")) {
      const video = document.createElement("video");
      video.className = "media-embed";
      video.controls = true;
      video.src = vaultUri(path);
      return this.attachmentDOM(view, video, "Edit video embed source");
    }
    if (extension === "pdf") {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "attachment-embed";
      button.textContent = `Open PDF: ${path}`;
      button.addEventListener("click", () => sendNative("attachment/open", { target: path }));
      return this.attachmentDOM(view, button, "Edit PDF embed source");
    }
    const container = document.createElement("section");
    container.className = "note-embed";
    const reservedHeight = this.estimatedHeight;
    container.style.minHeight = `${reservedHeight}px`;
    const title = document.createElement("button");
    title.type = "button";
    title.className = "embed-title";
    title.textContent = this.target;
    title.addEventListener("click", () => sendNative("link/open", { target: this.target }));
    const source = sourceEditButton(
      view, this.from, this.to, "Edit note embed source",
    );
    source.classList.add("embed-source-button");
    const body = document.createElement("div");
    body.className = "embed-body dimmed";
    body.textContent = "Loading embed…";
    container.append(title, source, body);
    requestNative<{ text?: string; path?: string }>(
      "embed/read", { target: this.target, depth: 1 },
    )
      .then((result) => {
        body.classList.remove("dimmed");
        body.innerHTML = renderMarkdown(result?.text ?? "");
        wireRenderedContent(body, result?.path ?? "");
        container.style.removeProperty("min-height");
        observeNoteEmbedResize(
          view, container, this.geometryContext, this.geometryKey,
        );
      })
      .catch((error) => {
        body.className = "embed-body render-error";
        body.textContent = error instanceof Error ? error.message : String(error);
        container.style.removeProperty("min-height");
        view.requestMeasure();
      });
    return markHardRenderedItem(
      view, container, this.from, this.to, false,
    );
  }

  private attachmentDOM(
    view: EditorView,
    content: HTMLElement,
    editLabel: string,
  ): HTMLElement {
    const container = document.createElement("span");
    container.className = `attachment-widget attachment-widget-${
      this.block ? "block" : "inline"}`;
    const source = sourceEditButton(
      view, this.from, this.to, editLabel,
    );
    source.classList.add("embed-source-button");
    container.append(content, source);
    return markHardRenderedItem(
      view, container, this.from, this.to, false,
    );
  }

  ignoreEvent(): boolean { return this.embed; }

  destroy(dom: HTMLElement): void {
    stopObservingWidgetResize(dom);
  }
}

export class MarkdownLinkWidget extends WidgetType {
  readonly geometryContext = previewGeometryContext;

  constructor(
    readonly target: string,
    readonly label: string,
    readonly from: number,
    readonly image: boolean,
    readonly to = from,
    readonly block = true,
  ) { super(); }

  eq(other: MarkdownLinkWidget): boolean {
    return other.target === this.target && other.label === this.label &&
      other.from === this.from && other.image === this.image &&
      other.to === this.to && other.block === this.block &&
      other.geometryContext.key === this.geometryContext.key;
  }

  private get geometryKey(): string {
    return `image:${this.target}:${this.label}`;
  }

  private get imageCacheKey(): string {
    if (/^data:/i.test(this.target)) return `data:${this.from}:${this.to}`;
    if (/^https?:/i.test(this.target)) return `remote:${this.target}`;
    return `local:${this.target}`;
  }

  private get linkedImage(): boolean {
    return !this.image && /^(?:!\[|<img\b)/i.test(this.label.trim());
  }

  get estimatedHeight(): number {
    if (!this.image || !this.block) return -1;
    return measuredHeight(this.geometryContext, this.geometryKey) ??
      estimatedCachedImageHeight(
        this.geometryContext, this.imageCacheKey, this.label, this.target,
      );
  }

  toDOM(view: EditorView): HTMLElement {
    if (this.image) {
      const image = document.createElement("img");
      const localCacheKey = `local:${this.target}`;
      const cacheKey = this.imageCacheKey;
      image.className = "image-embed";
      image.alt = this.label.split("|")[0];
      image.addEventListener("error", () => {
        const error = new Error(`Could not load image: ${this.target}`);
        reportError(error, "image");
        image.replaceWith(imageError(error));
      }, { once: true });
      const size = this.label.match(/\|(\d+)(?:x(\d+))?$/);
      if (size) {
        image.width = Number(size[1]);
        if (size[2]) image.height = Number(size[2]);
      }
      if (/^data:/i.test(this.target) ||
          (/^https?:/i.test(this.target) && remoteImagesAllowed())) image.src = this.target;
      else if (/^https?:/i.test(this.target)) {
        image.classList.add("remote-image-blocked");
        image.tabIndex = 0;
        image.title = "Remote image blocked — click to load";
        image.addEventListener("click", () => {
          image.src = this.target;
          image.classList.remove("remote-image-blocked");
        }, { once: true });
      }
      else {
        resolveLocalImage(
          image, this.target, localCacheKey, this.geometryContext,
        );
      }
      return interactiveImage(
        view, image, this.from, this.to, this.block, cacheKey,
        this.estimatedHeight, this.geometryContext, this.geometryKey,
      );
    }

    const link = document.createElement("a");
    link.className = "markdown-link";
    link.href = "#";
    link.innerHTML = renderMarkdownInline(this.label || this.target);
    wireRenderedContent(link);
    link.addEventListener("click", (event) => {
      event.preventDefault();
      if (this.linkedImage) return;
      if (isExternal(this.target)) sendNative("url/open", { uri: this.target });
      else if (this.target.startsWith("#")) sendNative("link/open", { target: this.target });
      else sendNative("attachment/open", { target: this.target, relative: true });
    });
    link.addEventListener("dblclick", () => reveal(view, this.from));
    if (this.linkedImage) {
      const container = document.createElement("span");
      container.className = "image-widget image-widget-inline linked-image-widget";
      container.append(
        link,
        sourceEditButton(view, this.from, this.to, "Edit linked image source"),
      );
      return markHardRenderedItem(
        view, container, this.from, this.to, true,
      );
    }
    return link;
  }

  ignoreEvent(): boolean { return this.image || this.linkedImage; }
}

function imageError(error: unknown): HTMLElement {
  const message = document.createElement("span");
  message.className = "render-error image-error";
  message.textContent = `Image unavailable: ${error instanceof Error ? error.message : String(error)}`;
  return message;
}

export class TaskWidget extends WidgetType {
  constructor(readonly status: string, readonly from: number) { super(); }
  eq(other: TaskWidget): boolean {
    return other.status === this.status && other.from === this.from;
  }

  toDOM(view: EditorView): HTMLElement {
    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.className = `task-checkbox task-status-${encodeURIComponent(this.status || "space")}`;
    checkbox.checked = this.status.toLowerCase() === "x";
    checkbox.indeterminate = this.status !== " " && this.status.toLowerCase() !== "x";
    checkbox.setAttribute("aria-label", `Task status ${this.status === " " ? "not done" : this.status}`);
    checkbox.addEventListener("pointerdown", (event) => event.stopPropagation());
    checkbox.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      if (this.status !== " " && this.status.toLowerCase() !== "x") {
        checkbox.checked = false;
        checkbox.indeterminate = true;
        return;
      }
      const next = this.status.toLowerCase() === "x" ? " " : "x";
      view.dispatch({ changes: { from: this.from + 1, to: this.from + 2, insert: next }, userEvent: "input" });
      view.focus();
    });
    return checkbox;
  }

  ignoreEvent(): boolean { return true; }
}

export class TagWidget extends WidgetType {
  constructor(readonly tag: string, readonly from: number) { super(); }
  eq(other: TagWidget): boolean { return other.tag === this.tag; }
  toDOM(view: EditorView): HTMLElement {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "cm-live-tag";
    button.textContent = `#${this.tag}`;
    button.addEventListener("click", () => sendNative("tag/open", { tag: this.tag }));
    button.addEventListener("dblclick", () => reveal(view, this.from));
    return button;
  }
  ignoreEvent(): boolean { return true; }
}

export class FootnoteWidget extends WidgetType {
  constructor(
    readonly text: string,
    readonly from: number,
    readonly definition = false,
    readonly target = from,
    readonly label = text,
  ) { super(); }

  eq(other: FootnoteWidget): boolean {
    return other.text === this.text && other.definition === this.definition &&
      other.target === this.target && other.label === this.label;
  }

  toDOM(view: EditorView): HTMLElement {
    if (!this.definition) {
      const reference = document.createElement("button");
      reference.type = "button";
      reference.className = "footnote-reference";
      reference.textContent = this.label;
      reference.setAttribute("aria-label", `Footnote ${this.label}`);
      reference.addEventListener("click", () => reveal(view, this.target));
      return reference;
    }
    const definition = document.createElement("aside");
    definition.className = "footnote-definition";
    const label = document.createElement("strong");
    label.textContent = this.label;
    const content = document.createElement("div");
    content.innerHTML = renderMarkdown(this.text);
    wireRenderedContent(content);
    definition.append(label, content);
    definition.addEventListener("pointerdown", (event) => {
      event.preventDefault();
      event.stopPropagation();
      reveal(view, this.from);
    });
    definition.addEventListener("click", () => reveal(view, this.from));
    definition.addEventListener("dblclick", () => reveal(view, this.from));
    return definition;
  }

  ignoreEvent(): boolean { return true; }
}

export class HtmlPreviewWidget extends WidgetType {
  constructor(
    readonly source: string,
    readonly from: number,
    readonly className = "markdown-preview",
    readonly to = from + source.length,
    readonly hard = false,
  ) { super(); }
  eq(other: HtmlPreviewWidget): boolean {
    return other.source === this.source && other.className === this.className &&
      other.from === this.from && other.to === this.to &&
      other.hard === this.hard;
  }

  toDOM(view: EditorView): HTMLElement {
    const container = document.createElement("div");
    container.className = this.className;
    container.innerHTML = renderMarkdown(this.source);
    wireRenderedContent(container);
    if (this.hard)
      return markHardRenderedItem(
        view, container, this.from, this.to, false,
      );
    const revealClick = (event: MouseEvent) => {
      event.preventDefault();
      event.stopPropagation();
      const selected = clickedSourceOffset(container, this.source, event);
      const firstContent = this.source.indexOf("\n") + 1;
      const fallback = Math.max(1, firstContent);
      const offset = this.className === "code-block-widget" &&
        (selected ?? -1) < fallback
        ? fallback
        : selected ?? fallback;
      revealAt(view, this.from + offset);
    };
    container.addEventListener("pointerdown", revealClick);
    container.addEventListener("click", revealClick);
    container.addEventListener("dblclick", revealClick);
    return container;
  }

  ignoreEvent(): boolean { return true; }
}

export class CalloutWidget extends WidgetType {
  constructor(
    readonly body: string,
    readonly type: string,
    readonly title: string,
    readonly fold: string,
    readonly from: number,
  ) { super(); }

  eq(other: CalloutWidget): boolean {
    return other.body === this.body && other.type === this.type && other.title === this.title && other.fold === this.fold;
  }

  toDOM(view: EditorView): HTMLElement {
    const details = document.createElement("details");
    details.className = "callout";
    details.dataset.callout = this.type;
    details.open = this.fold !== "-";
    const title = document.createElement("summary");
    title.className = "callout-title";
    const label = this.title || this.type.replace(/(^|-)(\p{L})/gu,
      (_m, prefix, letter) => `${prefix ? " " : ""}${letter.toUpperCase()}`);
    title.append(calloutIcon(this.type), document.createTextNode(label));
    wireRenderedContent(title);
    const body = document.createElement("div");
    body.className = "callout-content";
    body.innerHTML = renderMarkdown(this.body);
    wireRenderedContent(body);
    details.append(title, body);
    const revealClick = (event: MouseEvent) => {
      event.preventDefault();
      event.stopPropagation();
      if (event.target instanceof Node && title.contains(event.target)) {
        revealAt(view, this.from + 2);
        return;
      }
      const selected = clickedSourceOffset(body, this.body, event) ?? 0;
      revealAt(view, calloutBodyPosition(view, this.from, this.body, selected));
    };
    details.addEventListener("pointerdown", revealClick);
    details.addEventListener("click", revealClick);
    details.addEventListener("dblclick", revealClick);
    return details;
  }

  ignoreEvent(): boolean { return true; }
}

export class MermaidWidget extends WidgetType {
  constructor(readonly source: string, readonly from: number) { super(); }
  eq(other: MermaidWidget): boolean { return other.source === this.source; }

  toDOM(view: EditorView): HTMLElement {
    const container = document.createElement("div");
    container.className = "mermaid-widget";
    container.tabIndex = 0;
    const revealMermaid = (event: MouseEvent) => {
      event.preventDefault();
      event.stopPropagation();
      reveal(view, this.from);
    };
    container.addEventListener("pointerdown", revealMermaid);
    container.addEventListener("click", revealMermaid);
    container.addEventListener("dblclick", revealMermaid);
    const id = `phi-mermaid-${++mermaidSequence}`;
    ensureMermaidReady()
      .then((mermaid) => mermaid.render(id, this.source))
      .then(({ svg }) => { container.innerHTML = sanitizeHtml(svg); })
      .catch((error) => {
        container.className = "mermaid-widget render-error";
        container.textContent = error instanceof Error ? error.message : String(error);
        reportError(error, "mermaid");
      });
    return container;
  }

  ignoreEvent(): boolean { return true; }
}

interface SimpleProperty { key: string; value: string; from: number; to: number }

function simpleProperties(source: string, base: number): { properties: SimpleProperty[]; complex: boolean } {
  const properties: SimpleProperty[] = [];
  let complex = false;
  let offset = source.startsWith("---") ? source.indexOf("\n") + 1 : 0;
  const lines = source.slice(offset).split("\n");
  for (let index = 0; index < lines.length; index++) {
    const line = lines[index];
    const match = /^([A-Za-z0-9_-]+):[ \t]*(.*)$/.exec(line);
    if (match) {
      const hasNestedValue = !match[2] && /^\s+\S/.test(lines[index + 1] ?? "");
      if (hasNestedValue) complex = true;
      else {
        const valueFrom = base + offset + line.indexOf(match[2]);
        properties.push({ key: match[1], value: match[2], from: valueFrom, to: valueFrom + match[2].length });
      }
    } else if (line.trim() && line.trim() !== "---") {
      complex = true;
    }
    offset += line.length + 1;
  }
  return { properties, complex };
}

export class PropertiesWidget extends WidgetType {
  constructor(
    readonly source: string,
    readonly from: number,
    readonly to = from + source.length,
  ) { super(); }
  eq(other: PropertiesWidget): boolean {
    return other.source === this.source && other.from === this.from &&
      other.to === this.to;
  }

  toDOM(view: EditorView): HTMLElement {
    const container = document.createElement("section");
    container.className = "properties-widget";
    const heading = document.createElement("div");
    heading.className = "properties-heading";
    heading.textContent = "Properties";
    container.append(heading);
    try {
      const yaml = this.source.replace(/^---[^\n]*\n/, "").replace(/\n---\s*$/, "");
      const yamlDocument = parseDocument(yaml, { keepSourceTokens: true });
      if (yamlDocument.errors.length) throw yamlDocument.errors[0];
      const parsed = simpleProperties(this.source, this.from);
      for (const property of parsed.properties) {
        const row = document.createElement("label");
        row.className = "property-row";
        const key = document.createElement("span");
        key.textContent = property.key;
        const input = document.createElement("input");
        input.setAttribute("aria-label", property.key);
        if (/^(?:true|false)$/i.test(property.value)) {
          input.type = "checkbox";
          input.checked = property.value.toLowerCase() === "true";
        } else if (/^-?\d+(?:\.\d+)?$/.test(property.value)) {
          input.type = "number";
          input.value = property.value;
        } else if (/^\d{4}-\d{2}-\d{2}$/.test(property.value)) {
          input.type = "date";
          input.value = property.value;
        } else {
          input.value = property.value;
        }
        input.addEventListener("change", () => {
          const value = input.type === "checkbox" ? String(input.checked) : input.value;
          view.dispatch({ changes: { from: property.from, to: property.to, insert: value }, userEvent: "input" });
        });
        row.append(key, input);
        container.append(row);
      }
      if (parsed.complex) {
        const raw = document.createElement("button");
        raw.type = "button";
        raw.className = "property-source-button";
        raw.textContent = "Edit lists or nested properties in source";
        raw.addEventListener("click", () => reveal(
          view, this.from, { from: this.from, to: this.to },
        ));
        container.append(raw);
      }
    } catch (error) {
      const warning = document.createElement("button");
      warning.type = "button";
      warning.className = "render-error";
      warning.textContent = "Invalid or complex properties — edit source";
      warning.addEventListener("click", () => reveal(
        view, this.from, { from: this.from, to: this.to },
      ));
      container.append(warning);
    }
    return markHardRenderedItem(
      view, container, this.from, this.to, false,
    );
  }

  ignoreEvent(): boolean { return true; }
}

export class RawHtmlWidget extends WidgetType {
  constructor(
    readonly source: string,
    readonly from: number,
    readonly to = from + source.length,
    readonly remoteAllowed = remoteImagesAllowed(),
  ) { super(); }
  private get hard(): boolean {
    return /<(?:iframe|table)\b/i.test(this.source);
  }
  eq(other: RawHtmlWidget): boolean {
    return other.source === this.source &&
      other.from === this.from && other.to === this.to &&
      other.remoteAllowed === this.remoteAllowed;
  }
  toDOM(view: EditorView): HTMLElement {
    const container = document.createElement(rawHtmlIsBlock(this.source) ? "div" : "span");
    container.className = "raw-html-widget";
    container.innerHTML = renderRawHtml(this.source);
    wireRenderedContent(container);
    if (this.hard) {
      const source = sourceEditButton(
        view, this.from, this.to, "Edit HTML embed source",
      );
      source.classList.add("embed-source-button");
      container.append(source);
      return markHardRenderedItem(
        view, container, this.from, this.to, false,
      );
    }
    const revealHtml = (rawEvent: Event) => {
      const event = rawEvent as MouseEvent;
      event.preventDefault();
      event.stopPropagation();
      const offset = clickedSourceOffset(container, this.source, event) ?? 0;
      revealAt(view, this.from + offset);
    };
    container.addEventListener("pointerdown", revealHtml);
    container.addEventListener("click", revealHtml);
    container.addEventListener("dblclick", revealHtml);
    return container;
  }
  ignoreEvent(): boolean { return true; }
}
