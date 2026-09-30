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

adw_dep = dependency('libadwaita-1', version: '>= 1.6')
json_glib_dep = dependency('json-glib-1.0')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')

pdfv_sources = [
	'main.c',
	'pdfv-application.c',
	'pdfv-window.c',
	'document-history.c',
	'pdfv-document-properties.c',
	'pdfv-page-selector.c',
	'pdfv-thumbnail-list.c',
	'pdfv-workspace.c',
	'workspace-history.c',
	'workspace-file-ops.c',
	'pdfv-settings.c',
	'markdown-editor.c',
	'markdown-export.c',
	'markdown-editor-bridge.c',
	'markdown-resource-scheme.c',
	'markdown-vault-adapter.c',
	'web-view.c',
]

# PdfvWebView has one backend per platform; see web-view-backend.h.
if host_machine.system() == 'windows'
	add_languages('cpp', native: false)
	# WebView2Loader.dll is loaded at run time; it has no import library.
	web_view_deps = [
		cc.find_library('ole32'),
		cc.find_library('comctl32'),
	]
	pdfv_sources += 'web-view-webview2.cpp'
else
	web_view_deps = [dependency('webkitgtk-6.0', version: '>= 2.40')]
	pdfv_sources += 'web-view-webkit.c'
endif

# Windows shows the embedded icon and version for the executable, and starts
# it without a console window.
pdfv_win_subsystem = 'console'
if host_machine.system() == 'windows'
	version_parts = meson.project_version().split('.')
	foreach _ : range(version_parts.length(), 4)
		version_parts += '0'
	endforeach
	rc_configuration = configuration_data()
	rc_configuration.set('VERSION', meson.project_version())
	rc_configuration.set('VERSION_COMMA', ','.join(version_parts))
	pdfv_rc = configure_file(
		input: '../data/windows/pdfv.rc.in',
		output: 'pdfv.rc',
		configuration: rc_configuration,
	)
	pdfv_sources += import('windows').compile_resources(pdfv_rc,
		include_directories: include_directories('../data/windows'),
		depend_files: files('../data/windows/ai.korsakov.Phi.ico'),
	)
	pdfv_win_subsystem = 'windows'
endif

editor_install_dir = get_option('prefix') / get_option('datadir') / 'phi' / 'editor'
editor_source_dir = meson.project_source_root() / 'web' / 'editor' / 'dist'

pdfv_app = executable('pdfv', pdfv_sources,
	dependencies: [
		gtk_dep,
		adw_dep,
		phi_dep,
		mupdf_dep,
		web_view_deps,
		json_glib_dep,
		gdk_pixbuf_dep,
	],
	c_args: [
		'-DPDFV_EDITOR_DIR="' + editor_install_dir + '"',
		'-DPDFV_EDITOR_SOURCE_DIR="' + editor_source_dir + '"',
	],
	win_subsystem: pdfv_win_subsystem,
	# A per-user prefix such as ~/.local/lib64 is not normally searched by the
	# dynamic loader. Keep the executable paired with the libphi installed in
	# the same prefix instead of accidentally loading an older system copy.
	install_rpath: '$ORIGIN/../' + get_option('libdir'),
	install: true
)

install_subdir('../web/editor/dist',
	install_dir: get_option('datadir') / 'phi' / 'editor',
	strip_directory: true,
)

npm = find_program('npm', required: false)
if npm.found()
	run_target('markdown-editor-web',
		command: [npm, '--prefix', meson.project_source_root() / 'web' / 'editor',
		          'run', 'build'],
	)
endif
