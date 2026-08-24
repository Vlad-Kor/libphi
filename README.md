Phi Document Viewer is a hirarchical markdown editor and pdf viewer built with gtk4/ libadwaita. 

### build instructions

```bash
meson setup _build --prefix="$HOME/.local"
meson compile -C _build
meson install -C _build
```