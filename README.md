# Cage: a Wayland kiosk [![builds.sr.ht status](https://builds.sr.ht/~hjdskes.svg)](https://builds.sr.ht/~hjdskes?)

<img src="https://www.hjdskes.nl/img/projects/cage/cage.svg" alt="Cage's logo" width="150px" align="right">

This is Cage, a Wayland kiosk. A kiosk runs a single, maximized
application.

This README is only relevant for development resources and instructions. For a
description of Cage and installation instructions for end-users, please see
[its project page](https://www.hjdskes.nl/projects/cage) and [the
Wiki](https://github.com/Hjdskes/cage/wiki/).

## Release signatures

Releases are signed with
[6EBC43B1](http://keys.gnupg.net/pks/lookup?op=vindex&fingerprint=on&search=0x37C445296EBC43B1)
and published on [GitHub](https://github.com/Hjdskes/cage/releases).

## Building and running Cage

You can build Cage with the [meson](https://mesonbuild.com/) build system. It
requires wayland, wlroots and xkbcommon to be installed. Note that Cage is
developed against the latest tag of wlroots, in order to not constantly chase
breaking changes as soon as they occur.

Simply execute the following steps to build Cage:

```
$ meson build
$ ninja -C build
```

By default, this builds a debug build. To build a release build, use `meson
build --buildtype=release`.

Cage comes with compile-time support for XWayland. To enable this,
first make sure that your version of wlroots is compiled with this
option. Then, add `-Dxwayland=true` to the `meson` command above. Note
that you'll need to have the XWayland binary installed on your system
for this to work.

You can run Cage by running `./build/cage APPLICATION`. If you run it from
within an existing X11 or Wayland session, it will open in a virtual output as
a window in your existing session. If you run it at a TTY, it'll run with the
KMS+DRM backend. In debug mode (default build type with Meson), press
<kbd>Alt</kbd>+<kbd>Esc</kbd> to quit. For more configuration options, see
[Configuration](https://github.com/Hjdskes/cage/wiki/Configuration).

Cage is based on the annotated source of tinywl and rootston.

## Bugs

For any bug, please [create an
issue](https://github.com/Hjdskes/cage/issues/new) on
[GitHub](https://github.com/Hjdskes/cage).

## License

Please see
[LICENSE](https://github.com/Hjdskes/cage/blob/master/LICENSE) on
[GitHub](https://github.com/Hjdskes/cage).

Copyright © 2018-2020 Jente Hidskes <dev@hjdskes.nl>
