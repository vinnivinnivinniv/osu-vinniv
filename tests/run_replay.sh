#!/bin/bash
# Hands-free rendered autoplay with synthetic touches to keep the OS at 120Hz.
# Android (Vivo/ColorOS in particular) drops to 60Hz after ~5s without touch.
set -e

if [ -z "$1" ]; then
    echo "usage: $0 /sdcard/.../some.osu"
    exit 2
fi

MAP_PATH="$1"
TRIGGER="/sdcard/Android/data/sh.ppy.osuvinniv/files/test_replay.txt"

echo "Map: $MAP_PATH"
printf '%s\n' "$MAP_PATH" > /tmp/replay_trigger.txt
adb push /tmp/replay_trigger.txt "$TRIGGER" > /dev/null

adb shell am force-stop sh.ppy.osuvinniv
adb logcat -c
(adb logcat -v threadtime > /tmp/replay.log &)
sleep 2
adb shell am start -n sh.ppy.osuvinniv/org.libsdl.app.SDLActivity

# Keepalive touches every 3s in the corner (won't hit any gameplay column
# at 4K layout since playfield is centered). Runs in background.
(
    while true; do
        sleep 3
        adb shell input tap 20 20 > /dev/null 2>&1
        if ! grep -q "REPLAY_RESULT:" /tmp/replay.log 2>/dev/null; then
            :    # keep going
        else
            break
        fi
    done
) &
TOUCH_PID=$!

# Poll for result
for i in $(seq 1 60); do
    if grep -q "REPLAY_RESULT:" /tmp/replay.log 2>/dev/null; then
        echo "  result captured (poll $i)"
        break
    fi
    printf "  poll %d/60 ...\n" "$i"
    sleep 5
done
kill $TOUCH_PID 2>/dev/null || true
pkill -f "adb logcat" 2>/dev/null

echo ""
echo "=== REPLAY_RESULT ==="
grep "REPLAY_RESULT:" /tmp/replay.log | tail -1 || { echo "no result"; exit 1; }

echo ""
echo "=== Frame time (last 15s of rendered playback) ==="
grep "FPS\[" /tmp/replay.log | tail -15
