project(
  'i3d',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=c23',
    'warning_level=3',
    'werror=true',
    'buildtype=release',
  ],
)

cc = meson.get_compiler('c')
luajit_dep = dependency('luajit')
math_dep = cc.find_library('m', required: true)

# Keep the single-file upstream library isolated from the daemon's strict
# warning policy while retaining C23 and the selected build type.
yyjson_include = include_directories('vendor/yyjson', is_system: true)
yyjson_library = static_library(
  'yyjson-vendored',
  'vendor/yyjson/yyjson.c',
  include_directories: yyjson_include,
  install: false,
  override_options: ['warning_level=0', 'werror=false'],
)
yyjson_dep = declare_dependency(
  include_directories: yyjson_include,
  link_with: yyjson_library,
)

i3d_sources = files(
  'src/daemon.c',
  'src/ipc.c',
  'src/log.c',
  'src/lua_runtime.c',
  'src/main.c',
  'src/pid.c',
  'src/xcb_backend.c',
)

i3d_executable = executable(
  'i3d',
  i3d_sources,
  dependencies: [luajit_dep, math_dep, yyjson_dep],
  include_directories: include_directories('src'),
  install: true,
)

node = find_program('node')
test(
  'fake-i3 integration',
  node,
  args: [
    files('tests/integration.js'),
    i3d_executable,
    files('tests/fake_i3.js'),
    meson.current_source_dir() / 'examples',
  ],
  timeout: 30,
)

install_data(
  'dist/i3d.service',
  install_dir: get_option('datadir') / 'systemd/user',
)
install_data(
  'API.rst',
  install_dir: get_option('datadir') / 'doc/i3d',
)
