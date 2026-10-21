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

## This session shipped (2026-10-16 → 17)

Mechanics (see `docs/mechanics.md` for the audit table):
- SV/BPM timing-point accumulator using lazer `GetMostCommonBeatLength`
- Earliest-unjudged note selection; no skip-forward on expired note
- Hold tail scored on completion/release (2 per hold, matching lazer)
- Accuracy weights: Perfect = 305 (was 300)
- SS rank = S-tier accuracy with no Good/Ok/Meh/Miss
- Touch→mouse synthesis disabled (was double-judging per tap)
- Per-TP `sampleSet`; press plays target-note sample
- Custom sample index ≥ 2 only; addition-bank separate
- Filename override for hitnormal; bare-name fallback in resolver
- LN tail SV-aware position; head/tail cap anchors unified

Preview:
- Streaming decoder + worker thread; removes 1.4 s OGG decode hitch

Song select:
- Async scan on worker thread; determinate progress bar
- Title-first sort (both cold and cache-hit paths)
- `[NK]` badge prefix on title line (lazer parity)
- Right-half panel with row clipping; scrollbar fixed
- Random button with scroll-to-visible
- Row hitbox respects viewport bounds
- Settings button moved upper-left; works during cold scan

Audio fidelity:
- Per-map hitsound resolution: custom index, addition bank, filename
- 147 maps in the local library use mid-map bank switches; all now
  audible with the correct timbre per section

Docs:
- `docs/scroll.md` — scroll model reference
- `docs/mechanics.md` — gameplay faithfulness audit
- NOTES overhaul: build section, lessons learned, cache discipline

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

**See `docs/scroll.md` for the current implementation.** The section
below is the original design sketch, kept for context. It predates
the port and does not match what shipped; trust `docs/scroll.md`.
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

- **Search field.** Filter the song list as you type. Needs:
  - `SDL_StartTextInput` / `SDL_EVENT_TEXT_INPUT` wiring for the
    Android soft keyboard.
  - Filter state and a decision on how filtered results map to
    `songs` indices (view over `songs` vs a parallel vector).
  - A text-input widget: caret, backspace, clear.
  - Filter-aware selection, preview, and play.
  Deferred to a dedicated session; touches the whole song-select UX.

- **Video decode / poster frame.** Maps with `Video,0,"clip.mp4"` and
  no shipped image render with no background (O108ROCKET Garden set,
  Vivlos, etc.). Options:
  - Extract first frame with ffmpeg (large dependency, real work).
  - Fall back to `clip.jpg` / `clip.png` if the mapper shipped a still
    alongside (partially done in the resolver — helps but doesn't
    solve video-only maps).
  - Show a "video" placeholder with the song title.

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

## Cache version discipline

`song_cache.cpp` writes `songs.index` keyed on the top-level songs dir
mtime. Per-folder edits that don't bump that mtime aren't re-scanned.

**Rule:** whenever parser or resolver behavior changes for a field that
lands in `SongEntry`, bump the cache version (`v3` -> `v4`) in
`song_cache.cpp`. Two sites: the reader's `compare(0, 2, "vN")` and the
writer's `<< "vN "` line. Bumping forces a one-time rescan on first
launch and every stale entry gets rebuilt.

Made this mistake when adding PreviewTime (v2->v3, done right) and again
just now: parser/resolver bg fixes did NOT bump, so Murumoo kept stale
`bga.mp4` until a manual cache delete. Should have been v3 -> v4.

## Async song scan

Cold-cache startup blocks the main thread for 5-10 s while 500+ `.osu`
files are parsed for metadata. Same class as the old preview decode:
work that must happen, on the thread that draws.

Fix shape (mirrors `PreviewPlayer`):
- `scan_songs()` already returns a `std::vector<SongEntry>`; make it
  callable from a worker.
- `main()` starts the worker on first frame, shows the song select
  screen immediately with an empty list + `Scanning...` label.
- Worker posts progress (files parsed / total) via an atomic; main
  draws it.
- On completion, worker hands back the vector under a mutex, main
  swaps and stops showing the label.
- Cache-hit path stays synchronous — reading `songs.index` is ~10 ms.
  Only the rescan path needs the worker.

Edge cases to watch:
- Tap during scan — entries `songs` may be empty; hit-test must
  bail if `songs.empty()`.
- Preview/bg pointers into `songs[i]` need re-anchoring when the
  vector is replaced.
- Two rapid cold starts (user force-quits mid-scan) — worker must be
  joinable on shutdown, no detached threads.

## Song select: text masking at panel boundaries

Symptom: rows partially scrolled off the top or bottom of the song-
select panel draw their backgrounds (clipped correctly) but render
no text — because the text pass is unclipped by design.

Why the text pass is unclipped: SDL3's `SDL_RenderDebugText` interleaves
badly with `SDL_SetRenderClipRect` when render scale is also changing.
With clip active and scale != 1.0, the debug-text texture batch fails
and no text renders at all (see commit history around the 2-pass fix).
Unclipping the text pass was the workaround that made text appear.

Current compromise: text only draws for rows whose text block fits
fully inside the panel. Partial rows show bg only. Visible as "pop-in"
during scroll instead of smooth scroll-then-clip.

Proper fix (any of these):
  1. Pre-render each row's text to an SDL_Texture (cache by index).
     Then draw textured quads — textures respect clip rects cleanly.
     Row text is static per song, so the cache never invalidates.
     Memory: ~600 rows x 2 text textures x ~1KB = ~1.2 MB. Fine.
  2. Use SDL_SetRenderTarget to an offscreen panel-sized surface,
     draw rows (with text) to it, then blit the whole surface.
     More invasive; changes the whole render path for song select.
  3. Once a proper font renderer lands (see Low priority: Better fonts),
     this problem likely goes away — stb_truetype-based rendering
     uses texture draws that behave normally with clip rects.

Option 1 is the recommended path when this becomes worth doing.

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


- **SliderPatternGenerator port** (converter phase 3d). 500+ lines from
  lazer's `SliderPatternGenerator.cs`. Decomposes sliders into
  head/repeat/tail notes, assigns columns per segment. The four test
  maps `100374`, `1450162`, `4869637`, `zero-length-slider` are
  slider-first, so porting sliders unlocks the big test-prefix jump.
  Two to three round-trips, most complex piece left.

- **SpinnerPatternGenerator port** (converter phase 3d). ~100 lines.
  Simpler than sliders. Converts spinners to holds (if duration >=
  100ms) or single notes, with special 8K column handling. Cheap win
  once sliders land.

- **KeyMod support for the converter.** `ManiaModKey1..10` force a
  target column count. Threading `target_columns` through
  `convert_to_mania` (~50 lines) unlocks the 3K-18K goldens, extending
  test coverage from 3 maps to 23. Needed for full column-count parity
  proof.

- **Stair-type finalization bug.** `HitCirclePatternGenerator::generate()`
  doesn't update `stair_type` after emitting notes. Lazer does a foreach
  that flips stair <-> reverse-stair when emitted notes hit the boundary
  columns. Affects dense maps (time_sep <= 95ms). Not hit by 20544 (too
  sparse), but will diverge the moment slider port unlocks longer
  prefixes. Small fix (3 lines), harness catches regression immediately.

## Medium priority

- **Song select: bg image as row background.** Currently each row is a
  flat dark blue rectangle. Lazer uses the map's `bg.jpg` / `bg.png`
  as the row background, dimmed (opacity ~50%) so text stays legible.
  Task: load bg for each visible row (throttled — only rows in viewport,
  cache by folder to avoid re-decode per scroll frame), draw as texture
  fill behind text. Requires: `load_song_bg` currently works only for
  `songs[selected_index]`, needs to become per-row with a small LRU
  cache or a "bg keyed on folder" map.

- **Song select: long-press → details panel.** Right-click equivalent
  on touch. Lazer's pattern: press starts a circular progress
  indicator at the touch point; release before completion = normal tap
  (selects/plays); hold past completion (~400ms) = context action.
  Task: state machine on the FINGER_DOWN/MOTION/UP sequence with a
  circular arc rendered at the touch point. On completion, open a
  details panel (mapper, source, tags, per-difficulty stats, bg preview
  at larger size). Do not conflict with the existing drag-to-scroll
  gesture — only trigger long-press when movement < N pixels.
- Result → song select (currently tap = retry; only Esc goes back)
- On-screen restart button (R key works)
- Star rating display in song select (needs diffcalc — probably
  reuse a smaller difficulty calculation or skip entirely)
- Backup/shared audio: currently every song folder duplicates its
  hitsounds; could share via files/skin/default/
- **Quadrant touch controls for 4K.** Alternative to the current
  horizontal 4-column tap layout. Splits the screen into four quadrants
  (upper-left, upper-right, lower-left, lower-right). Upper pair hits
  the outer two columns; lower pair hits the inner two. Familiar from
  DDR-style phone layouts. Ships **alongside** the standard column
  layout, user-selectable in settings.

  Design constraints:
  - **4K only.** 5K+ has too many columns per quadrant to be legible
    on a phone screen; a 5K "2+1+2" split is tempting but speculative.
    Ship 4K first, defer higher.
  - **Geometry**: split the safe-area rect (not the raw window rect —
    matters when the safe-area toggle ships) into two halves each
    axis. Outer columns are the top two quadrants; inner columns are
    the bottom two. Left half of screen = left column of each pair.
  - **Press feedback matters more than with columns.** A quadrant
    press lights both the outer-left key strip and outer-right key
    strip simultaneously. Without the two-column flash the mental
    mapping breaks and notes look unresponsive.
  - **Settings key**: `touch_layout=columns|quadrants`. Ignored on
    key counts other than 4. Reset to `columns` if the map's key count
    changes away from 4.
  - **Interaction with drag**: the song-select drag handler already
    uses two-finger gesture state (`song_drag_id`). Quadrant input is
    per-touch-column, uses `active_fingers` keyed by column index.
    No conflict — the existing column-based touch path just maps
    quadrant → column instead of x-coordinate → column.
  - **Not for OTG keyboards.** This is a touch-only alternative.
    Keyboard dispatch (SDLK_*) is orthogonal and unaffected.

- **Arrow key + Enter keyboard navigation.** Currently only Esc
  (back/cancel), R/`` ` `` (retry), and gameplay column keys are bound.
  Add the standard nav set so the app is playable without touch:

  **In song select (AppState::SongSelect)**
  - `Up` / `Down` — move `selected_index` ±1, keep row visible
  - `Page Up` / `Page Down` — jump by visible row count
  - `Home` / `End` — top / bottom of the list
  - `Enter` — start play on the currently-selected entry
  - No-op when `songs.empty()` (during scan)

  **In settings (AppState::Settings)**
  - `Up` / `Down` — cycle `settings_row` focus indicator
  - `Left` / `Right` — decrement / increment focused slider
  - `Enter` — enter keybind-capture mode for the focused row
  - `Escape` — exit settings (already works)

  **In gameplay (AppState::Playing)**
  - Existing: column keys per keybind, R/`` ` `` restart, Esc pause

  Conflicts to check: none of these collide with default 4K
  (`D F J K`) or 7K (`S D F Space J K L`) column keys.

- **10K key-drop investigation.** Symptom: on 10K maps, individual
  column presses intermittently fail to register — the corresponding
  column doesn't flash, and no note is judged. Pressing all 10
  simultaneously *does* occasionally register every column visually.
  Presently unclear whether the failure is at the keyboard layer,
  Android input, SDL event dispatch, or our binding lookup.

  **Ranked hypotheses** (most to least likely):
  1. **Physical keyboard rollover limit.** Most USB keyboards ship
     with 6KRO — the hardware cannot report more than 6 concurrent
     keys. NKRO keyboards break this but often need a hardware mode
     toggle. Symptom matches. **Test first.**
  2. **scrcpy `uhid` 6-key HID limit.** Already documented in Lessons.
     If testing is done through scrcpy, this alone explains everything.
  3. **Default 10K binding has problem keys.** `A S D F V N J K L
     Semicolon`. `Semicolon` is layout-dependent; `V` and `N` are
     classic ghost positions. If drops cluster on specific columns,
     this is the cause.
  4. **SDL3 event queue drops under fast input.** Less likely — SDL
     queues are generous — but worth instrumenting.
  5. **`!e.key.repeat` filter eats real presses.** Rare, but the
     filter interacts with rollover.

  **Diagnostic ladder** (in order):
  1. Rule out scrcpy. Test with real OTG keyboard, no scrcpy running.
     If drops disappear, this was the cause and there's no code fix.
  2. Rule out the keyboard. Test with a different keyboard. If one
     drops and another doesn't, it's rollover. If both drop the same
     columns, it's our code.
  3. Log every KEY_DOWN with key code, name, repeat, and current
     key_count. Mashing 10K and grepping missing columns confirms
     whether the event reached us.
  4. Check cluster pattern: same columns always failing = hardware
     ghosting; random = queue overflow.
  5. Try a rollover-friendlier 10K default — e.g. `A S D F G H J K L
     Semicolon` splits into two clean hands.

  **Mitigations**: change default binding (1-line), add settings
  warning when 10K is selected, or document NKRO requirement. Blast
  radius is contained — dispatch is `bindings[keys]` lookup in
  `SDL_EVENT_KEY_DOWN` plus the same in KEY_UP.

  **Reference**: `SDL3/SDL_keyboard.h`, the scrcpy 6-key note in
  `## Lessons learned`.


## Mania key conversions (Key mods)

`ManiaModKey1` through `ManiaModKey10` convert any non-mania beatmap to
a mania beatmap of the selected column count. Because they are a
conversion, they carry a **0.90× score multiplier** — separate from
anything the std → mania path itself does.

Reference: `osu.Game.Rulesets.Mania/Mods/ManiaKeyMod.cs`,
`osu.Game.Rulesets.Mania/Scoring/ManiaScoreMultiplierCalculator.cs`.

Key points:
- Key mods are no-ops on **native mania maps**:
  `if (mbc.IsForCurrentRuleset) return;` — the mod doesn't change the
  map. Whether lazer still applies the multiplier in that case is not
  fully clear; the calculator runs on the mod list, not on whether the
  mod had an effect.
- Score multiplier: **0.90×** (`new_key_mod_multiplier`). Was 1.0×
  (`old_key_mod_multiplier`) until tachyon 2025.718.0 (2025-07-18,
  ppy/osu#30506). Replays older than that get 1.0×.
- Only one key mod can be active at a time — they are mutually
  incompatible.
- Ranked maps use the default column count (from the mod's own
  `KeyCount`); any key count from 1 to 10 is allowed.
- Converted maps display the Key mod's column count, not the source
  map's natural key count.

**Not yet implemented in this client.** Port order when ready:
1. `ManiaBeatmapConverter` with target column count from Key mod
2. Score multiplier application at result time
3. Converter pattern generators (hitcircle, slider, spinner)


- **Star sort: title primary, stars secondary.** Current order is
  stars-then-title, which interleaves difficulties from different songs
  by rating tier. User's mental model is:
      Agate 2.5, Agate 4.5, Beowulf 1.5, Beowulf 9.5
  (alphabetical by title, then ascending stars within each title).
  Affects both the cold-path and cache-hit comparators in
  `song_select.cpp`.

- **Touch control visual bounding boxes.** Lazer draws transparent white
  rounded rectangles over each tappable region. Critical for `screen`
  and `quadrants` layouts where there's no column strip as spatial
  anchor — users coming from physical keyboard have zero tactile
  reference. Draw per-strip rectangles in gameplay, dim when not
  pressed. Could also apply to `columns` layout for consistency. ~1
  hour of render + hit-test integration.

- **Scroll direction (up) setting.** Notes rise from bottom to judgement
  line instead of falling. On touch, fingers rest over the empty lower
  half rather than obscuring incoming notes. Settings key:
  `scroll_direction=down|up`. Requires flipping the y-formula sign in
  the note draw loop AND head/tail cap anchor positions. ~30 lines
  plus careful testing across all three touch layouts.

- **Quadrant paired-strip lighting.** When a quadrant is pressed, both
  corresponding column strips flash (UL -> cols 0 AND 3 both light).
  Without the paired flash, quadrant mode reads as broken — one press
  animates two judgement-line columns but only one note judges.
  Deferred as cosmetic; becomes more important once bounding boxes
  land since the boxes emphasize the two-per-tap mapping.

## Low priority
- Mods: DT / HT / HR. DT/HT change scroll_pps and audio rate
  together; needs a "rate" concept in the audio stream too.
- Score persistence: best score per (map, difficulty) in a local file
- Better fonts (currently SDL's 8x8 debug font)
- Custom hitsound banks per map (SampleSet / SampleIndex in .osu)
- Std → mania converts
- **FPS cap / HUD artifacts.** The HUD reads FPS over a 1 s window and
  will not exceed panel refresh even with `fps_cap=0`. Two causes:
  1. **`vsync_enabled` is evaluated once at startup** as
     `(cfg_fps_cap == 120)`. Changing the cap at runtime updates
     `cfg_fps_cap` but does not re-evaluate vsync — the code
     deliberately avoids re-enabling vsync because
     `SDL_SetRenderVSync(renderer, 1)` blocks on Android and can
     freeze the event loop. Net: set `fps_cap=0` and the HUD still
     reads ~120. **Restart the app** and vsync stays off, software
     cap becomes the only limiter.
  2. **Android compositor can throttle to panel refresh regardless.**
     On a 120 Hz panel the HUD will read ~120 even with vsync off.
     Truly exceeding panel refresh needs SurfaceFlinger-level work
     and isn't useful for a rhythm game.
  What matters is frame-time **stability** at the cap. Stable 120
  with p99 under 9 ms beats a spiky 144.
- **Power / thermal / frame-time benchmark logging.** Dev-only,
  gated on `benchmark=1` in settings.ini. 1 Hz sample, hooked into
  the existing `FPS[%d]` log cadence. Per-sample metrics:
  FPS + frame-time mean/max/p99 + dropped-frame count;
  battery current (`/sys/class/power_supply/battery/current_now`),
  voltage, SoC, and temperature (tenths of a degree C);
  SoC thermal zones (highest non-battery `thermal_zone*/temp` as
  `soc_temp`, distinct from battery temp and the value that predicts
  throttling); per-cluster CPU freq (`cpu0/4/6/cpufreq/scaling_cur_freq`);
  GPU freq on Qualcomm (`/sys/class/kgsl/kgsl-3d0/gpuclk`);
  VmRSS from `/proc/self/status`. Output as JSON-lines to
  `files/logs/bench_<UTC>.jsonl`; append a summary record on
  session end.
  **SELinux caveat**: `/sys/class/power_supply/*`, thermal zones,
  and cpufreq files are frequently restricted to system UIDs on
  modern Android. Logger must cache per-path read success and
  never spam errors; fall back to what's readable (`/proc/self/status`
  always works). First run on the V29e tells us what's accessible.
  Purpose: validate SAMPLE_CONCURRENCY pools don't cost CPU over
  long sessions, async scan doesn't spike power on launch, SV math
  doesn't push GPU harder on SV-heavy sections, 120 FPS is stable
  not bursty, and thermal throttling on the V29e doesn't manifest
  as silent FPS drop.
- **Skip intro on retry / auto-seek to first note.** Maps with
  long silent intros (10–30 s) force sitting through the intro on
  every retry. Lazer doesn't skip; mobile friction is real.
  Design: at play start / retry, compute `first_note_time`. If
  `> skip_threshold_ms` (≈ 3000–5000), begin playback at
  `first_note_time - lead_in_ms` (≈ 1500–2000, enough for the first
  note to visibly approach). Retry always skips; fresh load only
  if intro is long.
  Implementation: current clock is
  `now_ms = (wav_len - SDL_GetAudioStreamQueued) / bytes_per_ms`.
  Queue `wav_data[skip_offset .. wav_len]` and track `skip_to_ms`;
  clock becomes `skip_to_ms + (played / bytes_per_ms)`. Pause/resume
  already manipulates `audio_start_tick`; skip offset is orthogonal.
  A ~150 ms fade-in softens the attack.
  Difficulty: 1–2 h for core skip; +1 h for fade-in and indicator.
  Pairs with eventual "resume from last failure point" — both are
  "don't restart from 0" variants; do this first and the mechanism
  extends.
- **Bar lines.** Cosmetic beat markers.


- **Video frame-time investigation.** User reports video maps feel wrong
  despite stable HUD FPS (1s average). Possible causes, ranked:
    1. Frame time variance — 120 avg with occasional 8ms+ spikes. HUD
       shows average, not p99. Fix: log p99/max per second.
    2. `SDL_LockTexture` + memcpy of 3.7 MB per frame on render
       thread. Memory-bandwidth-limited on SD695. Fix: smaller copy
       path (persistent-mapped or worker-side copy).
    3. Worker pacing sleep interfering with scheduler. Fix: thread
       pinning.
    4. Native 24fps video placebo — not a bug, video source is 23.976fps.
  Measure first (item 1) before touching code.

- **Audio offset analyzer.** Suggest `cfg_audio_offset_ms` based on
  recent play history. Track signed median hit error across last N
  plays, propose adjustment, show "Based on last N plays, suggested
  offset is X ms" with an apply button — mirrors lazer's `Suggest
  offset` row. Requires per-play hit error accumulation and a new
  settings row. ~1 hour.

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
- **scrcpy's `--keyboard=uhid` caps at 6 simultaneous keys.** It presents
  a USB HID boot-protocol keyboard, whose report format has exactly 6
  key slots. 7K charts with 6-note chords + a hold, or full 7-note chords,
  will drop the 7th. Not our bug; a real USB keyboard via scrcpy's HID
  shim has the same limit. Use an OTG keyboard with NKRO if testing those.
- **scrcpy on Ubuntu 24.04 / Pop!_OS ships as 1.25 and crashes on Android 15**
  (`NoSuchMethodException` on `SurfaceControl.createDisplay`). Purge it and
  use the self-managed build in `~/Work/scrcpy/`; add to PATH via `~/.bashrc`.

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
- **Per-K keybindings** (ccffca3). Table-driven settings screen;
  capture UI rebinds 4K-10K live. `keys_Nk=` in settings.ini.
  Defaults match modern osu!mania. See "Settings: per-K key mapping"
  below for the original design notes.
- **Settings label swap** (ccffca3). Audio Offset was displaying FPS
  because of an `else if` branch assigning to `fill` instead of `buf`,
  leaving the previous row's value in the string buffer. Rewritten as
  a table — one code path per row type.

## Std → Mania conversion (planned feature)
**Status:** std maps stay on disk. Song select filters them out with
`if (m.mode != 3) return false;` in `song_select.cpp`. That guard is
NON-DESTRUCTIVE and is what we remove when the converter lands.

Reference: ppy/osu ManiaBeatmapConverter.cs + Beatmaps/Patterns/Legacy/*
- Converter is seeded from (DrainRate + CircleSize) * 20 +
  (int)(OD * 41.2) + round(AR) — deterministic output.
- Density computed over last 7 notes; drives column distribution.
- Port targets: SliderPatternGenerator.cs, SpinnerPatternGenerator.cs,
  and the base circle handler in the same folder.

**The std → mania conversion itself is 1.0× — it does NOT reduce score.**
An unconverted std map played under mania rules scores the same as a
native mania map of the same content. What reduces score is applying a
**Key mod** on top of the conversion (see next section).
