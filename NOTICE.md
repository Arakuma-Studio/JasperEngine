# Notice

JasperEngine is a fork of [Doriax](https://github.com/doriaxengine/doriax) by Eduardo Doria and contributors, released under the MIT License.

- Fork point: Doriax `main` at commit `29348d7b4d9b03ee1f29dc184f9863f36dc12c2d` (2026-09-29), tagged `doriax-base` in this repository. `git diff doriax-base` shows every change made since.
- The full Doriax history is preserved in this repository.
- The Doriax name was replaced with JasperEngine by `tools/rebrand.py`. Per-file copyright headers (`(c) Eduardo Doria and contributors`) are kept as they were.
- Doriax's copyright notice is retained in [LICENSE](LICENSE), as the MIT License requires.

Bundled third-party libraries under `libs/`, `engine/libs/`, and `engine/libs/shadercompiler/libs/` keep their own licenses.

Vector graphics of the HTML/CSS UI (all MIT):

| Library | Version | Location | Used for |
|---|---|---|---|
| [ThorVG](https://github.com/thorvg/thorvg) | 1.2.0 (`caa5af1167d7`) | `engine/libs/thorvg/` (`LICENSE`) | Lottie (`<lottie>`), CPU renderer |
| [LunaSVG](https://github.com/sammycage/lunasvg) | 3.5.0 (`cf3594d5232e`) | `engine/libs/lunasvg/` (`LICENSE`) | SVG (RmlUi's SVG plugin, `.svg` textures) |
| [plutovg](https://github.com/sammycage/plutovg) | 1.3.3 (bundled with LunaSVG) | `engine/libs/lunasvg/plutovg/` (`LICENSE`; its rasteriser derives from FreeType's, `source/FTL.TXT`) | LunaSVG's rasteriser |

HTML parsing of the HTML/CSS UI's compatibility layer:

| Library | Version | Location | Used for |
|---|---|---|---|
| [lexbor](https://github.com/lexbor/lexbor) | 3.1.0 (`master` at `f4cbbcd91359`, no release tag yet) | `engine/libs/lexbor/` (Apache-2.0: `LICENSE`, `NOTICE`; one local patch, `PATCHES.md`) | HTML5 parsing of `.html` documents (`engine/core/htmlui/compat/html/HtmlParser`) |

Test data: the html5lib tree-construction tests (MIT, `engine/tests/htmlui/fixtures/compat/html5lib/LICENSE`).
