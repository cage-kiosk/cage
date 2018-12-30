project('cage', 'c',
  version: '0.0.1',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
  ],
)

add_project_arguments(
  [
    '-DWLR_USE_UNSTABLE',
    '-Wall',
    '-Werror',
    '-Wundef',
    '-Wno-unused-parameter',
  ],
  language: 'c',
)

if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG', language : 'c')
endif

wlroots        = dependency('wlroots')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')
wayland_server = dependency('wayland-server')
xkbcommon      = dependency('xkbcommon')

wl_protocol_dir = wayland_protos.get_pkgconfig_variable('pkgdatadir')
wayland_scanner = find_program('wayland-scanner')
wayland_scanner_server = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

server_protocols = [
  [wl_protocol_dir, 'stable/xdg-shell/xdg-shell.xml'],
]

server_protos_headers = []

foreach p : server_protocols
  xml = join_paths(p)
  server_protos_headers += wayland_scanner_server.process(xml)
endforeach

server_protos = declare_dependency(
  sources: server_protos_headers,
)

cage_sources = [
  'cage.c',
  'output.c',
  'seat.c',
  'view.c',
  'xdg_shell.c',
]

cage_headers = [
  'output.h',
  'seat.h',
  'server.h',
  'view.h',
  'xdg_shell.h',
]

executable(
  meson.project_name(),
  cage_sources + cage_headers,
  dependencies: [
    server_protos,
    wayland_server,
    wlroots,
    xkbcommon,
  ],
  install: true,
)
