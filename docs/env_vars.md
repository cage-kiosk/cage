# Environment variables

Cage sets the following environment variables:

* `DISPLAY`: if compiled with Xwayland support, this will be set to the name of
  the X display used for Xwayland.
* `WAYLAND_DISPLAY`: specifies the name of the Wayland display that Cage is
  running on.

The following environment variables can be used to configure behavior of
libraries that Cage uses.  Effectively, these environment variables configure
the Cage session.

## XKB environment variables

```
XKB_DEFAULT_RULES
XKB_DEFAULT_MODEL
XKB_DEFAULT_LAYOUT
XKB_DEFAULT_VARIANT
XKB_DEFAULT_OPTIONS
```

These environment variables configure the XKB keyboard settings. See
xkeyboard-config(7).

## Wlroots environment variables

* `DISPLAY`: if set probe X11 backend.
* `WAYLAND_DISPLAY`, `_WAYLAND_DISPLAY`, `WAYLAND_SOCKET`: if set probe Wayland
  backend.
* `XCURSOR_PATH`: directory where xcursors are located.

For a complete list of wlroots environment variables, see
https://github.com/swaywm/wlroots/blob/master/docs/env_vars.md.
