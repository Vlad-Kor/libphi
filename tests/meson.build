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

test_document = executable('test-document', 'test-document.c',
	include_directories: inc,
	dependencies: [phi_dep],
)

test('document-test', test_document)

test_document_view = executable('test-document-view',
	'test-document-view.c',
	include_directories: inc,
	dependencies: [phi_dep],
	c_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"'],
)

test('document-view-test', test_document_view)

test_vector_render = executable('test-vector-render', 'test-vector-render.c',
	include_directories: inc,
	dependencies: [phi_dep],
)

test('vector-render-test', test_vector_render)

# Tests of pdfv application code; these link sources from src/ and need the
# application's dependencies.
if get_option('app')
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

	test_workspace_history = executable('test-workspace-history',
		'test-workspace-history.c',
		'../src/workspace-history.c',
		include_directories: [inc, include_directories('../src')],
		dependencies: [gio_dep],
	)

	test('workspace-history-test', test_workspace_history)

	test_markdown_vault = executable('test-markdown-vault',
		'test-markdown-vault.c',
		'../src/markdown-vault-adapter.c',
		include_directories: [inc, include_directories('../src')],
		dependencies: [gio_dep, json_glib_dep, gdk_pixbuf_dep],
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

	test_document_history = executable('test-document-history',
		'test-document-history.c',
		'../src/document-history.c',
		include_directories: [inc, include_directories('../src')],
		dependencies: [gio_dep],
	)

	test('document-history-test', test_document_history)

	test_window_components = executable('test-window-components',
		'test-window-components.c',
		'../src/pdfv-document-properties.c',
		'../src/pdfv-page-selector.c',
		'../src/pdfv-thumbnail-list.c',
		include_directories: [inc, include_directories('../src')],
		dependencies: [phi_dep, mupdf_dep, adw_dep],
		c_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"'],
	)

	test('window-components-test', test_window_components)
endif

# PyGObject smoke test against the uninstalled typelib and library.
python3 = find_program('python3', required: false)
if is_variable('phi_gir') and python3.found()
	gi_env = environment()
	gi_env.prepend('GI_TYPELIB_PATH', meson.project_build_root() / 'phi')
	if host_machine.system() == 'windows'
		# Windows has no RUNPATH: the uninstalled libphi and MuPDF DLLs must
		# be on PATH for the typelib to load.
		gi_env.prepend('PATH', meson.project_build_root() / 'phi',
			meson.project_build_root() / 'subprojects' / 'mupdf' / 'source')
	else
		gi_env.prepend('LD_LIBRARY_PATH', meson.project_build_root() / 'phi')
	endif
	test('gi-test', python3,
		args: [files('test-gi.py')],
		env: gi_env,
		depends: phi_gir,
	)
endif
