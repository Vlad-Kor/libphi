# libphi - High performance document renderer for GTK
# Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

phi_src = [
	'phierrors.c',
	'phidocument.c',
	'phipage.c',
	'phiview.c',

	'phigiostream.c',
	'phinodedevice.c',
]

phi_lib = library('phi', phi_src,
	include_directories: inc,
	dependencies: [
		gio_dep,
		gtk_dep,
		mupdf_dep,
	],
	install: true
)

phi_dep = declare_dependency(
	include_directories: inc,
	link_with: phi_lib,
	dependencies: [
		gio_dep,
		gtk_dep
	]
)
meson.override_dependency('phi', phi_dep)
