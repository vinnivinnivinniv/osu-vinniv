# osu!vinniv — known gaps & future work

## Build & test

```
Build (Java + C++):   ./gradlew assembleDebug
Clean native rebuild: ./gradlew clean :app:assembleDebug
Native only:          ./gradlew :app:externalNativeBuildDebug
Release (for perf):   ./gradlew assembleRelease
Install:              adb install -r app/build/outputs/apk/debug/app-debug.apk
Java SDK:             JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64  (JDK 21 breaks the build)
```

**Build succeeded != new code on device.** `assembleDebug` writes an APK
on the host; the phone keeps whatever was installed last. Always
`adb install -r` between build and test. Confirm the .so is newer than
the source you edited:

```
ls -la --time-style=full-iso app/jni/src/main.cpp \
    app/build/intermediates/cxx/Debug/*/obj/arm64-v8a/libmain.so
```

**Logcat capture pattern** (buffer rolls on fast device):

```
adb shell am force-stop sh.ppy.osuvinniv
adb logcat -c
(adb logcat > /tmp/startup.log &)
sleep 1
adb shell am start -n sh.ppy.osuvinniv/org.libsdl.app.SDLActivity
sleep 5
pkill -f 'adb logcat' 2>/dev/null
grep 'SDL/APP' /tmp/startup.log
```

**Refresh-rate cap on the V29e:** drops to 60 Hz after ~2 min idle. Force with
`adb shell settings put system peak_refresh_rate 120` and
`adb shell settings put system min_refresh_rate 120`.

## Per-map hitsounds (parser + runtime)

**Status: IMPLEMENTED.** `map_sample_bank.cpp` resolves per-map hitsounds
from the song folder; `bundled_sounds.h` carries Argon defaults as OGG C
arrays written on first launch. Remaining gap: custom bank files are not
copied from `.osz` when the exporter renamed them.

The rest of this section is the original problem statement, kept for context.

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

## Song preview (implemented d90687b)

Selection-change preview streams via a dedicated worker thread. Main
thread never blocks on decode. Architecture:

- `audio_decode.{h,cpp}` - streaming open/seek/read over dr_mp3 and
  stb_vorbis; WAV is one-shot (files are small). Interface:
  `audio_decoder_open/seek/read/close`.
- `preview_player.{h,cpp}` - `PreviewPlayer` owns a `std::thread`, an
  `std::atomic<uint64_t>` generation counter, and a condvar. Main thread
  only calls `request(path, preview_ms, gain)` / `stop()` / `set_gain()`;
  it never touches the SDL_AudioStream directly.
- Worker loop: wait for generation bump -> open decoder -> seek to
  `preview_ms` (or 40% of track if `preview_ms < 0`, matching lazer's
  `WorkingBeatmap.PrepareTrackForPreview`) -> stream 2048-frame chunks,
  keeping ~1.5 s queued via `SDL_GetAudioStreamQueued` -> on EOF seek
  back to `preview_frame` and continue -> on generation change, tear
  down and pick up the newest request.
- Cancellation: fast-scrolling 20 songs in 2 s just bumps the counter
  20 times. The worker discards in-flight work each time it re-checks.
  No thread churn, no stale audio.

**Why not whole-file decode (the previous approach):**
`stb_vorbis_decode_filename` on a 200 s OGG took **1434 ms** on a
Snapdragon 695, synchronously on the main thread. Every selection change
hitched for over a second. Lazer uses BASS, which streams; this is the
small equivalent.

`load_audio_to_pcm` in `audio_load.cpp` **stays** for gameplay. That path
still needs the whole buffer because `now_ms` is derived from
`SDL_GetAudioStreamQueued`, which assumes everything is queued up-front.
Streaming gameplay audio is a separate, larger change (the clock model
would need to become position-based, like lazer's
`MasterGameplayClockContainer`).

**Follow-ups worth doing later:**
- Fade-in on preview start (lazer's `DuckMomentarily`, ~800 ms restore).
  Currently preview slams on.
- Bg image pre-decode. `stbi_load` on 2560x1440 JPEG is 40-80 ms
  synchronous on the same selection-change path. Once the audio stall
  was removed, this becomes the visible hitch.
- Backpressure sleep is 20 ms. If underruns appear on a loaded device,
  drop to 10 ms.

## High priority
- **Variable BPM / SV support.** See "Mapper-set scroll speed" above.
- **Per-K key mapping.** See "Settings: per-K key mapping" below.
  Unblocks 6K/7K keyboard. Defaults: `S D F Space J K L` for 7K.

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

## Lessons learned (read before your first patch)

- **Build succeeded != installed.** `assembleDebug` writes an APK on the
  host. The device keeps whatever was installed last. Always
  `adb install -r` between build and test. This bit twice in one session.
- **Never commit a patch you haven't built.** Brace imbalances and
  declaration-order issues pass a text-diff review and fail the compiler
  only when CMake actually reruns.
- **Marker-anchored edits, not line indices.** Every off-by-one bug today
  came from reasoning about line numbers from a printed snippet. Use
  `src.count(marker) == N` assertions. The file shifts between patches;
  markers don't.
- **Verify every patch with grep on full context, not just the marker.**
  The result-trigger fix shipped with its opening brace missing because
  the post-patch `sed` dump showed only one line. Print the whole block.
- **The `if (marker in src)` guard pattern is a trap.** If the marker
  appears anywhere (including in a comment) the branch silently no-ops.
  Use `src.count(marker) == expected_count`.
- **Declare state variables at the top of `main()`**, before any lambda
  captures them. `cfg_*`, `map_bank`, `last_miss_sound_tick`, and
  `selected_index` all caused "undeclared identifier" errors when
  placed later.

## Notes
- **FPS cap change requires restart** for vsync to toggle. In-app change
  updates the software cap but leaves the SDL vsync state alone, because
  `SDL_SetRenderVSync(renderer, 1)` can block on Android.

## Known bugs
- None currently open.

## Recently fixed
- **Result-screen trigger** (f6ba3a7). Was firing on
  `notes.back().end_time` (last *head*, not last *tail*) and was
  missing `!paused` / `!resuming` guards. Now uses `last_note_end`
  recomputed on map load and gates on `app_state == AppState::Playing`.
- **Song preview streaming** (d90687b). Removed the 1434 ms main-thread
  OGG decode that hitched every selection change.
- **Cache v3** (f6ba3a7). `SongEntry.preview_time` added; readers reject
  `v2` files so a stale index can't silently lose the field.

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
