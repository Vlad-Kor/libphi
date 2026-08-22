import type { EditorSettings, EditorTheme } from "./types";

export const defaultSettings: Required<EditorSettings> = {
  sourceMode: false,
  lineWrapping: true,
  latexConceal: false,
  readableLineWidth: true,
  allowRemoteImages: false,
  executableSnippets: false,
  snippets: "",
};

let runtimeSettings: Required<EditorSettings> = { ...defaultSettings };

export function updateRuntimeSettings(settings: Required<EditorSettings>): void {
  runtimeSettings = { ...settings };
  document.body.classList.toggle("full-width-editor", !settings.readableLineWidth);
}

export function remoteImagesAllowed(): boolean {
  return runtimeSettings.allowRemoteImages;
}

export function applyTheme(theme: EditorTheme): void {
  const root = document.documentElement;
  root.dataset.theme = theme.dark ? "dark" : "light";
  root.style.setProperty("--editor-font-scale", String(theme.fontScale ?? 1));
  if (theme.background) root.style.setProperty("--editor-bg", theme.background);
  else root.style.removeProperty("--editor-bg");
  root.style.colorScheme = theme.dark ? "dark" : "light";
}
