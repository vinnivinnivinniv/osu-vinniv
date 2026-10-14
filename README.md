# osu!vinniv

A minimal osu!mania client for Android, written in C++ with SDL3.

**Rhythm is just a tap away.**

## Why

osu!lazer on Android struggles to hold 60 FPS on mid-range hardware, not because the device is slow, but because the framework's per-frame update cost is heavy by design. A blank SDL3 + OpenGL ES loop on the same hardware runs at roughly **2000 FPS**. This project asks how much of that headroom a purpose-built mania client can use.

## Target

A working 4K / 7K mania client that:

- Reads `.osu` beatmap files
- Plays with sample-accurate audio sync
- Handles taps, holds, and releases
- Scores like lazer (or close enough)
- Holds 120 FPS on a Snapdragon 695

## Not trying to be

- osu!lazer
- A general-purpose rhythm game framework
- A skin editor
- A storyboard renderer
- Online-enabled

## Status

- [x] Phase 1: SDL3 + GLES3 hello-world, ~2000 FPS on Vivo V29e
- [ ] Phase 2: `.osu` parser
- [ ] Phase 3: static playfield render
- [ ] Phase 4: scrolling + audio
- [ ] Phase 5: judgement + scoring
- [ ] Phase 6: minimal UI
- [ ] Phase 7: polish

## Build

Requires Android NDK r27, CMake 3.22, and the SDL3 submodule.

```
git submodule update --init --recursive
export ANDROID_HOME=$HOME/Android/Sdk
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Layout

```
app/jni/src/         C++ source
  main.cpp           SDL init + main loop
app/src/main/java/   SDL's Android Java wrappers (unmodified)
SDL/                 SDL3 (submodule, release-3.2.0)
```

## License

MIT. See [LICENSE](LICENSE).

"osu!" and "ppy" are trademarks of ppy Pty Ltd. This is an independent
community project, not affiliated with ppy.
