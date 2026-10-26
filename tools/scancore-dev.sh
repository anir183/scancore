#!/bin/sh
# Starts SCANCORE from this checkout, built first when the code changed
# (~/.local/bin/scancore points here). Nothing changed: it starts at once.
# A build that fails starts the last one that worked, with a notice.
# Already running: the arguments go to that window (SingleInstance), so no
# build is started under it.
repo=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
build=${SCANCORE_BUILD_DIR:-$repo/build/release}
app=$build/bin/scancore
log=${XDG_CACHE_HOME:-$HOME/.cache}/scancore-dev-build.log

say() {
    command -v notify-send > /dev/null 2>&1 && notify-send -a SCANCORE -i com.mediumdev.scancore "SCANCORE" "$1"
    echo "scancore: $1" >&2
}

# The app's own command line starts with its path (this script's is "sh …").
if ! pgrep -f "^$app( |\$)" > /dev/null 2>&1 && [ -f "$build/build.ninja" ]; then
    # A dry run says whether anything needs building.
    if [ -n "$(ninja -C "$build" -n scancore 2>/dev/null | grep -v '^ninja: no work to do')" ]; then
        say "Building the latest changes…"
        if nice -n 19 ninja -C "$build" -j4 scancore > "$log" 2>&1; then
            say "Built. Starting."
        elif [ -x "$app" ]; then
            say "The build failed (see $log); starting the last build."
        else
            say "The build failed (see $log)."
            exit 1
        fi
    fi
fi
exec "$app" "$@"
