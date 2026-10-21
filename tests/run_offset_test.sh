#!/bin/bash
# Deterministic test for suggested_offset_ms formula.
# Uses am start -S (force-stop + start in one call) plus polling for the
# result line, to eliminate the process-death race and cold-start variance.

set -e

if [ -z "$1" ]; then
    echo "usage: $0 /sdcard/.../some.osu"
    exit 2
fi

MAP_PATH="$1"
TRIGGER="/sdcard/Android/data/sh.ppy.osuvinniv/files/test_autoplay.txt"

run_case() {
    local bias=$1
    local offset=$2
    printf '%s\nbias_ms=%d\noffset_ms=%d\n' "$MAP_PATH" "$bias" "$offset" \
        > /tmp/offset_trigger.txt
    adb push /tmp/offset_trigger.txt "$TRIGGER" > /dev/null

    # -S: force-stop the running instance before starting. Avoids the
    # race where am start just foregrounds a still-alive process that
    # never re-reads the trigger file.
    adb shell am start -S -n sh.ppy.osuvinniv/org.libsdl.app.SDLActivity > /dev/null

    # Poll for the result line. 15 x 0.5s = 7.5s cap, plenty for
    # 320-note headless autoplay (runs in ~50ms once map is parsed).
    local line=""
    for i in $(seq 1 15); do
        line=$(adb logcat -d 2>/dev/null | grep "OFFSET_FORMULA:" | tail -1)
        if [ -n "$line" ]; then break; fi
        sleep 0.5
    done

    if [ -z "$line" ]; then
        echo "  bias=$bias offset=$offset → no result within 7.5s"
        return 1
    fi
    echo "  $line"
    echo "$line" | grep -q "PASS" || return 1
    return 0
}

# Clear buffer once at start so stray lines from prior sessions don't
# contaminate the first case.
adb logcat -c

echo "Running offset formula test cases..."
PASS=0; FAIL=0

run_case 0 0       && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
run_case 20 0      && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
run_case 20 -20    && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
run_case -15 0     && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
# Extra: full compensation — bias and offset cancel, suggested = -bias
run_case 30 -30    && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

echo ""
echo "OFFSET TEST: $PASS pass, $FAIL fail"
[ "$FAIL" -eq 0 ]
