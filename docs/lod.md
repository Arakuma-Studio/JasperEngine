# Mesh detail levels (LOD) and cross-fade

A mesh far from the camera draws a simpler copy of itself: a **detail level**. JasperEngine makes
up to three levels for every 3D mesh (LOD 1-3, each about half the triangles of the one before)
and picks, every frame, the coarsest level whose difference from the full mesh projects to less
than the scene's **Threshold** in pixels. A level change cross-fades: for half a second both
levels draw through a fine screen-space dither, so the mesh never pops (spec S54).

New projects have LOD on. A scene saved before LOD existed keeps it off; turn it on in the scene's
settings.

## Where the levels come from

- **Cooked models** (the Model Importer, S53) carry their levels in the cooked `.glb`
  (`JE_mesh_lods`): a game reads them, nothing is simplified on the device.
- Anything else (a model the cook has not processed yet, a mesh made from a script) gets its levels
  built at load on a background job, with the same recipe the cook uses.
- A level's **error** is how far its surface is from the full mesh, in the mesh's units, measured
  at its vertices, edge midpoints and triangle centres. Selection compares it, at the mesh's scale,
  with the size of a pixel at the mesh's distance.
- **Who gets levels:** 3D triangle meshes drawn with the built-in Mesh or Lit shader, skinned ones
  included (their bone weights guide the simplification). Terrains (they have their own),
  sprites, tilemaps and polygons (drawn pixel for pixel), and meshes drawn with a custom or library
  shader (its vertex stage may move the vertices) keep their full mesh.

## Settings

**Scene settings, Mesh Detail (LOD):**

| Setting | Default | What it does |
|---|---|---|
| Enabled | on | Off: no levels are built or drawn |
| Threshold | 1 px | The error a level may show on screen. 2-4 px saves more on phones |
| Cross Fade | on | Level changes fade with a dither. Off: no fade shaders are built or shipped |
| Cross-fade Duration | 0.5 s | How long a fade lasts (0.05-2 s) |

**Mesh (Inspector, under Detail Levels):**

| Setting | Default | What it does |
|---|---|---|
| Detail Levels | on | Off: this mesh always draws its full mesh |
| Detail Bias | 1 | Above 1 keeps detail longer, below 1 drops it sooner |
| Fade Mode | Cross Fade | None: the mesh switches levels at once |
| Animate Cross-fading | on | Fades over the scene's Cross-fade Duration. Off: fades over a band of distance |
| Fade Transition Width | 0.1 | With Animate Cross-fading off, the share of each level's distance band the fade takes. Instanced meshes always fade this way |
| Cull Below Screen Height | 0 | The mesh is not drawn when it is shorter than this share of the view (0: never). It fades out with Cross Fade on |

**Project Settings > Quality:** **LOD Bias** (1) multiplies every mesh's Detail Bias.
**Shadow LOD Bias** (1) does the same for what shadow maps draw: shadows already cast from about
one level coarser than the camera would pick, and 2 brings them back to the camera's rule. A
Device Tier multiplies both again (`docs/device-tiers.md`).

## What a fade costs

- A fading mesh draws twice (once per level) for the fade's duration, with a variant of its
  shader that discards the dithered pixels. Every other draw keeps its own shader, so the GPU keeps
  its early depth test and hidden surface removal for them; fading draws come after the others.
- Nothing fades, and nothing extra is built, when a mesh does not change level, is under 8 pixels
  tall, was not drawn the frame before, or when the camera cuts (the error budget jumps 2x in a
  frame). Those changes switch at once.
- The fade clock is game time in Play (a paused game holds its fades) and editor time in Edit mode,
  so the Scene view fades too.
- With Cross Fade off, Mesh LOD off or Fade Mode None, no fade shader is collected, compiled or
  shipped.
- The depth pre-pass leaves fading meshes out; SSAO, the scene depth input and the SSR G-buffer
  draw the level a fading mesh shows most of. Shadow maps never fade: they switch at their own,
  light-relative levels. Cameras that render to textures draw one level for their own view.

## Seeing what LOD does

- **Scene view Draw Mode** (the toolbar's first dropdown in 3D scenes): **Shaded**,
  **Wireframe**, **Shaded Wireframe**, and under Mesh Analysis **LOD Level** (each mesh tinted
  by its level: LOD 0 green, 1 blue, 2 amber, 3 red; a fading mesh shows both, dithered) and
  **Overdraw** (each layer of the meshes adds heat; bright areas shade their pixels many times).
  The mode is saved with your layout and never affects a game.
- **Stats** (the chart button next to the Draw Mode, and the Game view's Stats): draw calls,
  triangles and vertices, meshes per level (`LOD 0: 120, 1: 40, 2: 8, 3: 2`), meshes fading and
  culled for their size, and the GPU memory meshes take.
- **Agents:** `capture_model_lods` (MCP group `meshes`) renders a contact sheet of a model's or an
  entity's levels with their triangles, errors, the screen height each is drawn below and how much
  it differs from LOD 0; `get_render_stats` returns the same counters as Stats.

## Lua

```lua
local mesh = Mesh(scene, self.entity)
mesh.fadeMode = LodFadeMode.CROSS_FADE
mesh.cullBelow = 0.02          -- not drawn under 2 % of the screen's height
mesh:forceLod(2)               -- always LOD 2 (-1: automatic again)
print(mesh.currentLod)
scene.crossFadeDuration = 0.3
```

`docs/scripting.md` lists every property.
