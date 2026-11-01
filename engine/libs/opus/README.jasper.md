# libopus in JasperEngine

- Upstream: https://github.com/xiph/opus, tag `v1.6.1` (BSD-3-Clause, `COPYING`; contributors in `AUTHORS`).
- Used by: the HTML UI's `<video>` audio (`engine/core/media/OpusAudioDecoder.cpp`,
  `docs/html-ui/phase-6-video.md`).

## Vendored (byte for byte from the tag)
`COPYING`, `AUTHORS`, `README`, `include/*.h`, every `celt/*.h`, `silk/*.h` and `src/*.h` except the
fixed-point and NE10 mode tables (`celt/static_modes_fixed.h`, `celt/static_modes_fixed_arm_ne10.h`,
`celt/static_modes_float_arm_ne10.h`), and the 66 `.c` files listed in `CMakeLists.txt`. These are
the objects a program that calls the decoder API (`opus_decoder_*`, `opus_decode_float`,
`opus_multistream_decoder_*`, `opus_packet_get_nb_samples`, `opus_strerror`) links, read from the
link map of a build of the whole library.

Not vendored: the encoder, `dnn/` (deep PLC, DRED, OSCE), the x86/ARM intrinsics and asm, `tests/`,
`training/`, `doc/`, `scripts/`, and the autotools/meson/CMake files.

## Ours
- `CMakeLists.txt`: a float build, `OPUS_BUILD USE_ALLOCA HAVE_LRINT HAVE_LRINTF`, `OPUS_EXPORT`
  empty, hidden visibility.
- The Xcode targets `opus-ios`/`opus-macos` (`tools/htmlui/xcode_add_video_libs.py`).

## Updating
Copy the same file set from the new tag. If the build then misses a symbol, rebuild the full
library once with `-Wl,-Map` and add the `.c` files the decoder's objects pull in. Then run the
video audio tests (`HTMLUI_TEST_ARGS=--gtest_filter=VideoAudio*`) and the Xcode script with `--check`.
