# cmark-gfm: system libraries by default; -Dlink_cmark=static builds via wrap
if get_option('link_cmark') == 'static'
  cmark_sub = subproject('cmark-gfm', default_options: ['default_library=static'])
  cmark = cmark_sub.get_variable('cmark_gfm_dep')
  cmark_ext = cmark
else
  cmark = dependency('libcmark-gfm', required: false)
  if not cmark.found()
    error('libcmark-gfm is required. Install libcmark-gfm-dev and libcmark-gfm-extensions-dev (Debian/Ubuntu) or libcmark-gfm-devel (Fedora), or configure with -Dlink_cmark=static.')
  endif

  # Distro packages differ: Debian folds the extensions into libcmark-gfm.pc,
  # others expose libcmark-gfm-extensions separately. Accept either form.
  cmark_ext = dependency('libcmark-gfm-extensions', required: false)
  if not cmark_ext.found()
    cmark_ext = meson.get_compiler('c').find_library('cmark-gfm-extensions')
  endif
endif

mdview_core = static_library(
  'mdview-core',
  'src/parser.cpp',
  include_directories: include_directories('include'),
  dependencies: [cmark, cmark_ext]
)

mdview_core_dep = declare_dependency(
  link_with: mdview_core,
  include_directories: include_directories('include'),
  dependencies: [cmark, cmark_ext]
)

executable('test_parser',
  'src/test_parser.cpp',
  include_directories: include_directories('include'),
  dependencies: [mdview_core_dep],
  install: false
)
