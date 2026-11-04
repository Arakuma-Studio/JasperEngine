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
  - `PixelImage` (RGBA8 plus an optional index plane) is owned by S44.
  - OKLab and CIEDE2000 colour maths is owned by S43.
  - The `.aseprite` codec lives in `editor/pixelart/aseprite/` (S44).
  - Pixel-art lints live in `editor/pixelart/lint/` (owned by S46, reused by S47 and S48).
- **New asset types** (appended to `AssetType`):
  - `Palette` (`.palette`, S43)
  - `AsepriteSprite` (`.aseprite`/`.ase`, S44)
  - `SpriteAtlas` (`.spriteatlas`, S45)
  - `Tileset` (`.tileset`, S50)
  - Bitmap fonts use the existing font pipeline where possible (S51)
- **MCP groups:**
  - `pixel_art` (S41, S42, S43, S46, S48)
  - `sprites` (S44, S45, S47)
  - The existing `particles` group for S49, and the existing tilemap tools and their group for
    S50.
  - Each group stays within 12 tools and 15,000 characters (`tools/mcp/budget.json`). Extend an
    existing tool before adding a new one.
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

## Specs

| ID | Name | Depends on | Branch | State |
|---|---|---|---|---|
| S41 | Pixel Perfect Camera | none | feature/s41-pixel-perfect-camera | spec in progress |
| S42 | Pixel Art Project Mode & Validator | S41 | feature/s42-pixel-art-mode | spec in progress |
| S43 | Palettes | none | feature/s43-palettes | spec in progress |
| S44 | Aseprite Import | none | feature/s44-aseprite-import | spec in progress |
| S45 | Sprite Atlas | S44 | feature/s45-sprite-atlas | spec in progress |
| S46 | Pixel Art Cleanup | S43, S44 | feature/s46-pixel-art-cleanup | spec in progress |
| S47 | Agent Sprite Drawing | S43, S44, S46 (lints) | feature/s47-agent-sprite-drawing | spec in progress |
| S48 | Pixel FX | S43 | feature/s48-pixel-fx | spec in progress |
| S49 | Pixel Particles | S43, S41 | feature/s49-pixel-particles | spec in progress |
| S50 | Grid Tilemap | S41, S44 | feature/s50-grid-tilemap | spec in progress |
| S51 | Pixel Fonts | S41 | feature/s51-pixel-fonts | spec in progress |
| S52 | Sprite Instancing | S41 | feature/s52-sprite-instancing | spec in progress |

The states, in order: spec in progress → spec done → implementing → in review → merged and pushed.

## Build infrastructure

`docs/specs/IMPLEMENTING.md` describes the remote build servers (`rb.sh`, servers A/B/C). They
are not present on this host, and Docker is not installed here. Builds and tests therefore run on
a rented Linux build box. The procedure for using it is recorded below once the box is set up.

## Log

- 2026-11-04: Research done on all 7 sources. The owner chose Aseprite over an in-editor sprite
  editor, ruled out hosted image generation, dropped debuff, and allowed copying code with any
  licence. The licence rule was removed from `CLAUDE.md` and from the global decisions in
  `docs/specs/README.md`. Writing of specs S41–S52 started on `specs/pixel-art`.
