# FFmpeg headers

Public API headers only, copied from the Arch Linux `ffmpeg 2:8.1.2` package
(FFmpeg 8.x: libavutil 60, libavcodec 62, libavformat 62, libswscale 9,
libswresample 6). Used by `modules/mp4` to compile its runtime `dlopen`
bindings; no FFmpeg code or binaries are compiled into or shipped with the
engine. The libraries are loaded from the user's system at runtime and must
match the major versions above.

FFmpeg is licensed under the LGPL 2.1+ (see https://ffmpeg.org/legal.html).
These headers are unmodified.

To update: copy `libavutil`, `libavcodec`, `libavformat`, `libswscale` and
`libswresample` from `/usr/include` of a machine with the target FFmpeg
version into `include/`, then adjust the major-version expectations in
`modules/mp4/ffmpeg_symbols.cpp` if they changed.
