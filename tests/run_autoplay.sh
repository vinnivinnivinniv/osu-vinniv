#!/bin/bash
# Runs the on-device autoplay test for one .osu map, asserts PASS.
# Usage: ./run_autoplay.sh "/sdcard/.../some.osu"
set -e

if [ -z "$1" ]; then
    echo "usage: $0 /sdcard/.../some.osu"
    exit 2
fi

MAP_PATH="$1"
TRIGGER="/sdcard/Android/data/sh.ppy.osuvinniv/files/test_autoplay.txt"

echo "Map: $MAP_PATH"

# Write trigger file LOCALLY then push. Avoids all shell quoting issues
# with apostrophes, spaces, brackets in the map path.
printf '%s\n' "$MAP_PATH" > /tmp/autoplay_trigger.txt
adb push /tmp/autoplay_trigger.txt "$TRIGGER" > /dev/null

# Restart app and capture
adb shell am force-stop sh.ppy.osuvinniv
adb logcat -c
(adb logcat -v threadtime > /tmp/autoplay.log &)
sleep 2
adb shell am start -n sh.ppy.osuvinniv/org.libsdl.app.SDLActivity
sleep 8
pkill -f "adb logcat" 2>/dev/null

# Grep result
RESULT=$(grep "TEST_RESULT:" /tmp/autoplay.log | tail -1)
if [ -z "$RESULT" ]; then
    echo "FAIL: no TEST_RESULT line — check /tmp/autoplay.log"
    echo "--- last 20 SDL lines ---"
    grep "SDL/APP" /tmp/autoplay.log | tail -20
    exit 1
fi

echo "$RESULT"
echo "$RESULT" | grep -q "PASS" && exit 0 || exit 1
