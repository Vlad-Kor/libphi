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

test_device = executable('test-device', 'test-device.c',
	include_directories: inc,
	dependencies: [phi_dep, mupdf_dep],
)

test('device-test', test_device)
