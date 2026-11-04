# Aseprite fixtures (S44)

Real files made by Aseprite, read by `editor/tests/pixelart/` (codec, blend, fuzz) and the MCP
tests. Each folder keeps its upstream licence; NOTICE.md lists them. Never edit a file here: the
codec tests compare re-saves byte for byte against them.

| Folder | Source | Licence | What the tests use it for |
|---|---|---|---|
| `aseprite-tests/` | Aseprite's `tests/sprites` (`UPSTREAM-README.md` describes each file) | MIT (`LICENSE.txt`) | colour modes and transparent indices (`abcd`, `bg-index-3`, `2f-index-3x3`, `4f-index-4x4`), groups and visibility (`groups2`, `groups3abc`), tags with every direction and repeat (`tags3`, `tags3x123reps`), linked and empty cels (`1empty3`, `link`, `point*frames`), slices with per-frame keys (`slices`, `slices-moving`), tilemaps in each mode (`2x2tilemap2x2tile`, `2x3tilemap-indexed`, `3x2tilemap-grayscale`), user properties (`file-tests-props`), z-index (`z-order`) |
| `asefile/` | the test data of the asefile crate: `.aseprite` files with the PNG frames Aseprite exported from them (`<name>.png`, `<name>_<frame>.png`) | MIT (`LICENSE`) | the golden corpus: every frame rendered pixel for pixel against Aseprite's export (blend modes, opacity, linked cels, old 6-bit palettes, grayscale, indexed, big canvases, cels past the canvas, tilemaps with flips, tilesets, slices, user data) |
| `tenten/` | the sprites of tenten by debuff, with two of its sheets (`buff@22x22.png`, `spiker@16x16.png`) | MIT (`LICENSE`) | indexed game sprites as a small game ships them: reads, re-saves byte for byte, imports |

Features without a real-Aseprite golden here (documents built in code or hand-made values test
them; `docs/aseprite.md`, "What is verified"):

- group opacity and group blend modes (Aseprite 1.3's composed groups);
- layer opacity × cel opacity combinations beyond `layers_and_tags`;
- Aseprite's `--data` JSON next to `describe` (frames, tags, slices);
- Hue / Saturation after Aseprite's 1.3.15 fix (`blend_saturation_bug` and the asefile goldens
  predate it: they are checked with `legacyHsl`);
- files the writer makes opened by Aseprite itself (the manual `aseprite_compat` gate).
