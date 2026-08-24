import DOMPurify from "dompurify";
import MarkdownIt from "markdown-it";
import footnote from "markdown-it-footnote";
import taskLists from "markdown-it-task-lists";
import { calloutIcon } from "./callout-icons";
import Prism from "prismjs";
import "prismjs/components/prism-bash";
import "prismjs/components/prism-c";
import "prismjs/components/prism-cpp";
import "prismjs/components/prism-css";
import "prismjs/components/prism-java";
import "prismjs/components/prism-javascript";
import "prismjs/components/prism-json";
import "prismjs/components/prism-latex";
import "prismjs/components/prism-markdown";
import "prismjs/components/prism-python";
import "prismjs/components/prism-rust";
import "prismjs/components/prism-typescript";
import "prismjs/components/prism-yaml";
import { sendNative } from "../bridge";
import { renderMath, wireMathScroll } from "../math/mathjax";
import { remoteImagesAllowed } from "../settings";

const escapeHtml = (value: string): string => value
  .replace(/&/g, "&amp;")
  .replace(/</g, "&lt;")
  .replace(/>/g, "&gt;")
  .replace(/"/g, "&quot;");

const md = new MarkdownIt({
  html: true,
  linkify: true,
  typographer: false,
  breaks: false,
  highlight(code, language): string {
    const grammar = Prism.languages[language];
    const value = grammar ? Prism.highlight(code, grammar, language) : escapeHtml(code);
    return `<pre class="language-${escapeHtml(language || "text")}"><code>${value}</code></pre>`;
  },
}).use(footnote).use(taskLists, { enabled: true, label: true });

const protectedHtml = /<(span|div|kbd|details|summary|sup|sub|small|mark|table|thead|tbody|tr|th|td|iframe)(\s[^>]*)?>[\s\S]*?<\/\1\s*>/gi;

function escapeAttribute(value: string): string {
  return escapeHtml(value);
}

function prepare(source: string): { source: string; html: string[] } {
  const html: string[] = [];
  let value = source.replace(protectedHtml, (match) => {
    const index = html.push(match) - 1;
    return `<phi-raw-html data-index="${index}"></phi-raw-html>`;
  });
  value = value
    .replace(/%%[\s\S]*?%%/g, "")
    .replace(/==([^\n=]+)==/g, "<mark>$1</mark>")
    .replace(/(!)?\[\[([^\]\n]+)\]\]/g, (_match, embed: string, raw: string) => {
      const split = raw.lastIndexOf("|");
      let target = (split >= 0 ? raw.slice(0, split) : raw).trim();
      if (split >= 0 && target.endsWith("\\")) target = target.slice(0, -1);
      const label = split >= 0 ? raw.slice(split + 1).trim() : target;
      const attributes = `data-wikilink="${escapeAttribute(target)}"`;
      if (embed)
        return `<button type="button" class="internal-embed" ${attributes}>${escapeAttribute(label)}</button>`;
      return `<a href="#" class="internal-link" ${attributes}>${escapeAttribute(label)}</a>`;
    });
  return { source: value, html };
}

function sanitize(html: string, allowFrames: boolean): string {
  return DOMPurify.sanitize(html, {
    USE_PROFILES: { html: true, svg: true, svgFilters: true },
    ADD_TAGS: allowFrames ? ["phi-raw-html", "iframe"] : ["phi-raw-html"],
    ADD_ATTR: [
      "data-index", "data-wikilink", "data-callout", "target", "sandbox",
      "loading", "referrerpolicy", "allowfullscreen", "frameborder",
    ],
    ALLOW_DATA_ATTR: true,
    FORBID_TAGS: [
      "script", "object", "embed", "form", ...(allowFrames ? [] : ["iframe"]),
    ],
    FORBID_ATTR: ["srcdoc"],
  });
}

export function sanitizeHtml(html: string): string {
  return sanitize(html, false);
}

export function renderMarkdown(source: string): string {
  const prepared = prepare(source);
  let rendered = md.render(prepared.source);
  rendered = rendered.replace(
    /<phi-raw-html data-index="(\d+)"><\/phi-raw-html>/g,
    (_match: string, index: string) => (prepared.html[Number(index)] ?? "")
      .replace(/^<([a-z][\w-]*)(?=\s|>)/i, '<$1 data-phi-raw-html=""'),
  );
  return applyRemoteContentPolicy(sanitize(rendered, true));
}

export function renderMarkdownInline(source: string): string {
  const prepared = prepare(source);
  let rendered = md.renderInline(prepared.source);
  rendered = rendered.replace(
    /<phi-raw-html data-index="(\d+)"><\/phi-raw-html>/g,
    (_match: string, index: string) => (prepared.html[Number(index)] ?? "")
      .replace(/^<([a-z][\w-]*)(?=\s|>)/i, '<$1 data-phi-raw-html=""'),
  );
  return applyRemoteContentPolicy(sanitize(rendered, true));
}

function safeFrameDimension(value: string): string {
  const trimmed = value.trim();
  return /^(?:\d+(?:\.\d+)?(?:px|em|rem|vh|vw|%)|auto)$/i.test(trimmed)
    ? trimmed
    : "";
}

function frameSource(value: string): string {
  let candidate = value.trim();
  const markdownLink = /^\[[^\]]*\]\((https:\/\/[\s\S]+)\)$/.exec(candidate);
  if (markdownLink) candidate = markdownLink[1].trim();
  try {
    const url = new URL(candidate);
    if (url.protocol !== "https:" || url.username || url.password) return "";
    return url.href;
  } catch {
    return "";
  }
}

function secureIframes(root: ParentNode): void {
  root.querySelectorAll<HTMLIFrameElement>("iframe").forEach((frame) => {
    const source = frameSource(frame.getAttribute("src") ?? "");
    if (!source) {
      const error = document.createElement("span");
      error.className = "render-error iframe-embed-error";
      error.textContent = "Embedded page blocked: only HTTPS URLs are supported";
      frame.replaceWith(error);
      return;
    }

    const width = safeFrameDimension(frame.style.width);
    const height = safeFrameDimension(frame.style.height);
    frame.removeAttribute("style");
    if (width) frame.style.width = width;
    if (height) frame.style.height = height;
    frame.classList.add("iframe-embed");
    frame.setAttribute("sandbox", "allow-forms allow-popups allow-same-origin allow-scripts");
    frame.setAttribute("loading", "lazy");
    frame.setAttribute("referrerpolicy", "no-referrer");
    frame.removeAttribute("allow");
    frame.removeAttribute("name");
    if (!frame.title) frame.title = new URL(source).hostname;
    if (remoteImagesAllowed()) frame.src = source;
    else {
      frame.dataset.remoteSrc = source;
      frame.removeAttribute("src");
    }
  });
}

function applyRemoteContentPolicy(sanitized: string): string {
  const template = document.createElement("template");
  template.innerHTML = sanitized;
  secureIframes(template.content);
  if (!remoteImagesAllowed()) {
    template.content.querySelectorAll<HTMLImageElement>('img[src^="http://"], img[src^="https://"]').forEach((image) => {
      image.dataset.remoteSrc = image.src;
      image.removeAttribute("src");
      image.classList.add("remote-image-blocked");
    });
  }
  return template.innerHTML;
}

export function renderRawHtml(source: string): string {
  return applyRemoteContentPolicy(sanitize(source, true));
}

export function rawHtmlIsBlock(source: string): boolean {
  return source.includes("\n") || /^\s*<iframe\b/i.test(source);
}

export function wireRenderedLinks(root: HTMLElement): void {
  root.querySelectorAll<HTMLElement>("[data-wikilink]").forEach((element) => {
    element.addEventListener("click", (event) => {
      event.preventDefault();
      sendNative(element.classList.contains("internal-embed") ? "link/open" : "link/open", {
        target: element.dataset.wikilink ?? "",
        ctrl: (event as MouseEvent).ctrlKey,
        shift: (event as MouseEvent).shiftKey,
        middle: (event as MouseEvent).button === 1,
      });
    });
  });
  root.querySelectorAll<HTMLAnchorElement>("a[href]").forEach((anchor) => {
    if (anchor.dataset.wikilink) return;
    anchor.addEventListener("click", (event) => {
      event.preventDefault();
      const href = anchor.getAttribute("href") ?? "";
      if (/^(?:https?|mailto|obsidian):/i.test(href))
        sendNative("url/open", { uri: href });
      else if (href.startsWith("#"))
        sendNative("link/open", { target: href });
      else
        sendNative("attachment/open", { target: href, relative: true });
    });
  });
}

function calloutTitle(type: string): string {
  return type.replace(/(^|-)(\p{L})/gu,
    (_match, prefix: string, letter: string) => `${prefix ? " " : ""}${letter.toUpperCase()}`);
}

function wireRenderedCallouts(root: HTMLElement): void {
  const blockquotes = [...root.querySelectorAll("blockquote")].reverse();
  for (const blockquote of blockquotes) {
    const first = blockquote.firstElementChild;
    if (!first) continue;
    const walker = document.createTreeWalker(first, NodeFilter.SHOW_TEXT);
    const text = walker.nextNode() as Text | null;
    if (!text) continue;
    const marker = /^\[!([^\]\s]+)\]([+-])?(?:[ \t]+([^\n]*))?(?:\n|$)/.exec(text.data);
    if (!marker) continue;
    text.data = text.data.slice(marker[0].length);
    if (!first.textContent?.trim() && first.children.length === 0) first.remove();
    const details = document.createElement("details");
    details.className = "callout";
    const type = marker[1].toLowerCase();
    details.dataset.callout = type;
    details.open = marker[2] !== "-";
    const summary = document.createElement("summary");
    summary.className = "callout-title";
    summary.append(calloutIcon(type), document.createTextNode(
      marker[3]?.trim() || calloutTitle(type),
    ));
    const content = document.createElement("div");
    content.className = "callout-content";
    while (blockquote.firstChild) content.append(blockquote.firstChild);
    details.append(summary, content);
    blockquote.replaceWith(details);
  }
}

function wireRenderedMath(root: HTMLElement): void {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const nodes: Text[] = [];
  for (let node = walker.nextNode() as Text | null; node;
       node = walker.nextNode() as Text | null) {
    const parent = node.parentElement;
    if (!parent || parent.closest("code, pre, [data-phi-raw-html], .math-widget")) continue;
    nodes.push(node);
  }
  const pattern = /(?<!\\)(\$\$([\s\S]+?)\$\$|\\\[([\s\S]+?)\\\]|\\\(([^\n]+?)\\\)|\$([^\n$]+?)\$)/g;
  for (const node of nodes) {
    const source = node.data;
    pattern.lastIndex = 0;
    let match: RegExpExecArray | null;
    let cursor = 0;
    const fragment = document.createDocumentFragment();
    while ((match = pattern.exec(source))) {
      fragment.append(document.createTextNode(source.slice(cursor, match.index)));
      const display = match[0].startsWith("$$") || match[0].startsWith("\\[");
      const latex = match[2] ?? match[3] ?? match[4] ?? match[5] ?? "";
      const element = document.createElement("span");
      element.className = display ? "math-widget math-display" : "math-widget math-inline";
      element.tabIndex = 0;
      element.setAttribute("aria-label", `LaTeX: ${latex}`);
      if (display) wireMathScroll(element);
      fragment.append(element);
      void renderMath(latex, display, element);
      cursor = match.index + match[0].length;
    }
    if (cursor) {
      fragment.append(document.createTextNode(source.slice(cursor)));
      node.replaceWith(fragment);
    }
  }
}

function wireRemoteImages(root: HTMLElement): void {
  root.querySelectorAll<HTMLImageElement>("img[data-remote-src]").forEach((image) => {
    image.tabIndex = 0;
    image.title = "Remote image blocked — click to load";
    image.addEventListener("click", () => {
      image.src = image.dataset.remoteSrc ?? "";
      delete image.dataset.remoteSrc;
      image.classList.remove("remote-image-blocked");
    }, { once: true });
  });
}

function wireRemoteIframes(root: HTMLElement): void {
  root.querySelectorAll<HTMLIFrameElement>("iframe[data-remote-src]").forEach((frame) => {
    const wrapper = document.createElement("span");
    wrapper.className = "iframe-embed-placeholder";
    const button = document.createElement("button");
    button.type = "button";
    button.className = "iframe-embed-load";
    button.textContent = "Load embedded page";
    button.title = "Remote embedded content is disabled by default";
    frame.replaceWith(wrapper);
    wrapper.append(frame, button);
    button.addEventListener("pointerdown", (event) => event.stopPropagation());
    button.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      frame.src = frame.dataset.remoteSrc ?? "";
      delete frame.dataset.remoteSrc;
      wrapper.replaceWith(frame);
    }, { once: true });
  });
}

export function wireRenderedContent(root: HTMLElement): void {
  wireRenderedCallouts(root);
  wireRenderedLinks(root);
  wireRenderedMath(root);
  wireRemoteImages(root);
  wireRemoteIframes(root);
}
