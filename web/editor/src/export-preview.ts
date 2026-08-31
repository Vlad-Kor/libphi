import { acceptNativeResponse, reportError, sendNative } from "./bridge";
import { PhiMarkdownEditor } from "./editor";
import { noteTitle, type ExportDocument } from "./export-utils";
import { defaultSettings } from "./settings";
import type { EditorSettings, NativeMessage } from "./types";

interface ExportPreviewState {
  multiple: boolean;
  title: string;
  author: string;
  fontSize: number;
  documents: ExportDocument[];
  settings?: EditorSettings;
  preamble?: string;
  revision?: number;
}

function byId(id: string): HTMLElement {
  const element = document.getElementById(id);
  if (!element) throw new Error(`The PDF preview is missing #${id}`);
  return element;
}

const pdfCover = byId("pdf-cover");
const pdfTitle = byId("pdf-title");
const pdfAuthor = byId("pdf-author");
const pdfSections = byId("pdf-sections");

let state: ExportPreviewState = {
  multiple: false,
  title: "",
  author: "",
  fontSize: 16,
  documents: [],
};
let editors: PhiMarkdownEditor[] = [];
let generation = 0;

function applyMetadata(): void {
  const title = state.title.trim();
  const author = state.author.trim();
  pdfTitle.textContent = title;
  pdfAuthor.textContent = author;
  pdfAuthor.toggleAttribute("hidden", !author);
  pdfCover.toggleAttribute("hidden", !state.multiple);
  document.title = title || "Phi PDF Preview";
  document.querySelector<HTMLMetaElement>('meta[name="author"]')!.content = author;
  document.documentElement.style.setProperty(
    "--pdf-font-size", `${Math.max(12, Math.min(24, state.fontSize))}px`,
  );
  for (const editor of editors) {
    editor.updateTheme({ dark: false, fontScale: state.fontSize / 16 });
  }
}

function imagesSettled(): boolean {
  return [...document.images].every((image) => image.complete);
}

async function waitUntilSettled(expectedGeneration: number): Promise<void> {
  await document.fonts?.ready;
  const started = performance.now();
  let lastMutation = performance.now();
  const observer = new MutationObserver(() => { lastMutation = performance.now(); });
  observer.observe(pdfSections, {
    childList: true,
    subtree: true,
    attributes: true,
    characterData: true,
  });
  while (expectedGeneration === generation && performance.now() - started < 10000) {
    const quiet = performance.now() - lastMutation > 450;
    const pending = pdfSections.querySelector(".dimmed");
    if (quiet && !pending && imagesSettled()) break;
    await new Promise<void>((resolve) => window.setTimeout(resolve, 100));
  }
  observer.disconnect();
  await new Promise<void>((resolve) => requestAnimationFrame(() =>
    requestAnimationFrame(() => resolve())));
}

async function rebuild(): Promise<void> {
  const currentGeneration = ++generation;
  for (const editor of editors) editor.view.destroy();
  editors = [];
  pdfSections.replaceChildren();

  state.documents.forEach((exportDocument, index) => {
    const section = document.createElement("section");
    section.className = "pdf-note";
    section.dataset.path = exportDocument.path;
    if (state.multiple) {
      const heading = document.createElement("h1");
      heading.className = "pdf-note-title";
      heading.textContent = noteTitle(exportDocument.name);
      section.append(heading);
    }
    const mount = document.createElement("div");
    mount.className = "export-note-editor";
    section.append(mount);
    pdfSections.append(section);

    const editor = new PhiMarkdownEditor(mount);
    editor.updateSettings({
      ...defaultSettings,
      ...state.settings,
      sourceMode: false,
      exportMode: true,
      readableLineWidth: false,
      executableSnippets: false,
    });
    editor.updateTheme({ dark: false, fontScale: state.fontSize / 16 });
    editor.openDocument({
      documentId: `export-${index}-${exportDocument.path}`,
      path: exportDocument.path,
      text: exportDocument.text,
      revision: 0,
      lineEnding: "LF",
      preamble: state.preamble ?? "",
    });
    editors.push(editor);
  });
  applyMetadata();
  await waitUntilSettled(currentGeneration);
  if (currentGeneration === generation)
    sendNative("export/preview-ready", { revision: state.revision ?? 0 });
}

function receive(input: NativeMessage | string): void {
  try {
    const message = typeof input === "string"
      ? JSON.parse(input) as NativeMessage : input;
    if (message.protocol !== 1)
      throw new Error(`Unsupported preview protocol: ${String(message.protocol)}`);
    if (acceptNativeResponse(message)) return;
    if (message.type === "export/initialize" || message.type === "export/documents") {
      state = { ...state, ...(message.payload as unknown as Partial<ExportPreviewState>) };
      void rebuild();
    } else if (message.type === "export/metadata") {
      state = { ...state, ...(message.payload as unknown as Partial<ExportPreviewState>) };
      applyMetadata();
      void waitUntilSettled(generation).then(() =>
        sendNative("export/preview-ready", { revision: state.revision ?? 0 }));
    }
  } catch (error) {
    reportError(error, "export-preview/receive");
  }
}

window.nativeEditorReceive = receive;
window.addEventListener("error", (event) =>
  reportError(event.error ?? event.message, "export-preview/window"));
window.addEventListener("unhandledrejection", (event) =>
  reportError(event.reason, "export-preview/promise"));
sendNative("export/ready", {
  capabilities: ["live-preview", "mathjax", "mermaid", "note-embeds"],
});
