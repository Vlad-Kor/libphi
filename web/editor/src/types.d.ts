declare global {
  interface Window {
    nativeEditorReceive?: (message: NativeMessage | string) => void;
    phiMarkdownEditor?: NativeMarkdownEditor;
    webkit?: {
      messageHandlers?: {
        native?: { postMessage(value: string): void };
      };
    };
    phiEditorPerformance?: {
      enabled: boolean;
      reset(): void;
      snapshot(): Record<string, {
        count: number;
        mean: number;
        p50: number;
        p95: number;
        max: number;
      }>;
    };
  }
}

export interface NativeMessage {
  protocol: 1;
  id?: string;
  type: string;
  payload?: Record<string, unknown>;
}

export interface OpenDocument {
  documentId: string;
  path: string;
  text: string;
  revision: number;
  lineEnding: "LF" | "CRLF";
  preamble?: string;
  imageGeometry?: Array<{
    target: string;
    path: string;
    width: number;
    height: number;
  }>;
  scrollState?: DocumentScrollState;
}

export interface DocumentScrollState {
  anchor: number;
  offset: number;
  top: number;
}

export interface EditorTheme {
  dark: boolean;
  fontScale?: number;
  background?: string;
  foreground?: string;
  toolbar?: string;
  entry?: string;
  border?: string;
  accent?: string;
}

export interface EditorSettings {
  sourceMode?: boolean;
  exportMode?: boolean;
  lineWrapping?: boolean;
  latexConceal?: boolean;
  readableLineWidth?: boolean;
  allowRemoteImages?: boolean;
  executableSnippets?: boolean;
  imagePasteStyle?: "wiki-embed" | "markdown-link";
  workspaceMode?: boolean;
  snippets?: string;
  snippetVariables?: string;
}

export interface DocumentSnapshot {
  documentId: string;
  baseRevision: number;
  editorRevision: number;
  text: string;
  scrollState: DocumentScrollState;
}

export interface NativeMarkdownEditor {
  openDocument(document: OpenDocument): void;
  updateSettings(settings: EditorSettings): void;
  updateTheme(theme: EditorTheme): void;
  runCommand(command: string, payload?: Record<string, unknown>): void;
  getDocument(): string;
  getState(): Record<string, unknown>;
  focus(): void;
  flush(): Promise<DocumentSnapshot>;
}

export {};
