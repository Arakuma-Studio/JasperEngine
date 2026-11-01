# FFmpeg public headers (vendored, headers only)

- Used by: the Linux video backend (`engine/platform/linux/FFmpegBackend*.cpp`), which plays MP4 (H.264, HEVC,
  AAC, ...) through the **system's** FFmpeg libraries loaded with `dlopen` at run time
  (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.6). Nothing of FFmpeg is compiled, linked or shipped
  by the engine: these are only the declarations each ABI major is compiled against, the way Firefox vendors
  them (`dom/media/platforms/ffmpeg/ffmpegNN`).
- One directory per `libavcodec` major, each with the matching set of the other libraries (the set a
  distribution ships together):

  | Directory | FFmpeg release | avcodec | avformat | avutil | swresample | swscale |
  |---|---|---|---|---|---|---|
  | `58/` | 4.4.5 | 58 | 58 | 56 | 3 | 5 |
  | `59/` | 5.1.6 | 59 | 59 | 57 | 4 | 6 |
  | `60/` | 6.1.2 | 60 | 60 | 58 | 4 | 7 |
  | `61/` | 7.1.1 | 61 | 61 | 59 | 5 | 8 |
  | `62/` | 8.0 | 62 | 62 | 60 | 6 | 9 |

- What is vendored: only the headers that `libavformat/avformat.h`, `libavformat/avio.h`,
  `libavcodec/avcodec.h`, `libavutil/{avutil,channel_layout,pixdesc,opt,display}.h`,
  `libswresample/swresample.h` and `libswscale/swscale.h` include (their `gcc -M` closure), unmodified, as
  `make install-headers` of each release installs them (`./configure --disable-everything --disable-x86asm`;
  `libavutil/avconfig.h` is the little-endian one of x86-64 and aarch64).
- Licence: LGPL 2.1 or later (`COPYING.LGPLv2.1`). FFmpeg is dynamically loaded, never linked; see `NOTICE.md`.
- Update: download the release, configure as above, `make install-headers DESTDIR=...`, copy the same closure into
  `<avcodec major>/`, and add the major to `FFMPEG_SETS` in `engine/platform/linux/FFmpegLoader.cpp` and to
  `engine/CMakeLists.txt`.
