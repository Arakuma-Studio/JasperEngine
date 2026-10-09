# Importer fixtures (phase-2 §5.4)

Served by `tools/htmlui/fixture_server.py --root engine/tests/htmlui/fixtures/compat/net` to
`jasperengine-htmlui-editor-tests` (`editor/htmlui/tests/`). The server builds the Google Fonts
CSS, the per-weight TTFs (`gstatic/base.ttf` with its OS/2 weight and italic bits patched), the
license lists, the font metadata and the dotLottie archives itself; the files here are what it
serves as they are.

- `gstatic/base.ttf`: Karla Regular (SIL Open Font License 1.1, `gstatic/OFL.txt`), copied from
  `libs/imgui/misc/fonts/Karla-Regular.ttf`.
- `cdn/`: a Font-Awesome-like stylesheet (WOFF2 + TTF sources), a WOFF2-only one, an `@import`
  chain with a cycle and a Google Fonts import.
- `img/`: a 4x4 JPEG, an SVG, a WebP and a PNG (generated).
- `lottie/anim.json`: a Lottie animation with an external image asset (`images/img_0.png`).
- `lfpage/animation.html`: a LottieFiles-like page that names its dotLottie file.
- `project/`: a project (`project.yaml`, `assets/ui/*.html|css`) the tests copy and import into.
