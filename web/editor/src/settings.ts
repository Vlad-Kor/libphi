import type { EditorSettings, EditorTheme } from "./types";

export const defaultSettings: Required<EditorSettings> = {
  sourceMode: false,
  lineWrapping: true,
  latexConceal: false,
  allowRemoteImages: false,
  executableSnippets: false,
  snippets: "",
};

let runtimeSettings: Required<EditorSettings> = { ...defaultSettings };

export function updateRuntimeSettings(settings: Required<EditorSettings>): void {
  runtimeSettings = { ...settings };
}

export function remoteImagesAllowed(): boolean {
  return runtimeSettings.allowRemoteImages;
}

export function applyTheme(theme: EditorTheme): void {
  const root = document.documentElement;
  root.dataset.theme = theme.dark ? "dark" : "light";
  root.style.setProperty("--editor-font-scale", String(theme.fontScale ?? 1));
  root.style.colorScheme = theme.dark ? "dark" : "light";
}
