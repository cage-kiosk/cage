wayland_protos = dependency('wayland-protocols', version: '>=1.14')
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

protocols = [
	wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	'wlr-layer-shell-unstable-v1.xml',
]

wl_protos_src = []

foreach xml : protocols
	wl_protos_src += custom_target(
		xml.underscorify() + '_c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
	wl_protos_src += custom_target(
		xml.underscorify() + '_server_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach
