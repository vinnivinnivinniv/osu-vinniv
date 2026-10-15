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

## Mapper-set scroll speed (SV / BPM)
Currently scroll_pps is constant for the whole map, from the user's
global scroll_speed setting. Real behavior:

1. **BPM changes** — the visible time range shrinks at higher BPM so
   the density looks the same across the map. Lazer implements this as
   `RelativeScaleBeatLengths = true` in DrawableManiaRuleset.
   Effect: base_beat_length / current_beat_length multiplies the
   scroll rate.

2. **SV (inherited TimingPoints, field 7 == 0)** — mapper overrides
   scroll rate per section. `MultiplierControlPoint.Velocity` is the
   SV multiplier. Applied as a multiplier on top of BPM scaling.

3. Both feed into `TimeRange.Value` per frame, which then drives the
   y-position formula.

Reference:
  osu.Game.Rulesets.Mania/UI/DrawableManiaRuleset.cs
  osu.Game/Rulesets/UI/Scrolling/Algorithms/SequentialScrollAlgorithm.cs
  osu.Game/Rulesets/Timing/MultiplierControlPoint.cs

Implementation sketch:
  - Parser: store all [TimingPoints] as
    `vector<TimingPoint>{ time, beatLength, uninherited, sv_multiplier }`
  - Runtime: for note at time T, find the active timing point.
    scroll_multiplier = (base_beat_length / active_beat_length) *
                        active_sv_multiplier
    local scroll_pps = base_scroll_pps * scroll_multiplier
  - base_scroll_pps continues to come from the user's scroll_speed
    setting (1.0-40.0 → time range formula).

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

## Song progress bar
Thin horizontal bar across the top (or bottom) of the playfield that
fills over the song length. Data: track position from audio stream vs
total audio length. Redraw per frame from a cached texture.
Reference: osu.Game/Screens/Play/SongProgress.cs

## Settings: per-K key mapping
Currently keybinds are hardcoded: D F J K for 4K, plus per-note column
input from touch. Users need to bind keys per column count (1K through
10K) and save them. Storage: settings.ini entries like
  keys_4k = D,F,J,K
  keys_7k = S,D,F,Space,J,K,L
Parsing: split on comma, store as vector<int> (SDL scancodes).
Settings screen: keyboard capture mode or text entry.
Reference: osu.Game.Rulesets.Mania/ManiaInputManager.cs,
osu.Game/Input/Bindings/*.cs

## UI reference (from real lazer, captured 2026-10-16)

### Song select — details panel (selected beatmap)
- Large bg image behind, dimmed
- Card centered: osu! logo, title, artist
- Mod icons row
- Difficulty name + star rating chip
- Source + Mapper text below
- Right sidebar: Visual Settings (Background dim / Background blur sliders,
  Storyboard/video toggle, Beatmap skins toggle, Beatmap colours toggle,
  Combo colour normalisation slider)
- Bottom: Audio Settings (Beatmap hitsounds toggle, Audio offset slider)
- Bottom-left: big pink "Back" button
- Warning banner top-left: "This beatmap contains scenes with rapidly
  flashing colours" — epilepsy warning

### Song select — listing (all difficulties)
- Search bar top-right with magnifier icon
- "N matches" count under search
- Star Rating filter: dual-handle slider 0.0 to ∞
- Toggles: "Show converts"
- Dropdowns: Sort (Title), Group (None), Collection (All beatmaps)
- Row per difficulty: [4K] Version mapped by mapper, star rating, mod icons
- Selected row highlighted with vertical accent color
- Bottom bar: Back | Mods | Random | Options buttons
- osu! logo bottom-right corner
- Settings icon top-left (grid)

### Gameplay HUD
- Score top-left in digital font, blue gradient background bar
- Progress bar integrated into score widget
- Accuracy top-right, percent format
- PP counter top-right below accuracy
- Leaderboard entry (#1 with avatar + player name) left-middle
- Left/right side: vertical difficulty bar with tick markers
- Bottom: song progress bar (thin line), elapsed time left, total time right
- X button bottom-right (pause/exit)
- MISS text centered above judgement line in red
- Hold notes: gradient body, arrow glyphs on head/tail
- Column backgrounds: colored vertical strips, one per key
- Column hit lighting: bright flash on hit

### Result screen
- Circular rank ring, colored by rank (SSH=rainbow, SS/S=green, A=blue, B=orange, C=red, D=purple)
- Rank letter centered inside ring
- Full score (large) below
- Mod icons row
- Difficulty + mapper
- 3-column stat block: ACCURACY | MAX COMBO | PP
- Row of hit judgements: PERFECT | GREAT | GOOD | OK | MEH | MISS with counts
- "Played on <date>" timestamp
- Bottom bar: Back | Save replay | Retry | More options | Favorite

### Settings screen (lazer parity)
- Sidebar: General, Skin, Input, User Interface, Gameplay, osu!, osu!taiko,
  osu!mania, Audio, Graphics, Online
- Search bar top of content area
- Volume sliders: Master, Master (window inactive), Effect, Music — all
  slider with % label
- Offset adjustment section: Audio offset slider (ms), suggested offset
  from last play, "Apply suggested offset" button

## Song list row format (lazer parity)
Reference: lazer song select difficulty listing, captured 2026-10-16.

Each difficulty row shows (left to right):
  1. Star rating chip — colored pill with ★ and value, e.g. [★ 1.84]
     Color ramps: 1.x = green, 2.x = blue-green, 3.x = teal,
     4.x = yellow, 5.x = orange, 6.x = red. Follows lazer's
     OsuColour.ForStarDifficulty() gradient.
  2. "[NK] Difficulty name mapped by mapper"
     - N = key count, K suffix. E.g. "[4K]", "[7K]".
     - Difficulty name in lighter text.
     - "mapped by <mapper>" in dimmed text after.
  3. Mod icons row on the right (circular glyphs) — only if the
     score for this difficulty was set with mods. Uses the mod's
     IconSymbol. On our client: no mods yet, so skip.
  4. RANKED badge — small colored chip, only on the set header row.

Header row (top of a set) shows:
  - Title in large white
  - Artist in smaller dimmed text
  - Ranked / Loved / Qualified / Graveyard badge

Our current row format is:
  Line 1: "Artist - Title"
  Line 2: "[Version]  NK  ODxx"

Upgrade path:
  - Add star chip (draw circle + text inside via SDL_RenderDebugText,
    or pre-render a small texture per star tier)
  - Reformat line 2 to "[4K] Version mapped by mapper"
  - Group rows by beatmap set: title/artist header once per set,
    difficulty rows indented underneath. Requires a set-aware scan
    (our `songs/` layout already groups by folder, so this is mostly
    a rendering change).

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
