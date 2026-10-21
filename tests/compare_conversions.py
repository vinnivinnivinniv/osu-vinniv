#!/usr/bin/env python3
"""Compare our converter output against the lazer golden.
Exit 0 = pure-circle prefix matches, 1 = divergence, 2 = argument error.
"""

import sys


def parse_records(path):
    records = []
    with open(path) as f:
        for raw in f:
            line = raw.rstrip()
            if not line:
                continue
            parts = line.split("|")
            if len(parts) < 7:
                continue
            try:
                st = float(parts[0])
                stype = int(parts[1])
                rng = (int(parts[2]), int(parts[3]), int(parts[4]), int(parts[5]))
            except ValueError:
                continue
            records.append({
                "source_time": st,
                "source_type": stype,
                "rng": rng,
                "notes": parts[6],
            })
    return records


def circle_prefix_len(records):
    """Number of leading source objects we can verify.

    Currently allows: circle (1), unknown/default (0), spinner (8).
    Sliders (2) still gate-stop — SliderPatternGenerator isn't ported
    yet, so any slider object will diverge by design.
    """
    for i, r in enumerate(records):
        if r["source_type"] not in (1, 0, 8):
            return i
    return len(records)


def main(ours_path, golden_path):
    ours = parse_records(ours_path)
    golden = parse_records(golden_path)

    if not golden:
        print("FAIL: golden is empty")
        return 1

    limit = circle_prefix_len(golden)
    if limit < len(golden):
        print(f"  (pure-circle prefix = {limit} of {len(golden)} objects)")

    for i in range(limit):
        if i >= len(ours):
            print(f"FAIL at object #{i}: ours ends prematurely")
            return 1
        a = ours[i]
        b = golden[i]
        if abs(a["source_time"] - b["source_time"]) > 2.0:
            print(f"FAIL at object #{i}: source_time "
                  f"ours={a['source_time']} golden={b['source_time']}")
            return 1
        if a["rng"] != b["rng"]:
            print(f"FAIL at object #{i} (t={b['source_time']:.0f}): RNG")
            print(f"    ours   (W,X,Y,Z) = {a['rng']}")
            print(f"    golden (W,X,Y,Z) = {b['rng']}")
            return 1
        if a["notes"] != b["notes"]:
            print(f"FAIL at object #{i} (t={b['source_time']:.0f}): notes")
            print(f"    ours   = {a['notes']}")
            print(f"    golden = {b['notes']}")
            return 1

    print(f"PASS: {limit} circle objects match")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <ours.txt> <golden.txt>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
