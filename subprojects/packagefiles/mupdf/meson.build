project('mupdf', 'c', 'cpp',
	version: run_command(
		find_program('awk'), '/#define FZ_VERSION / { gsub(/"/, "", $3); print $3 }',
		meson.current_source_dir() / 'include/mupdf/fitz/version.h',
		capture: true,
		check: true
	).stdout().strip(),
	meson_version: '>= 1.2',
	default_options: ['warning_level=1', 'c_std=gnu11', 'cpp_std=c++14']
)
version_split = meson.project_version().split('.')

hexdump_gen = generator(find_program('bash'),
	arguments: [meson.current_source_dir() / 'scripts' / 'hexdump.sh', '@INPUT@'],
	capture: true,
	output: '@BASENAME@.c'
)

subdir('resources/fonts')

make = find_program('make')
thirdparty_libs = ['freetype', 'gumbo', 'harfbuzz', 'libjpeg', 'lcms2', 'mujs', 'zlib', 'jbig2dec', 'openjpeg', 'brotli', 'extract']
thirdparty_deps = []
foreach thirdparty : thirdparty_libs
	thirdparty_out = run_command(make, '-f', meson.current_source_dir() / 'meson-thirdparty.mk', 'library=@0@'.format(thirdparty.to_upper()),
		capture: true,
		check: true
	).stdout().splitlines()
	if thirdparty_out.length() != 3
		error('meson-thirdparty.mk for @0@ returned unexpected output; expected 3 lines got @1@'.format(thirdparty, thirdparty_out.length()))
	endif

	thirdparty_src = thirdparty_out[0] != '' ? thirdparty_out[0].split(' ') : []

	thirdparty_public_flags = []
	thirdparty_public_inc = []
	foreach flag : thirdparty_out[1] != '' ? thirdparty_out[1].split(' ') : []
		if flag.startswith('-I')
			thirdparty_public_inc += include_directories(flag.substring(2))
		else
			thirdparty_public_flags += flag
		endif
	endforeach

	thirdparty_private_flags = []
	thirdparty_private_inc = []
	foreach flag : thirdparty_out[2] != '' ? thirdparty_out[2].split(' ') : []
		if flag.startswith('-I')
			thirdparty_private_inc += include_directories(flag.substring(2))
		else
			thirdparty_private_flags += flag
		endif
	endforeach

	thirdparty_deps += declare_dependency(
		compile_args: thirdparty_public_flags,
		include_directories: thirdparty_public_inc,
		link_with: static_library('thirdparty-@0@'.format(thirdparty), thirdparty_src,
			c_args: thirdparty_public_flags + thirdparty_private_flags,
			cpp_args: thirdparty_public_flags + thirdparty_private_flags,
			include_directories: thirdparty_public_inc + thirdparty_private_inc,
			install: false
		)
	)
endforeach

inc = include_directories('include')
install_subdir('include/mupdf', install_dir: get_option('includedir'), strip_directory: false)

subdir('source')
