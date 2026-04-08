# 4.12: first release with Gdk::Graphene wrappers, Snapshot::translate and Widget::compute_bounds
gtkmm    = dependency('gtkmm-4.0', version: '>= 4.12')
adwaita  = dependency('libadwaita-1', required: (frontend == 'adwaita'))

deps = [gtkmm, mdview_core_dep]
if adwaita.found()
  deps += adwaita
endif

gnome = import('gnome')
resources = gnome.compile_resources(
  'mdview-resources',
  'resources/app.gresource.xml',
  c_name: 'mdview',
  source_dir: 'resources'
)

includes = include_directories('include', '.')
sources = files(
  'src/main.cpp',
  'src/app.cpp',
  'src/main_window.cpp',
  'src/markdown_view.cpp',
  'src/search_bar.cpp',
  'src/smooth_scrolled_window.cpp',
)

executable('mdview',
  [sources, resources],
  include_directories: [includes],
  dependencies: deps,
  install: true,
  # If the dependency was found (which happens when frontend == 'adwaita'),
  # define the macro for the C++ code.
  cpp_args: (frontend == 'adwaita') ? ['-DUSE_ADWAITA'] : []
)

# Install desktop file (absolute Exec for the configured prefix)
desktop_conf = configuration_data()
desktop_conf.set('BINDIR', get_option('prefix') / get_option('bindir'))
configured_desktop = configure_file(
  input: 'data/com.adamve.mdview.desktop.in',
  output: 'com.adamve.mdview.desktop',
  configuration: desktop_conf
)
install_data(configured_desktop,
  install_dir: get_option('datadir') / 'applications'
)

# Check for rsvg-convert to generate PNG icons
rsvg_convert = find_program('rsvg-convert', required: false)

if rsvg_convert.found()
  # Generate and install PNG icons from SVG
  icon_sizes = ['16', '22', '24', '32', '48', '64', '128', '256', '512']
  foreach size : icon_sizes
    icon_output = 'com.adamve.mdview-' + size + '.png'
    custom_target('icon-' + size + 'x' + size,
      input: 'data/com.adamve.mdview.svg',
      output: icon_output,
      command: [rsvg_convert, '-w', size, '-h', size, '@INPUT@', '-o', '@OUTPUT@'],
      install: true,
      install_dir: get_option('datadir') / 'icons' / 'hicolor' / size / 'apps'
    )
  endforeach
else
  # rsvg-convert not available, just install SVG (modern desktops support it)
  message('rsvg-convert not found, only SVG icon will be installed. Install librsvg2-bin for PNG generation.')
endif

# Install SVG icon to scalable folder (for high-resolution displays)
install_data('data/com.adamve.mdview.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps'
)

# Post-install script to update icon and desktop databases
# Note: These require the respective tools to be installed on the system
# install: update-icon-caches -q $datadir/icons
# install: update-desktop-database -q $datadir/applications

# Install appstream metadata (GNOME Software)
install_data('data/com.adamve.mdview.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo'
)

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('metainfo', appstreamcli,
    args: ['validate', '--no-net', '--explain', meson.current_source_dir() / 'data' / 'com.adamve.mdview.metainfo.xml'],
    suite: ['metadata']
  )
endif
