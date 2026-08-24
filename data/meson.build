# Data files for Phi PDF Viewer

# Use the executable from this installation prefix. A bare `pdfv` would be
# resolved through the desktop session's PATH and could launch another copy
# installed under a different prefix.
desktop_configuration = configuration_data()
desktop_configuration.set(
  'PDFV_EXECUTABLE',
  get_option('prefix') / get_option('bindir') / 'pdfv'
)
configure_file(
  input: 'ai.korsakov.Phi.desktop.in',
  output: 'ai.korsakov.Phi.desktop',
  configuration: desktop_configuration,
  install: true,
  install_dir: get_option('datadir') / 'applications'
)

# Appstream metainfo
install_data(
  'ai.korsakov.Phi.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo'
)

# Icons
install_data(
  'icons/hicolor/scalable/apps/ai.korsakov.Phi.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps'
)

install_data(
  'icons/hicolor/256x256/apps/ai.korsakov.Phi.png',
  install_dir: get_option('datadir') / 'icons/hicolor/256x256/apps'
)

# Keep the application's license and third-party overview available in every
# installation, including user-local installs without a package manager.
install_data(
  '../COPYING',
  '../THIRD_PARTY_NOTICES.md',
  install_dir: get_option('datadir') / 'doc' / 'phi'
)
