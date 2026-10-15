# osu!vinniv — known gaps & future work

## High priority
- **Variable BPM / SV support.** Currently the renderer uses one
  scroll_pps for the whole map. Maps with BPM changes or SV (inherited
  timing points) scroll incorrectly. Implementation:
    1. Parser: store all [TimingPoints] as
       `struct TimingPoint { double time; double beatLength; bool uninherited; double sv; }`
    2. Render: for each note, find the active timing point at note.time,
       compute the local scroll multiplier, apply to y.
  Reference: `DrawableManiaRuleset.cs:RelativeScaleBeatLengths`,
  `SequentialScrollAlgorithm.cs` in ppy/osu.

## Medium priority
- Result → song select (currently tap = retry; only Esc goes back)
- On-screen restart button (R key works)
- Star rating display in song select (needs diffcalc — probably
  reuse a smaller difficulty calculation or skip entirely)
- Backup/shared audio: currently every song folder duplicates its
  hitsounds; could share via files/skin/default/

## Low priority
- Mods: DT / HT / HR. DT/HT change scroll_pps and audio rate
  together; needs a "rate" concept in the audio stream too.
- Score persistence: best score per (map, difficulty) in a local file
- Better fonts (currently SDL's 8x8 debug font)
- Custom hitsound banks per map (SampleSet / SampleIndex in .osu)
- Std → mania converts

## Known bugs
- None currently open
