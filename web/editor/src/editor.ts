import { autocompletion, closeBrackets, closeBracketsKeymap, completionKeymap } from "@codemirror/autocomplete";
import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
import { bracketMatching, defaultHighlightStyle, HighlightStyle, indentOnInput, syntaxHighlighting } from "@codemirror/language";
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { Compartment, EditorState } from "@codemirror/state";
import { findNext, findPrevious, openSearchPanel, searchKeymap } from "@codemirror/search";
import { crosshairCursor, drawSelection, dropCursor, EditorView, highlightSpecialChars, keymap, rectangularSelection } from "@codemirror/view";
import { tags } from "@lezer/highlight";
import { parseDocument } from "yaml";
import { acceptNativeResponse, requestNative, reportError, sendNative } from "./bridge";
import { formattingKeymap, runEditingCommand } from "./commands";
import { latexSuite, setCustomSnippets } from "./latex-suite/engine";
import { invalidateMath, updatePreamble } from "./math/mathjax";
import { obsidianCompletion } from "./obsidian/completion";
import { livePreview, refreshLivePreview } from "./obsidian/live-preview";
import { applyTheme, defaultSettings, updateRuntimeSettings } from "./settings";
import type { DocumentSnapshot, EditorSettings, EditorTheme, NativeMarkdownEditor, NativeMessage, OpenDocument } from "./types";

const previewCompartment = new Compartment();
const wrappingCompartment = new Compartment();
const themeCompartment = new Compartment();
const obsidianHighlightStyle = HighlightStyle.define([
  { tag: tags.strong, fontWeight: "750", textDecoration: "none" },
  { tag: [tags.heading1, tags.heading2, tags.heading3, tags.heading4, tags.heading5, tags.heading6], textDecoration: "none" },
]);

export class PhiMarkdownEditor implements NativeMarkdownEditor {
  readonly view: EditorView;
  private documentId = "";
  private documentPath = "";
  private baseRevision = 0;
  private editorRevision = 0;
  private dirty = false;
  private originalText = "";
  private lineEnding: "LF" | "CRLF" = "LF";
  private settings: Required<EditorSettings> = { ...defaultSettings };
  private snapshotTimer = 0;
  private documentClasses: string[] = [];
  private darkTheme = document.documentElement.dataset.theme === "dark";

  constructor(parent: HTMLElement) {
    updateRuntimeSettings(this.settings);
    this.view = new EditorView({
      parent,
      state: this.createState(""),
    });
  }

  private createState(text: string): EditorState {
    return EditorState.create({
      doc: text,
      extensions: [
        EditorState.lineSeparator.of(this.lineEnding === "CRLF" ? "\r\n" : "\n"),
        highlightSpecialChars(),
        history(),
        drawSelection(),
        dropCursor(),
        EditorState.allowMultipleSelections.of(true),
        indentOnInput(),
        bracketMatching(),
        closeBrackets(),
        rectangularSelection(),
        crosshairCursor(),
        syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
        syntaxHighlighting(obsidianHighlightStyle),
        markdown({ base: markdownLanguage, addKeymap: true }),
        autocompletion({ override: [obsidianCompletion], activateOnTyping: true }),
        keymap.of([
          ...formattingKeymap,
          { key: "Mod-s", preventDefault: true, run: () => { void this.requestSave(); return true; } },
          ...closeBracketsKeymap,
          ...completionKeymap,
          ...searchKeymap,
          ...historyKeymap,
          ...defaultKeymap.filter((binding) => binding.key !== "Tab"),
          indentWithTab,
        ]),
        ...latexSuite,
        previewCompartment.of(this.settings.sourceMode ? [] : livePreview),
        wrappingCompartment.of(this.settings.lineWrapping ? EditorView.lineWrapping : []),
        themeCompartment.of(EditorView.theme({}, { dark: this.darkTheme })),
        EditorView.updateListener.of((update) => {
          if (update.docChanged) this.documentChanged();
          queueMicrotask(() => this.enhanceSearchPanel());
        }),
        EditorView.domEventHandlers({
          paste: (event, view) => this.handlePaste(event, view),
        }),
      ],
    });
  }

  private enhanceSearchPanel(): void {
    const panel = this.view.dom.querySelector<HTMLElement>(".cm-panel.cm-search");
    if (!panel || panel.querySelector(".phi-replace-toggle")) return;
    const toggle = document.createElement("button");
    toggle.type = "button";
    toggle.className = "phi-replace-toggle";
    toggle.textContent = "›";
    toggle.setAttribute("aria-label", "Show replace controls");
    toggle.title = "Show replace controls";
    toggle.addEventListener("click", () => {
      const visible = panel.classList.toggle("phi-show-replace");
      toggle.textContent = visible ? "⌄" : "›";
      toggle.setAttribute("aria-label", visible ? "Hide replace controls" : "Show replace controls");
      toggle.title = visible ? "Hide replace controls" : "Show replace controls";
    });
    panel.insertBefore(toggle, panel.firstChild);
  }

  private documentChanged(): void {
    this.editorRevision++;
    this.dirty = true;
    sendNative("document/changed", {
      documentId: this.documentId,
      baseRevision: this.baseRevision,
      editorRevision: this.editorRevision,
      dirty: true,
    });
    window.clearTimeout(this.snapshotTimer);
    this.snapshotTimer = window.setTimeout(() => this.sendSnapshot("document/changed"), 180);
  }

  private snapshot(): DocumentSnapshot {
    return {
      documentId: this.documentId,
      baseRevision: this.baseRevision,
      editorRevision: this.editorRevision,
      text: this.getDocument(),
    };
  }

  private sendSnapshot(type: string, id?: string): DocumentSnapshot {
    const snapshot = this.snapshot();
    this.applyDocumentClasses(snapshot.text);
    sendNative(type, { ...snapshot, path: this.documentPath }, id);
    return snapshot;
  }

  private applyDocumentClasses(text: string): void {
    for (const name of this.documentClasses) document.body.classList.remove(name);
    this.documentClasses = [];
    const match = /^---[ \t]*\r?\n([\s\S]*?)\r?\n---[ \t]*(?:\r?\n|$)/.exec(text);
    if (!match) return;
    try {
      const yaml = parseDocument(match[1]);
      if (yaml.errors.length) return;
      const value = yaml.toJS() as { cssclasses?: unknown } | null;
      const raw = value?.cssclasses;
      const classes = Array.isArray(raw) ? raw.map(String) : typeof raw === "string" ? raw.split(/\s+/) : [];
      this.documentClasses = classes.filter((name) =>
        /^[A-Za-z_][A-Za-z0-9_-]*$/.test(name) && name !== "source-mode");
      for (const name of this.documentClasses) document.body.classList.add(name);
    } catch {
      // Invalid or complex YAML remains editable as source.
    }
  }

  private async requestSave(): Promise<void> {
    this.sendSnapshot("document/save");
  }

  private handlePaste(event: ClipboardEvent, view: EditorView): boolean {
    const image = [...(event.clipboardData?.items ?? [])].find((item) => item.type.startsWith("image/"));
    if (image) {
      event.preventDefault();
      const file = image.getAsFile();
      if (!file) return true;
      const reader = new FileReader();
      reader.onload = () => {
        const encoded = String(reader.result ?? "").split(",").pop() ?? "";
        requestNative<{ path?: string; name?: string }>("attachment/create", { mimeType: file.type, data: encoded })
          .then((result) => {
            if (!result.path) return;
            const name = (result.name ?? "Pasted image").replace(/\.[^.]+$/, "").replace(/]/g, "\\]");
            view.dispatch(view.state.replaceSelection(`![${name}](<${result.path}>)`));
          })
          .catch((error) => reportError(error, "attachment/paste", this.documentPath));
      };
      reader.readAsDataURL(file);
      return true;
    }
    const selection = view.state.selection.main;
    const plain = event.clipboardData?.getData("text/plain") ?? "";
    if (!selection.empty && /^(https?|mailto):\S+$/i.test(plain)) {
      event.preventDefault();
      const label = view.state.sliceDoc(selection.from, selection.to);
      view.dispatch({ changes: { from: selection.from, to: selection.to, insert: `[${label}](${plain})` }, userEvent: "input.paste" });
      return true;
    }
    return false;
  }

  private revealFragment(target: string): void {
    const hash = target.indexOf("#");
    if (hash < 0 || hash + 1 >= target.length) return;
    let fragment = target.slice(hash + 1);
    try { fragment = decodeURIComponent(fragment); } catch { /* retain source */ }
    const block = fragment.startsWith("^");
    const wanted = (block ? fragment.slice(1) : fragment).trim().toLocaleLowerCase();
    for (let number = 1; number <= this.view.state.doc.lines; number++) {
      const line = this.view.state.doc.line(number);
      if (block) {
        const match = /(?:^|\s)\^([A-Za-z0-9-]+)\s*$/.exec(line.text);
        if (!match || match[1].toLocaleLowerCase() !== wanted) continue;
      } else {
        const match = /^#{1,6}[ \t]+(.+?)[ \t]*#*[ \t]*$/.exec(line.text);
        if (!match || match[1].trim().toLocaleLowerCase() !== wanted) continue;
      }
      this.view.dispatch({ selection: { anchor: line.from }, scrollIntoView: true });
      this.view.focus();
      return;
    }
  }

  openDocument(document: OpenDocument): void {
    window.clearTimeout(this.snapshotTimer);
    this.documentId = document.documentId;
    this.documentPath = document.path;
    this.baseRevision = document.revision;
    this.editorRevision = 0;
    this.dirty = false;
    this.originalText = document.text;
    this.lineEnding = document.lineEnding;
    updatePreamble(document.preamble ?? "");
    this.applyDocumentClasses(document.text);
    setCustomSnippets(this.settings.snippets);
    this.view.setState(this.createState(document.text));
    this.view.focus();
    sendNative("document/state", { documentId: this.documentId, dirty: false, editorRevision: 0 });
  }

  updateSettings(settings: EditorSettings): void {
    const previous = this.settings;
    this.settings = { ...this.settings, ...settings };
    updateRuntimeSettings(this.settings);
    const effects = [];
    if (previous.sourceMode !== this.settings.sourceMode)
      effects.push(previewCompartment.reconfigure(this.settings.sourceMode ? [] : livePreview));
    if (previous.lineWrapping !== this.settings.lineWrapping)
      effects.push(wrappingCompartment.reconfigure(this.settings.lineWrapping ? EditorView.lineWrapping : []));
    if (previous.allowRemoteImages !== this.settings.allowRemoteImages)
      effects.push(refreshLivePreview.of(null));
    if (previous.snippets !== this.settings.snippets)
      setCustomSnippets(this.settings.snippets);
    if (effects.length) this.view.dispatch({ effects });
    document.body.classList.toggle("source-mode", this.settings.sourceMode);
  }

  updateTheme(theme: EditorTheme): void {
    applyTheme(theme);
    if (this.darkTheme === theme.dark) return;
    this.darkTheme = theme.dark;
    this.view.dispatch({
      effects: themeCompartment.reconfigure(
        EditorView.theme({}, { dark: this.darkTheme }),
      ),
    });
  }

  runCommand(command: string): void {
    if (command === "editor.toggleSourceMode") {
      this.updateSettings({ sourceMode: !this.settings.sourceMode });
      sendNative("document/state", { sourceMode: this.settings.sourceMode });
      return;
    }
    if (command === "editor.find") { openSearchPanel(this.view); return; }
    if (command === "editor.findNext") { findNext(this.view); return; }
    if (command === "editor.findPrevious") { findPrevious(this.view); return; }
    runEditingCommand(command, this.view);
  }

  getDocument(): string {
    if (!this.dirty) return this.originalText;
    const text = this.view.state.doc.toString();
    return this.lineEnding === "CRLF" ? text.replace(/\n/g, "\r\n") : text;
  }

  getState(): Record<string, unknown> {
    const selection = this.view.state.selection.main;
    return {
      documentId: this.documentId,
      path: this.documentPath,
      dirty: this.dirty,
      sourceMode: this.settings.sourceMode,
      editorRevision: this.editorRevision,
      selection: { anchor: selection.anchor, head: selection.head },
      scrollTop: this.view.scrollDOM.scrollTop,
    };
  }

  focus(): void { this.view.focus(); }

  async flush(): Promise<DocumentSnapshot> {
    window.clearTimeout(this.snapshotTimer);
    const snapshot = this.sendSnapshot("document/flush");
    await Promise.resolve();
    return snapshot;
  }

  markSaved(revision: number, savedEditorRevision = this.editorRevision): void {
    this.baseRevision = revision;
    if (savedEditorRevision >= this.editorRevision) {
      const text = this.getDocument();
      this.dirty = false;
      this.originalText = text;
    }
    sendNative("document/state", {
      documentId: this.documentId,
      dirty: this.dirty,
      revision,
      savedEditorRevision,
    });
  }

  receive(message: NativeMessage): void {
    if (acceptNativeResponse(message)) return;
    const payload = message.payload ?? {};
    switch (message.type) {
      case "document/open": this.openDocument(payload as unknown as OpenDocument); break;
      case "document/external-update":
        if (!this.dirty) this.openDocument(payload as unknown as OpenDocument);
        else sendNative("document/state", { documentId: this.documentId, conflict: true, externalRevision: payload.revision });
        break;
      case "document/saved": this.markSaved(
        Number(payload.revision ?? this.baseRevision + 1),
        Number(payload.editorRevision ?? this.editorRevision),
      ); break;
      case "settings/update": this.updateSettings(payload as EditorSettings); break;
      case "theme/update": this.updateTheme(payload as unknown as EditorTheme); break;
      case "preamble/update":
        updatePreamble(String(payload.preamble ?? ""));
        invalidateMath();
        this.view.dispatch({ effects: refreshLivePreview.of(null) });
        break;
      case "navigation/reveal": this.revealFragment(String(payload.target ?? "")); break;
      case "command/run": this.runCommand(String(payload.command ?? "")); break;
      case "document/flush": void this.flush().then((snapshot) => sendNative("document/flush", { ...snapshot }, message.id)); break;
      case "vault/files-changed": break;
      default: reportError(new Error(`Unknown native message: ${message.type}`), "bridge", this.documentPath);
    }
  }
}
