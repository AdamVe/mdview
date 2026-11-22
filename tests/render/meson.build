# PNG render regression tests (skip with exit code 77 when no display backend).
# Included via subdir() from frontends/gtk/meson.build, which provides
# gtkmm, gtest, gtest_inc, includes and mdview_exe from its scope.

test_render_exe = executable('test_render',
  'test_render.cpp',
  include_directories: [includes, gtest_inc],
  dependencies: [gtkmm, gtest],
  install: false,
)
test('render_png', test_render_exe,
  args: [mdview_exe, meson.current_source_dir() / 'fixtures'],
  suite: ['render'],
  timeout: 180,
)
