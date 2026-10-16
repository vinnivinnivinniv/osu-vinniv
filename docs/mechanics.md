# Gameplay mechanics — faithfulness audit

Source of truth: ppy/osu reference at `~/Work/reference/osu`.

Read this before touching judgement, scoring, or hold logic. Every row
below is a claim about parity. If you change a row, update the reference
line and test protocol at the bottom.

Status legend:
- ✅  Verified against reference this session
- ⚠️  Suspected deviation, not yet proven on-device
- ❌  Known bug, fix pending
- 🟢  Accepted gap, intentional, not a bug

## Verified

| Mechanic | Reference | Notes |
|---|---|---|
| Hit window anchors (OD0/5/10) | `ManiaHitWindows.cs` | 22.4/19.4/13.9, 64/49/34, 97/82/67, 127/112/97, 151/136/121, 188/173/158 |
| `DifficultyRange` piecewise at d=5 | `IBeatmapDifficultyInfo.cs` | `od_window()` — matches |
| Base score per tier | `ManiaScoreProcessor.cs` | Perfect=305 override, then 300/200/100/50 |
| Score formula | `ManiaScoreProcessor.ComputeTotalScore` | `150000·comboP + 850000·acc^(2+2acc)·accP` |
| Note selection: earliest-unjudged | `OrderedHitPolicy` | fixed 2026-10-16 |
| Note lock — no skip past started note | `OrderedHitPolicy.IsHittable` | |
| Force-miss earlier objects on hit | `OrderedHitPolicy.HandleHit` | |
| Miss sweep at `note.time + miss_window` | `DrawableNote.CheckForResult` | |
| SV-aware head/tail y-position | `SequentialScrollAlgorithm` | fixed 2026-10-16 (see docs/scroll.md) |
| Hold head judged on press | `DrawableHoldNote.OnPressed` | |
| Hold tail judged on release/complete | `DrawableHoldNote.OnReleased` | fixed 2026-10-16 |
| Tail release lenience 1.5× | `TailNote.RELEASE_WINDOW_LENIENCE` | |
| HitSound bitfield layering | osu file format, field 4 | 1=normal, 2=whistle, 4=finish, 8=clap |
| Accuracy weights (Perfect = 305) | `ManiaScoreProcessor.GetBaseScoreForResult` | fixed 2026-10-16 (was 300) |
| SS rank requires no Good/Ok/Meh/Miss | `ManiaScoreProcessor.RankFromScore` | fixed 2026-10-16 |
| Touch→mouse synthesis disabled | `SDL_HINT_TOUCH_MOUSE_EVENTS` | fixed 2026-10-16; was double-judging per tap |
| Hold body judgement (3 per hold) | `DrawableHoldNote.CheckForResult` | fixed 2026-10-16; was 2 per hold |
| `total_notes` = taps + 3·holds | `EnumerateHitObjects` | fixed 2026-10-16 |

## Known bugs

### (none currently open)

### #19 — Miss sound cooldown
`main.cpp` throttles miss sounds to 1 per 150 ms. Lazer plays one per
miss. In dense sections we swallow legitimate feedback. Low priority —
the counter is still correct, only the audio is throttled.

### #20 — Result screen trigger is time-based, not state-based
Ours: `now_ms > last_note_end + 2000`.
Lazer: fires when all judgements resolve (miss windows expire + holds
release). Edge case: a hold whose tail extends past `last_note_end`
would fire the result before the tail judged. Rare.

### #21 — No sliding sample during hold
Lazer loops the note's `sliding` sample while `IsHolding`. We play the
head hitsound once and stop. Cosmetic — sounds thin on long holds.

## Suspected (needs playtest)

- **#22 — Tail hitsound on release.** Lazer plays the tail's sample
  when the tail is judged. Ours may already via `play_note_normal_hitsound`
  on press; not verified.
- **#23 — Body break tier.** We always give Meh on early release with
  clean head. Lazer caps via `GetCappedResult`, but the exact tier
  depends on `Tail.IsHit` at break time. Needs comparison.

## Accepted gaps (not bugs)

- HP drain and fail state — not implemented, no fail
- Mods (DT/HT/HR/EZ/…) — not implemented
- Score persistence — not implemented
- Judgement popups / hit lighting — cosmetic only
- Leaderboard / replay — not implemented

## Test protocol

Two maps prove most regressions:

1. `test-import/Sasaki Sayaka...` — 175 BPM, single timing point, hold-heavy
2. `Synthetic SevenKey/[7K] Test.osu` — 4-note SV ladder, 3.5 s, fast reset

For each: play in our client at scroll speed X. Play the same map in
lazer at scroll speed X. Compare final combo count, accuracy, rank, and
score. Divergence > 1% on any is a real mechanics bug. Log the inputs.

Sanity checks after any scoring change:
- 100% Perfect play → acc = 100.00%, rank = SS (no Good+)
- One Good, rest Perfect → rank = S, acc < 100%
- One Meh, rest Perfect → rank = S, acc noticeably < 100%
- One miss in a 100-note map → acc ≈ 99%, not ≈ 98%

## History

- 2026-10-16: initial audit. Fixed accuracy weights (300→305), SS rank
  condition, touch-to-mouse double-judge, hold tail scoring, SV-aware
  tail position, earliest-unjudged note selection. Filed bugs #17–#23.
