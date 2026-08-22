import { reportError, sendNative } from "./bridge";
import { PhiMarkdownEditor } from "./editor";
import { applyTheme } from "./settings";
import type { NativeMessage } from "./types";

const parent = document.getElementById("editor");
if (!parent) throw new Error("Missing editor mount point");

applyTheme({ dark: window.matchMedia("(prefers-color-scheme: dark)").matches, fontScale: 1 });
const editor = new PhiMarkdownEditor(parent);
window.phiMarkdownEditor = editor;
window.nativeEditorReceive = (input: NativeMessage | string) => {
  try {
    const message = typeof input === "string" ? JSON.parse(input) as NativeMessage : input;
    if (message.protocol !== 1) throw new Error(`Unsupported editor protocol: ${String(message.protocol)}`);
    editor.receive(message);
  } catch (error) {
    reportError(error, "bridge/receive");
  }
};

window.addEventListener("error", (event) => reportError(event.error ?? event.message, "window"));
window.addEventListener("unhandledrejection", (event) => reportError(event.reason, "promise"));
sendNative("editor/ready", { capabilities: ["source", "live-preview", "mathjax", "mermaid", "latex-suite"] });
// MathJax and Mermaid are loaded by their first preview widget. Notes that do
// not use them never pay their startup cost.
