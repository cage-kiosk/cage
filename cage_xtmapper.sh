#!/bin/bash
if [ "$(id -u)" != "0" ]; then
	echo "This script requires root access to access waydroid shell."
	exit 1
fi

if [ $# -eq 0 ]; then
	echo "User not specified."
	exit 1
fi

while [ $# -gt 0 ]; do
    case "$1" in
    --user)
        shift
        user="$1"
        ;;
    --window-width)
        shift
        XTMAPPER_WIDTH="$1"
        ;;
    --window-height)
        shift
        XTMAPPER_HEIGHT="$1"
        ;;
    --window-no-title-bar)
        shift
        export WLR_NO_DECORATION=1
        ;;
    *)
	echo "Invalid argument"
        exit 1
	;;
    esac
    shift
done

export XTMAPPER_WIDTH=${XTMAPPER_WIDTH:-1280}
export XTMAPPER_HEIGHT=${XTMAPPER_HEIGHT:-720}

waydroid container stop
systemctl restart waydroid-container.service

su "$user" --command "./build/cage waydroid show-full-ui" | (
	while [[ -z $(waydroid shell getprop sys.boot_completed) ]]; do
		sleep 1;
	done;
	waydroid shell -- sh -c 'test -d /data/media/0/Android/data/xtr.keymapper/files/xtMapper.sh || mkdir -p /data/media/0/Android/data/xtr.keymapper/files/'
	echo 'exec /system/bin/app_process -Djava.library.path=$(echo /data/app/*/xtr.keymapper*/lib/x86_64) -Djava.class.path=$(echo /data/app/*/xtr.keymapper*/base.apk) / xtr.keymapper.server.RemoteServiceShell "$@"' |\
 waydroid shell -- sh -c 'test -f /data/media/0/Android/data/xtr.keymapper/files/xtMapper.sh || exec cat > /data/media/0/Android/data/xtr.keymapper/files/xtMapper.sh'
	exec waydroid shell -- sh /data/media/0/Android/data/xtr.keymapper/files/xtMapper.sh --wayland-client
)
