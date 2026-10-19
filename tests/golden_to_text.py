#!/usr/bin/env python3
"""Convert lazer -expected-conversion.json to text format.
Exits 3 on unusable JSON so run.sh can skip that map cleanly.
"""

import json
import sys


def load_source_types(osu_path):
    types = []
    in_hit = False
    with open(osu_path, encoding="utf-8", errors="ignore") as f:
        for raw in f:
            line = raw.rstrip()
            if line == "[HitObjects]":
                in_hit = True
                continue
            if in_hit and line.startswith("["):
                break
            if not in_hit or not line or line.startswith("//"):
                continue
            parts = line.split(",")
            if len(parts) < 4:
                continue
            try:
                t = int(parts[3])
            except ValueError:
                continue
            types.append(t & 0x0B)
    return types


def main(json_path, osu_path):
    try:
        with open(json_path) as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        print(f"SKIP: invalid JSON: {e}", file=sys.stderr)
        return 3

    mappings = data.get("Mappings")
    if mappings is None:
        print("SKIP: no 'Mappings' key (sample-conversion golden?)", file=sys.stderr)
        return 3

    source_types = load_source_types(osu_path)

    for i, m in enumerate(mappings):
        st = float(m.get("StartTime", 0.0))
        stype = source_types[i] if i < len(source_types) else 0
        w = int(m.get("RandomW", 0))
        x = int(m.get("RandomX", 0))
        y = int(m.get("RandomY", 0))
        z = int(m.get("RandomZ", 0))
        objs = m.get("Objects", [])
        notes = ";".join(
            f"{float(o['StartTime']):.3f}:{int(o['Column'])}:{float(o['EndTime']):.3f}"
            for o in objs
        )
        print(f"{st:.3f}|{stype}|{w}|{x}|{y}|{z}|{notes}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <expected.json> <source.osu>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
