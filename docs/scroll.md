# Mania scroll model

How note y-positions are computed. Read this before touching scroll
or SV handling. The port went through three incorrect versions before
landing; the failure modes are documented at the bottom.

## The one-sentence version

Each timing point contributes a multiplier. Multipliers accumulate
into a monotonically increasing "scroll position". A note's y is the
difference between its scroll position and the current scroll position,
scaled to pixels.

## Reference (lazer)

Source of truth:
- `osu.Game/Rulesets/UI/Scrolling/Algorithms/SequentialScrollAlgorithm.cs`
- `osu.Game/Rulesets/UI/Scrolling/DrawableScrollingRuleset.cs`
- `osu.Game.Rulesets.Mania/UI/DrawableManiaRuleset.cs`
- `osu.Game/Beatmaps/Beatmap.cs::GetMostCommonBeatLength`
- `osu.Game/Rulesets/Timing/MultiplierControlPoint.cs`

### The multiplier, per timing point

MultiplierControlPoint.Multiplier = Velocity * EffectPoint.ScrollSpeed
                                     * BaseBeatLength / TimingPoint.BeatLength

For mania specifically:
- Velocity = SV multiplier. 1.0 for uninherited points; -100 / beatLength
  for inherited points (where beatLength is negative). Set from
  [TimingPoints] field 2.
- EffectPoint.ScrollSpeed = 1.0 for native mania. Non-1 only on converts.
- BaseBeatLength = Beatmap.GetMostCommonBeatLength(), NOT 1000.
  See below.
- TimingPoint.BeatLength = the currently active uninherited beat length
  (in ms). Inherited points carry through the value from the previous
  uninherited point.

### GetMostCommonBeatLength

Not "frequency mode". The real algorithm:

1. Consider only uninherited timing points.
2. Each point owns the span from its own time to the next uninherited
   point's time. The last point owns up to the last hit object time.
3. The first point is forced to start at t=0, not at its declared time
   (osu-stable compat, affects short maps disproportionately).
4. Group by beatLength (rounded to 1e-3 ms to neutralize fp).
5. Sum durations per group.
6. Return the beatLength with the largest total duration.

### SequentialScrollAlgorithm

PositionMapping[i] holds the accumulated scroll position at each
timing point:

    positionMappings[0].Position = 0
    positionMappings[i].Position = positionMappings[i-1].Position
                                 + (t[i] - t[i-1]) * multiplier[i-1] / timeRange

Where timeRange = MAX_TIME_RANGE / scrollSpeed and MAX_TIME_RANGE = 11485.

relativePositionAt(t):
- Find the last timing point with time <= t (binary search).
- Return mapping.Position + (t - mapping.Time) * multiplier / timeRange.

PositionAt(noteTime, now, timeRange, scrollLength):
- Return (relativePositionAt(noteTime) - relativePositionAt(now)) * scrollLength.

Where scrollLength is the pixel height of the scrolling area
(judgement line y-position, in our port).

## Our port

Files:
- parser.h, parser.cpp -- reads [TimingPoints], computes per-point
  multiplier and per-map base_beat_length.
- main.cpp -- rebuild_timing_positions() and raw_position_at()
  lambdas, called from the note draw loop.

### Parser output

Beatmap::timing_points is a time-sorted vector<TimingPoint>. Each
TimingPoint carries:
- time -- the point's declared time in ms
- beat_length -- the active uninherited beat length at this time
- uninherited -- true if the source line had a positive beatLength
- multiplier -- velocity * base_beat_length / beat_length

Beatmap::base_beat_length is computed after the vector is built,
using lazer's GetMostCommonBeatLength algorithm above.

### Runtime

rebuild_timing_positions() runs once per map load and computes a
raw_positions vector (unnormalized, i.e. not divided by timeRange --
we factor that out to the y-formula):

    raw_positions[0] = 0
    raw_positions[i] = raw_positions[i-1] + (t[i] - t[i-1]) * multiplier[i-1]

raw_position_at(t) binary-searches for the active point and returns:

    raw_positions[i] + (t - t[i]) * multiplier[i]

The note draw loop:

    double time_range = 11485.0 / (double)cfg_scroll_speed;
    double rp_now     = raw_position_at(now_ms);
    // ...
    double rp_note = raw_position_at(n.time);
    float y = judgement_y - (float)((rp_note - rp_now) * judgement_y / time_range);

Equivalent to lazer's PositionAt(noteTime, now, timeRange, judgement_y),
with scrollLength = judgement_y.

### No-timing-point maps

If [TimingPoints] is empty, raw_position_at(t) returns t (identity),
which yields the pre-SV flat scroll behavior. Real mania maps always
have at least one timing point, so this path is only for defense.

## What's approximate

1. Multiplier recomputation, not velocity tracking. The parser stores
   multiplier directly. To change the base, it recovers
   velocity = multiplier * beat_length / 1000.0 and re-applies. This
   depends on the 1000.0 literal having been used in the first pass.
   If you change the parser's first-pass formula, update this recovery.

2. No EffectPoint.ScrollSpeed. Native mania always has 1.0.
   If converts ever ship, this needs to be read from the difficulty
   point.

3. BaseBeatLength * SliderMultiplier cancellation is implicit.
   Lazer does baseBeatLength /= SliderMultiplier in
   DrawableScrollingRuleset and p.BaseBeatLength *= SliderMultiplier
   in DrawableManiaRuleset. We just don't multiply at all. If we ever
   add non-mania rulesets, revisit.

4. GetMostCommonBeatLength first-point-at-t=0. We implement it.
   If a map's first timing point is far from t=0, the first point's
   span extends backward, which affects grouping. Matches lazer.

## Test procedure

### Baseline test -- single-BPM map

Any 4K map with one [TimingPoints] line. Play it at your usual scroll
speed. The rate should match pre-SV behavior: since there's one beat
length, base_beat_length == beat_length, so multiplier == 1.0, and
y = judgement_y * (note_time - now) / time_range -- the flat formula.

If baseline is wrong, stop. Nothing else matters until this is right.

### SV ladder test -- Synthetic SevenKey/[7K] Test.osu

Current timing points on disk:

    [TimingPoints]
    1000,500,4,2,0,100,1,0    # 120 BPM, SV=1.0
    2000,-50,4,2,0,100,0,0    # SV=2.0
    3000,-200,4,2,0,100,0,0   # SV=0.5
    4000,-100,4,2,0,100,0,0   # SV=1.0

Expected on the 7K map:
- 1.0-2.0 s: baseline
- 2.0-3.0 s: 2x faster (slope change, no position jump)
- 3.0-4.0 s: 0.5x slower
- 4.0-4.5 s: baseline again

Notes already below 2000 at t=2000 do not teleport. A note at t=1900
is at the same y just before and just after now_ms crosses 2000. Only
its rate of descent changes.

### BPM-change test -- test-import/Sasaki Sayaka...

Timing points on disk:

    660,   342.857...,  1, 0   # 175 BPM
    33574, -100,        0, 1   # SV=1.0, kiai on
    57060, -100,        0, 0   # SV=1.0, kiai off
    58602, 342.857...,  1, 1   # 175 BPM, kiai on
    64087, -100,        0, 0   # SV=1.0, kiai off

Single distinct BPM (175). So base_beat_length == 342.857,
multiplier == 1.0 at every point, and the scroll rate is flat --
indistinguishable from pre-SV. Play this map after any scroll change
as a regression check: if it feels different from before, something
in the multiplier chain changed.

## Debugging a wrong scroll rate

In order:

1. Log the parsed state. On map load:

       SDL_Log("SCROLL: base=%.3f tps=%zu",
               map.base_beat_length, map.timing_points.size());
       for (const auto& tp : map.timing_points)
           SDL_Log("  t=%.0f bl=%.3f un=%d mult=%.4f",
                   tp.time, tp.beat_length, (int)tp.uninherited, tp.multiplier);

   A single-BPM map should show mult == 1.0 everywhere. Anything else
   means the base_beat_length computation is wrong.

2. Compare to lazer. Play the same map in lazer at scroll speed X.
   Note the pixel-y of a note at a fixed time offset. Match X here.
   If they disagree, the discrepancy is multiplicative -- divide the two
   effective rates to get the ratio, then look for a factor in the
   multiplier chain (see "Reference").

3. Check the 1000.0 literal. It appears in:
   - parser.cpp: tp.multiplier = velocity * 1000.0 / beat_length (first pass)
   - parser.cpp: velocity = tp.multiplier * tp.beat_length / 1000.0 (recovery)
   If you change one, change the other.

## History

- Initial port used 1000.0 as base, matching TimingControlPoint.DEFAULT_BEAT_LENGTH.
  This is wrong for mania because RelativeScaleBeatLengths = true
  overrides base to GetMostCommonBeatLength().
- Effective rate was beat_length_reference / current_beat_length where
  reference was 60 BPM (1000 ms), making high-BPM maps ~3x too fast.
- Corrected by computing base_beat_length per map with lazer's
  duration-weighted algorithm.
