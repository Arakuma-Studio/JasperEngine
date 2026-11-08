# Procedural Materials

A Procedural Material is a node graph (`.procmat`, kind `procedural_material`) the editor bakes into ordinary
PNGs and an ordinary `.material`. A game never sees the graph: link the baked `.material` like any other.

Two tool groups: `graphs` (`list_graph_node_types`, `get_graph`, `validate_graph`, `edit_graph`, `list_graph_kinds`)
and `materials` (`preview_procedural_material`, `bake_procedural_material`). Enable both:
`tool_groups {enable: ["graphs", "materials"]}`.

## Writing a graph

- Find nodes with `list_graph_node_types {kind: "procedural_material", search: "brick"}` (or `category`, `ids`).
  Each node is one line: `perlin(scale_x=4[1..32] ...) -> output:f`, inputs before the `;`, parameters with
  their default and range, outputs with their type (`f` grey, `rgb`, `rgba`). Without a filter you get the
  categories and their counts. `detail: true` adds the full ports and parameters.
- Every graph has one `material_output` node. Its inputs are the bake's outputs: `base_color`, `normal`,
  `occlusion`, `roughness`, `metallic` (the three are one ORM image), `emission`, `height`.
- Leave `normal` unconnected when `height` is connected: the bake makes the normal from the float height, in
  the engine's convention, steeper with `normal_strength` (1 is Material Maker's Normal Map at strength 1).
  A connected `normal` (a library normal node's) is converted from Material Maker's convention.
- Edit with `edit_graph` (one undo step) or write the file yourself: a direct edit reports its problems in the
  next result's `external_changes`. A misspelled parameter or node type is an error with a did-you-mean.

## Previewing

`preview_procedural_material {path}` evaluates a `.procmat` on the editor's device without baking it, or
measures a `.material`, an image, or `entity` + `submesh`. It changes nothing.

- The contact sheet: top row flat, the lit sphere, a 2x2 tiling; bottom row one cell per output. `sheet` picks
  parts (`flat`, `sphere`, `tiled`, `channels`); `sheet: []` returns the metrics only.
- `size` (default 512) and `seed` are the evaluation's; `seed` defaults to the graph's `metadata.seed` (what the
  window's lock, reroll and Variations keep), else 0, and a call's `seed` is never written back. The same graph, seed and size give the same
  numbers on one device; `device: {tier, name}` names it (`gpu`, or `software` for llvmpipe and lavapipe).
- `checks` rows have S35's shape. A FAIL counts rounds: past `materials.qa.fix_rounds` in a row, stop and report.
- `fail_on: "fail" | "warn"` makes a reached verdict an error.
- For a graph, `fixes` lists the single edits the rows point at (`{check, node, op}`, an `edit_graph` op): a seam made
  by a scale with a fraction, a Normal input the bake could make from Height instead.
- Past `timeout_seconds` the call is an error with no image: a sheet is never an older material's.

| Check | Fails or warns when | Threshold |
|---|---|---|
| `tileability` | FAIL: a wrap edge's seam score passes it (synthesize_texture's score) | `materials.qa.seam_fail` 1.5 |
| `repetition` | WARN: the pattern repeats visibly when tiled (autocorrelation peak) | `materials.qa.repetition_warn` 0.9 |
| `histogram` | WARN: base colour clipped to black or white | `materials.qa.clipped_percent` 1 |
| `base_color` | WARN: non-metal albedo outside sRGB 30..240 | `materials.qa.dielectric_outside_percent` 10 |
| `normal` | FAIL: not unit length, or facing away; WARN: green against the height's slope (DirectX) | `materials.qa.normal_length_error` 0.05 |
| `roughness` | WARN: flat (p99 - p1) | `materials.qa.roughness_min_range` 0.05 |
| `metallic` | WARN: too many texels between metal and dielectric | `materials.qa.metallic_mid_percent` 10 |
| `height_8bit` | WARN: a shader of the material samples the 8-bit height (parallax steps) | none |

Thresholds are project settings (`set_project_settings`), never tool arguments; a result lists the ones that
differ from their defaults. `metrics.cost` estimates the cooked textures per platform.

`reference` (an image of the project) adds an advisory comparison with no status: `palette_delta_e` (colour
distance of the dominant colours), `spectrum` (difference of the detail scales) and `edges` (edge density).
Numbers cannot judge a look: show the sheet to the user.

## Baking

`bake_procedural_material {path}` writes, by default into a folder named after the graph beside it
(`assets/mats/stone.procmat` -> `assets/mats/stone/`):

- `stone_base_color.png` (sRGB), `stone_normal.png`, `stone_orm.png` (red occlusion, green roughness, blue
  metallic), `stone_emission.png` (sRGB), `stone_height.png` (one channel), for the outputs the graph has
  (`outputs` picks some); each `.meta` states its import settings;
- `stone.material` with the slots filled (the ORM in both the occlusion and metallic-roughness slots).

It is one undo step (`undo` in the result has its id). A re-bake changes only the bake's slots: every other
field of the `.material` stays, and so does a factor you tuned on a baked slot. A bake of some `outputs` leaves the
others' slots as the earlier bake left them; an output the graph no longer has loses its slot (and emission its
factor). A bake still running when another project opens writes nothing. It is refused while the graph has
errors, and a PNG at an output's path that is not this graph's bake needs `overwrite: true`. `fail_on` writes
nothing when the baked images' checks reach it. Defaults: size 1024 (64 to 8192), the graph's `metadata.seed`.

Every baked PNG records how it was made (a `jasper.procmat` chunk the export strips). When the graph changes
since (a value, a connected or disconnected output, the graph's seed when the bake took it), the bake is stale: the
Project window's badge turns yellow, the Inspector says so, and `check --materials` warns. Moving nodes, a device or
tier change, or a seed the bake was given, does not make a bake stale. Nothing re-bakes on its own.

## From a shell

- `jasperengine-editor check --project <dir> --materials`: graph problems (errors) and stale bakes (warnings),
  compiler style.
- `jasperengine-editor procedural-material bake <file.procmat> [--size n] [--seed n] [--outputs a,b] [--output dir]`:
  the bake through the editor that has the project open, else a one-shot headless one, else (no display, no Xvfb)
  in-process on the CPU tier. Exit codes: 0 baked; 1 the bake failed; 2 usage (an unknown flag, a size that is no
  power of two); 3 the connection was lost, timed out or interrupted, or another editor holds the project;
  4 internal.
