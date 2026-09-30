#!/usr/bin/env bash
# Builds the Windows installer, Phi-<version>-windows-x86_64-setup.exe, in
# the output directory. Run it from an MSYS2 UCRT64 shell with the packages
# listed in .github/workflows/windows-installer.yml installed:
#
#   build-aux/windows/build-installer.sh [output directory]
#
# PHI_BUILD_NUMBER, if set, becomes the last part of the installer's numeric
# file version so that later builds of one version are recognisably newer.
set -euo pipefail

source_dir=$(cd "$(dirname "$0")/../.." && pwd)
output_dir=$(realpath -m "${1:-$source_dir/_installer}")
build_dir=$source_dir/_build-windows
bundle_dir=$source_dir/_bundle-windows
mkdir -p "$output_dir"

"$source_dir/build-aux/windows/bundle.sh" "$build_dir" "$bundle_dir"

version=$(meson introspect --projectinfo "$build_dir" |
          python -c 'import json, sys; print(json.load(sys.stdin)["version"])')
IFS=. read -r -a parts <<< "$version"
while ((${#parts[@]} < 3)); do parts+=(0); done
version_quad="${parts[0]}.${parts[1]}.${parts[2]}.$((${PHI_BUILD_NUMBER:-0} % 65536))"
# Builds from a checkout say which commit they are.
if revision=$(git -C "$source_dir" rev-parse --short HEAD 2>/dev/null); then
	display_version="$version+$revision"
else
	display_version=$version
fi

# Microsoft's bootstrapper installs the WebView2 Runtime on the rare systems
# without it. It may be redistributed with applications.
bootstrapper=$source_dir/_build-windows/MicrosoftEdgeWebview2Setup.exe
if [[ ! -f $bootstrapper ]]; then
	curl -fsSL --retry 3 -o "$bootstrapper" \
		'https://go.microsoft.com/fwlink/p/?LinkId=2124703'
fi

setup=$output_dir/Phi-$display_version-windows-x86_64-setup.exe
makensis -V2 \
	"-DVERSION=$display_version" \
	"-DVERSION_QUAD=$version_quad" \
	"-DBUNDLE_DIR=$(cygpath -w "$bundle_dir")" \
	"-DOUTFILE=$(cygpath -w "$setup")" \
	"-DWEBVIEW2_BOOTSTRAPPER=$(cygpath -w "$bootstrapper")" \
	"$(cygpath -w "$source_dir/build-aux/windows/installer.nsi")"

echo "Built $setup"
