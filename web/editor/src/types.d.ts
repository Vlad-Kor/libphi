declare global {
  interface Window {
    nativeEditorReceive?: (message: NativeMessage | string) => void;
    phiMarkdownEditor?: NativeMarkdownEditor;
    webkit?: {
      messageHandlers?: {
        native?: { postMessage(value: string): void };
      };
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
}

export interface EditorTheme {
  dark: boolean;
  fontScale?: number;
  background?: string;
}

export interface EditorSettings {
  sourceMode?: boolean;
  lineWrapping?: boolean;
  latexConceal?: boolean;
  readableLineWidth?: boolean;
  allowRemoteImages?: boolean;
  executableSnippets?: boolean;
  snippets?: string;
}

export interface DocumentSnapshot {
  documentId: string;
  baseRevision: number;
  editorRevision: number;
  text: string;
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
