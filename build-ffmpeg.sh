#!/bin/bash
# build-ffmpeg.sh — cross-compile a trimmed FFmpeg 7.0 for Android arm64-v8a.
# Run from repo root. Writes libs + headers into app/jni/ffmpeg/.

set -euo pipefail

NDK="${NDK:-$HOME/Android/Sdk/ndk/27.0.12077973}"
API=21
ARCH=arm64
TARGET=aarch64-linux-android
CPU=armv8-a

if [ ! -d "$NDK" ]; then
    echo "FAIL: NDK not found at $NDK"
    echo "Set NDK=/path/to/ndk and re-run."
    exit 1
fi

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$TOOLCHAIN/sysroot"
SRC="$(pwd)/third_party/ffmpeg-src"
OUT="$(pwd)/app/jni/ffmpeg"

if [ ! -d "$SRC" ]; then
    echo "FAIL: FFmpeg source not found at $SRC"
    echo "Run the download step first."
    exit 1
fi

mkdir -p "$OUT/include" "$OUT/lib"

cd "$SRC"

echo "=== configure ==="
./configure \
    --prefix="$OUT" \
    --enable-cross-compile \
    --target-os=android \
    --arch="$ARCH" \
    --cpu="$CPU" \
    --cc="$TOOLCHAIN/bin/${TARGET}${API}-clang" \
    --cxx="$TOOLCHAIN/bin/${TARGET}${API}-clang++" \
    --ar="$TOOLCHAIN/bin/llvm-ar" \
    --ranlib="$TOOLCHAIN/bin/llvm-ranlib" \
    --nm="$TOOLCHAIN/bin/llvm-nm" \
    --strip="$TOOLCHAIN/bin/llvm-strip" \
    --sysroot="$SYSROOT" \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-network \
    --disable-everything \
    --enable-decoder=h264,mpeg4,vp6,vp6a,vp6f \
    --enable-demuxer=mov,avi,flv \
    --enable-protocol=file \
    --enable-parser=h264,mpeg4

echo "=== make ==="
make -j"$(nproc)"

echo "=== install ==="
make install

echo "=== done ==="
ls -la "$OUT/lib/$ARCH/"
