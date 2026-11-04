# Aseprite fixtures (S44)

Real files made by Aseprite, read by `editor/tests/pixelart/` (codec, blend, fuzz) and the MCP
tests. Each folder keeps its upstream licence; NOTICE.md lists them. Never edit a file here: the
codec tests compare re-saves byte for byte against them.

| Folder | Source | Licence | What the tests use it for |
|---|---|---|---|
| `aseprite-tests/` | Aseprite's `tests/sprites` (`UPSTREAM-README.md` describes each file) | MIT (`LICENSE.txt`) | colour modes and transparent indices (`abcd`, `bg-index-3`, `2f-index-3x3`, `4f-index-4x4`), groups and visibility (`groups2`, `groups3abc`), tags with every direction and repeat (`tags3`, `tags3x123reps`), linked and empty cels (`1empty3`, `link`, `point*frames`), slices with per-frame keys (`slices`, `slices-moving`), tilemaps in each mode (`2x2tilemap2x2tile`, `2x3tilemap-indexed`, `3x2tilemap-grayscale`), user properties (`file-tests-props`), z-index (`z-order`) |
| `asefile/` | the test data of the asefile crate: `.aseprite` files with the PNG frames Aseprite exported from them (`<name>.png`, `<name>_<frame>.png`) | MIT (`LICENSE`) | the golden corpus: every frame rendered pixel for pixel against Aseprite's export (blend modes, opacity, linked cels, old 6-bit palettes, grayscale, indexed, big canvases, cels past the canvas, tilemaps with flips, tilesets, slices, user data) |
| `s44-corpus/` | made with Aseprite 1.3.18 by `tools/pixelart/make_golden_corpus.lua` / `.py` (`corpus.json` lists every command; regenerate with `JASPER_ASEPRITE=<aseprite> python3 tools/pixelart/make_golden_corpus.py corpus`) | MIT (`LICENSE`) | §9's corpus: group opacity and blend (`group_blend`), layer × cel opacity 1/127/254 (`cel_layer_opacity`), nested composed groups (`nested_groups`), tilemap flips, user data of every property type, indexed + background, the four tag directions × repeat 0/1/3, slices with per-frame and hidden keys; `<name>_NN.png` are Aseprite's `--save-as` frames (groups flattened, as its CLI does), `<name>_composed_NN.png` its renderer's (`Image:drawSprite`), `<name>.json` its `--data` |
| `s44-mcp/` | made with Aseprite 1.3.18 by `make_hero_idle.lua` and `make.py` beside them (`JASPER_ASEPRITE=<aseprite> python3 editor/tests/fixtures/aseprite/s44-mcp/make.py`; it checks the result against Aseprite's `--data`) | MIT (the scripts' headers) | the MCP tests' character `hero_idle` (32x32, layers Shadow, Body and a hidden `events` layer whose frame-1 cel says `onStep("left", 1)`, tags `walk` 0-3 and `Idle` 4-5 ping-pong × 3, slice `hurt` moving on frame 1 and hidden from frame 4, sprite properties `light2d.intensity` 2.5 and `note`) and `hero_idle_recolor` (the same with red Body cels) |
| `tenten/` | the sprites of tenten by debuff, with two of its sheets (`buff@22x22.png`, `spiker@16x16.png`) | MIT (`LICENSE`) | indexed game sprites as a small game ships them: reads, re-saves byte for byte, imports |

Every `.aseprite` file of `asefile/` and `aseprite-tests/` also has Aseprite's `--data` JSON beside it
(`<name>.data.json`, `make_golden_corpus.py data`), and `aseprite-tests/bg-index-3_01.png` is
Aseprite's `--save-as` of that indexed sprite with a background layer. `AsepriteParityTest`
compares the codec with all of it; `tools/pixelart/aseprite_compat.py` is the gate the other way
round (Aseprite opening the files the writer makes).

Two things Aseprite's batch mode cannot give a golden for: a file whose header says groups are
composed (a batch save never sets that flag; the corpus script patches it in), and the slice keys
after frame 0 (no Lua API: the script patches them and Aseprite re-saves the file).
