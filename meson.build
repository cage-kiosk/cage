project('cage', 'c',
  version: '0.1.1',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=3',
  ],
)

add_project_arguments(
  [
    '-DWLR_USE_UNSTABLE',
    '-Wall',
    '-Wundef',
    '-Wno-unused-parameter',
  ],
  language: 'c',
)

if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG', language : 'c')
endif

cc = meson.get_compiler('c')

is_freebsd = host_machine.system().startswith('freebsd')
if is_freebsd
  add_project_arguments(
    [
      '-Wno-format-extra-args',
      '-Wno-gnu-zero-variadic-macro-arguments',
    ],
    language: 'c'
  )
endif

wlroots        = dependency('wlroots', version: '>= 0.9.1')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')
wayland_server = dependency('wayland-server')
pixman         = dependency('pixman-1')
xkbcommon      = dependency('xkbcommon')
math           = cc.find_library('m')

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

if get_option('xwayland')
  wlroots_has_xwayland = cc.get_define('WLR_HAS_XWAYLAND', prefix: '#include <wlr/config.h>', dependencies: wlroots) == '1'
  if not wlroots_has_xwayland
    error('Cannot build Cage with XWayland support: wlroots has been built without it')
  else
    have_xwayland = true
  endif
else
  have_xwayland = false
endif

version = '@0@'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
  git_commit = run_command([git, 'rev-parse', '--short', 'HEAD'])
  git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'])
  if git_commit.returncode() == 0 and git_branch.returncode() == 0
    version = '@0@-@1@ (branch \'@2@\')'.format(
      meson.project_version(),
      git_commit.stdout().strip(),
      git_branch.stdout().strip(),
    )
  endif
endif

conf_data = configuration_data()
conf_data.set10('CAGE_HAS_XWAYLAND', have_xwayland)
conf_data.set_quoted('CAGE_VERSION', version)

cage_sources = [
  'cage.c',
  'idle_inhibit_v1.c',
  'output.c',
  'render.c',
  'seat.c',
  'util.c',
  'view.c',
  'xdg_shell.c',
]

cage_headers = [
  configure_file(input: 'config.h.in',
		 output: 'config.h',
		 configuration: conf_data),
  'idle_inhibit_v1.h',
  'output.h',
  'render.h',
  'seat.h',
  'server.h',
  'util.h',
  'view.h',
  'xdg_shell.h',
]

if conf_data.get('CAGE_HAS_XWAYLAND', 0) == 1
  cage_sources += 'xwayland.c'
  cage_headers += 'xwayland.h'
endif

executable(
  meson.project_name(),
  cage_sources + cage_headers,
  dependencies: [
    server_protos,
    wayland_server,
    wlroots,
    xkbcommon,
    pixman,
    math,
  ],
  install: true,
)

summary = [
	'',
	'Cage @0@'.format(version),
	'',
	'    xwayland: @0@'.format(have_xwayland),
	''
]
message('\n'.join(summary))
