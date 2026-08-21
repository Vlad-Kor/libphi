# Phi Document Viewer

Phi is a native GTK/libadwaita PDF viewer with an embedded, Obsidian-compatible Markdown editor. Markdown notes use CodeMirror 6 Live Preview, offline MathJax, Mermaid, GFM/Obsidian extensions, vault-scoped links and embeds, and LaTeX Suite-style snippets. The `.md` source remains the canonical document.

## Building on Fedora

Building the complete application requires both the native C toolchain and Node.js/npm. Node is only used to compile the browser-side editor; the installed application does not use Node and `web/editor/node_modules/` must not be committed.

Install the build dependencies:

```sh
sudo dnf install \
  gcc meson ninja-build pkgconf-pkg-config \
  gtk4-devel libadwaita-devel mupdf-devel \
  webkitgtk6.0-devel json-glib-devel \
  nodejs nodejs-npm
```

Build and test the static editor bundle:

```sh
cd web/editor
npm ci
npm run check
npm test
npm run build
cd ../..
```

`npm ci` recreates `node_modules/` from the pinned `package-lock.json`. Commit changes to the TypeScript sources, lockfile, and generated `web/editor/dist/` assets, but never commit `node_modules/`.

Configure, build, and test the native application:

```sh
meson setup _build-release --buildtype=release --prefix=/usr
meson compile -C _build-release
meson test -C _build-release --print-errorlogs
```

If `_build-release` already exists, reconfigure it instead:

```sh
meson setup _build-release --reconfigure --buildtype=release --prefix=/usr
```

Run the uninstalled build:

```sh
./_build-release/src/pdfv
```

Install it system-wide:

```sh
sudo meson install -C _build-release
```

The explicit `/usr` prefix avoids accidentally leaving a newer executable in `/usr/local/bin` while an older `/usr/bin/pdfv` is still selected by the desktop launcher. Close all running Phi windows before testing a newly built executable because the application uses a single desktop application ID.

## Development workflow

After changing only C sources:

```sh
meson compile -C _build-release
```

After changing anything under `web/editor/src/`, rebuild the frontend before compiling or installing:

```sh
npm --prefix web/editor run check
npm --prefix web/editor test
npm --prefix web/editor run build
meson compile -C _build-release
```

The generated bundle is installed under `share/phi/editor/` and works fully offline. LaTeX snippets are configured at runtime in **Settings**, not read from files in `doc/` during the build.

Open either a PDF or Markdown file directly, or use **Open Folder…** to browse both formats in the native workspace sidebar. When a note is inside an Obsidian vault, Phi discovers the nearest parent containing `.obsidian/` and uses that directory for wikilinks and attachment lookup.

Markdown is autosaved after edits and flushed again before a tab or window is allowed to close. `Ctrl+S` remains available as an immediate flush and shows a reminder that autosave is active. Workspace search (`Ctrl+Shift+F`) indexes both PDFs and Markdown notes.

The installed desktop file advertises `application/pdf`, `text/markdown`, and `text/x-markdown`, so GNOME Files can offer Phi for `.pdf` and `.md` files. After a manual system-wide install, refresh desktop associations if the new MIME choices have not appeared yet:

```sh
sudo update-desktop-database /usr/share/applications
```

Further implementation and troubleshooting notes are in [doc/markdown-editor.md](doc/markdown-editor.md).
