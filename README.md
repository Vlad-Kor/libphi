# Phi Document Viewer

Phi is a hierarchical Markdown editor and PDF viewer built with GTK 4 and
libadwaita.

## Build from source

For a single-user install:

```bash
meson setup _build --prefix="$HOME/.local"
meson compile -C _build
meson install -C _build
```

## License and third-party software

Phi is free software under the GNU Affero General Public License, version 3
or later. See [`COPYING`](COPYING). Copyright notices in individual source
files identify their respective authors.

See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for bundled and
optional dependencies. The installed web editor carries its exhaustive,
versioned license inventory in `share/phi/editor/THIRD_PARTY_LICENSES.txt`.

