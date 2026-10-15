# osu!vinniv — known gaps & future work

## Per-map hitsounds (parser + runtime)
Currently the client uses a fixed soft-bank hitsound palette:
files/hit.wav, clap.wav, whistle.wav, finish.wav, miss.wav.
Every map sounds the same regardless of what the author intended.

Real behavior: each hit object carries
  field 4 = hitSound bitfield (1=normal, 2=whistle, 4=finish, 8=clap)
  field 6 = hitSample descriptor "bank:name:suffix:volume:filename"
The map's SampleSet (from [General] or [TimingPoints]) picks which bank
(normal/soft/drum) to source from.

Correct fix:
1. Parser: also read field 6 to get the sample bank/suffix.
2. At import: copy the .osz's hitsound files (soft-hitnormal.wav etc.)
   into the song folder; they're already extracted by miniz.
3. Runtime: play_sound picks per-note by combining bank + name.
4. Fallback: if a specific bank file is missing, fall back to the
   client default (current behaviour).

Without this, hitsounds on any map that uses a custom bank will
sound wrong. Most ranked maps use the default soft/normal banks, so
impact is moderate until we support custom banks.

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

## Asset types
- **Map thumbnail** — small crop, for song select row preview
- **Background image** — full bg.jpg / bg.png shown behind gameplay and selected song
- These are separate assets and need separate load paths. bg.jpg lives
  in the set folder; thumbnails may not exist in .osz at all (BeatmapExporter
  can generate them but they're not shipped in every pack).

## Star rating sort
- Native mania maps: sort by computed mania star rating.
- Converted std maps: star rating is computed from the std difficulty
  values, not the mania conversion. So sorting converts by mania stars
  would misrepresent them.
- Policy: when the std->mania converter ships, use the original std
  star rating for sort order on converted entries. Mark them with a
  "(Convert)" tag in song select.

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

## Notes
- **FPS cap change requires restart** for vsync to toggle. In-app change
  updates the software cap but leaves the SDL vsync state alone, because
  `SDL_SetRenderVSync(renderer, 1)` can block on Android.

## Known bugs
- None currently open

## Std → Mania conversion (planned feature)
**Status:** std maps stay on disk. Song select filters them out with
`if (m.mode != 3) return false;` in `song_select.cpp`. That guard is
NON-DESTRUCTIVE and is what we remove when the converter lands.

Reference: ppy/osu ManiaBeatmapConverter.cs + Beatmaps/Patterns/Legacy/*
- Key mods 4K–9K are ranked, 1K–3K and 10K are unranked.
- Score multiplier: 0.90x (changed from 1.0x in Aug 2025, PR #30506).
- Key mods only apply to converts, not native mania maps.
- Converter is seeded from (DrainRate + CircleSize) * 20 +
  (int)(OD * 41.2) + round(AR) — deterministic output.
- Density computed over last 7 notes; drives column distribution.
- Port targets: SliderPatternGenerator.cs, SpinnerPatternGenerator.cs,
  and the base circle handler in the same folder.
- Converted maps display 0.90x multiplier and use NK column count.
