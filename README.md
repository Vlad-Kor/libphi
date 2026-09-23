# Phi Document Viewer

Phi is a hierarchical Markdown editor and PDF viewer built with GTK 4 and
libadwaita.

## Build from source

With Flatpak (recommended):

```bash
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --install --force-clean flatpak-build ai.korsakov.Phi.yml
```
Directly with Meson:

```bash
meson setup _build --prefix="$HOME/.local"
meson compile -C _build
meson install -C _build
```

## Using libphi from other applications

The PDF viewer widget is part of libphi, which only needs GLib/GIO, GTK 4 and
MuPDF. Build just the library, without the application's libadwaita,
WebKitGTK and JSON-GLib dependencies:

```bash
meson setup _build-lib -Dapp=false --prefix="$PWD/_install"
meson install -C _build-lib
```

This installs `Phi-1.0.typelib`, so PyGObject applications can use libphi
directly. Point GObject Introspection and the dynamic loader at the prefix
(`lib` or `lib64`, depending on the platform; on Windows add the `bin`
directory to `PATH` instead of setting `LD_LIBRARY_PATH`):

```bash
export GI_TYPELIB_PATH="$PWD/_install/lib64/girepository-1.0"
export LD_LIBRARY_PATH="$PWD/_install/lib64"
```

```python
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Phi", "1.0")
from gi.repository import GLib, Gtk, Phi

# PDF data produced in memory, for example by Typst; no file is needed.
document = Phi.Document.new_from_bytes(GLib.Bytes.new(pdf_data), None)
viewer = Phi.DocumentView(document=document)
window = Gtk.Window(child=Gtk.ScrolledWindow(child=viewer))
```

## License and third-party software

Phi is free software under the GNU Affero General Public License, version 3
or later. See [`COPYING`](COPYING). Copyright notices in individual source
files identify their respective authors.

See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for bundled and
optional dependencies. The installed web editor carries its exhaustive,
versioned license inventory in `share/phi/editor/THIRD_PARTY_LICENSES.txt`.
