project('hypr-osk', 'cpp',
  version: '0.1.0',
  default_options: ['buildtype=release'],
)

cpp_compiler = meson.get_compiler('cpp')
if cpp_compiler.has_argument('-std=c++23')
  add_global_arguments('-std=c++23', language: 'cpp')
elif cpp_compiler.has_argument('-std=c++2b')
  add_global_arguments('-std=c++2b', language: 'cpp')
else
  error('C++23 required')
endif
add_global_arguments('-Wall', language: 'cpp')

hyprland_headers = dependency('hyprland')
hyprland_deps = [
  hyprland_headers,
  dependency('pixman-1'),
  dependency('libinput'),
  dependency('wayland-server'),
  dependency('xkbcommon'),
  dependency('libdrm'),
]

shared_module('hypr-osk',
  'src/main.cpp',
  cpp_args: ['-DWLR_USE_UNSTABLE'],
  dependencies: [hyprland_deps],
  install: true)
