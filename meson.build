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

project('phi', 'c',
	version: '0.1',
	default_options: ['warning_level=2', 'c_std=gnu2x']
)

cc = meson.get_compiler('c')

gtk_dep = dependency('gtk4', version: '>= 4.14')
gio_dep = dependency('gio-2.0')
mupdf_dep = dependency('mupdf')

inc = include_directories('.')
subdir('phi')
subdir('src')