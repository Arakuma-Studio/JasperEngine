# libwebm in JasperEngine

- Upstream: https://chromium.googlesource.com/webm/libwebm, tag `libwebm-1.0.0.32` (BSD-3-Clause,
  `LICENSE.TXT`; Google's additional patent grant, `PATENTS.TXT`; `AUTHORS.TXT`).
- Used by: the HTML UI's `<video>` (`engine/core/htmlui/media/VideoDemuxer.cpp`,
  `docs/html-ui/phase-6-video.md`): the WebM/Matroska parser only.

## Vendored (byte for byte from the tag)
`LICENSE.TXT`, `PATENTS.TXT`, `AUTHORS.TXT`, `README.libwebm`, `mkvparser/mkvparser.{h,cc}`.
Not vendored: the muxer, `mkvreader` (the engine reads through `FileData`), WebVTT, `webm_parser`,
the tools, tests and build files.

## Ours
`CMakeLists.txt` (target `webm`, hidden visibility) and the Xcode targets `webm-ios`/`webm-macos`
(`tools/htmlui/xcode_add_video_libs.py`).
