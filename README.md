```
meson setup _build-release --buildtype=release
meson compile -C _build-release
sudo meson install -C _build-release
```