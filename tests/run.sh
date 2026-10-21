#!/bin/bash
# Host-side tests. Parser + converter golden-diff.
set -e
cd "$(dirname "$0")"

echo "Building parser tests..."
g++ -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter \
    test_parser.cpp -o test_parser

echo "Running parser tests..."
./test_parser

echo ""
echo "Building converter test..."
g++ -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter \
    test_converter.cpp -o test_converter

echo ""
echo "Running converter comparison against lazer goldens..."

# Only Mode 0 maps with a Mappings-schema golden are testable here.
# Other goldens exist but are gated on KeyMod or use a sample-conversion
# schema (different JSON shape) which we don't consume yet.
#
# Spinner verification: 20544 (2 spinners) and 100374 (5 leading spinners)
# now extend their compared prefixes, proving SpinnerPatternGenerator
# matches lazer's output. The *-samples-* goldens (e.g.
# spinner-convert-samples) use a different schema for sample-fidelity
# testing and are not usable with the Mappings consumer.
#
# KeyMod (forced-column) verification is NOT covered here. Lazer's NK.osu
# fixtures are all Mode:3 (already mania), so ManiaModKey<N> is a no-op
# against them (IsForCurrentRuleset short-circuit). Their goldens capture
# pass-through behavior, not conversion. To verify KeyMod we would need
# Mode:0 source maps with matching NK goldens generated from
# ManiaModKey<N>. Deferred until KeyMod ships as a runtime feature.
CORE_MAPS="
20544
100374
1450162
"

DATA_DIR=data/mania_conversion
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail_count=0
pass_count=0
skip_count=0

for base in $CORE_MAPS; do
    osu="$DATA_DIR/${base}.osu"
    golden="$DATA_DIR/${base}-expected-conversion.json"

    if [ ! -f "$osu" ] || [ ! -f "$golden" ]; then
        echo "SKIP: ${base} (missing input or golden)"
        skip_count=$((skip_count + 1))
        continue
    fi

    # If the filename ends in NK (1K, 4K, 7K, 10K), pass that as
    # forced_columns to simulate ManiaModKey<N>. Otherwise auto heuristic.
    cols=""
    if [[ "$base" =~ ^([0-9]+)K$ ]]; then
        cols="${BASH_REMATCH[1]}"
    fi

    local_rc=0
    if [ -n "$cols" ]; then
        ./test_converter "$osu" "$cols" > "$TMP/${base}-ours.txt" 2>"$TMP/${base}-err.txt" || local_rc=$?
    else
        ./test_converter "$osu" > "$TMP/${base}-ours.txt" 2>"$TMP/${base}-err.txt" || local_rc=$?
    fi

    if [ "$local_rc" -eq 4 ]; then
        echo "SKIP: ${base} (mania map + forced_columns)"
        skip_count=$((skip_count + 1))
        continue
    elif [ "$local_rc" -ne 0 ]; then
        echo "FAIL: ${base} (parse or convert error, rc=$local_rc)"
        sed 's/^/    /' "$TMP/${base}-err.txt" | head -3
        fail_count=$((fail_count + 1))
        continue
    fi

    if ! python3 golden_to_text.py "$golden" "$osu" > "$TMP/${base}-golden.txt" 2>"$TMP/${base}-gerr.txt"; then
        echo "SKIP: ${base} (golden unusable: $(head -1 "$TMP/${base}-gerr.txt"))"
        skip_count=$((skip_count + 1))
        continue
    fi

    echo "--- ${base} ---"
    if python3 compare_conversions.py "$TMP/${base}-ours.txt" "$TMP/${base}-golden.txt"; then
        pass_count=$((pass_count + 1))
    else
        fail_count=$((fail_count + 1))
    fi
done

echo ""
echo "Converter: ${pass_count} pass, ${fail_count} fail, ${skip_count} skip"
if [ "$fail_count" -gt 0 ]; then
    exit 1
fi
