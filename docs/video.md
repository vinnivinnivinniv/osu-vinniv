# Video playback — design

Status: design only. No code shipped. Session plan at the bottom.

## Reference

- `~/Work/reference/osu-framework/osu.Framework/Graphics/Video/`
  - `VideoDecoder.cs` (1002 lines) — FFmpeg wrapper, worker thread, queue
  - `Video.cs` (234 lines) — drawable, frame consumption, loop/play-once
  - `VideoSprite.cs` (30 lines) — sprite with YUV shader
- `~/Work/reference/osu/osu.Game/Beatmaps/Formats/LegacyBeatmapDecoder.cs:457-467`
  — `Video` event handler

### What lazer does

Backend: FFmpeg via `FFmpeg.AutoGen`. Hardware decoders (MediaCodec on
Android, VAAPI/VideoToolbox/DXVA elsewhere) tried first, software
fallback via `tryDisableHwDecoding`.

Pixel format: decoded frames are converted to `AV_PIX_FMT_YUV420P` and
uploaded to a video texture; the fragment shader converts YUV->RGB.
`GetConversionMatrix()` picks BT.709 vs BT.601 based on resolution
(704x576 threshold).

Frame queue: `ConcurrentQueue<DecodedFrame>` with `max_pending_frames = 3`.
Decoder thread pushes; main thread drains via `GetDecodedFrames()`.
Textures are pooled and recycled via `ReturnFrames()`.

Looping: a `Looping` bool on the decoder. On EOF, `Seek(0)`. Set
externally by `Video.cs` per playback context.

Audio: never decoded. Only the video stream is opened.

Offset: `[Events]` `Video,<offset>,"file.mp4"` — the offset field is
discarded by `LegacyBeatmapDecoder`. Video always plays from beatmap
time 0 regardless of the declared offset.


## Decisions

### Decoder: FFmpeg 7.x, static, software-first

Cross-compile with:
    --enable-decoder=h264,mpeg4,flv,vp6
    --enable-demuxer=mov,avi,flv
    --enable-protocol=file
    --disable-everything
    --enable-static --disable-shared

(indented above to avoid the paste-handler issue with long flag lines)

Covers the local library: 96 mp4 (h264), 51 avi (mpeg4 part 2 / xvid),
4 flv (h264 or vp6). AVI is 36% of the collection — mp4-only would miss
over a third.

Estimated APK growth: ~4 MB (7.77 -> ~12 MB).

Hardware decode via FFmpeg's `mediacodec` is a config change, not a
rewrite, if software decode can't sustain the frame budget. Deferred
until measured.

### Pixel path: SDL3 native YUV, no shader

SDL3 ships `SDL_UpdateYUVTexture` for `SDL_PIXELFORMAT_IYUV` (YUV420P).
The renderer converts YUV->RGB on the GPU. Functionally identical to
lazer's shader path, zero custom GL.

Fallback if source isn't 4:2:0 (rare): `swscale` to `AV_PIX_FMT_RGBA`
on the worker thread, upload as `SDL_PIXELFORMAT_RGBA32`. Fast path
covers mp4/avi/flv in practice.

### Explicit divergence from lazer: video in song select

Lazer plays video during gameplay only. Our client will play it in
song select too, looped, as a preview. This is a feature addition, not
a parity goal. Document it here so future readers don't chase a "fix"
that isn't broken.


## API surface

Two files, patterned on the preview audio subsystem
(`audio_decode.*` + `preview_player.*`).

### video_decode.h / .cpp — FFmpeg wrapper

    struct VideoDecoder {
        bool open(const char* path);
        bool seek_to_ms(double ms);
        int  read_frame(VideoFrame& out);   // 0 at EOF, >0 on success
        void close();
        double duration_ms() const;
        double frame_rate() const;
    };

### video_player.h / .cpp — worker thread, queue, textures

    struct VideoPlayer {
        void request(const char* path, bool looping, double start_ms);
        void stop();
        void tick();                        // main thread, uploads latest frame
        SDL_Texture* current_texture();     // nullptr if none ready
        bool ready();                       // at least one frame decoded
    };

Single worker thread, `std::atomic<uint64_t>` generation counter for
cancellation. Mutex + condvar for queue handoff. Max 3 pending frames
in the queue to match lazer's backpressure.

Textures: small pool (3-4 pre-allocated), recycled via
`current_texture()` being consumed each frame. Same shape as
`availableTextures` in the reference.


## Behavior rules

| Aspect | Rule | Notes |
|---|---|---|
| Video in song select | Yes, looped | Divergence from lazer |
| Video in gameplay | Yes, play once, hold last frame | Matches lazer |
| Video audio | Ignored | Beatmap audio is the soundtrack |
| Fade-in | 200 ms from black on first frame | Polish |
| Fallback while loading | Static bg shown immediately | Instant UI |
| Fallback on decode failure | Static bg only | Current behavior preserved |
| Loop boundary | Seek(0) on EOF when looping | Matches lazer |
| Offset field | Ignored | Matches lazer's decoder |

## Integration points

Line references at `656d3f4` in `app/jni/src/main.cpp`:

1. Line 786 — `load_song_bg` lambda. Extend to also request video
   player for song select if `songs[idx].video_path` is non-empty.

2. Line 1509 — `songs[selected_index].bg_path != song_bg_key` swap.
   Add a parallel `song_video_key` check; on change, stop current
   video, request new one.

3. Line 1653 — GAMEPLAY BG draw. Change to:
   `if (video_player.ready()) draw video; else if (song_bg_tex) draw static`.
   On entering gameplay, re-request video with loop=false, start_ms=0.


## Scan changes

`parser.cpp::parse_osu_file` — new field `Beatmap::video_file`,
populated from [Events] `Video,<offset>,"file.mp4"` lines. Filename
only; offset discarded.

`song_select.cpp::read_meta` — copy to `SongEntry::video_path`. New
field on the struct. Cache version bump required (v4 -> v5).

New state in `main()`:
- `VideoPlayer song_video;` — one player, reused across maps
- `std::string song_video_key;` — current video path

## Session plan

1. Session 1 (recon) — done. This doc.
2. Session 2 — FFmpeg build + `video_decode.{h,cpp}`. Test binary
   opens mp4, decodes first 10 frames to RGBA. No SDL yet.
3. Session 3 — `video_player.{h,cpp}` + song-select integration.
   Video loops in song select.
4. Session 4 — Gameplay integration. Play once, hold last frame,
   pause/retry handled correctly.
5. Session 5 (half) — Fade-in polish, missing-video placeholder,
   scanner + cache changes.

Each session has a clear deliverable and can be verified in isolation.


## Known risks

1. FFmpeg cross-compile complexity. Config flags, toolchain paths,
   sysroot wrangling. Budget 3-4 hours for session 2. Fallback if it
   stalls: prebuilt FFmpeg NDK package, or switch to MediaCodec direct
   (mp4-only, JNI).

2. SD695 decode headroom. Software h264 at 720p on Kryo 660 should be
   fine, but we need to measure. Target: sustained decode at 30 fps or
   more (osu! video sources are usually 30 fps; we don't need 120 fps
   decode). If software can't sustain, upgrade path is
   `--enable-mediacodec` — a config change, not a rewrite.

3. AVI codec variance. 51 AVI files in the library. Most will be
   MPEG-4 Part 2 (Xvid/DivX), which FFmpeg handles well. Some may be
   WMV3 or MPEG-2 — those decode slowly or not at all. Acceptable:
   failure falls back to static bg, tracked via log line.

4. APK size. 7.77 -> ~12 MB. Real, but 12 MB is still tiny.

5. Interaction with per-diff bg fix. `song_bg_key` is keyed on
   `bg_path`. Video keying must be independent — two diffs might share
   the same video but different stills, or vice versa. Key on video
   path, separate from bg path.

## What this doc is NOT

- Not a mechanism for actually decoding video in-game yet. That's
  sessions 2-4.
- Not a commitment to hardware decode. Software first, measure, then
  decide.
- Not parity with lazer for song-select video. Lazer doesn't do it.
  This is our addition.
- Not a port of `VideoDecoder.cs`. That file is 1002 lines of hardware
  decode + error recovery + texture pooling + backend abstraction. We
  ship a simpler software-only version and extend if needed.


## Build verification (session 2)

FFmpeg n7.0 cross-compiled on first attempt with `build-ffmpeg.sh`.
Configure flags as documented.

Artifacts in `app/jni/ffmpeg/lib/` (unlinked):

| Library | Size |
|---|---|
| libavcodec.a | 19 MB |
| libavformat.a | 4.2 MB |
| libavutil.a | 4.1 MB |
| libswscale.a | 4.6 MB |
| libswresample.a | 555 KB |
| **Total** | **~34 MB** |

Architecture verified: extracting a member from `libavcodec.a` and
running `file` reports `ELF 64-bit LSB relocatable, ARM aarch64`.
Note: `file` on the `.a` itself reports `current ar archive`, not
`ELF` — the ELF metadata is per-member, not per-archive.

Sizes above are **pre-link**. The final binary pulls only the symbols
we call. Given `--disable-everything` + three demuxers + five decoders,
expected real APK delta is 2-4 MB. Measured in session 3 when CMake
actually links.
