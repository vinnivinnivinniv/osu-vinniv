# Gameplay mechanics — faithfulness audit

Source of truth: ppy/osu reference at `~/Work/reference/osu`.

## Verified against reference
- Hit window anchors (OD0/5/10): 22.4/19.4/13.9, 64/49/34, 97/82/67,
  127/112/97, 151/136/121, 188/173/158
- Piecewise DifficultyRange interpolation at d=5 (verified in od_window)
- Base score per tier: 305 / 300 / 200 / 100 / 50
- Score formula: 150000*combo + 850000*acc^(2+2acc)
- Note selection: earliest-unjudged, no skip-forward on expired note
- Note lock: no skip past a note whose start has passed
- Miss sweep fires at note.time + miss_window
- SV-aware head + tail y-position
- Hold head judged on press; tier from |press - head.time|
- Hold tail judged on completion/release; 1.5x release-window lenience

## Suspected deviations
- Head miss + held-through: lazer force-misses the tail; we drop it silently
- Body break tier: we always give Meh on early release with clean head;
  lazer's tier depends on whether the tail was considered "hit" at break time
- Combo counting on tail: verified we now increment; not yet verified against
  a real lazer run on the same map
- Judgement popup timing and hit-light visuals not yet compared

## Not implemented (accepted gaps)
- HP drain and fail state
- Mods (DT/HT/HR/EZ/etc.)
- Score persistence
- Per-difficulty rank display in song select

## Test protocol
Two maps prove most regressions:
1. `test-import/Sasaki Sayaka...` — 175 BPM single-BPM, hold-heavy
2. `Synthetic SevenKey/[7K] Test.osu` — SV ladder, small, fast to reset

For each: play in our client at scroll speed X. Play in lazer at scroll speed
X. Compare final combo count and score. Any divergence > 1% is a real
mechanics bug. Log the inputs, don't eyeball.
