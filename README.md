# Phi Document Viewer

Phi is a native GTK/libadwaita PDF viewer with an embedded, Obsidian-compatible Markdown editor. Markdown notes use CodeMirror 6 Live Preview, offline MathJax, Mermaid, GFM/Obsidian extensions, vault-scoped links and embeds, and LaTeX Suite-style snippets. The `.md` source remains the canonical document.

## Build

Required development dependencies include GTK 4, libadwaita, MuPDF, WebKitGTK 6.0, JSON-GLib, Meson, and a C compiler. Fedora provides the WebKit headers in `webkitgtk6.0-devel`.

The generated web bundle is checked in under `web/editor/dist`, so an installed application does not require Node.js. To rebuild or test the frontend:

```sh
cd web/editor
npm ci
npm run check
npm test
npm run build
```

If `doc/my_snippets.txt` exists, the frontend build embeds it as the LaTeX Suite snippet set; otherwise it uses the built-in compatible defaults.

Build and install the native application:

```sh
meson setup _build-release --buildtype=release
meson compile -C _build-release
meson test -C _build-release --print-errorlogs
sudo meson install -C _build-release
```

Open either a PDF or Markdown file directly, or open a workspace folder to browse both formats in the native sidebar.
