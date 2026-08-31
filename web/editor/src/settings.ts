import type { EditorSettings, EditorTheme } from "./types";

export const defaultSettings: Required<EditorSettings> = {
  sourceMode: false,
  exportMode: false,
  lineWrapping: true,
  latexConceal: false,
  readableLineWidth: true,
  allowRemoteImages: false,
  executableSnippets: false,
  imagePasteStyle: "wiki-embed",
  workspaceMode: false,
  snippets: "",
  snippetVariables: "",
};

let runtimeSettings: Required<EditorSettings> = { ...defaultSettings };

export function updateRuntimeSettings(settings: Required<EditorSettings>): void {
  runtimeSettings = { ...settings };
  document.body.classList.toggle("full-width-editor", !settings.readableLineWidth);
}

export function remoteImagesAllowed(): boolean {
  return runtimeSettings.allowRemoteImages;
}

export function exportPreviewMode(): boolean {
  return runtimeSettings.exportMode;
}

export function currentEditorSettings(): Readonly<Required<EditorSettings>> {
  return runtimeSettings;
}

export function applyTheme(theme: EditorTheme): void {
  const root = document.documentElement;
  const setColor = (name: string, value: string | undefined) => {
    if (value) root.style.setProperty(name, value);
    else root.style.removeProperty(name);
  };
  root.dataset.theme = theme.dark ? "dark" : "light";
  root.style.setProperty("--editor-font-scale", String(theme.fontScale ?? 1));
  setColor("--editor-bg", theme.background);
  setColor("--editor-fg", theme.foreground);
  setColor("--editor-toolbar-bg", theme.toolbar);
  setColor("--editor-entry-bg", theme.entry);
  setColor("--editor-border", theme.border);
  setColor("--editor-accent", theme.accent);
  root.style.colorScheme = theme.dark ? "dark" : "light";
}
