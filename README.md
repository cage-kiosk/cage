# Cage: a Wayland kiosk

This is Cage, a Wayland kiosk. A kiosk runs a single, maximized
application.

User input such as moving, resizing, minimizing and unmaximizing
windows is ignored. Dialogs are supported, although they too cannot be
resized nor moved. Instead, dialogs are simply centered on the
screen. There is no configuration for Cage.  When the application is
closed, Cage closes as well.

Cage supports a single, static output. It does not support hotplugging
nor rotation. Input-wise, Cage supports pointer input, keyboard input
and (soon) touch input. Copy and paste works as well.

Cage does not support Xwayland, nor any protocols other than
xdg-shell.  That is, there is no support for panels, virtual
keyboards, screen capture, primary selection, etc.  Open a PR if you
want to see support for some of these; they can likely be added
without much work. Cage fulfills my needs in its current state.

Notable omissions from Cage, to be added in a future version:
- Damage tracking, which tracks which parts of the screen are changing
  and minimizes redraws accordingly.
- HiDPI support.

## Building and running Cage

You can build Cage by simply running `make`. It requires wayland,
wlroots and xkbcommon to be installed.

You can run Cage by running `./cage APPLICATION`. If you run it from
within an existing X11 or Wayland session, wlroots will open a virtual
output as a window in your existing session. If you run it at a TTY,
it'll run with the KMS+DRM backend. In debug mode (`make debug`),
press Alt+Esc to quit.

Cage is based on the annotated source of
[TinyWL](https://gist.github.com/ddevault/ae4d1cdcca97ffeb2c35f0878d75dc17).

## Bugs

For any bug, please [create an
issue](https://github.com/Hjdskes/cage/issues/new) on
[GitHub](https://github.com/Hjdskes/cage).

## License

Please see
[LICENSE](https://github.com/Hjdskes/cage/blob/master/LICENSE) on
[GitHub](https://github.com/Hjdskes/cage).

Copyright © 2018 Jente Hidskes <hjdskes@gmail.com>
