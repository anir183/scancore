#!/bin/sh
# Frame times of Retro's page turns and the welcome's steps against their
# budget (tests/app/FrameTimes.qml), on this computer or an Android device.
#
#   tools/frame-times.sh desktop [build dir]     OpenGL, off screen, 412x880
#   tools/frame-times.sh android                 a debug build already
#                                                installed (it reads
#                                                files/start.json); set
#                                                ANDROID_SERIAL first
#
# Extra SCANCORE_START fields go in FRAME_TIMES, e.g.
#   FRAME_TIMES='"scenarios":["welcome"],"repeats":9'
# Prints one FRAMES line per scenario; exits 1 when one is over budget.
set -e
repo=$(cd "$(dirname "$0")/.." && pwd)
where=${1:-desktop}
extra=${FRAME_TIMES:+,$FRAME_TIMES}

if [ "$where" = desktop ]; then
    app=${2:-$repo/build/prof}/bin/scancore
    work=$(mktemp -d "${TMPDIR:-/tmp}/scancore-frames.XXXXXX")
    trap 'rm -rf "$work"' EXIT
    XDG_DATA_HOME=$work/data XDG_CACHE_HOME=$work/cache XDG_CONFIG_HOME=$work/config \
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=rhi SCANCORE_PROFILE=1 QT_FORCE_STDERR_LOGGING=1 \
    MANGOHUD=0 DISABLE_MANGOHUD=1 \
    SCANCORE_START="{\"probe\":\"file://$repo/tests/app/FrameTimes.qml\",\"size\":[412,880]$extra}" \
    timeout 600 "$app" > "$work/log" 2>&1 && status=0 || status=$?
    grep -E "FRAMES" "$work/log" | sed 's/^qml: //'
    exit $status
fi

# Android: the probe goes into the app's files, start.json names it.
[ -n "$ANDROID_SERIAL" ] || { echo "Set ANDROID_SERIAL (the emulator, never a phone with a real library)"; exit 2; }
package=com.mediumdev.scancore
files=/data/user/0/$package/files
adb push "$repo/tests/app/FrameTimes.qml" /data/local/tmp/FrameTimes.qml > /dev/null
adb shell am force-stop $package
adb shell run-as $package cp /data/local/tmp/FrameTimes.qml $files/FrameTimes.qml
printf '{"probe":"file://%s/FrameTimes.qml"%s}' "$files" "$extra" > "${TMPDIR:-/tmp}/scancore-start.json"
adb push "${TMPDIR:-/tmp}/scancore-start.json" /data/local/tmp/start.json > /dev/null
adb shell run-as $package cp /data/local/tmp/start.json $files/start.json
adb shell setprop debug.scancore.profile 1
adb logcat -c
adb shell am start -n $package/org.qtproject.qt.android.bindings.QtActivity > /dev/null
# Until the probe has reported (or ten minutes).
for _ in $(seq 1 300); do
    sleep 2
    adb shell pidof $package > /dev/null 2>&1 || break
done
adb shell setprop debug.scancore.profile 0
adb logcat -d | grep -E "FRAMES" | sed 's/^.*FRAMES/FRAMES/'
adb logcat -d | grep -q "OVER BUDGET" && exit 1 || exit 0
