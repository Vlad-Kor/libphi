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

Install it system-wide:

```sh
sudo meson install -C _build-release
```


