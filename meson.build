project('cage', 'c',
  version: '0.2.0',
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

if get_option('debug-damage-tracking')
  add_project_arguments('-DDEBUG_DAMAGE_TRACKING', language : 'c')
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

wlroots        = dependency('wlroots', version: '>= 0.11.0')
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

scdoc = dependency('scdoc', version: '>=1.9.2', native: true, required: get_option('man-pages'))
if scdoc.found()
  scdoc_prog = find_program(scdoc.get_pkgconfig_variable('scdoc'), native: true)
  sh = find_program('sh', native: true)
  mandir = get_option('mandir')
  man_files = [
    'cage.1.scd'
  ]
  foreach filename : man_files
    topic = filename.split('.')[-3].split('/')[-1]
    section = filename.split('.')[-2]
    output = '@0@.@1@'.format(topic, section)

    custom_target(
      output,
      input: filename,
      output: output,
      command: [
        sh, '-c', '@0@ < @INPUT@ > @1@'.format(scdoc_prog.path(), output)
      ],
      install: true,
      install_dir: '@0@/man@1@'.format(mandir, section)
    )
  endforeach
endif

cageng_sources = [
  'desktop/output.c',
  'desktop/renderer.c',
  'desktop/util.c',
  'desktop/view.c',
  'desktop/xdg_shell.c',
  'input/cursor.c',
  'input/keyboard_group.c',
  'input/pointer.c',
  'input/seat.c',
  'cageng.c',
]

cageng_headers = [
  configure_file(input: 'config.h.in',
		 output: 'config.h',
		 configuration: conf_data),
  'desktop/output.h',
  'desktop/renderer.h',
  'desktop/util.h',
  'desktop/view.h',
  'desktop/xdg_shell.h',
  'input/cursor.h',
  'input/keyboard_group.h',
  'input/pointer.h',
  'input/seat.h',
  'serverng.h',
]

# if conf_data.get('CAGE_HAS_XWAYLAND', 0) == 1
#   cage_sources += 'xwayland.c'
#   cage_headers += 'xwayland.h'
# endif

executable(
  meson.project_name() + 'ng',
  cageng_sources + cageng_headers,
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
  '    debug: @0@'.format(get_option('buildtype').startswith('debug')),
  '    debug damage tracking: @0@'.format(get_option('debug-damage-tracking')),
	'    xwayland: @0@'.format(have_xwayland),
	''
]
message('\n'.join(summary))
