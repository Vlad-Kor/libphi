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
} from "../obsidian/markdown";
import { remoteImagesAllowed } from "../settings";

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

function reveal(view: EditorView, position: number): void {
  view.dispatch({ selection: { anchor: Math.min(position + 1, view.state.doc.length) }, scrollIntoView: true });
  view.focus();
}

function vaultUri(path: string): string {
  return `vault:///${path.split("/").map(encodeURIComponent).join("/")}`;
}

function isExternal(target: string): boolean {
  return /^(?:https?|mailto|obsidian):/i.test(target);
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
  toDOM(): HTMLElement {
    const bullet = document.createElement("span");
    bullet.className = "list-bullet";
    bullet.textContent = "•";
    bullet.setAttribute("aria-hidden", "true");
    return bullet;
  }
}

export class MathWidget extends WidgetType {
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
      other.editing === this.editing;
  }

  toDOM(view: EditorView): HTMLElement {
    const element = document.createElement(this.display ? "div" : "span");
    element.className = this.display ? "math-widget math-display" : "math-widget math-inline";
    if (this.editing) element.classList.add("math-edit-preview");
    element.tabIndex = 0;
    element.setAttribute("aria-label", `LaTeX: ${this.latex}`);
    element.addEventListener("click", () => reveal(view, this.from));
    if (this.display) wireMathScroll(element);
    void renderMath(this.latex, this.display, element);
    return element;
  }

  ignoreEvent(): boolean { return false; }
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

function interactiveImage(
  view: EditorView,
  image: HTMLImageElement,
  from: number,
  to: number,
  block: boolean,
): HTMLElement {
  const container = document.createElement("span");
  container.className = `image-widget image-widget-${block ? "block" : "inline"}`;
  image.addEventListener("load", () => view.requestMeasure());
  const source = document.createElement("button");
  source.type = "button";
  source.className = "image-source-button";
  source.title = "Edit image source";
  source.setAttribute("aria-label", "Edit image source");
  source.append(imageCodeIcon());
  source.addEventListener("pointerdown", (event) => event.stopPropagation());
  source.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    reveal(view, from);
  });

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
  container.addEventListener("pointerdown", (event) => event.stopPropagation());
  container.append(image, source, handle);
  return container;
}

export class LinkWidget extends WidgetType {
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
      other.embed === this.embed && other.block === this.block;
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
      image.classList.add("dimmed");
      requestNative<{ path?: string }>("attachment/resolve", { target: path })
        .then((result) => {
          if (!result.path) throw new Error("Image was not found in the vault");
          image.src = vaultUri(result.path);
          image.classList.remove("dimmed");
        })
        .catch((error) => {
          reportError(error, "image");
          image.replaceWith(imageError(error));
        });
      return interactiveImage(view, image, this.from, this.to, this.block);
    }
    if (["mp3", "ogg", "wav", "flac", "m4a"].includes(extension ?? "")) {
      const audio = document.createElement("audio");
      audio.className = "media-embed";
      audio.controls = true;
      audio.src = vaultUri(path);
      return audio;
    }
    if (["mp4", "webm", "ogv", "mov"].includes(extension ?? "")) {
      const video = document.createElement("video");
      video.className = "media-embed";
      video.controls = true;
      video.src = vaultUri(path);
      return video;
    }
    if (extension === "pdf") {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "attachment-embed";
      button.textContent = `Open PDF: ${path}`;
      button.addEventListener("click", () => sendNative("attachment/open", { target: path }));
      return button;
    }
    const container = document.createElement("section");
    container.className = "note-embed";
    const title = document.createElement("button");
    title.type = "button";
    title.className = "embed-title";
    title.textContent = this.target;
    title.addEventListener("click", () => sendNative("link/open", { target: this.target }));
    const body = document.createElement("div");
    body.className = "embed-body dimmed";
    body.textContent = "Loading embed…";
    container.append(title, body);
    requestNative<{ text?: string }>("embed/read", { target: this.target, depth: 1 })
      .then((result) => {
        body.classList.remove("dimmed");
        body.innerHTML = renderMarkdown(result?.text ?? "");
        wireRenderedContent(body);
      })
      .catch((error) => {
        body.className = "embed-body render-error";
        body.textContent = error instanceof Error ? error.message : String(error);
      });
    container.addEventListener("dblclick", (event) => {
      if (event.target === container || event.target === body) reveal(view, this.from);
    });
    return container;
  }

  ignoreEvent(): boolean { return this.embed; }
}

export class MarkdownLinkWidget extends WidgetType {
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
      other.to === this.to && other.block === this.block;
  }

  toDOM(view: EditorView): HTMLElement {
    if (this.image) {
      const image = document.createElement("img");
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
        image.classList.add("dimmed");
        requestNative<{ path?: string }>("attachment/resolve", {
          target: this.target,
          relative: true,
        }).then((result) => {
          if (!result.path) throw new Error("Image was not found in the vault");
          image.src = vaultUri(result.path);
          image.classList.remove("dimmed");
        }).catch((error) => {
          reportError(error, "image");
          image.replaceWith(imageError(error));
        });
      }
      return interactiveImage(view, image, this.from, this.to, this.block);
    }

    const link = document.createElement("a");
    link.className = "markdown-link";
    link.href = "#";
    link.innerHTML = renderMarkdownInline(this.label || this.target);
    wireRenderedContent(link);
    link.addEventListener("click", (event) => {
      event.preventDefault();
      if (isExternal(this.target)) sendNative("url/open", { uri: this.target });
      else if (this.target.startsWith("#")) sendNative("link/open", { target: this.target });
      else sendNative("attachment/open", { target: this.target, relative: true });
    });
    link.addEventListener("dblclick", () => reveal(view, this.from));
    return link;
  }

  ignoreEvent(): boolean { return this.image; }
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
  ignoreEvent(): boolean { return false; }
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
    definition.addEventListener("dblclick", () => reveal(view, this.from));
    return definition;
  }

  ignoreEvent(): boolean { return false; }
}

export class HtmlPreviewWidget extends WidgetType {
  constructor(readonly source: string, readonly from: number, readonly className = "markdown-preview") { super(); }
  eq(other: HtmlPreviewWidget): boolean { return other.source === this.source && other.className === this.className; }

  toDOM(view: EditorView): HTMLElement {
    const container = document.createElement("div");
    container.className = this.className;
    container.innerHTML = renderMarkdown(this.source);
    wireRenderedContent(container);
    container.addEventListener("dblclick", () => reveal(view, this.from));
    return container;
  }

  ignoreEvent(): boolean { return false; }
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
    title.textContent = this.title || this.type.replace(/(^|-)(\p{L})/gu, (_m, prefix, letter) => `${prefix ? " " : ""}${letter.toUpperCase()}`);
    const body = document.createElement("div");
    body.className = "callout-content";
    body.innerHTML = renderMarkdown(this.body);
    wireRenderedContent(body);
    details.append(title, body);
    details.addEventListener("dblclick", (event) => {
      if (event.target !== title) reveal(view, this.from);
    });
    return details;
  }

  ignoreEvent(): boolean { return false; }
}

export class MermaidWidget extends WidgetType {
  constructor(readonly source: string, readonly from: number) { super(); }
  eq(other: MermaidWidget): boolean { return other.source === this.source; }

  toDOM(view: EditorView): HTMLElement {
    const container = document.createElement("div");
    container.className = "mermaid-widget";
    container.tabIndex = 0;
    container.addEventListener("dblclick", () => reveal(view, this.from));
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

  ignoreEvent(): boolean { return false; }
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
  constructor(readonly source: string, readonly from: number) { super(); }
  eq(other: PropertiesWidget): boolean { return other.source === this.source; }

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
        raw.addEventListener("click", () => reveal(view, this.from));
        container.append(raw);
      }
    } catch (error) {
      const warning = document.createElement("button");
      warning.type = "button";
      warning.className = "render-error";
      warning.textContent = "Invalid or complex properties — edit source";
      warning.addEventListener("click", () => reveal(view, this.from));
      container.append(warning);
    }
    return container;
  }

  ignoreEvent(): boolean { return false; }
}

export class RawHtmlWidget extends WidgetType {
  constructor(
    readonly source: string,
    readonly from: number,
    readonly remoteAllowed = remoteImagesAllowed(),
  ) { super(); }
  eq(other: RawHtmlWidget): boolean {
    return other.source === this.source &&
      other.remoteAllowed === this.remoteAllowed;
  }
  toDOM(view: EditorView): HTMLElement {
    const container = document.createElement(rawHtmlIsBlock(this.source) ? "div" : "span");
    container.className = "raw-html-widget";
    container.innerHTML = renderRawHtml(this.source);
    wireRenderedContent(container);
    container.addEventListener("dblclick", () => reveal(view, this.from));
    return container;
  }
  ignoreEvent(): boolean { return false; }
}
