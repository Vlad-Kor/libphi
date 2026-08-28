import { renderMarkdown } from "./markdown/render";

export type ImagePasteStyle = "wiki-embed" | "markdown-link";
export const PHI_MARKDOWN_CLIPBOARD_TYPE = "application/x-phi-markdown";

export function pastedImageMarkdown(
  result: { path?: string; name?: string },
  style: ImagePasteStyle,
  workspaceMode: boolean,
): string {
  if (!result.path) return "";
  const filename = result.name ??
    result.path.split("/").filter(Boolean).at(-1) ?? "Pasted image";
  if (workspaceMode && style === "wiki-embed")
    return `![[${filename.replace(/]/g, "\\]")}]]`;
  const label = filename.replace(/\.[^.]+$/, "").replace(/]/g, "\\]");
  return `![${label}](<${result.path}>)`;
}

export function markdownClipboardHtml(markdown: string): string {
  const template = document.createElement("template");
  template.innerHTML = renderMarkdown(markdown);
  template.content.querySelectorAll("[data-phi-raw-html]").forEach(
    (element) => element.removeAttribute("data-phi-raw-html"),
  );
  const wrapper = document.createElement("div");
  wrapper.dataset.phiMarkdownSource = markdown;
  wrapper.append(template.content);
  return wrapper.outerHTML;
}

export function clipboardMarkdownSource(html: string): string | null {
  if (!html) return null;
  const document = new DOMParser().parseFromString(html, "text/html");
  const wrapper = document.querySelector<HTMLElement>(
    "[data-phi-markdown-source]",
  );
  return wrapper?.dataset.phiMarkdownSource ?? null;
}

/** WebKitGTK exposes clipboard collections as array-like DOM lists, but some
 * versions do not make them iterable. Array.from handles both forms. */
export function clipboardImageFile(data: DataTransfer | null): File | null {
  const item = Array.from(data?.items ?? [])
    .find((candidate) => candidate.type.startsWith("image/"));
  const itemFile = item?.getAsFile();
  if (itemFile) return itemFile;
  return Array.from(data?.files ?? [])
    .find((candidate) => candidate.type.startsWith("image/")) ?? null;
}

export function clipboardMayContainNativeImage(data: DataTransfer | null): boolean {
  if (!data) return false;
  const items = Array.from(data.items ?? []);
  const files = Array.from(data.files ?? []);
  const types = Array.from(data.types ?? []);
  return items.some((item) => item.type.startsWith("image/")) ||
    files.some((file) => file.type.startsWith("image/")) ||
    types.some((type) => type.startsWith("image/") || type === "Files") ||
    (items.length === 0 && files.length === 0 && types.length === 0);
}

function safeDestination(value: string | null): string {
  const destination = (value ?? "").trim();
  if (!destination || /^(?:javascript|vbscript|data):/i.test(destination)) return "";
  return destination.replace(/[<>]/g, (character) => `\\${character}`);
}

function fencedCode(value: string, language = ""): string {
  const runs = value.match(/`+/g) ?? [];
  const fence = "`".repeat(Math.max(3, ...runs.map((run) => run.length + 1)));
  return `${fence}${language}\n${value.replace(/\n$/, "")}\n${fence}`;
}

function inlineCode(value: string): string {
  const runs = value.match(/`+/g) ?? [];
  const fence = "`".repeat(Math.max(1, ...runs.map((run) => run.length + 1)));
  const padding = value.startsWith("`") || value.endsWith("`") ? " " : "";
  return `${fence}${padding}${value}${padding}${fence}`;
}

function renderList(element: Element, depth: number): string {
  const ordered = element.tagName === "OL";
  const start = ordered ? Number.parseInt(element.getAttribute("start") ?? "1", 10) || 1 : 1;
  const lines: string[] = [];
  let index = 0;
  for (const child of Array.from(element.children)) {
    if (child.tagName !== "LI") continue;
    const nested = Array.from(child.children).filter((item) =>
      item.tagName === "UL" || item.tagName === "OL");
    const clone = child.cloneNode(true) as Element;
    for (const list of Array.from(clone.querySelectorAll(":scope > ul, :scope > ol")))
      list.remove();
    const body = renderChildren(clone, depth).trim();
    const marker = ordered ? `${start + index}. ` : "- ";
    const indent = "  ".repeat(depth);
    const continuation = `\n${indent}  `;
    lines.push(`${indent}${marker}${body.replace(/\n/g, continuation)}`.trimEnd());
    for (const list of nested) lines.push(renderList(list, depth + 1));
    index++;
  }
  return lines.join("\n");
}

function renderNode(node: Node, depth: number): string {
  if (node.nodeType === Node.TEXT_NODE) return node.textContent ?? "";
  if (node.nodeType !== Node.ELEMENT_NODE) return "";
  const element = node as Element;
  const children = () => renderChildren(element, depth);
  switch (element.tagName) {
    case "BR": return "\n";
    case "P":
    case "DIV":
    case "SECTION":
    case "ARTICLE": return `${children().trim()}\n\n`;
    case "H1":
    case "H2":
    case "H3":
    case "H4":
    case "H5":
    case "H6": return `${"#".repeat(Number(element.tagName[1]))} ${children().trim()}\n\n`;
    case "STRONG":
    case "B": return `**${children()}**`;
    case "EM":
    case "I": return `*${children()}*`;
    case "DEL":
    case "S":
    case "STRIKE": return `~~${children()}~~`;
    case "CODE": return element.parentElement?.tagName === "PRE"
      ? element.textContent ?? "" : inlineCode(element.textContent ?? "");
    case "PRE": {
      const code = element.querySelector(":scope > code");
      const language = code?.className.match(/(?:^|\s)language-([^\s]+)/)?.[1] ?? "";
      return `${fencedCode(code?.textContent ?? element.textContent ?? "", language)}\n\n`;
    }
    case "A": {
      const label = children().trim();
      const destination = safeDestination(element.getAttribute("href"));
      return destination ? `[${label || destination}](${destination})` : label;
    }
    case "IMG": {
      const destination = safeDestination(element.getAttribute("src"));
      const alt = (element.getAttribute("alt") ?? "").replace(/]/g, "\\]");
      return destination ? `![${alt}](${destination})` : alt;
    }
    case "UL":
    case "OL": return `${renderList(element, depth)}\n\n`;
    case "BLOCKQUOTE": {
      const body = children().trim().replace(/^/gm, "> ");
      return `${body}\n\n`;
    }
    case "HR": return "---\n\n";
    default: return children();
  }
}

function renderChildren(element: Element, depth: number): string {
  return Array.from(element.childNodes).map((node) => renderNode(node, depth)).join("");
}

export function clipboardHtmlToMarkdown(html: string): string {
  const document = new DOMParser().parseFromString(html, "text/html");
  return renderChildren(document.body, 0)
    .replace(/[ \t]+\n/g, "\n")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
}
