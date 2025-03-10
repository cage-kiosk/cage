cage(1)

# NAME

cage - a Wayland kiosk compositor

# SYNOPSIS

*cage* [options...] [--] [_application_...]

# DESCRIPTION

Cage runs a single, maximized application. Cage can run multiple applications,
but only a single one is visible at any point in time. User interaction and
activities outside the scope of the running application are prevented.

# OPTIONS

*-d*
	Don't draw client side decorations when possible.

*-D*
	Enable debug logging.

*-h*
	Show the help message.

*-m* <mode>
	Set the multi-monitor behavior. Supported modes are:
	*last* Cage uses only the last connected monitor.
	*extend* Cage extends the display across all connected monitors.

*-s*
	Allow VT switching

*-n*
	Enable numlock

*-v*
	Show the version number and exit.

# ENVIRONMENT

_DISPLAY_
	If compiled with Xwayland support, this will be set to the name of the
	X display used for Xwayland. Otherwise, probe the X11 backend.

_WAYLAND_DISPLAY_
	Specifies the name of the Wayland display that Cage is running on.

_XCURSOR_PATH_
	Directory where cursors are located.

_XCURSOR_SIZE_
	Specifies the configured cursor size.

_XCURSOR_THEME_
	Specifies the configured cursor theme.

_XKB_DEFAULT_RULES_, _XKB_DEFAULT_MODEL_, _XKB_DEFAULT_LAYOUT_,
_XKB_DEFAULT_VARIANT_, _XKB_DEFAULT_OPTIONS_
	Configures the xkb keyboard settings. See *xkeyboard-config*(7).

# SEE ALSO

*xkeyboard-config(7)*

# BUGS

Report bugs at https://github.com/cage-kiosk/cage

# AUTHORS

Jente Hidskes <dev@hjdskes.nl>
