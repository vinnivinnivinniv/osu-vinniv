#!/bin/bash
set -e
TRIGGER="/sdcard/Android/data/sh.ppy.osuvinniv/files/test_video_bench.txt"
adb shell "touch $TRIGGER"
adb shell am force-stop sh.ppy.osuvinniv
adb logcat -c
(adb logcat -v threadtime > /tmp/vbench.log &)
sleep 2
adb shell am start -n sh.ppy.osuvinniv/org.libsdl.app.SDLActivity
sleep 20
pkill -f "adb logcat" 2>/dev/null
grep "VBENCH_RESULT:" /tmp/vbench.log | tail -1 || { echo "FAIL: no result"; exit 1; }
