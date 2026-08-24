# Data files for Phi PDF Viewer

# Desktop file
install_data(
  'ai.korsakov.Phi.desktop',
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
