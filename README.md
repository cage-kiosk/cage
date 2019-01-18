# Cage: a Wayland kiosk

This is Cage, a Wayland kiosk. A kiosk runs a single, maximized
application.

This README is only relevant for development resources and
instructions. For a description of Cage and installation instructions
for end-users, please see [its project
page](https://hjdskes.nl/projects/cage).

## Building and running Cage

You can build Cage with the [meson](https://mesonbuild.com/) build
system. It requires wayland, wlroots and xkbcommon to be
installed. Simply execute the following steps to build Cage:

```
$ meson build
$ ninja -C build
```

Cage comes with compile-time support for XWayland. To enable this,
first make sure that your version of wlroots is compiled with this
option. Then, add `-Dxwayland=true` to the `meson` command above. Note
that you'll need to have the XWayland binary installed on your system
for this to work.

You can run Cage by running `./build/cage APPLICATION`. If you run it
from within an existing X11 or Wayland session, it will open in a
virtual output as a window in your existing session. If you run it at
a TTY, it'll run with the KMS+DRM backend. In debug mode (default
build type with Meson), press Alt+Esc to quit. To build a release
build, use `meson build --buildtype=release`.

Cage is based on the annotated source of
[TinyWL](https://gist.github.com/ddevault/ae4d1cdcca97ffeb2c35f0878d75dc17) and rootston.

## Bugs

For any bug, please [create an
issue](https://github.com/Hjdskes/cage/issues/new) on
[GitHub](https://github.com/Hjdskes/cage).

## License

Please see
[LICENSE](https://github.com/Hjdskes/cage/blob/master/LICENSE) on
[GitHub](https://github.com/Hjdskes/cage).

Copyright © 2018-2019 Jente Hidskes <hjdskes@gmail.com>
