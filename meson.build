project('mdview', 'cpp', 'c',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++26',
    'warning_level=2',
    'buildtype=debugoptimized'
  ]
)

# single source of truth for the version shown in the About dialog
add_project_arguments('-DMDVIEW_VERSION="@0@"'.format(meson.project_version()), language: 'cpp')

frontend = get_option('frontend')
host_system = host_machine.system()

if host_system == 'darwin' or host_system == 'windows'
  if frontend == 'adwaita'
    error('The "adwaita" frontend is only supported on Linux.')
  endif
endif

use_adwaita = (frontend == 'adwaita')

subdir('core')

if frontend == 'gtk' or frontend == 'adwaita'
  subdir('frontends/gtk')
  # Render regression suite needs the built mdview binary; tests/render/meson.build
  # consumes gtkmm/gtest/mdview_exe from the shared subdir scope.
  subdir('tests/render')
endif