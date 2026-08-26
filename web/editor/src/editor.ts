import { autocompletion, closeBrackets, closeBracketsKeymap, completionKeymap } from "@codemirror/autocomplete";
import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
import { bracketMatching, defaultHighlightStyle, HighlightStyle, indentOnInput, syntaxHighlighting } from "@codemirror/language";
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { Compartment, EditorSelection, EditorState } from "@codemirror/state";
import { findNext, findPrevious, openSearchPanel, searchKeymap } from "@codemirror/search";
import { crosshairCursor, drawSelection, dropCursor, EditorView, highlightSpecialChars, keymap, rectangularSelection } from "@codemirror/view";
import { tags } from "@lezer/highlight";
import { parseDocument } from "yaml";
import { acceptNativeResponse, requestNative, reportError, sendNative } from "./bridge";
import {
  clipboardHtmlToMarkdown,
  clipboardImageFile,
  clipboardMayContainNativeImage,
  markdownClipboardHtml,
} from "./clipboard";
import { formattingKeymap, runEditingCommand } from "./commands";
import { installKineticScroll } from "./kinetic-scroll";
import { latexSuite, setCustomSnippets } from "./latex-suite/engine";
import { latexEnhancements } from "./latex-suite/enhancements";
import { invalidateMath, updatePreamble } from "./math/mathjax";
import { markdownCompletion } from "./markdown/completion";
import { livePreview, refreshLivePreview } from "./markdown/live-preview";
import { applyTheme, defaultSettings, updateRuntimeSettings } from "./settings";
import type { DocumentSnapshot, EditorSettings, EditorTheme, NativeMarkdownEditor, NativeMessage, OpenDocument } from "./types";

const previewCompartment = new Compartment();
const wrappingCompartment = new Compartment();
const themeCompartment = new Compartment();
const latexEnhancementsCompartment = new Compartment();

type SearchIcon = "search" | "replace" | "previous" | "next" |
  "options" | "close" | "select-all";

function searchIcon(name: SearchIcon): SVGSVGElement {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.classList.add("phi-symbolic-icon");
  svg.setAttribute("viewBox", "0 0 16 16");
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.5");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  const paths: Record<SearchIcon, string[]> = {
    search: ["M11 6.5a4.5 4.5 0 1 1-9 0 4.5 4.5 0 0 1 9 0Z", "m10 10 3.5 3.5"],
    replace: ["M2.5 4.5h9", "m9.5 2 2 2-2 2", "M13.5 11.5h-9", "m6.5-2-2 2 2 2"],
    previous: ["m4.5 10 3.5-3.5 3.5 3.5"],
    next: ["m4.5 6 3.5 3.5L11.5 6"],
    options: ["M8 2.25v1.2M8 12.55v1.2M2.25 8h1.2M12.55 8h1.2M3.95 3.95l.85.85M11.2 11.2l.85.85M12.05 3.95l-.85.85M4.8 11.2l-.85.85", "M10.5 8A2.5 2.5 0 1 1 5.5 8a2.5 2.5 0 0 1 5 0Z"],
    close: ["m4 4 8 8M12 4l-8 8"],
    "select-all": ["M6 3H3v3M10 3h3v3M13 10v3h-3M6 13H3v-3"],
  };
  for (const data of paths[name]) {
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", data);
    svg.append(path);
  }
  return svg;
}

function iconButton(button: HTMLButtonElement, icon: SearchIcon, fallback: string): void {
  const label = button.getAttribute("aria-label") || button.textContent?.trim() || fallback;
  button.classList.add("phi-icon-button");
  button.replaceChildren(searchIcon(icon));
  button.setAttribute("aria-label", label);
  button.title = label;
}
const markdownHighlightStyle = HighlightStyle.define([
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
    installKineticScroll(this.view.scrollDOM);
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
        syntaxHighlighting(markdownHighlightStyle),
        markdown({ base: markdownLanguage, addKeymap: true }),
        autocompletion({ override: [markdownCompletion], activateOnTyping: true }),
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
        latexEnhancementsCompartment.of(
          latexEnhancements(this.settings.latexConceal),
        ),
        previewCompartment.of(this.settings.sourceMode ? [] : livePreview),
        wrappingCompartment.of(this.settings.lineWrapping ? EditorView.lineWrapping : []),
        themeCompartment.of(EditorView.theme({}, { dark: this.darkTheme })),
        EditorView.updateListener.of((update) => {
          if (update.docChanged) this.documentChanged();
          queueMicrotask(() => this.enhanceSearchPanel());
        }),
        EditorView.domEventHandlers({
          copy: (event, view) => this.handleCopy(event, view),
          paste: (event, view) => this.handlePaste(event, view),
        }),
      ],
    });
  }

  private enhanceSearchPanel(): void {
    const panel = this.view.dom.querySelector<HTMLElement>(".cm-panel.cm-search");
    if (!panel || panel.querySelector(".phi-search-grid")) return;
    const search = panel.querySelector<HTMLInputElement>('input[name="search"]');
    const replace = panel.querySelector<HTMLInputElement>('input[name="replace"]');
    const previous = panel.querySelector<HTMLButtonElement>('button[name="prev"]');
    const next = panel.querySelector<HTMLButtonElement>('button[name="next"]');
    const select = panel.querySelector<HTMLButtonElement>('button[name="select"]');
    const replaceOne = panel.querySelector<HTMLButtonElement>('button[name="replace"]');
    const replaceAll = panel.querySelector<HTMLButtonElement>('button[name="replaceAll"]');
    const close = panel.querySelector<HTMLButtonElement>('button[name="close"]');
    const caseLabel = panel.querySelector<HTMLElement>('input[name="case"]')?.closest("label");
    const regexpLabel = panel.querySelector<HTMLElement>('input[name="re"]')?.closest("label");
    const wordLabel = panel.querySelector<HTMLElement>('input[name="word"]')?.closest("label");
    if (!search || !replace || !previous || !next || !select || !replaceOne ||
        !replaceAll || !close || !caseLabel || !regexpLabel || !wordLabel) return;

    const entry = (input: HTMLInputElement, icon: "search" | "replace", label: string) => {
      const wrapper = document.createElement("div");
      wrapper.className = `phi-search-entry phi-${icon}-entry`;
      input.placeholder = label;
      wrapper.append(searchIcon(icon), input);
      return wrapper;
    };
    const searchEntry = entry(search, "search", "Search");
    searchEntry.classList.add("phi-search-field");
    const replaceEntry = entry(replace, "replace", "Replace");
    replaceEntry.classList.add("phi-replace-field", "phi-replace-row-item");

    iconButton(previous, "previous", "Previous Match");
    iconButton(next, "next", "Next Match");
    iconButton(close, "close", "Close Search");
    const navigation = document.createElement("div");
    navigation.className = "phi-search-navigation";
    navigation.append(previous, next);

    const replaceToggle = document.createElement("button");
    replaceToggle.type = "button";
    replaceToggle.className = "phi-icon-button phi-replace-toggle";
    replaceToggle.append(searchIcon("replace"));
    replaceToggle.setAttribute("aria-label", "Search and Replace");
    replaceToggle.setAttribute("aria-pressed", "false");
    replaceToggle.title = "Search and Replace";
    replaceToggle.addEventListener("click", () => {
      const visible = panel.classList.toggle("phi-show-replace");
      replaceToggle.setAttribute("aria-pressed", String(visible));
      if (visible) replace.focus();
      else search.focus();
    });

    const optionsButton = document.createElement("button");
    optionsButton.type = "button";
    optionsButton.className = "phi-icon-button phi-search-options-button";
    optionsButton.append(searchIcon("options"));
    optionsButton.setAttribute("aria-label", "Search Options");
    optionsButton.setAttribute("aria-haspopup", "menu");
    optionsButton.setAttribute("aria-expanded", "false");
    optionsButton.title = "Search Options";
    const options = document.createElement("div");
    options.className = "phi-search-options-popover";
    options.id = "phi-search-options";
    options.role = "menu";
    options.hidden = true;
    optionsButton.setAttribute("aria-controls", options.id);
    for (const label of [regexpLabel, caseLabel, wordLabel]) {
      label.classList.add("phi-search-option");
      options.append(label);
    }
    select.classList.add("phi-select-all");
    select.replaceChildren(searchIcon("select-all"), document.createTextNode("Select All Matches"));
    const optionsSeparator = document.createElement("div");
    optionsSeparator.className = "phi-search-options-separator";
    optionsSeparator.setAttribute("role", "separator");
    options.append(optionsSeparator, select);
    const optionsWrapper = document.createElement("div");
    optionsWrapper.className = "phi-search-options";
    optionsWrapper.append(optionsButton, options);
    const setOptionsVisible = (visible: boolean) => {
      options.hidden = !visible;
      optionsButton.setAttribute("aria-expanded", String(visible));
      optionsButton.classList.toggle("phi-active", visible);
    };
    optionsButton.addEventListener("click", () =>
      setOptionsVisible(options.hasAttribute("hidden")));
    options.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      event.stopPropagation();
      setOptionsVisible(false);
      optionsButton.focus();
    });
    const outsideOptions = (event: PointerEvent) => {
      if (!optionsWrapper.contains(event.target as Node)) setOptionsVisible(false);
    };
    document.addEventListener("pointerdown", outsideOptions, true);
    const observer = new MutationObserver(() => {
      if (panel.isConnected) return;
      document.removeEventListener("pointerdown", outsideOptions, true);
      observer.disconnect();
    });
    observer.observe(this.view.dom, { childList: true, subtree: true });

    replaceOne.classList.add("phi-replace-one", "phi-replace-row-item");
    replaceAll.classList.add("phi-replace-all", "phi-replace-row-item");
    close.classList.add("phi-search-close");
    const grid = document.createElement("div");
    grid.className = "phi-search-grid";
    grid.append(
      searchEntry,
      navigation,
      replaceToggle,
      optionsWrapper,
      close,
      replaceEntry,
      replaceOne,
      replaceAll,
    );
    panel.replaceChildren(grid);
    /* Moving CodeMirror's focused input into our custom grid makes WebKit
     * drop focus. Restore it explicitly so Ctrl+F is immediately typable. */
    search.focus({ preventScroll: true });
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

  private handleCopy(event: ClipboardEvent, view: EditorView): boolean {
    if (!event.clipboardData || view.state.selection.ranges.every((range) => range.empty))
      return false;
    const markdown = view.state.selection.ranges
      .map((range) => view.state.sliceDoc(range.from, range.to))
      .join("\n");
    event.clipboardData.setData("text/plain", markdown);
    event.clipboardData.setData("text/html", markdownClipboardHtml(markdown));
    event.preventDefault();
    return true;
  }

  private handlePaste(event: ClipboardEvent, view: EditorView): boolean {
    const clipboard = event.clipboardData;
    const file = clipboardImageFile(clipboard);
    const insertAttachment = (result: { path?: string; name?: string }) => {
      if (!result.path) return;
      const name = (result.name ?? "Pasted image")
        .replace(/\.[^.]+$/, "").replace(/]/g, "\\]");
      view.dispatch(view.state.replaceSelection(`![${name}](<${result.path}>)`));
    };
    if (file) {
      event.preventDefault();
      const reader = new FileReader();
      reader.onload = () => {
        const encoded = String(reader.result ?? "").split(",").pop() ?? "";
        requestNative<{ path?: string; name?: string }>(
          "attachment/create",
          { mimeType: file.type, data: encoded },
          30_000,
        )
          .then(insertAttachment)
          .catch((error) => reportError(error, "attachment/paste", this.documentPath));
      };
      reader.onerror = () => reportError(
        reader.error ?? new Error("Could not read the pasted image"),
        "attachment/paste",
        this.documentPath,
      );
      reader.readAsDataURL(file);
      return true;
    }
    const selection = view.state.selection.main;
    const plain = clipboard?.getData("text/plain") ?? "";
    if (!selection.empty && /^(https?|mailto):\S+$/i.test(plain)) {
      event.preventDefault();
      const label = view.state.sliceDoc(selection.from, selection.to);
      view.dispatch({ changes: { from: selection.from, to: selection.to, insert: `[${label}](${plain})` }, userEvent: "input.paste" });
      return true;
    }
    const html = clipboard?.getData("text/html") ?? "";
    if (html) {
      const markdown = clipboardHtmlToMarkdown(html);
      if (markdown) {
        event.preventDefault();
        view.dispatch({
          changes: view.state.changeByRange((range) => ({
            changes: { from: range.from, to: range.to, insert: markdown },
            range: EditorSelection.cursor(range.from + markdown.length),
          })).changes,
          userEvent: "input.paste",
        });
        return true;
      }
    }
    /* WebKitGTK can emit an entirely empty DataTransfer even while the GTK
     * clipboard advertises image/png. Let the native host read it directly. */
    if (!plain && !html && clipboardMayContainNativeImage(clipboard)) {
      event.preventDefault();
      requestNative<{ path?: string; name?: string }>(
        "attachment/paste", {}, 30_000,
      )
        .then(insertAttachment)
        .catch((error) => reportError(error, "attachment/paste", this.documentPath));
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
    setCustomSnippets(this.settings.snippets, this.settings.snippetVariables);
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
    if (previous.latexConceal !== this.settings.latexConceal)
      effects.push(latexEnhancementsCompartment.reconfigure(
        latexEnhancements(this.settings.latexConceal),
      ));
    if (previous.allowRemoteImages !== this.settings.allowRemoteImages)
      effects.push(refreshLivePreview.of(null));
    if (previous.snippets !== this.settings.snippets ||
        previous.snippetVariables !== this.settings.snippetVariables)
      setCustomSnippets(this.settings.snippets, this.settings.snippetVariables);
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
