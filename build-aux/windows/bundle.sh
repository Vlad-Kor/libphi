#!/usr/bin/env bash
# Builds Phi and collects everything it needs into a self-contained folder
# for the Windows installer. Run it from an MSYS2 UCRT64 shell:
#
#   build-aux/windows/bundle.sh [build directory] [bundle directory]
#
# The bundle has the usual prefix layout (bin, lib, share, etc), which GLib,
# GTK and gdk-pixbuf resolve relative to the executable at run time.
set -euo pipefail

source_dir=$(cd "$(dirname "$0")/../.." && pwd)
build_dir=$(realpath -m "${1:-$source_dir/_build-windows}")
bundle_dir=$(realpath -m "${2:-$source_dir/_bundle-windows}")
prefix=${MINGW_PREFIX:-/ucrt64}

if [[ ${MSYSTEM:-} != UCRT64 ]]; then
	echo "Run this script from an MSYS2 UCRT64 shell." >&2
	exit 1
fi

rm -rf "$bundle_dir"
# The prefix only decides where meson installs; nothing in pdfv depends on it
# at run time on Windows.
options=(
	--prefix="$(cygpath -m "$bundle_dir")"
	--buildtype=release
	-Dapp=true
	-Dtests=false
	-Dintrospection=disabled
)
if [[ -d $build_dir/meson-private ]]; then
	meson configure "$build_dir" "${options[@]}"
else
	meson setup "$build_dir" "$source_dir" "${options[@]}"
fi
meson compile -C "$build_dir"
meson install -C "$build_dir" --no-rebuild

# Loaded at run time, so the dependency walk below cannot find them:
# WebView2Loader.dll by the web view, gdbus.exe to hand files to a running
# instance, and the helpers GLib spawns processes with.
for program in WebView2Loader.dll gdbus.exe \
               gspawn-win64-helper.exe gspawn-win64-helper-console.exe; do
	cp "$prefix/bin/$program" "$bundle_dir/bin/"
done

# Image loaders, for image sizes of Markdown previews and GTK's icons.
loaders_dir=lib/gdk-pixbuf-2.0/2.10.0/loaders
mkdir -p "$bundle_dir/$loaders_dir"
for loader in png jpeg gif bmp ico tiff webp avif; do
	for file in "$prefix/$loaders_dir"/libpixbufloader-$loader.dll \
	            "$prefix/$loaders_dir"/libpixbufloader_$loader.dll \
	            "$prefix/$loaders_dir"/pixbufloader-$loader.dll; do
		[[ -f $file ]] && cp "$file" "$bundle_dir/$loaders_dir/"
	done
done
cp "$prefix/$loaders_dir"/pixbufloader_svg.dll "$bundle_dir/$loaders_dir/"

# Copies the MSYS2 DLLs every executable and module links against, until no
# new ones appear.
copy_dependencies() {
	local changed=1
	while ((changed)); do
		changed=0
		while IFS= read -r dll; do
			local name
			name=$(basename "$dll")
			if [[ ! -f $bundle_dir/bin/$name ]]; then
				cp "$dll" "$bundle_dir/bin/"
				changed=1
			fi
		done < <(find "$bundle_dir" -iname '*.exe' -o -iname '*.dll' |
		         xargs -d '\n' ldd 2>/dev/null |
		         awk -v prefix="$(cygpath -u "$prefix")/bin/" \
		             'index($3, prefix) == 1 { print $3 }' | sort -u)
	done
}
copy_dependencies

# Run from inside the bundle, the loader query writes the loaders' paths
# relative to the installation, which gdk-pixbuf resolves at run time.
cp "$prefix/bin/gdk-pixbuf-query-loaders.exe" "$bundle_dir/bin/"
(unset GDK_PIXBUF_MODULEDIR GDK_PIXBUF_MODULE_FILE
 "$bundle_dir/bin/gdk-pixbuf-query-loaders.exe" --update-cache)
rm "$bundle_dir/bin/gdk-pixbuf-query-loaders.exe"

# GTK's settings schemas.
mkdir -p "$bundle_dir/share/glib-2.0/schemas"
cp "$prefix"/share/glib-2.0/schemas/org.gtk.gtk4.*.gschema.xml \
	"$bundle_dir/share/glib-2.0/schemas/"
rm -f "$bundle_dir"/share/glib-2.0/schemas/org.gtk.gtk4.Inspector.gschema.xml
glib-compile-schemas "$bundle_dir/share/glib-2.0/schemas"

# Icons: GTK and libadwaita draw their symbolic icons from Adwaita, and
# fall back to hicolor.
mkdir -p "$bundle_dir/share/icons"
cp -r "$prefix/share/icons/Adwaita" "$bundle_dir/share/icons/"
mkdir -p "$bundle_dir/share/icons/hicolor"
cp "$prefix/share/icons/hicolor/index.theme" "$bundle_dir/share/icons/hicolor/"
gtk4-update-icon-cache -q -f -t "$bundle_dir/share/icons/Adwaita"
gtk4-update-icon-cache -q -f -t "$bundle_dir/share/icons/hicolor"

# Translations of the toolkit strings, as a Linux system would have them.
for domain in gtk40 libadwaita glib20; do
	for catalog in "$prefix"/share/locale/*/LC_MESSAGES/$domain.mo; do
		[[ -f $catalog ]] || continue
		relative=${catalog#"$prefix/"}
		mkdir -p "$bundle_dir/$(dirname "$relative")"
		cp "$catalog" "$bundle_dir/$relative"
	done
done

# Fonts are found through fontconfig when Pango uses it.
if [[ -f $bundle_dir/bin/libfontconfig-1.dll ]]; then
	mkdir -p "$bundle_dir/etc"
	cp -r "$prefix/etc/fonts" "$bundle_dir/etc/"
fi

# Licenses of everything shipped.
mkdir -p "$bundle_dir/share/licenses"
for dll in "$bundle_dir"/bin/*.dll "$bundle_dir"/bin/*.exe; do
	package=$(pacman -Qqo "$prefix/bin/$(basename "$dll")" 2>/dev/null) ||
		continue
	name=${package#"${MINGW_PACKAGE_PREFIX:-mingw-w64-ucrt-x86_64}-"}
	if [[ -d $prefix/share/licenses/$name &&
	      ! -d $bundle_dir/share/licenses/$name ]]; then
		cp -r "$prefix/share/licenses/$name" "$bundle_dir/share/licenses/"
	fi
done

# Nothing below is needed at run time.
rm -rf "$bundle_dir/include" "$bundle_dir/lib/pkgconfig" \
       "$bundle_dir/share/applications" "$bundle_dir/share/metainfo"
find "$bundle_dir" -name '*.dll.a' -delete
find "$bundle_dir/bin" "$bundle_dir/lib" -iname '*.exe' -o -iname '*.dll' |
	grep -v -i 'WebView2Loader.dll' |
	xargs -d '\n' strip --strip-unneeded 2>/dev/null || true

echo "Bundled Phi into $bundle_dir ($(du -sh "$bundle_dir" | cut -f1))"
