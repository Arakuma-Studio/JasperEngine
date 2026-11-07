# Pixel-art feature family: ledger

The logbook for specs S41–S52: the goals, the decisions shared by every spec, what is done, what
is in progress and what is left. Update it whenever a spec changes state. The newest entries go at
the top of the log.

## Goal

Make JasperEngine the best engine for pixel-art games made by people, by AI agents, or by both
together.

- Pixel art renders correctly by default: integer scaling, one pixel size for everything, pixel
  snapping, and lossless import.
- Aseprite is the drawing tool for people. Agents draw through MCP tools that edit the same
  `.aseprite` files, so a person and an agent can work on one sprite.
- Agents can clean up, check and measure art: grid recovery, palette snapping, craft lints,
  contact sheets and the pixel-art rule validator.
- Palettes, pixel effects, pixel particles, a grid tilemap with autotiling, pixel fonts and
  instanced sprites complete the 2D toolset.

## Decisions made by the owner (2026-11-04)

- Aseprite is the editing tool. There is no in-editor sprite editor or painting canvas. The
  engine has a read-only Sprite Preview panel, plus MCP drawing tools for agents.
- There is no hosted or AI image generation. Agents only draw, clean up and check art.
- debuff (debuffjp/debuff) is out of scope. spryte and tenten (same GitHub org) stay in.
- This is a private engine: code from any source may be copied, whatever its licence. Copied code
  keeps its header and gets a `NOTICE.md` entry naming where it came from.
- Specs first (S41–S52). Then one branch and one worktree per spec, implemented by parallel
  agents. When a spec is fully done, it is merged into `main` (after merging `origin/main`) and
  pushed.

## Sources studied

The research clones are under `/tmp/pixelart-research/<name>`. They are read-only and untrusted:
never build or run them.

| Source | What we take |
|---|---|
| Pixelorama (MIT) | Drawing algorithms (pixel-perfect line, scanline fill, hue-shift shading, symmetry), cleanEdge, OmniScale and rotxel rotation, tilemap peering autotile, the indexed palette model, and the Aseprite importer as a reference |
| tenten (MIT, debuffjp) | The agent drawing model: atomic batches of ops, an echo after each edit, rendering to PNG, diffs, errors with hints, and the `.aseprite` read/write codec |
| ritmo (MIT) | The pixel-art fixer (grid detection, palette snapping by vote, speck cleanup, lost colours, outline repair), slicing AI-made sprite sheets, craft lints, and verdicts with exit codes |
| pixelfx (MIT) | Effect techniques (dithered fade, dissolve, ramp recolour, transitions), particle presets and time-driven (closed-form) particles, 27 palette ramps, contact sheets and effect lints |
| spryte (MIT) | One pixel-snap rule shared by the CPU and the shaders, shared-edge snapping, bulk sprites with optional per-instance streams, auto-measured bitmap fonts, and the pixel-parity test method |
| @asynkimo post | The pixel-art rendering rules behind S41 and S42 |

## Shared conventions (every spec follows these)

- **Branches:** `feature/sNN-<slug>`. **Worktrees:** `.worktrees/sNN` (locally excluded from git).
- **Runtime code:** `engine/core/pixelart/`. **Editor code shared by the family:**
  `editor/pixelart/`.
  - Code homes outside `pixelart/`, accepted: `engine/core/sprite/` (sheet record, pool, geometry;
    S44, S45 adds sources), `engine/core/render/sprite/` (instancing, S52), `engine/core/tilemap/`
    and `editor/tilemap/` (S50).
  - `PixelImage` (RGBA8 plus an optional index plane), `PixelProblem.h` (the typed error and the
    `Verdict`) and `lint/LintTypes.h` (the lint result types) are owned by S44.
  - `Rgba8` (`engine/core/pixelart/Rgba8.h`, `{r, g, b, a = 255}`) is the family's one 8-bit colour,
    owned by S43 and used by S44's `PixelImage` (whichever of the two lands first adds it).
  - OKLab and CIEDE2000 colour maths is owned by S43, and so is the one Bayer table
    (`engine/shaderlib/includes/bayer.glsl`), which S48 and S49 include.
  - The pixel-snap rule is S41's: `floor(p + 0.5 + 2⁻⁷)` in base pixels, CPU `pixelart::snap*`
    (`PixelSnap.h`), GLSL `pixel_snap.glsl` (`pixelSnapRound`, `pixelSnapClip`,
    `pixelSnapCenterDelta`, `pixelSnapRotationStep`), frame-block members `pixelSnap` and
    `pixelSnapOffset`; Steps90 rounds rotations to 90° and snaps per vertex, Rotate On Grid keeps
    the angle and snaps the pivot. No spec rounds on its own.
  - The `.aseprite` codec lives in `editor/pixelart/aseprite/` (S44). The sprite-sheet runtime
    (`jasperengine.spritesheet` v1, `SpriteSheetPool`, `ImportedAssets`, the packer, the Sprite's
    `sheet`/`frame`/`frameName`) is S44's; S45 builds texture sheets and atlases on it.
  - Pixel-art lints live in `editor/pixelart/lint/`: the catalogue and `lint()` are S46's; the
    effect lints and the three rule lints `off_palette`, `partial_alpha`, `flicker` are S48's
    (`EffectLints`, S48 lands first and needs them); S47, S49 and S42 reuse them.
  - One validator: S42's `check_pixel_art` / `check --pixel-art`. Checks other specs hand over
    are its rules (`palette` from S43, `font_antialiasing` from S51, `particles_grid` from S49,
    `sprite_variants` from S48, `cleanup_outputs` registered by S46; S50 adds Tilemap Layers).
- **New asset types**, reserved values (`AssetType` is append-only and stored in `assetmap.bin`).
  The number is the reserved one whatever order the specs land in: each entry is written with its
  explicit value and `assetTypeFromName` skips values a sibling has not added yet.
  - `Palette = 24` (`.palette`, S43)
  - `AsepriteSprite = 25` (`.aseprite`/`.ase`, S44)
  - `SpriteAtlas = 26` (`.spriteatlas`, S45)
  - `Tileset = 27` (`.tileset`, S50)
  - Bitmap fonts are the existing `Font = 7` (`.bitmapfont`, `.fnt`; S51)
- **Format versions.** No spec fixes a scene or prefab version number. A spec that bumps takes the
  next version at landing, with a migration and a migration test. S44 is the only bump planned (its
  sheet keys, moved from S45); S45, S50 and S52 add optional keys only.
- **Shader bits** (engine property bits, uber features, particle bits) are appended at landing, in
  landing order, with fixed names: S41's `Pxs`/`Psb` (`UBER_SHARP_BILINEAR`), S43's
  `UBER_PALETTE`, S48's `UBER_GLITCH`, S49's `Pixel`/`Analytic`, S52's instancing bits.
- **`library:sprite`** after the family: 13 features (7 today, S43's PALETTE_SWAP and
  PALETTE_CYCLE, S48's DITHER_FADE, DISSOLVE, RAMP_RECOLOR, DROP_SHADOW) and one keyword group
  (`OUTLINE_STYLE`); the manifest has no feature-count limit; 7 of 8 fragment texture bindings. The
  per-instance FX slots (S52's `i_fx`, every neutral value 0): `fx.x` S43 palette row, `fx.y` FLASH
  (declared by S52), `fx.z` S48 DITHER_FADE, `fx.w` S48 DISSOLVE. All four are taken.
- **MCP groups:**
  - `pixel_art` (tools from S42, S43, S46, S48; S43 adds the group row in wave 5A)
  - `sprites` (S44, S45, S47; S44 adds the row)
  - `tilemaps` (new, S50); `edit_tilemap` and `get_tilemap` stay in core
  - Existing groups extended without new tools: `particles` (S49), `profiling` (S52),
    `editor_ux` (S41), core (S44, S45, S50, S51)
  - Each group stays within 12 tools and 15,000 characters and each new tool within 1,500
    (`tools/mcp/budget.json`; `exceptions` are for tools that predate the limit, not new ones).
    Extend an existing tool before adding a new one. Totals: "MCP budget" below.
- **Agent guide:** the `## 2D and 3D` section has no room, so the family shares one new digest
  section, `## Pixel art` (S41 creates it, S42 owns its wording), one line per spec, ≤ 1,300 bytes
  in all: S41+S42 ≈ 150, S43 ≈ 90, S44+S45+S47 ≈ 220, S46 ≈ 130, S48 ≤ 120, S50 ≈ 130, S51 ≤ 120,
  S52 ≤ 140. A line names only tools that exist when it lands.
- **Agent behaviour:**
  - A preview comes first; the same arguments plus `output: true` write the result.
  - Checks return PASS, WARN or FAIL with the measured values.
  - Verdicts are `clean`, `fixable` or `needs_person`.
  - Errors are typed: `{kind, message, fix, field}`.
  - Output is deterministic.
  - One tool call is one undo step.
- **Every user-facing feature** gets editor UI with Unity-like names and undo, a Lua API when it
  is runtime behaviour, MCP tools, docs under `docs/`, and an agent guide page under
  `docs/mcp/guide/`.

## Implementation order

- **Wave 5A:** S41, S43, S44, S52 (no spec needs another wave; within the wave, S43↔S44 and
  S52↔S41/S44 degrade gracefully in either order).
- **Wave 5B:** S42, S45, S48, S49, S51 (on wave 5A; same-wave reads are optional).
- **Wave 5C:** S46, S47, S50 (on 5A and 5B; S47's lint step follows S46 M1).

## MCP budget (estimates from the specs' §6 tables; `tool_budget.py --check` measures at landing)

| Group | Tool (spec) | Est. chars |
|---|---|---|
| `pixel_art` | `check_pixel_art` (S42) | ≈ 1,250 |
| | `edit_palette` (S43) | ≤ 1,450 |
| | `apply_palette` (S43) | ≤ 1,350 |
| | `fix_pixel_art` (S46) | ≤ 1,450 |
| | `lint_pixel_art` (S46) | ≤ 1,100 |
| | `capture_contact_sheet` (S48) | ≤ 1,450 |
| | **total** | **6 tools, ≈ 8,050 of 15,000** (7 and ≈ 8,500 with S51's `preview_font` fallback) |
| `sprites` | `set_sprite_import_settings` (S44) | ≤ 1,100 |
| | `edit_sprite_atlas` (S45) | ≤ 1,300 |
| | `get_sprite_atlas` (S45) | ≤ 900 |
| | `create_sprite` (S47) | ≤ 850 |
| | `get_sprite` (S47) | ≤ 350 |
| | `read_sprite_pixels` (S47) | ≤ 750 |
| | `edit_sprite` (S47; ops open in the schema, closed in the handler) | ≤ 1,450 |
| | `render_sprite` (S47) | ≤ 1,200 |
| | `diff_sprite` (S47) | ≤ 750 |
| | **total** | **9 tools, ≈ 8,650 of 15,000** |
| `tilemaps` | `edit_tileset` (S50) | ≤ 1,500 |
| | `import_tilemap` (S50) | ≤ 1,250 |
| | **total** | **2 tools, ≈ 2,750 of 15,000** |
| `particles` | `create_particle_effect` +130, `capture_particles` +380, `add_particle_module` +10 (S49) | +520, 11 tools (no new tool) |
| `profiling` | `get_render_stats` +220 (S52) | S15's own 7,000 entry; raised to the measured size if needed, still under 15,000 |
| `editor_ux` | `configure_game_view` +60 (S41) | within 24,000 / 21 |
| core | `set_sprite_frames` (S44 +110, S45 +70, description −120: stays ≤ its 2,075 exception), `get_asset_info` (+60 S44, +40 S45, +45 S51), `view_asset` (+35 S44, +170 S51), `write_file` +15 (S51), `get_tilemap` +400 and `edit_tilemap` ≤ its 2,523 (S50) | ≈ +830 to the default listing, each spec with a fallback |

### Upstream MCP rule changes (origin/main e8142a81e, merged 2026-11-04): these override the specs

- The agent guide is now ≤ 8,192 bytes, with ≥ 1,500 bytes of headroom, and the default listing
  is ≤ 99,000 characters.
- An area gets one entry in the guide's "Other areas" index: its key tool and
  `jasper://docs/<area>`. It gets a digest section only for rules agents broke without one.
- Any new guide line needs a case in `tools/mcp/eval` that fails without it. Hints agents must
  see go in `structuredContent`.
- So S41's planned `## Pixel art` guide section becomes an "Other areas" entry, `pixel_art`:
  `check_pixel_art`, `jasper://docs/pixel-art`. Every other spec's guide text goes in
  `docs/mcp/guide/<area>.md`. Record this in each spec's Deviations section.

## Specs

| ID | Name | Wave | Depends on | Branch | State |
|---|---|---|---|---|---|
| S41 | Pixel Perfect Camera | 5A | none | feature/s41-pixel-perfect-camera | in review |
| S42 | Pixel Art Project Mode & Validator | 5B | S41, S43, S44 | feature/s42-pixel-art-mode | spec done |
| S43 | Palettes | 5A | none (S44's codec for Aseprite palettes, same wave) | feature/s43-palettes | in review |
| S44 | Aseprite Import | 5A | none (S41's PPU, same wave) | feature/s44-aseprite-import | in review |
| S45 | Sprite Atlas | 5B | S44 | feature/s45-sprite-atlas | spec done |
| S46 | Pixel Art Cleanup | 5C | S43, S44, S42, S48 | feature/s46-pixel-art-cleanup | spec done |
| S47 | Agent Sprite Drawing | 5C | S43, S44, S42, S46 M1 (same wave) | feature/s47-agent-sprite-drawing | spec done |
| S48 | Pixel FX | 5B | S41, S43, S44, S52 | feature/s48-pixel-fx | spec done |
| S49 | Pixel Particles | 5B | S41, S43, S44 (S48's lints, same wave) | feature/s49-pixel-particles | spec done |
| S50 | Grid Tilemap | 5C | S41, S44, S52, S45, S42 | feature/s50-grid-tilemap | spec done |
| S51 | Pixel Fonts | 5B | S41 | feature/s51-pixel-fonts | spec done |
| S52 | Sprite Instancing | 5A | S41, S44 (same wave, optional) | feature/s52-sprite-instancing | waits for `dc/sprite-batching` on origin/main; scope narrowed |

The states, in order: spec in progress → spec done → implementing → in review → merged and pushed.

## Build infrastructure

`docs/specs/IMPLEMENTING.md` describes remote build servers (`rb.sh`, servers A/B/C). They don't
exist on this host, and the owner declined renting a box. Builds therefore run on this host.

- Docker 29.1.3 is installed here. The host has 8 cores, 15 GB RAM and 7 GB swap.
- `/var/lib/orquester/workspaces/jaspersito/je-remote/rb.sh` is a local stand-in with the same
  interface: `rb.sh <A|B|C> <cmd>`, `--img '<cmd>'` (inside the Linux image) and
  `--fetch <src> <dst>`.
  - The server letter is ignored.
  - It runs in the current worktree and waits for one of 2 global slots, with `JOBS=4` and
    `DOCKER_CPUS=4` each.
  - ccache is shared between worktrees, since each worktree mounts at `/src`.
- Real Aseprite 1.3.18.6, built from source in Docker for golden files (personal use, never
  redistributed): `/var/lib/orquester/workspaces/jaspersito/tools-aseprite/aseprite -b <args>`.
- `build/fixture-unity` (the MCP fixture project) is not available. MCP tests that need it are
  skipped, and reports must say so.

### Landing gate

**Owner's order (2026-11-05): no full suites at all, not even at the end.** Verification uses the
lighter, targeted runs below, for every spec and for the final check.

Per spec, the merged tree passes:
- `WERROR=1 tools/build.sh linux`, and the unit tests built with `RUN=0`;
- `tools/check-changed`;
- the spec's own unit tests and the suites it can affect, run with `UNIT_SUITE` and
  `--gtest_filter`;
- the spec's MCP test files, run as named files or with `-k`;
- `web` and `android` builds when runtime code changed;
- the area lints, plus the regenerated `TOOLS.md` and `tool_budget.py --check`.

`build/fixture-unity` (TappyPlane) isn't on this host, so tests that need it skip and the reports
list them. There is no CI.

## Log

- 2026-11-07: S44 review fixes are done at `5e68ac54b` (main `cbc4ee7fb` merged).
  - Passing: `WERROR` builds; filtered unit tests (core 15, editor 252); 183 MCP tests across 20
    files; `aseprite_compat.py` against real Aseprite (637/640 pass, 3 skipped where Aseprite's
    own exports disagree); lints.
  - `check-changed` was skipped, since its plan would be every test (no-full-suites rule).
  - `test_reflection` and `test_saved_output_determinism` failed on the merged tree, with
    differences in non-S44 fields. They're being checked on `main` `ac10792fb` to show whether
    they already fail there.
  - Three codec time budgets are over on this loaded host and are recorded in the spec.
  - Scene/prefab versions are 8/9; `AsepriteSprite = 25` (`main` has `SoundEvent = 28`).
- 2026-11-06: S41 review: 1 blocker (shader cache not bumped, so stale library shaders read a
  zero post colour) and 7 should-fix issues (shadow maps snapped, uniform size, input mapping in
  Integer Expand, test gaps, Vulkan/GLES3 not run, HiDPI, UI toggles). They were sent back to the
  implementer.
  - Shader bit rule: S41 keeps BLIT/uber bit 18 (`Psb`), and S43 renumbers `UBER_PALETTE`
    (`Pal`) to the next free bit at its final merge.
  - The S43 review (1 blocker: palette bindings missing from the cooked-scene runtime fields, so
    the prebuilt player loses Palette Swap; 5 should-fix) was also sent back to its implementer.
- 2026-11-06: S41 implementation reported at `47fdcec4d`, with main `fa493bae4` merged.
  - Passing: `WERROR` linux; 3,172 unit tests through `check-changed`; 8 `PixelPerfectGpu` tests
    on glcore; the web gate at device pixel ratio 2; android; 62 named MCP tests; lints.
  - Shader bits taken: `Pxs` = MESH 42 and DEPTH 19, UI 5, PARTICLES 11, BLIT `Psb` 18.
  - Pixel-snapped draws are not auto-instanced, and batchers must keep `pixelSnapDraw` and use
    `pixelSnapApply`. The Doriax lead was told, for `dc/sprite-batching`.
  - An independent review is running.
- 2026-11-06: S43 implementation reported at `b481a0454`, with main `fa493bae4` merged.
  - Passing: `WERROR` linux; filtered unit tests (core 248, editor 377); 12 MCP files; web and
    android builds; lints and generators; the smoke test.
  - Gaps it reported: AC7/AC12 verified only on glcore; AC11 waits for S52; AC14's GPU budgets
    need a GPU; AC19 has no goldens; Aseprite palettes wait for S44's codec. An independent review
    is running.
- 2026-11-06: The implementers stopped on a revoked auth token, after merging the S53–S73 `main`
  (S41 `5bb89cc5a`, S43 `872e0330e`, S44 `c17d9dd31`). `main` fast-forwarded again to `4f5899c00`
  (187 commits) and all three were revived. To avoid repeated large merges, each merges `main`
  once more only right before its final gate. `dc/sprite-batching` is still in progress (S52 keeps
  waiting).
- 2026-11-05: Owner: skip full suites entirely, even at the end. Work is verified with lighter
  runs only: `check-changed`, filtered unit tests, named MCP files, and the `WERROR` builds.
- 2026-11-05: The S41 implementer stopped on an API weekly rate limit, and the session
  restarted. All background jobs died. `main` fast-forwarded to `origin/main` `547a6e964` (344
  commits from other programs: S53–S73). The S41, S43 and S44 implementers were revived and told
  to merge it. `dc/sprite-batching` is still in progress, so S52 keeps waiting. Branch heads at
  revival: S41 `3323f4878`, S43 `6e5872eed`, S44 `d0f812e3c` (plus uncommitted review fixes).
- 2026-11-04: Owner's order: no full suites until the very end. The S43 implementer's queued
  unfiltered unit-test run was cancelled. The brief and the landing gate now use filtered runs
  only, and all three implementers were told.
- 2026-11-04: S44 implementation reported at `a06bf6491`, with `main` merged in.
  - Passing: `WERROR=1` linux, unit-tests, web and android builds; 5,733 unit tests; the MCP
    suite with `tools/fixtures/tween-curve` standing in for the missing fixture.
  - Still open: the Sprite Preview editor test and screenshots, `aseprite_compat.py`, and
    real-Aseprite goldens.
  - An independent review is running. The Aseprite CLI is being built from source in Docker
    (`/var/lib/orquester/workspaces/jaspersito/tools-aseprite/`) to author real goldens.
- 2026-11-04: Baseline `tools/build.sh unit-tests` of `main` (87 min): core 2,808 passed and
  1 failed (`NavBakeTest.RuntimeUpdateTiles`, failing on `main` before any pixel-art code);
  editor 2,842 passed and 7 disabled.
- 2026-11-04: Upstream rewrote `AGENTS.md` as the only manual (`CLAUDE.md` = `@AGENTS.md`)
  and removed CI. It now requires `WERROR=1`, a `tools/mcp/eval` case for new MCP text, migration
  fixtures, and the full suite before big pushes. `main` merged it and was pushed. The implementer
  brief was updated, and the S41, S43 and S44 implementers were told to merge `main` and follow
  the new rules.
- 2026-11-04: The split with the Doriax catch-up program is agreed and recorded in both ledgers.
  - `dc/sprite-batching` (their program, started now) owns generic dynamic batching: sprites,
    small meshes and UI quads; 2D reordering; settled objects; buffers shared by content; and the
    batch counters in `get_render_stats`.
  - It provides two hooks for S52: a sort key the batcher respects (layer and order first;
    batching only within a bucket), and a per-vertex or batch-key channel for per-sprite
    parameters (the S43 palette row, S48 flash/fade/dissolve, the S41 snap).
  - S52 narrows to Sorting Layers, Order in Layer and Sorting Groups, the Sprite Batch component
    (a bulk instanced API), and making the pixel-art per-sprite parameters batch-compatible
    through those hooks. It starts once `dc/sprite-batching` is merged on origin/main; the spec is
    re-scoped then.
  - `mesh-memory` and S44: whoever lands second merges onto the first. Their program adds no
    `AssetType`, so 24–27 stay ours. Shader, variant and update-flag bits take the next free bit
    at landing, in landing order, on both sides.
- 2026-11-04: `origin/main` brought the Doriax catch-up program (`docs/doriax-catchup/LEDGER.md`,
  19 specs in `docs/superpowers/specs/`). Its `sprite-batching` spec duplicates most of S52
  (dynamic sprite batching, 2D reordering, shared buffers, batch stats), so S52 was stopped before
  any code was written.
  - Proposed to that program's lead: they keep generic batching. S52 narrows to Sorting
    Layers/Groups, a Sprite Batch bulk API, and keeping the pixel-art per-sprite parameters
    batch-compatible. It lands after their sprite-batching. Waiting for their reply.
  - Other overlaps flagged to them: `mesh-memory` and S44 (`SpriteComponent` frames);
    `ui-components` and S45 (9-slice); `unicode-text` and S51; `agent-authoring` and the guide.
- 2026-11-04: Wave 5A implementation started: S41, S43, S44 and S52, each in
  `.worktrees/sNN` on its feature branch from `main` 7fe6b0b36. Each implementer follows
  `.worktrees/IMPLEMENTER-BRIEF.md` (local, not committed). A baseline unit-test run of `main` is
  in progress.
- 2026-11-04: All 12 specs are written (S41–S52) and a consistency pass reconciled them: snap
  rule, asset type numbers, the Bayer table, tool groups and budgets, FX slots, and the wave
  order. Docker is installed on this host. The baseline `tools/build.sh linux` of `main` passed:
  55 min cold with `JOBS=4`, 11 warnings. The specs were merged into `main` and pushed. Wave 5A
  (S41, S43, S44, S52) is next.
- 2026-11-04: Consistency pass over S41–S52: `get_sprite` (S47) everywhere; asset types reserved
  24–27; S41's snap rule and rotation policy in S49–S52; one Bayer table (S43); S42's validator
  takes the hand-offs of S43, S48, S49, S51 and S46; S44 owns the sprite-sheet runtime and lands
  before S45; version bumps take the next number at landing; `edit_sprite` within 1,500 characters
  (no exception); shared types moved to the earliest wave (S44: `PixelProblem`, `LintTypes`; S48:
  three rule lints); one `## Pixel art` guide section; waves 5A/5B/5C in `docs/specs/README.md`.

- 2026-11-04: Research done on all 7 sources. The owner chose Aseprite over an in-editor sprite
  editor, ruled out hosted image generation, dropped debuff, and allowed copying code with any
  licence. The licence rule was removed from `CLAUDE.md` and from the global decisions in
  `docs/specs/README.md`. Writing of specs S41–S52 started on `specs/pixel-art`.
