# FFmpeg (headers + static decode libraries)

`include/` holds the public API headers of FFmpeg 8.1.2 (libavutil 60,
libavcodec 62, libavformat 62, libswscale 9, libswresample 6), taken from the
minimal build described below (they are identical between the Linux and
Windows builds).

`lib-linuxbsd-x86_64/` and `lib-windows-x86_64/` hold static, decode-only
FFmpeg 8.1.2 builds (h264/hevc/aac decoders, mov demuxer, no external
libraries, no hardware acceleration, no network). When present, `modules/mp4`
links them directly (`MP4_FFMPEG_STATIC`) so mp4 playback works everywhere
with no system dependency: SteamOS, any Linux distro, and Windows (replacing
the Media Foundation backend). Deleting the `lib-*` directories falls back to
the old behavior: `dlopen` of the system FFmpeg on Linux (majors must match
the headers), Media Foundation on Windows.

Rebuild recipe (in the `godot-linux:4.7-f43` buildroot container, which needs
`dnf install nasm perl xz mingw64-gcc` per run):

```
COMMON="--enable-static --disable-shared --disable-programs --disable-doc
  --disable-avdevice --disable-avfilter --disable-everything
  --enable-decoder=h264,hevc,aac,aac_latm --enable-parser=h264,hevc,aac
  --enable-demuxer=mov --enable-protocol=file --disable-network
  --disable-autodetect --disable-debug --disable-iconv --disable-hwaccels
  --disable-vaapi --disable-vdpau --disable-libvpl"
# Linux (buildroot cross toolchain, glibc 2.28 floor):
PATH=/root/x86_64-godot-linux-gnu_sdk-buildroot/bin:$PATH \
  ./configure --cc=x86_64-godot-linux-gnu-gcc --enable-pic $COMMON
# Windows:
  ./configure --target-os=mingw32 --arch=x86_64 --enable-cross-compile \
    --cross-prefix=x86_64-w64-mingw32- $COMMON
```

FFmpeg is licensed under the LGPL 2.1+ (see https://ffmpeg.org/legal.html).
The sources are unmodified; static linking of an LGPL library into a
distributed binary requires providing a way to relink (the object files or
source offer covers this; the build recipe above plus the unmodified FFmpeg
sources satisfies it for personal distributions).
