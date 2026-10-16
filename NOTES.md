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

Mechanics (see `docs/mechanics.md` for the full audit table):
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
  - `SDL_StartTextInput` / `SDL_EVENT_TEXT_INPUT` wiring for the Android
    soft keyboard.
  - Filter state (query string) and a decision about how filtered results
    map to `songs` indices.
  - A text-input widget: cursor, backspace, clear.
  - Filter-aware selection / preview / play.
  Deferred to a dedicated session; touches the whole song-select UX.

- **Video decode / poster frame.** Maps with `Video,0,"clip.mp4"` and no
  shipped image currently render with no background (O108ROCKET Garden
  set, Vivlos, etc. once their mp4 is the only media). Options:
  - Extract first frame with ffmpeg (large dependency, real work).
  - Fall back to `clip.jpg` / `clip.png` if the mapper shipped a still
    alongside (partially done in the resolver — improves but doesn't
    solve video-only maps).
  - Show a "video" placeholder with the song title.

## Medium priority

- **Text masking on song list.** Rows partially scrolled off the top or
  bottom draw their backgrounds (clipped) but no text — the text pass is
  unclipped because `SDL_RenderDebugText` + `SDL_SetRenderClipRect` +
  render-scale changes interleave badly on GLES3. Visible as "pop-in"
  during scroll. Proper fix: pre-render each row's text to a cached
  `SDL_Texture`; textures respect clip rects cleanly. Roughly 45 min.

- **"Shrink game to avoid cameras and notches" toggle.** Matches lazer's
  Graphics → Layout setting. Adds `safe_area=0/1` to settings.ini; uses
  `SDL_GetWindowSafeArea` to set a render viewport and to pad input
  hit-tests accordingly. 60–90 min.

- **Song progress bar.** Thin bar across the top (or bottom) of the
  playfield, filling from `now_ms` vs `last_note_end`. Cosmetic; ~20 min.

- **Visual audit vs lazer.** Side-by-side screenshots comparing note
  height, hold body width, judgement line thickness, column colours,
  hit lighting, judgement popups. Half a session. The mechanics audit
  (docs/mechanics.md) is done; visuals are not.

- **#19 — Miss sound cooldown.** Currently throttled to 1 per 150 ms.
  Lazer has no throttle. Decide: match lazer (remove), or keep the
  throttle for comfort. 3-line change.

- **#20 — Result trigger timing.** Currently fires 2 s after
  `last_note_end`. Lazer fires when all judgements resolve (miss windows
  expire, holds release). Edge case: a hold whose tail extends past
  `last_note_end` would fire the result before the tail judged.

- **#21 — Sliding sample during hold.** Lazer loops the note's sliding
  sample while held. We play the head sample once and stop. Cosmetic
  but audible on long holds.

## Low priority

- **Mods: DT / HT / HR.** DT/HT change scroll_pps and audio rate together;
  needs a "rate" concept in the audio stream too. HR reshapes hit windows.
- **Score persistence.** Best score per (map, difficulty) in a local file.
- **Star rating display in song select.** Needs diffcalc (weeks).
- **Better fonts.** Currently SDL's 8×8 debug font. stb_truetype would
  also fix the row-text-masking problem in Medium priority.
- **Result → song select button.** Currently tap = retry; only Esc goes back.
- **On-screen restart button.** R key works today.
- **Backup/shared audio.** Every song folder duplicates its hitsounds;
  could share via files/skin/default/.
- **Std → mania converts.** Design captured in the section below.
- **Bar lines.** Cosmetic beat markers.

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
- Key mods 4K–9K are ranked, 1K–3K and 10K are unranked.
- Score multiplier: 0.90x (changed from 1.0x in Aug 2025, PR #30506).
- Key mods only apply to converts, not native mania maps.
- Converter is seeded from (DrainRate + CircleSize) * 20 +
  (int)(OD * 41.2) + round(AR) — deterministic output.
- Density computed over last 7 notes; drives column distribution.
- Port targets: SliderPatternGenerator.cs, SpinnerPatternGenerator.cs,
  and the base circle handler in the same folder.
- Converted maps display 0.90x multiplier and use NK column count.
