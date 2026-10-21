# AGENTS.md — onboarding for future sessions

Companion to `NOTES.md`. **NOTES.md = what's been done. AGENTS.md = how to work here without stepping on landmines.** Read both before touching code. If they disagree, the code wins — fix the doc.

---

## Project at a glance

Native C++ SDL3 Android app for playing and converting osu! maps. Not a lazer port — independent parser + renderer, purpose-built for a single-file Android binary. Everything ends up in `main.cpp` (~17k lines today). Converters are separate files (`converter*.cpp`, `parser.cpp`), but UI, state machine, and input all live in `main.cpp`.

- Target: arm64-v8a Android only
- Build: Gradle → CMake → clang (NDK 27)
- SDL3 + FFmpeg (video) + stb/dr_libs (audio) vendored under `third_party/`
- Cache: text index at `/sdcard/Android/data/sh.ppy.osuvinniv/files/cache/songs.index`

---

## Build / install / test

```
./gradlew :app:assembleDebug 2>&1 | tail -20
adb install -r app/build/outputs/apk/debug/app-debug.apk

adb logcat -c && adb logcat | grep -E "INTERLUDE|OFFSET|SONGS|VIDEO|POOL"

./tests/run.sh                    # 3/3 converter + 37/37 parser
./tests/run_offset_test.sh <map>  # 5/5 offset formula
./tests/run_autoplay.sh <map>     # judgement headless
```

Builds are slow (10–30s) because `main.cpp` is one translation unit and any touch triggers a full recompile. Budget accordingly. Splitting `main.cpp` into multiple TUs would help but isn't planned.

---

## The three clocks (do not break)

Three times coexist in the gameplay loop. **Never** touch one without verifying the others stay consistent.

| Variable | Formula | Used by |
|---|---|---|
| `now_ms` | `current_tick - audio_start_tick + drift + map_offset_ms` | visual note position |
| `map_t` (inside `judge_col`) | `t - audio_start_tick + drift + map_offset_ms` | hit judgement |
| `d` | `note.time - map_t` | signed error (positive = early) |

`map_offset_ms` replaced the old `cfg_audio_offset_ms` reference in both clocks. If a future patch swaps one but not the other, gameplay timing will diverge from visual timing and *only* `tests/run_offset_test.sh` catches it — the human-playable build will not feel obviously broken until the offset is >30ms.

`audio_drift_ms` is computed by comparing `SDL_GetAudioStreamQueued` progress against the wall clock, smoothed 0.9/0.1. It corrects AAudio startup latency. Don't reset it without `audio_start_tick` also being reset.

---

## Line-number patches are unreliable

Line-number patches (`sed -n '200,265p'`-style) needed re-grep every time because a single added line shifts every subsequent line number. **Rules:**

1. **Always anchor on unique marker strings**, not line numbers.
2. Before writing, `assert s.count(marker) == 1`. If the assert fails, stop and re-grep the actual text — do not loosen the assert.
3. **Write-after-each-repl.** A multi-step patch that `sys.exit(1)`s midway loses all prior in-memory replacements. Every `repl()` closure must `open(path, "w").write(src)` inside itself.
4. **Verify with grep after every patch.** Not "FILE WRITTEN". Grep for the inserted marker string. If the build error and source state disagree, the build is right.

Terminal swallowing also bit hard: multi-line Python with `\n` in strings gets mangled when pasted. Use `chr(10)` or write to `/tmp/patch.py` via heredoc instead of inline `python3 -c "..."`.

---

## SDL3 render quirks

- **`SDL_SetRenderScale` is sticky.** `SDL_RenderDebugText` scales its glyphs AND its coordinates (coordinates are divided by scale before drawing). `SDL_RenderLine` only multiplies coordinates, no glyph behavior — if you forget to reset scale to 1.0, lines fly off-screen.
- **Always reset scale to 1.0 after a UI block.** Helper functions that draw text at scale should reset on exit.
- **`SDL_SetRenderViewport`/`ClipRect` leak.** Song list uses `ClipRect` for scrolling; if not reset to NULL after, backgrounds/notes disappear. There's a `SDL_SetRenderClipRect(renderer, NULL)` sweep at the top of the SongSelect and Interlude render blocks specifically to defend against this.
- **`SDL_RenderTexture` respects `dst.w/h` but not aspect.** For any texture whose source aspect is not the screen aspect, compute `dst` yourself. Both the song select and interlude BG blocks do cover-fit math; don't shortcut it.
- **`SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE` is 8px per char.** Centering: `x_px = (win_w - strlen(txt) * 8 * scale) / 2`, then divide by `scale` when passing to `SDL_RenderDebugText`.

---

## Test harness invariants

- **`run_autoplay.sh`** sets `audio_start_tick = 0`, `audio_drift_ms = 0.0`, `cfg_audio_offset_ms = 0`. If you add new state to the normal play path that assumes a nonzero `audio_start_tick` or a real drift value, the autoplay test will silently produce wrong judgements. Check the headless init block when adding state.
- **Offset formula test** depends on `judge_col` using `map_t = t - audio_start_tick + drift + offset`. Changing the sign of `map_offset_ms` in either clock breaks this test; the test asserts `median_d == -bias_ms - offset_ms`.
- **`test_replay.txt`** path is used for rendered autoplay (video + frame-time probe). It's currently **broken** — autoplay injection never fires because `now_ms` doesn't advance in rendered mode. See NOTES.md "Known broken".

---

## State machine

`AppState` enum: `SongSelect`, `Interlude`, `Playing`, `Result`, `Settings`.

```
SongSelect --(tap song)--> Interlude --(2s / tap)--> Playing --(end)--> Result
    ^                        |                        |                |
    |                        +--(back btn)--+         +--(pause)-->  Pause menu
    |                                       |                          |
    +---------------------------------------+--------------------------+
                                                                       |
                                                              (retry or quit)
```

**Interlude** is the loading buffer + per-map offset tweaks screen:
- On entry: `pending_map_load = true`, timers reset
- Frames 0..29: no loading yet (spinner visible)
- Frame 30: load runs (BLOCKS the main thread — spinner freezes mid-rotation, then disappears)
- After load: 2-second timer, or tap to skip
- Touch target: tap empty space → start, tap slider → drag, tap SAVE → commit offset, tap USE → snap to suggestion, tap Back → SongSelect
- Back-key handler must cancel `pending_map_load`, else freeze on back

**Pause menu** (mid-Playing only):
- Continue → 3-second resume countdown
- Retry → goes BACK to Interlude (re-shows slider, resets to saved offset from `map_offsets[path]`)
- Quit → SongSelect

---

## The offset pipeline

| Setting | Location | Scope |
|---|---|---|
| `cfg_audio_offset_ms` | `settings.ini` | global default |
| `map_offsets[path]` | in-memory `std::map` | per-map, **not persisted** |
| `map_offset_ms` | local gameplay var | active for this play |
| `suggested_offset_ms` | derived from `session_medians` | session-global |

Flow on Interlude entry: `map_offset_ms = map_offsets.count(path) ? map_offsets[path] : cfg_audio_offset_ms`

- **SAVE** button: writes `map_offsets[path] = map_offset_ms` (slider value). Label shows `SAVE <value>` so it's unambiguous.
- **USE** button: sets `map_offset_ms = suggested_offset_ms` (no save). Label is `USE`, sits next to the suggestion text.
- **Result-screen APPLY**: sets `cfg_audio_offset_ms = suggested_offset_ms` (global, persists to ini)
- **Settings APPLY**: same (global, persists)

`map_offsets` is not persisted across app restarts. If you want per-map saves to survive a restart, extend the cache file or add a separate `offsets.ini`. That's a follow-up.

---

## Cache version discipline

`song_cache.cpp` writes a text index with a `vN` prefix. Current: **v9**. 15 pipe-delimited fields per entry.

**Any `SongEntry` field addition requires v10** + reader and writer updated together + clear on version mismatch. If you bump the struct without the version, users get silent parse failures or stale entries.

---

## Committing

- One logical change per commit. Don't mix feature + bug fix.
- **Body must say why**, not what. `git log --oneline` should be readable as a changelog.
- Never amend published commits. HEAD is force-pushable only if you're certain no one else is on the branch.
- `NOTES.md` gets an entry for each multi-session feature. **AGENTS.md** stays evergreen — update it only when a rule here changes.

```
git add <files>
printf '%s\n' 'Title line' '' 'Body explaining why' > /tmp/msg
git commit -F /tmp/msg
git push origin main
```

---

## What NOT to touch

- **`parser.cpp` timing-point logic** — verified against lazer's goldens (20544 objects, Spinner 100374). Any change needs a re-run of `./tests/run.sh`.
- **`converter_legacy.cpp` RNG state transitions** — matches lazer's `LegacyRandom` byte-for-byte. Break it and pattern generators will silently diverge from goldens.
- **`song_cache.cpp` version prefix** without bumping v9 → v10.
- **`audio_start_tick` initialization in autoplay block** — set to 0 for determinism, not the normal +2000ms lead-in.

---

## Known landmines (as of this session)

1. **Replay mode hangs** — rendered autoplay injection never fires.
2. **Settings screen paginated** (done, verified by user). 15 rows split across two pages: page 0 = Audio/Game (Music, Hitsound, Scroll, FPS, Offset, Scroll dir, KeyMod — 7 rows), page 1 = Mods/Keys (Touch layout + 4K-10K keys — 8 rows). Page toggle button bottom-right of settings. Both pages fit within 1080p usable height. Tested at 2400x1080. If a future row addition pushes either page past 8 rows, add a third page rather than shrinking row_h.
3. **Torus font not ported** — `SDL_RenderDebugText` is a blocky 8x8 font. Lazer's UI is Torus. Full port needs SDL3_ttf + vendored TTF + text cache.
4. **SliderPatternGenerator missing** — 532 lines of lazer C# to port. Largest remaining converter piece. 3 sessions.
5. **`map_offsets` not persisted** — per-map offsets reset on app restart.
6. **Taiko converts render video bottom-left** — expected behavior (taiko video layout), not a bug. Added to NOTES.md "laugh at it" section.

---

## Where to look for specific things

| Task | Files / sections |
|---|---|
| Adding a setting row | `setting_rows[]` in `main.cpp` ~line 230 |
| Adding a state | `enum class AppState`, all `if (app_state == ...)` blocks |
| Adding a hitsound pool | `SamplePool` structs + `play_note_sample()` |
| Fixing offset math | `judge_col`, `now_ms` calc, `run_offset_test.sh` |
| Video pipeline | `video_decode.cpp` (ffmpeg) + `video_player.cpp` (async) + SAR handling |
| File scan / cache | `song_select.cpp` (scan) + `song_cache.cpp` (persist) |
| Converter column heuristic | `converter.cpp` + `converter_legacy.cpp` |
| Pattern generators | `converter_legacy.cpp` (HitCircle/Spinner done; Slider TBD) |

---

## If something is broken and you can't tell why

1. `adb logcat -c && adb logcat | grep -E "INTERLUDE|OFFSET|SONGS|VIDEO|POOL|MAP"`
2. Add `SDL_Log("HERE: %d", __LINE__)` to the suspect block
3. Build → install → reproduce → read the log
4. If the log contradicts the code, **trust the log**. The compiler doesn't lie about which branch ran; grep might be matching a stale copy.

## If you're about to change `main.cpp` and it's 17k lines

Split first if you can — `main.cpp` is already unwieldy. Some extraction candidates (low risk):
- The settings screen render block (~120 lines) → `settings_screen.cpp`
- The result screen render block (~150 lines) → `result_screen.cpp`
- The song select render block (~250 lines) → `song_select_render.cpp`

Do NOT split `main.cpp` and rewrite logic at the same time. One or the other.

---

## Note on this session's fix

The APPLY/USE split and per-map retry fix landed because the previous semantics (`APPLY auto-snaps to suggestion when available`) made manual per-map offsets impossible. If a future agent sees the old behavior described anywhere in NOTES.md history, that description is obsolete — trust this file's "The offset pipeline" section and the code.