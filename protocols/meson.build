wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
wayland_scanner = find_program('wayland-scanner', native: true)

wayland_scanner_server_header = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_private_code = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

server_protocols = [
  [wl_protocol_dir, 'stable/xdg-shell/xdg-shell.xml'],
  ['wlr-layer-shell-unstable-v1.xml'],
]

wl_protos_headers = []
wl_protos_src = []

foreach p : server_protocols
  xml = join_paths(p)
  wl_protos_headers += wayland_scanner_server_header.process(xml)
  wl_protos_src += wayland_scanner_private_code.process(xml)
endforeach

lib_server_protos = static_library(
  'server_protos',
  wl_protos_headers + wl_protos_src,
  dependencies: wayland_server.partial_dependency(compile_args: true),
)

server_protos = declare_dependency(
  link_with: lib_server_protos,
  sources: wl_protos_headers,
)
