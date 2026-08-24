# libphi - High performance document renderer for GTK
# Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
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

test_text = executable('test-text', 'test-text.c',
	include_directories: inc,
	dependencies: [phi_dep, mupdf_dep],
	c_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"'],
)

test('text-test', test_text)

test_workspace = executable('test-workspace',
	'test-workspace.c',
	'../src/pdfv-workspace.c',
	include_directories: [inc, include_directories('../src')],
	dependencies: [phi_dep, mupdf_dep],
	c_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"'],
)

test('workspace-test', test_workspace, timeout: 30)

test_workspace_file_ops = executable('test-workspace-file-ops',
	'test-workspace-file-ops.c',
	'../src/workspace-file-ops.c',
	include_directories: [inc, include_directories('../src')],
	dependencies: [gio_dep],
)

test('workspace-file-ops-test', test_workspace_file_ops)

test_markdown_vault = executable('test-markdown-vault',
	'test-markdown-vault.c',
	'../src/markdown-vault-adapter.c',
	include_directories: [inc, include_directories('../src')],
	dependencies: [gio_dep, json_glib_dep],
	c_args: ['-DTEST_MARKDOWN_FIXTURE_DIR="' + meson.current_source_dir() / 'markdown-fixtures' + '"'],
)

test('markdown-vault-test', test_markdown_vault)

test_settings = executable('test-settings',
	'test-settings.c',
	'../src/pdfv-settings.c',
	include_directories: [inc, include_directories('../src')],
	dependencies: [gio_dep],
)

test('settings-test', test_settings)

test_document_view = executable('test-document-view',
	'test-document-view.c',
	'../src/pdfv-document-view.c',
	include_directories: [inc, include_directories('../src')],
	dependencies: [phi_dep, mupdf_dep, adw_dep],
	c_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"'],
)

test('document-view-test', test_document_view)

test_window_components = executable('test-window-components',
	'test-window-components.c',
	'../src/pdfv-document-view.c',
	'../src/pdfv-document-properties.c',
	'../src/pdfv-page-selector.c',
	'../src/pdfv-thumbnail-list.c',
	include_directories: [inc, include_directories('../src')],
	dependencies: [phi_dep, mupdf_dep, adw_dep],
	c_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"'],
)

test('window-components-test', test_window_components)
