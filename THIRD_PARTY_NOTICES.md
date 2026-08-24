# Third-party notices

Phi is licensed under the GNU Affero General Public License, version 3 or
later. The complete license is in [`COPYING`](COPYING).

Phi includes or can be built with the following third-party works. This file
is an overview; the complete license texts distributed with each component
remain authoritative.

## Web editor assets

The installed web editor contains JavaScript packages under Apache-2.0,
BSD-2-Clause, BSD-3-Clause, ISC, MIT, MIT-0, and Unlicense terms. It also
contains source-derived material from Obsidian LaTeX Suite and VimTeX under
the MIT License, and from MathJax under the Apache License 2.0.

The exhaustive, versioned inventory, copyright notices, license texts, and
upstream NOTICE files are generated from the actual production bundles and
the dependency metadata for copied upstream-built assets (including
MathJax's Speech Rule Engine worker). It is installed next to the assets as:

`share/phi/editor/THIRD_PARTY_LICENSES.txt`

The generated JavaScript files point to that document in their leading
comment. Regenerate both the assets and inventory with `npm run build` from
`web/editor` after changing web dependencies.

## MuPDF fallback

Phi normally links to the system MuPDF library. If Meson cannot find it, the
project's pinned MuPDF wrap builds MuPDF and its required libraries and fonts
from source. MuPDF is available under the GNU Affero General Public License,
version 3 or later. That fallback source is not maintained by Phi; Phi adds a
Meson build integration and does not replace the upstream license terms.

Fallback installations include the MuPDF license and the license or notice
files for its compiled third-party libraries and embedded fonts under:

`share/doc/phi/licenses/mupdf/`

This software is based in part on the work of the Independent JPEG Group.

## System libraries

GTK, GLib, libadwaita, WebKitGTK, JSON-GLib, and a system-provided MuPDF are
linked as system dependencies and are not copied by a normal Meson install.
Distributors that bundle those libraries must include the license materials
required by the exact versions they redistribute.

## Binary distribution

Anyone distributing a Phi binary must provide the complete corresponding
source required by the AGPL for that exact binary. If the bundled MuPDF
fallback was used, the source archive must include the pinned MuPDF
subproject and its recursively fetched dependencies. See `README.md` for the
release command and checklist.
