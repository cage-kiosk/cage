wl_protocol_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')

test_protocols = [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  wl_protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
  # Not part of wayland-protocols; vendored from wlr-protocols.
  'wlr-virtual-pointer-unstable-v1.xml',
  'wlr-output-management-unstable-v1.xml',
]

test_protocol_src = []
foreach xml : test_protocols
  test_protocol_src += custom_target(
    fs.stem(xml) + '-code',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  test_protocol_src += custom_target(
    fs.stem(xml) + '-client-header',
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

pointer_constraints_client = executable(
  'pointer-constraints-client',
  ['pointer-constraints.c'] + test_protocol_src,
  dependencies: [wayland_client, math],
)

test(
  'pointer-constraints',
  find_program('run-test.sh'),
  args: [cage, pointer_constraints_client],
  timeout: 90,
)
