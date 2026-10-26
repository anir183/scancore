#!/bin/sh
# tools/shot.sh <out.png> <width> <height> '<settings json>' [section] [extra start json]
# A screenshot of the app on a throwaway profile (tools/probes/Shot.qml),
# drawn with OpenGL off screen at exactly that size. LOOK_AS=android for the
# phone layout. Example:
#   tools/shot.sh /tmp/home.png 360 800 '{"uiMode":"retro","glass":true}' home '"wrappedSeason":2026'
set -e
repo=$(cd "$(dirname "$0")/.." && pwd)
app=${SCANCORE_APP:-$repo/build/prof/bin/scancore}
out=$1; w=$2; h=$3; settings=${4:-'{}'}; go=${5:-home}; extra=${6:+,$6}
work=$(mktemp -d "${TMPDIR:-/tmp}/scancore-shot.XXXXXX")
trap 'rm -rf "$work"' EXIT
[ -n "$LOOK_AS" ] && export SCANCORE_LOOK_AS=$LOOK_AS
XDG_DATA_HOME=$work/data XDG_CACHE_HOME=$work/cache XDG_CONFIG_HOME=$work/config \
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=rhi MANGOHUD=0 QT_FORCE_STDERR_LOGGING=1 \
SCANCORE_START="{\"probe\":\"file://$repo/tools/probes/Shot.qml\",\"size\":[$w,$h],\"settings\":$settings,\"go\":\"$go\",\"out\":\"$out\"$extra}" \
timeout 90 "$app" > "$work/log" 2>&1 || { tail -20 "$work/log"; exit 1; }
