# JasperEngine: how mesh optimization and rendering features should appear to AI agents

Audit of the MCP server, CLI and Lua API at commit `b4f7816ea` (`forge-meshopt`, main), read only.

**How to read the labels.** `[V file:line]` = verified by reading that line. `[I]` = inferred from reading code, not
run. `[M]` = measured by a script of mine, outside the repo, on repo text; no editor ran. `[A]` = an assumption or a
design choice to confirm. No build, editor or MCP call ran: **`tool_budget.py --check` was not run**, so every budget
number below is a static estimate. Run the gate on a build server before relying on them.

---

## 0. Headline findings

1. **`assets_pipeline` and `profiling` are full on tool count.** `assets_pipeline` is 12 of 12, `profiling` 7 of 7,
   so "add to the group" needs a budget exception. A new group is the sanctioned route. `[V docs/mcp/TOOLS.md:7, tools/mcp/budget.json:20-23,37-41, docs/mcp/conventions.md:117-119]`
2. **The agent guide has about 110 bytes free**, not 1,610: the gate requires 1,500 bytes of headroom, and the guide
   is about 14,774 of the 14,884 it may use. `[M from editor/mcp/McpInstructions.cpp]` The rules forbid raising the
   limit or trimming other digests. `[V docs/mcp/conventions.md:210-212]` The digest has to be one clause.
3. **Core has little room**: about 96,300 of 100,000 characters, likely an underestimate. `[M, see 1.4]` So new work
   must not add parameters to core tools; output fields cost nothing against the budget.
4. **LOD already exists at runtime and is nearly invisible to agents.** `MeshLodPool` runs
   `meshopt_simplifyWithAttributes`, selection uses screen-space error, and it is exposed by two scene settings, two
   Mesh properties and two Scene properties. `[V engine/core/pool/MeshLodPool.cpp:160-171, engine/core/subsystem/RenderSystem.cpp:8222-8233, editor/mcp/tools/SceneTools.cpp:816-817, engine/core/script/binding/GameObjectClassesLua.cpp:1842-1843, engine/core/script/binding/CoreClassesLua.cpp:3198-3199]`
   It is **off by default**, has no error budget (`target_error` is `FLT_MAX`), no agent doc, and no test of its behaviour. `[V engine/core/SceneSettings.h:354, editor/resources/templates/3d/Main.scene:8, engine/core/pool/MeshLodPool.cpp:167,170; grep of docs/mcp/guide, tools/mcp/tests and engine/tests finds none]`
5. **`result_error` is not a pure geometric distance.** It is in mesh units here (`meshopt_SimplifyErrorAbsolute`) but
   with attributes it includes the normal and UV quadric terms. `[V engine/core/pool/MeshLodPool.cpp:20,160-167; engine/libs/meshoptimizer/simplifier.cpp:2527-2528,2622]` Keep it for selection; report an independently sampled deviation for verification.
6. **The `triangles` stat counts every draw**, shadow and depth passes included. `[V engine/core/render/ObjectRender.cpp:128]` A "150k triangles" budget must say which number it means.
7. **`jasperengine-editor call` already works as a CI gate** if the tool ends with `call->fail(text, structured)`; no
   CLI change is needed. `[V docs/mcp/cli.md:129-135, editor/cli/CallCommand.cpp:568-571]`

**Proposal in one line:** a new group `meshes` with 4 tools (about 4,350 characters `[M]`), output-only additions to
6 existing tools, new `quality.*` project settings, and a `check --meshes` CLI flag.

---

## 1. What exists today

### 1.1 Tools that touch this area

| Tool | Group | What it gives for geometry | Gap |
|---|---|---|---|
| `get_asset_info` | core | model: mesh names, **primitive counts**, materials, nodes, parsed from the glTF JSON only `[V editor/mcp/tools/AssetTools.cpp:386-406]`; an FBX reads its glTF conversion and never converts `[V AssetTools.cpp:925-937]` | no triangles, vertices or bytes, though accessors hold them |
| `list_assets` | core | `type: model`, `details` adds sizes and image dimensions `[V AssetTools.cpp:3774-3792]` | no geometry numbers |
| `view_asset` | core | model thumbnail `[V AssetTools.cpp:3815-3820]` | one image, no levels |
| `get_model_info` | core | a loaded Model's mesh GameObjects, bones, clips, morphs `[V editor/mcp/tools/WorldTools.cpp:5852-5860]` | requires an entity; submesh count only, no triangles or LODs `[V WorldTools.cpp:5056-5070]` |
| `set_model` | core | loads a model, `merge_static_meshes` `[V WorldTools.cpp:5862-5875]` | the only import-like option |
| `set_mesh_geometry` | core | primitives only, "not for models" `[V editor/mcp/tools/ComponentTools.cpp:1279-1285]` | unrelated |
| `set_scene_settings` / `get_scene_settings` | core | `mesh_lod_enabled`, `mesh_lod_threshold` (pixels) `[V SceneTools.cpp:816-817]` | no per-platform value |
| `set_properties` | core | `lodEnabled`, `lodBias`, `cullDistance` (instanced) `[V editor/reflect/components/RenderReflect.cpp:190-191,324]` | no forced level, no read of level or error |
| `set_texture_settings` | core | sampler state on a component, not the file `[V docs/mcp/TOOLS.md:2434-2440]` | not import settings |
| `get/set_texture_import_settings`, `reimport_textures`, `get_texture_cook_report` | assets_pipeline | S14, the closest analogue `[V AssetTools.cpp:4688-4755]` | textures only |
| `find_references`, `get_asset_dependencies`, `list_missing_assets` | assets_pipeline | who uses a model, without loading scenes `[V docs/mcp/guide/assets.md:160-169]` | reusable for use counts |
| `get_render_stats` (`jasper://stats`) | profiling | draws, instances, triangles, passes, alive resources `[V editor/mcp/tools/ProfilerTools.cpp:167-200]` | no culled, no LOD histogram, no vertex or index memory |
| `get_frame_times` | profiling | always-on history plus named monitors, including `Draw calls`, `Triangles` `[V ProfilerTools.cpp:213-263, engine/core/profiler/EngineProfiling.cpp:157-160]` | new monitors cost no schema |
| `get_gpu_passes`, `capture_pass_output`, `capture_profile`, `get_cpu_profile` | profiling | per-pass triangles and images `[V ProfilerTools.cpp:1062-1150]` | enough to split main from shadow |
| `get_vram_report` | profiling | category `Meshes (VB/IB)`, owners `[V engine/core/profiler/GpuMemory.h:32,50; ProfilerTools.cpp:950-984]` | usage flags exist but are not reported separately `[V GpuMemory.h:110-111]` |
| `get_shadow_stats`, `get_atmosphere_info`, `get_post_processing_status`, `get_particle_stats` | various | feature-level "what ran" pattern | none for meshes |
| `screenshot`, `configure_game_view` | core, editor_ux | captures; simulates texture family and atmosphere tier `[V docs/mcp/TOOLS.md:1501-1524, editor/mcp/tools/ViewTools.cpp:1660-1678]` | **no image diff tool for agents**; `pngdiff.py` is test-only `[V tools/mcp/pngdiff.py:1-14]` |
| `bake_vertex_animation` | animation | preview plus contact sheet, then same arguments with `output` `[V editor/mcp/tools/VatTools.cpp:1305-1343]` | the dry-run precedent |

Below MCP, other relevant facts:

- CLI: `check` has no mesh flag `[V docs/mcp/checks.md:10]`; `benchmark --mesh-lod on|off` overrides the setting and records draws `[V editor/cli/CommandLine.cpp:348, editor/App.cpp:2968-2970,4467]`.
- `meshoptimizer` 1.2 is vendored and already ships in every game `[V engine/libs/meshoptimizer/meshoptimizer.h:15, NOTICE.md:30, engine/notices.json:29]`.
- glTF import reports unsupported extensions, but without a fix `[V engine/core/subsystem/MeshSystemGltfPbr.cpp:31-37,138-146]`.
- There is no model import settings class. `TextureImportSettings` under `import.textureImporter` in the `.meta` is the slot to copy `[V editor/texture/TextureImportSettings.h:120-156]`.

### 1.2 How LOD works today (it constrains the tool design)

- Up to `MAX_MESH_LODS` = 4 levels; fixed targets 1, 0.5, 0.25, 0.125. `[V engine/core/Engine.h:93-94, MeshLodPool.cpp:18,159]`
- **LODs are index ranges over the same vertex buffer**, so a level shrinks triangles and index bytes but never vertex bytes, and adds index memory. `[V engine/core/pool/MeshLodPool.h:26-31, MeshLodPool.cpp:151]`
- Built at load as `Background Heavy` jobs named `mesh-lod-simplify`; `get` returns null while pending. `[V MeshLodPool.h:54, MeshLodPool.cpp:125-135]` So a measurement right after enabling reads level 0. `[I]`
- Stops when a level shrinks less than 10%; meshes under 8 vertices or 12 triangles get none. `[V MeshLodPool.cpp:145,173]`
- Selection: `allowed = threshold * (perUnit * distance + constant) / bias`; a level qualifies while `lodError[l] * worldScale <= allowed`. `perUnit = 2 tan(fov/2) / viewHeight`. `[V RenderSystem.cpp:8193-8207,8226-8232]` **So results depend on view height**, and a mesh-level error is the max across submeshes. `[V RenderSystem.cpp:8180-8190]`
- From that formula each level has a computable switch distance: `d = (err * scale * bias / threshold - constant) / perUnit`. `[I]`
- Culling is CPU: `isBatchVisible` (frustum), `isLayerVisible` (layer masks), instance `cullDistance`, and instance buckets per level. `[V RenderSystem.cpp:13895, 9394, engine/core/component/InstancedMeshComponent.h:113,128]` GPU-driven culling is on the do-not-copy list, so the design must not imply it. `[V docs/specs/README.md:23]`
- Toggling `meshLodEnabled` rebuilds every mesh's levels. `[V RenderSystem.cpp:14567,14743-14744]`

### 1.3 The closest analogues, and the pattern to copy

**S14 texture tools** (`editor/mcp/tools/AssetTools.cpp`):

| Aspect | What it does |
|---|---|
| Registration | `registerTextureTools`, one module row with group `assets_pipeline` `[V McpTools.cpp:47]` |
| Parameters | `paths[]`, `settings` (`anyObject`), `platform` enum; report takes `platforms[]`, `include_preview` `[V AssetTools.cpp:4707-4748]` |
| Validation | handler checks each key and lists valid ones; checks **every** file before writing any `[V AssetTools.cpp:4471-4535]` |
| Undo | one `InteractiveEdit` of `SetAssetMetaCmd`, `isNoOp` skips unchanged, reply says "Undo with scope project" `[V AssetTools.cpp:4538-4557]` |
| Long work | `reimport_textures` uses `wait`, `timeout_s`, `waitUntil`, **no job**, and `timed_out` `[V AssetTools.cpp:4614-4623]`. This predates the job rule and is not the model to copy `[I]` |
| Failure | a failed cook is `call->fail(text, report)`, so structured data survives `[V AssetTools.cpp:4611]` |
| Play | setters `allowedWhilePlaying = false`; readers default true `[V AssetTools.cpp:4717,4733]` |
| Budget | test caps the 4 tools at 2,400 characters `[V tools/mcp/tests/test_texture_import.py:75-78]` |
| Text | one short line; data in `structuredContent` `[V AssetTools.cpp:4556-4557]` |
| CLI | `cook-textures --check` exits 1; `check --textures` lists problems `[V docs/mcp/guide/textures.md:67-70]` |

**S15 profiling tools** (`ProfilerTools.cpp`): all read-only and allowed while playing `[V docs/specs/S15-profiling-diagnostics.md:316]`.
`capture_profile` is the one to copy for measuring: `readOnly` with `ToolActivity::Control`, because it keeps views drawing while it waits. `[V ProfilerTools.cpp:1062-1078]`
A caveat that bites budgets: edit mode only draws when something changes, and the tools that wait keep the view drawing. `[V docs/mcp/guide/profiling.md:65-66]`

**VAT and synthesis** are the dry-run pattern: "Without output nothing is written ... the same arguments + output write exactly that". `[V VatTools.cpp:1309-1310]` But they are annotated as edit tools, so they are refused when "Allow changes" is off, dry run included. `[V McpServer.cpp:1132-1134,1253-1259]` A dry run should be in a read-only tool. `[I]`

**Jobs:** `replyWhenJobDone(call, job, wait, timeoutSeconds, onFinished)` returns `{job_id, status: "running"}` past the timeout and forwards progress. `[V editor/mcp/McpJobs.h:113-118]` `publish_verify` is the "job fails with a problem list" precedent. `[V PublishTools.cpp:173-174]`

### 1.4 Budget left

| Scope | Limit | Used | Left | Source |
|---|---|---|---|---|
| Default listing (core) | 150 tools, 100,000 chars | 139, about 96,300 | 11 tools, under 3,700 chars | `[V TOOLS.md:7, budget.json:16-19; M]` |
| Group default | 12 tools, 15,000 chars | | | `[V budget.json:20-23]` |
| `assets_pipeline` | default | **12**, about 7,100 | **0 tools**, 7,900 chars | `[V TOOLS.md:7; M]` |
| `profiling` | 7 tools, 7,000 chars | **7**, about 4,660 (4,523 at S15's merge `[V S15:558]`) | **0 tools**, about 2,300 chars | `[V budget.json:37-41; M]` |
| `rendering_fx` | default | 10, about 4,760 | 2 tools, 10,200 chars | `[V TOOLS.md:7; M]` |
| `editor_ux` | 21 tools, 24,000 chars | **21**, about 14,600 | 0 tools | `[V budget.json:52-56; M]` |
| `diagnostics` | 16 tools | **16** | 0 tools | `[V budget.json:32-36; TOOLS.md:7]` |
| Each tool | 1,500 chars | | | `[V budget.json:24]` |
| Each description | 400 chars (review rule) | | | `[V docs/mcp/conventions.md:76-77]` |
| Guide, whole | 16,384 bytes, 1,500 headroom = **14,884 usable** | about 14,774 | **about 110 bytes** | `[V budget.json:25-30; M]` |

How the estimate was checked `[M]`: I rebuilt each `inputSchema` from `TOOLS.md` and compared with sizes recorded in
`budget.json`. Flat tools land within 4% (`import_files` 882 vs 882, `wait` 1515 vs 1534, `build_project` 1844 vs 1879).
Tools with nested objects undercount by up to 40% (`synthesize_texture` 1446 vs 2341). Core has nested tools, so its
real number is above 96,300 and its true headroom is smaller. `configure_game_view` estimates at 1,538 against its
1,500 limit with no exception, so the estimate is a little high there; either way it is **at its limit and cannot
take a parameter**. `[M, V budget.json:58-143 has no entry for it]`

**Consequences:** tools go in a new group; extensions are output-only; only `get_render_stats`'s description changes,
and it has 28 characters left (372 of 400 `[M]`), so it must be reworded (draft below is 394).

---

## 2. Agent UX design

### 2.1 Rules I designed to

- **Four tools, one per job an agent can't already do.** Everything else is an output field on an existing tool.
- **A dry run is the real cook**, cached. Predicted numbers are therefore exact, and applying afterwards is a cache hit. `[A]` This is what makes "predicted" trustworthy, and it is testable.
- **Headers first.** Whole-project scans read only the glTF JSON. Accessor counts and buffer view lengths give triangles, vertices and bytes without touching a buffer `[I]`; `check --anims` already reads the `.glb` JSON chunk this way. `[V docs/mcp/checks.md:219-222]`
- **Every finding carries a fix an agent can paste**, as `get_asset_info`'s `regenerate` arguments do. `[V docs/mcp/checks.md:194-196]`
- **Findings use the `check` shape** `{tool, path, line, col, severity, message, code, suggestion}` so MCP and CLI agree. `[V docs/mcp/checks.md:57-83]`
- `inspect_` rather than `audit_`: `audit` is not in `VERBS`, and a verb is added only when none fits. `[V tools/mcp/tests/test_tool_conventions.py:37-55, docs/mcp/conventions.md:34-35]`

### 2.2 Group and registration

```cpp
// editor/mcp/McpToolGroups.cpp, after assets_pipeline
{"meshes", "Mesh import optimization, compression, LOD, cooked meshes and render budgets."},
// editor/mcp/tools/McpTools.cpp, TOOL_MODULES
{registerMeshTools, "Meshes: import optimization, LOD and render budgets", "meshes"},
```

```json
// tools/mcp/budget.json, groups
"meshes": {"chars": 4800, "tools": 4, "spec": "S41's 4 tools (mesh import, LOD and render budgets), within 4,800 characters"}
```

Why not `assets_pipeline`: it is full, needs an exception, and a session wanting only geometry would pay for texture
synthesis and asset identity too (about 7,100 chars `[M]`). The new file `editor/mcp/tools/MeshTools.cpp` must be
listed in `CMakeLists.txt`. `[V docs/mcp/DESIGN.md:152]` S41 is the next free number. `[V docs/specs/README.md:109; A]`

### 2.3 The four tools

Sizes are `[M]` from a script applying `McpSchema.cpp`'s output rules to these drafts. Enabling the group costs **4,334
characters, about 1,080 tokens**; a session that doesn't pays nothing but 8 guide bytes.

| Tool | Chars | readOnly | destructive | idempotent | activity | allowedWhilePlaying |
|---|---|---|---|---|---|---|
| `inspect_meshes` | 1,051 | yes | no | yes | Auto (Read) | yes |
| `set_mesh_import_settings` | 1,391 | no | no | yes | Auto (Edit) | **no** |
| `check_render_budgets` | 995 | yes | no | no (a measurement) | Control | yes |
| `capture_mesh_lods` | 897 | yes (`writesFiles` on `save_path`) | no | yes | Control | yes |

#### `inspect_meshes`: discover and dry-run

> Rank models by geometry cost without loading them (glTF headers, cached): triangles, vertices, bytes, levels, findings with fixes. scene: weigh by uses there. detail full reads buffers (vertex cache, overdraw, fetch). With paths and settings: a dry run, nothing written, predicted numbers and error per level. Guide: jasper://docs/meshes.

```json
{
 "type": "object",
 "properties": {
  "paths": {
   "type": "array",
   "items": {
    "type": "string"
   },
   "description": "Omit for all models."
  },
  "scene": {
   "type": [
    "integer",
    "string"
   ]
  },
  "platform": {
   "type": "string",
   "enum": [
    "desktop",
    "mobile",
    "web"
   ]
  },
  "sort_by": {
   "type": "string",
   "enum": [
    "triangles",
    "scene_triangles",
    "bytes",
    "findings"
   ],
   "default": "triangles"
  },
  "limit": {
   "type": "integer",
   "minimum": 1,
   "maximum": 500,
   "default": 20
  },
  "detail": {
   "type": "string",
   "enum": [
    "header",
    "full"
   ],
   "default": "header"
  },
  "settings": {
   "type": "object",
   "additionalProperties": true,
   "description": "As set_mesh_import_settings; needs paths."
  },
  "wait": {
   "type": "boolean",
   "default": true
  },
  "timeout_seconds": {
   "type": "number",
   "minimum": 1,
   "maximum": 3600,
   "default": 120
  }
 }
}
```

- `detail: "header"`: JSON chunk only, no buffers. `"full"`: adds `meshopt_analyzeVertexCache`, `analyzeOverdraw`, `analyzeVertexFetch` and duplicate-vertex counts, cached by content hash as `TextureCookCache`'s `sources.json` does. `[V editor/texture/TextureCookCache.h:4-8; A]`
- `settings` needs `paths` (at most 16) and runs as a job past `timeout_seconds`. It is `anyObject` to avoid paying the settings schema three times; the same validator rejects a bad key and lists valid ones. `[A]`
- An FBX not yet converted is reported `not_converted`, never converted, matching `get_asset_info`. `[V AssetTools.cpp:925-935]`
- Results are written only to `.jasperengine/cache`, not project content. `[A]` Confirm against "Allow changes" policy: `get_asset_info` on an FBX counts as writing files because it makes a visible file. `[V docs/mcp/DESIGN.md:54-56]`

Text summary, about 300 bytes:

```
41 models, 3 with errors for mobile (0.12 s, 38 cached). Worst: assets/models/rock.glb 812,400 tris, 58.2 MB, no LOD, used 120x in scenes/Forest.scene. Dry run: inspect_meshes {paths: ["assets/models/rock.glb"], settings: {preset: "mobile"}}. Showing 20 of 41 (truncated).
```

`structuredContent`, ranking (about 400 bytes a row; 20 rows is about 2,000 tokens):

```json
{
  "platform": "mobile", "scene": "scenes/Forest.scene", "detail": "header", "sort_by": "scene_triangles",
  "total": 41, "scanned": 41, "cached": 38, "truncated": true,
  "totals": {"triangles": 2140233, "vertex_bytes": 91210000, "index_bytes": 18400000, "errors": 3, "warnings": 11},
  "models": [{
    "path": "assets/models/rock.glb", "guid": "4b1e9c0d...", "triangles": 812400, "vertices": 430112,
    "submeshes": 3, "materials": 2, "vertex_bytes": 44290000, "index_bytes": 9748800, "index_type": "u32",
    "attributes": ["POSITION", "NORMAL", "TEXCOORD_0"], "skinned": false, "morph_targets": 0,
    "uses": {"scene_objects": 3, "scene_instances": 120, "scene_triangles": 97488000},
    "settings": {"source": "preset", "preset": "balanced", "stored": false, "lod_levels": 1},
    "cook": {"status": "none"},
    "findings": [{
      "tool": "mesh", "path": "assets/models/rock.glb", "line": 0, "col": 0, "severity": "error",
      "code": "mesh-over-triangle-limit",
      "message": "812,400 triangles, over the mobile per-model limit of 100,000",
      "suggestion": "generate LODs and reduce it",
      "apply": {"paths": ["assets/models/rock.glb"], "settings": {"preset": "mobile", "lod_levels": 4}}
    }]
  }]
}
```

`structuredContent`, dry run. The levels of `selected` come from the same code the cook runs:

```json
{
  "dry_run": true, "written": false, "cache_hit": false, "ms": 1840,
  "models": [{
    "path": "assets/models/rock.glb", "platform": "mobile",
    "settings": {"preset": "mobile", "optimize": ["dedup", "vertex_cache", "overdraw", "vertex_fetch"], "compress": "meshopt", "quantize": "balanced", "lod_levels": 4, "lod_ratio": 0.5, "lod_max_error": 0.02},
    "before": {"triangles": 812400, "vertices": 430112, "vertex_bytes": 44290000, "index_bytes": 9748800, "acmr": 1.92, "overdraw": 2.4, "overfetch": 3.1, "disk_bytes": 54038800},
    "after":  {"triangles": 812400, "vertices": 412008, "vertex_bytes": 13184256, "index_bytes": 3120000, "acmr": 0.71, "overdraw": 1.3, "overfetch": 1.02, "disk_bytes": 7820000},
    "levels": [
      {"level": 0, "triangles": 812400, "ratio": 1.0, "error_mesh": 0.0, "switch_distance": 0.0},
      {"level": 1, "triangles": 406200, "ratio": 0.5, "error_mesh": 0.0018, "switch_distance": 14.2},
      {"level": 2, "triangles": 203100, "ratio": 0.25, "error_mesh": 0.0061, "switch_distance": 48.1},
      {"level": 3, "triangles": 92050, "ratio": 0.113, "error_mesh": 0.0200, "switch_distance": 157.7, "stopped_by": "lod_max_error"}
    ],
    "assumptions": {"view_height": 720, "fov_deg": 60, "threshold_px": 2.0, "bias": 1.0, "scale": 1.0},
    "quantization": {"position_error_mesh": 0.00031, "note": "added to each level's error"},
    "warnings": []
  }]
}
```

`switch_distance` shows the assumptions it used, because the answer changes with view height. `[V RenderSystem.cpp:8196-8205,8226]`

#### `set_mesh_import_settings`: apply

> Partial update of models' import settings (with platform, its override); equal values write nothing. One project undo step, then a cook job: waits unless wait is false (job_id, get_job). Returns before/after numbers. Dry run first: inspect_meshes settings.

```json
{
 "type": "object",
 "properties": {
  "paths": {
   "type": "array",
   "items": {
    "type": "string"
   },
   "description": "Models."
  },
  "platform": {
   "type": "string",
   "enum": [
    "desktop",
    "mobile",
    "web"
   ]
  },
  "settings": {
   "type": "object",
   "properties": {
    "preset": {
     "type": "string",
     "enum": [
      "none",
      "balanced",
      "mobile",
      "web"
     ]
    },
    "optimize": {
     "type": "array",
     "items": {
      "type": "string",
      "enum": [
       "dedup",
       "vertex_cache",
       "overdraw",
       "vertex_fetch"
      ]
     }
    },
    "compress": {
     "type": "string",
     "enum": [
      "none",
      "meshopt"
     ]
    },
    "quantize": {
     "type": "string",
     "enum": [
      "none",
      "balanced",
      "aggressive"
     ]
    },
    "lod_levels": {
     "type": "integer",
     "minimum": 1,
     "maximum": 4
    },
    "lod_ratio": {
     "type": "number",
     "minimum": 0.05,
     "maximum": 0.9,
     "description": "Triangles kept per level."
    },
    "lod_max_error": {
     "type": "number",
     "description": "World units at scale 1."
    },
    "lod_attribute_weight": {
     "type": "number",
     "minimum": 0,
     "maximum": 1
    }
   },
   "additionalProperties": false
  },
  "reset": {
   "type": "array",
   "items": {
    "type": "string"
   },
   "description": "Settings back to the preset's."
  },
  "reimport": {
   "type": "boolean"
  },
  "wait": {
   "type": "boolean",
   "default": true
  },
  "timeout_seconds": {
   "type": "number",
   "minimum": 1,
   "maximum": 3600,
   "default": 120
  }
 },
 "required": [
  "paths"
 ]
}
```

- `settings` is **closed** here, so agents see enums in the definition; a closed object takes JSON only on the CLI, not `KEY=VALUE`. `[V docs/mcp/cli.md:97-98]` `reset` replaces S14's "null removes", which a closed schema can't state cheaply. `[A]` Flat `lod_*` keys and an `optimize` array keep it under 1,500; my first draft with nested `lod` and per-attribute bits measured 1,774. `[M]`
- Writes `import.meshImporter` through `SetAssetMetaCmd` (project history), one `InteractiveEdit`, `isNoOp` skipping equal values, every file validated before any write, as S14. `[V AssetTools.cpp:4501-4552]` So undo restores the `.meta`, and the content-addressed cache makes the revert fast. `[I]`
- **`SetAssetMetaCmd::setWriteListener` holds one listener, which the texture queue owns.** `[V editor/command/type/SetAssetMetaCmd.h:46-48]` A mesh queue needs that made multi-subscriber or a dispatcher by key. `[I]`
- Cook runs in a job (`jobs().create("mesh-cook", ...)`, `replyWhenJobDone`, `setCancelHandler`), `JobBusy::None`, since cooks run in the background. `[V McpJobs.h:29-34,117-118; A]`
- `structured.undo` is added by the server. `[V McpServer.cpp:1349, McpUndo.cpp:117]`
- `allowedWhilePlaying = false`, as texture setters. `[V AssetTools.cpp:4717]` Edit activity is refused while an export runs. `[V McpRegistry.h:34]`

Text, about 220 bytes:

```
Updated 1 model (1 unchanged): cooked for desktop, mobile, web in 2.1 s (0 failed). rock.glb mobile: 54.0 -> 7.8 MB; levels 812k/406k/203k/92k tris. Undo: undo {scope: "project"}. Check: capture_mesh_lods {path: ...}.
```

`structuredContent`:

```json
{
  "updated": ["assets/models/rock.glb"], "unchanged": 1, "undo": {"id": 812, "name": "claude: Set Mesh Import Settings (rock.glb)"},
  "job": {"job_id": "job-14", "status": "succeeded", "ms": 2104},
  "models": [{"path": "assets/models/rock.glb", "platform": "mobile", "status": "cooked", "cache_hit": false,
              "key": "9f3a...", "before": {"disk_bytes": 54038800}, "after": {"disk_bytes": 7820000}, "levels": [{"level": 1, "triangles": 406200, "error_mesh": 0.0018}]}],
  "failed": []
}
```

With `wait: false`, or past the timeout: `{"job_id": "job-14", "status": "running", "updated": [...], "undo": {...}}`. The setting is committed; only the cook is pending.

#### `capture_mesh_lods`: verify visually and numerically

> See and measure a model's detail levels: a CPU-drawn contact sheet (no GPU, reproducible) and per level triangles, error in mesh and world units, switch distance, PSNR and changed pixels against level 0. settings: preview before applying. entity: real shaders through the Game view at each level instead. Ranks nothing: inspect_meshes does.

```json
{
 "type": "object",
 "properties": {
  "path": {
   "type": "string",
   "description": "A model."
  },
  "entity": {
   "type": [
    "integer",
    "string"
   ]
  },
  "scene": {
   "type": [
    "integer",
    "string"
   ]
  },
  "settings": {
   "type": "object",
   "additionalProperties": true,
   "description": "As set_mesh_import_settings."
  },
  "levels": {
   "type": "array",
   "items": {
    "type": "integer",
    "minimum": 0,
    "maximum": 3
   }
  },
  "platform": {
   "type": "string",
   "enum": [
    "desktop",
    "mobile",
    "web"
   ]
  },
  "max_width": {
   "type": "integer",
   "minimum": 16,
   "maximum": 2048,
   "default": 768
  },
  "save_path": {
   "type": "string",
   "description": "Also write the PNG."
  }
 }
}
```

**Answering the brief's question: use all three, for different reasons.**

| Metric | Source | Use | Limit |
|---|---|---|---|
| `error_mesh`, `error_world` | `result_error`, times world scale | The selection's own number, so it predicts switching `[V RenderSystem.cpp:8229]` | Mixes in normal and UV error at weights 0.5 and 0.3 `[V MeshLodPool.cpp:20]` |
| `deviation_world` {max, rms} | sampled surface-to-surface distance with a fixed seed `[A]` | The honest "how far did it move", in metres | Silhouette only, not shading |
| `image` {psnr_db, changed_fraction, silhouette_iou} | contact sheet against level 0 | "Would a person see it" | CPU draw ignores textures |

`changed_fraction` should use `pngdiff.compare`'s rule (channel tolerance 24) so the tool and tests agree. `[V tools/mcp/pngdiff.py:9]`

- **Default (`path`): the CPU rasterizer of `VatContactSheet`.** Z-buffered, 2x2 supersampled, two-sided Lambert, base colours, no GPU, used in CLI and headless tests. `[V editor/vat/VatContactSheet.h:9-13]` Results don't depend on the machine, so they are assertable on llvmpipe and on a real GPU. `[I]`
- **`entity` mode** renders through the Game view at each forced level with real shaders. It catches what the CPU sheet can't: normal maps, UV seams, skinning. It needs a camera, and its numbers depend on GPU, so tests check them structurally. `[I]`
- `settings` previews before applying (shares the dry-run cook).
- `save_path` makes it `writesFiles`, as `screenshot`. `[V editor/mcp/tools/ViewTools.cpp:1957-1959]`
- This removes the need for a general image-diff tool. `screenshot` is core, so adding `compare_to` there would spend scarce core budget. `[I]`

Text, about 200 bytes:

```
rock.glb, 4 levels on mobile. L1 406k tris, 1.8 mm, PSNR 41.2 dB, 0.4% changed. L2 203k, 6.1 mm, 36.8 dB, 1.1%. L3 92k, 20 mm, 31.5 dB, 3.9%. Largest change: L3 silhouette IoU 0.981.
```

`structuredContent`, plus one PNG as a `ToolResult` image (never base64 in text `[V docs/mcp/conventions.md:83]`; the CLI writes a file `[V docs/mcp/cli.md:113-115]`):

```json
{
  "path": "assets/models/rock.glb", "platform": "mobile", "renderer": "cpu_sheet", "image_width": 768,
  "assumptions": {"view_height": 720, "fov_deg": 60, "threshold_px": 2.0, "bias": 1.0, "scale": 1.0, "views": ["front_right", "side"]},
  "levels": [
    {"level": 0, "triangles": 812400, "index_bytes": 2437200, "vertex_bytes": 13184256, "error_mesh": 0, "error_world": 0, "deviation_world": {"max": 0, "rms": 0}, "switch_distance": 0, "image": {"psnr_db": null, "changed_fraction": 0, "silhouette_iou": 1}},
    {"level": 1, "triangles": 406200, "index_bytes": 1218600, "vertex_bytes": 13184256, "error_mesh": 0.0018, "error_world": 0.0018, "deviation_world": {"max": 0.0014, "rms": 0.0003}, "switch_distance": 14.2, "image": {"psnr_db": 41.2, "changed_fraction": 0.004, "silhouette_iou": 0.998}}
  ],
  "warnings": ["cpu_sheet draws base colours only: check normal-mapped models with entity"]
}
```

`vertex_bytes` staying constant across levels is correct and worth stating, since LODs share the vertex buffer. `[V MeshLodPool.h:26]`

#### `check_render_budgets`: the gate

> Compare a scene with a platform's budgets (Project Settings > Quality, scene settings, or budgets). measured: Game view frames at the platform's view height and LOD threshold, needs a camera. static: the scene at LOD 0, no GPU. Over budget is an error listing each violation, limit and top offenders (exit 1 from jasperengine-editor call).

```json
{
 "type": "object",
 "properties": {
  "scene": {
   "type": [
    "integer",
    "string"
   ]
  },
  "platform": {
   "type": "string",
   "enum": [
    "desktop",
    "mobile",
    "web"
   ]
  },
  "budgets": {
   "type": "object",
   "properties": {
    "triangles": {
     "type": "integer"
    },
    "draw_calls": {
     "type": "integer"
    },
    "instances": {
     "type": "integer"
    },
    "vertex_bytes": {
     "type": "integer"
    },
    "index_bytes": {
     "type": "integer"
    },
    "frame_ms": {
     "type": "number"
    }
   },
   "additionalProperties": false
  },
  "source": {
   "type": "string",
   "enum": [
    "measured",
    "static"
   ],
   "default": "measured"
  },
  "frames": {
   "type": "integer",
   "minimum": 1,
   "maximum": 600,
   "default": 60
  },
  "wait": {
   "type": "boolean",
   "default": true
  },
  "timeout_seconds": {
   "type": "number",
   "minimum": 1,
   "maximum": 600,
   "default": 120
  }
 }
}
```

- `measured`: applies the platform's threshold and view height **for the measurement only**, waits for pending LOD builds, keeps the view drawing, samples `frames`, compares the p95. Without the wait it would read level 0. `[V MeshLodPool.h:54; docs/mcp/guide/profiling.md:65-66; I]`
- `static`: sums the scene at LOD 0 with no camera, no frustum and no GPU, so it is an upper bound on geometry and draws, which cannot see culling or shadows. `[A]`
- **No budget found is an error, not a pass**, with the fix. "A filter that matches nothing is not a pass." `[V AGENTS.md "Test" section]`
- Over budget uses `job->fail(text, result)`, the `publish_verify` pattern. `[V PublishTools.cpp:174]`

Fail text, about 280 bytes:

```
FAIL scenes/Forest.scene on mobile (measured, 60 frames, p95): triangles 212,400 > 150,000 (+42%); draw_calls 131 > 100 (+31%). Top: Rock (88,200 tris, 41 draws), Tree (61,000, 22). Shadows are 38% of triangles. Next: inspect_meshes {scene: "scenes/Forest.scene", sort_by: "scene_triangles"}.
```

Pass text: `OK scenes/Forest.scene on mobile (measured, 60 frames, p95): triangles 118,300 of 150,000; draw_calls 84 of 100; instances 1,240 of 5,000.`

`structuredContent`:

```json
{
  "ok": false, "scene": "scenes/Forest.scene", "platform": "mobile", "source": "measured", "frames": 60, "statistic": "p95",
  "conditions": {"view_height": 720, "threshold_px": 2.0, "camera": "Main Camera", "playing": false, "lod_pending": 0},
  "budget_source": "project.yaml renderBudgets (scene entry)",
  "values": {"triangles": 212400, "main_triangles": 131700, "draw_calls": 131, "instances": 1240, "vertex_bytes": 13184256, "index_bytes": 3120000},
  "budgets": {"triangles": 150000, "draw_calls": 100, "instances": 5000},
  "violations": [
    {"metric": "triangles", "value": 212400, "limit": 150000, "over_fraction": 0.416, "code": "render-budget-triangles"},
    {"metric": "draw_calls", "value": 131, "limit": 100, "over_fraction": 0.31, "code": "render-budget-draw-calls"}
  ],
  "offenders": [{"entity": {"id": 12, "path": "Forest/Rock"}, "model": "assets/models/rock.glb", "triangles": 88200, "draw_calls": 41, "lod": [31, 8, 2, 0], "suggestion": "inspect_meshes {paths: [\"assets/models/rock.glb\"]}"}],
  "next": {"tool": "inspect_meshes", "arguments": {"scene": "scenes/Forest.scene", "sort_by": "scene_triangles"}}
}
```

### 2.4 Output-only additions to existing tools

| Tool | Addition | Budget effect |
|---|---|---|
| `get_render_stats` | a `geometry` block (below) | description rewritten, +22 chars, 394 of 400 `[M]` |
| `get_frame_times` | monitors `Meshes drawn`, `Meshes culled`, `LOD0..3 triangles`, `Mesh VRAM` | none: names go in the `monitors` list |
| `get_vram_report` | `usage_bytes {vertex, index}` on the `meshes` group | none |
| `get_asset_info` | model `meshes[]` gain `triangles`, `vertices`, `bytes`; adds `import` summary | none |
| `get_model_info` | `meshes[]` gain `triangles`, `lod_levels`, `lod_error[]` | none |
| `set_project_settings`, `get_project_settings` | `quality.*` keys (3.1) | none: flat keys in rows `[V ProjectTools.cpp:1459-1460, SettingsRows.cpp:2743-2745]` |

```text
get_render_stats description:
A view's last drawn frame: draws, instances, triangles, passes, pipelines/bindings, uniform and upload bytes, alive resources, pipeline cache, cpu_ms, fps, gpu{available,reason,frame_ms,semantics}, geometry{culled,lod_histogram,vertex_bytes,index_bytes}, html_ui, shader_creation. One frame is one sample. gpu.frame_ms is null until GPU timing ran (get_gpu_passes arms it). Also jasper://stats.
```

**(e) Per-frame numbers while playing.** `get_render_stats` needs no change to stay allowed: it doesn't override the default `allowedWhilePlaying = true`. `[V McpRegistry.h:52, ProfilerTools.cpp:1026-1040]` Its numbers are added to `renderStatsJson`, so `jasper://stats` gets them too. `[V ProfilerTools.cpp:167,1156-1163]` The new block:

```json
"geometry": {
  "meshes": {"total": 412, "drawn": 188, "culled": {"frustum": 201, "layer": 12, "distance": 11}},
  "instances": {"total": 5400, "drawn": 1240, "culled": 4160},
  "lod_histogram": {"enabled": true, "draws": [41, 62, 58, 27], "instances": [120, 400, 520, 200], "triangles": [96000, 21000, 9400, 5300], "pending_builds": 0},
  "triangles_by_pass": {"main": 131700, "shadow": 76200, "depth": 4500},
  "triangles_saved_by_lod": 402100,
  "vertex_bytes": 13184256, "index_bytes": 3120000, "cooked_meshes": 18, "source_meshes": 2
}
```

The draw path needs a few plain integer increments, counted for the main view only. `[I]` `lod_histogram.enabled: false` makes "all level 0 because LOD is off" unambiguous, which matters as it defaults off. `[V SceneSettings.h:354]` Counters only: no pass, draw or allocation, so disabled costs effectively nothing. `[V CLAUDE.md "A disabled feature costs nothing"; I]`

Read medians, not one frame: `get_frame_times {monitors: ["Triangles", "Meshes culled"]}`. `[V docs/mcp/guide/profiling.md:50]`

`get_runtime_entity` shows the current level through the `mesh` component once `lodLevel` is reflected. `[A]`

### 2.5 Failures that say what to do

| Situation | Today | Proposed |
|---|---|---|
| glTF requires an extension the engine doesn't read | `glTF %s requires %s, which the engine does not read; it is loaded without it` `[V MeshSystemGltfPbr.cpp:140]`, in the console only | `assets/models/rock.glb requires EXT_meshopt_compression, which this editor can't read, so its geometry is unusable. Fix: re-export without meshopt compression (in gltfpack, leave out its compression flags), then set_mesh_import_settings {paths: [...], settings: {compress: "meshopt"}} compresses at cook time. [gltf-required-extension]`, also as a finding in `inspect_meshes` and `check --meshes` |
| Same for `KHR_draco_mesh_compression` | as above | as above, naming Draco; `KHR_mesh_quantization` is the case to confirm (not in `supportedExtensions` `[V MeshSystemGltfPbr.cpp:31-37]`; a required extension loaded without its support may misread attributes `[I]`) |
| Unknown setting | | `settings.lod_ratios is not an import setting. Valid: preset, optimize, compress, quantize, lod_levels, lod_ratio, lod_max_error, lod_attribute_weight. Did you mean lod_ratio?` (the CLI catches closed keys first, exit 2 `[V docs/mcp/cli.md:102-104]`) |
| Not a model | | `Main.scene is not a model in the assets directory (gltf, glb, obj, fbx).` (mirrors `AssetTools.cpp:4347`) |
| FBX not converted | | `assets/models/a.fbx has no glTF conversion yet: import_files or set_model makes it. Then inspect it.` (mirrors `AssetTools.cpp:935`) |
| Error budget can't be met | | Warning, not failure: `rock.glb L3: stopped at 92,050 tris (ratio 0.113) because lod_max_error 0.02 was reached; raise lod_max_error for fewer triangles.` |
| Simplify gains nothing | | `tiny.gltf: 8 triangles, below the 12 LOD needs: no levels made. [mesh-lod-too-small]` `[V MeshLodPool.cpp:145]` |
| Settings that conflict | | Names both keys and values and which to change, e.g. a `quantize` level a skinned attribute can't take |
| Skinned or morphed model with LOD | | Warning with the risk and the fix, to be settled with the pipeline spec `[A]` |
| LOD off in the scene | | Finding: `scenes/Forest.scene has mesh_lod_enabled false, so rock.glb's 4 levels are unused. Fix: set_scene_settings {settings: {mesh_lod_enabled: true}}. [mesh-lod-disabled-in-scene]` |
| No budget set | | `No budget for mobile: set_project_settings quality.render_budgets, or pass budgets.` (isError) |
| No camera for measured | | `scenes/Empty.scene has no camera to measure through: create one, set_main_camera, or source: "static".` |
| LOD builds pending past the timeout | | `3 LOD builds still running after 120 s: raise timeout_seconds. Not compared.` (isError) |
| While playing, `set_` refused | server's standard refusal `[V McpServer.cpp:1273]` | unchanged |
| Cook failed | | `reimport`-style `failed: [{path, platform, error, suggestion}]`, job failed, exit 1 |

Stable finding codes: `mesh-over-triangle-limit`, `mesh-no-lod`, `mesh-lod-disabled-in-scene`, `mesh-lod-too-small`, `mesh-unindexed`, `mesh-index-wider-than-needed`, `mesh-unwelded`, `mesh-vertex-cache-poor`, `mesh-overdraw-high`, `mesh-too-many-submeshes`, `mesh-float-attributes`, `mesh-cook-stale`, `mesh-cook-failed`, `mesh-not-converted`, `gltf-required-extension`, `render-budget-*`.

### 2.6 Digest for the agent guide

There isn't room for a `## Meshes` section. Measured `[M]`: a minimal section plus the group name takes 108 of about 110 bytes and would leave later specs 2. **Add a clause to the existing "Feature pages" line instead:**

```
meshes (inspect_meshes, check_render_budgets),
```

That is 47 bytes, plus 8 for the group name in the generated Tool groups section: total about 14,829 of 14,884, leaving about 55 bytes. Textures have no digest section at all, so this is in line with precedent. `[V editor/mcp/McpInstructions.cpp:193-198]` `test_autosync.py` will check that both tool names exist. `[V docs/mcp/conventions.md:212-213]`

Everything else goes in `jasper://docs/meshes`, the group description and the tool descriptions. **Do not** add "LOD defaults to off" to the guide; `inspect_meshes` reports it as a finding at the moment it matters.

### 2.7 `docs/mcp/guide/meshes.md` (draft)

Picked up automatically: `CMakeLists.txt` globs this folder. `[V CMakeLists.txt:385]` It also needs a row in `docs/mcp/guide/README.md`. `[V docs/mcp/guide/README.md:44-45]`

````markdown
# Meshes

For agents: find expensive geometry, try a change, apply it, check it, hold a scene to a budget.
The user reference is `docs/meshes.md`.

The tools are the `meshes` group: `tool_groups {enable: ["meshes"]}`. Each is callable by name
without it.

| Tool | Answers |
|---|---|
| `inspect_meshes` | which models cost most, why, and what a change would give (a dry run) |
| `set_mesh_import_settings` | applies settings: one undo step, then a cook job |
| `capture_mesh_lods` | what each level looks like, and how far it moved |
| `check_render_budgets` | does this scene fit this platform; which things do not |
| `get_render_stats` (profiling) | the frame's draws, triangles, culled, LOD histogram, vertex and index bytes |

## The loop

1. `inspect_meshes {scene: "scenes/Forest.scene", sort_by: "scene_triangles"}`. It reads only the
   glTF headers, so a whole project is cheap. Each finding has a `suggestion` and often `apply`,
   the arguments to try.
2. `inspect_meshes {paths: ["assets/models/rock.glb"], settings: {preset: "mobile"}}`: a dry run.
   Nothing is written. The numbers are the real cook's, not estimates, and the cook is cached.
3. `set_mesh_import_settings` with the same `paths` and `settings`. `undo {scope: "project"}`
   reverts it. Keep the `job_id` if it came back running, and poll `get_job`.
4. `capture_mesh_lods {path}`: read the contact sheet and the per-level numbers. For a model with
   normal maps, also `capture_mesh_lods {entity}`.
5. `check_render_budgets {scene, platform: "mobile"}`. An error lists each metric over and the
   worst objects. Fix them and repeat.

## Settings

| Setting | Values | Notes |
|---|---|---|
| `preset` | none, balanced, mobile, web | the base; other settings override it |
| `optimize` | dedup, vertex_cache, overdraw, vertex_fetch | changes order and sharing, not the shape |
| `compress` | none, meshopt | on disk and in the build; decoded at load |
| `quantize` | none, balanced, aggressive | smaller vertices; adds a little error |
| `lod_levels` | 1 to 4 | 1 means no LODs |
| `lod_ratio` | 0.05 to 0.9 | triangles each level keeps of the one before |
| `lod_max_error` | world units at scale 1 | stops a level before it moves the surface further than this |
| `lod_attribute_weight` | 0 to 1 | how much normals and UVs resist simplifying |

`reset: ["lod_ratio"]` puts one back to the preset's. With `platform` (desktop, mobile, web) the
settings are that platform's override. They live in the model's `.meta`, under
`import.meshImporter`; commit the metas. A sub-asset path (`models/hero.glb#mesh:Body`) overrides
one mesh.

## LOD: what to expect

- A scene draws levels only with `mesh_lod_enabled` true (off by default): `set_scene_settings`.
  `mesh_lod_threshold` is the pixels of error a level may show; a Mesh's `lodBias` above 1 keeps
  detail longer, and `lodEnabled` false keeps it at full detail.
- Levels are index ranges over one vertex buffer: they cut triangles, add a little index memory
  and never cut vertex memory. To cut vertex memory use `quantize`, `compress` or `dedup`.
- Switching depends on view height: a level switches at a distance, and that distance is shorter
  on a lower resolution. `switch_distance` lists the assumptions it used.
- Levels build in the background when not cooked. `lod_histogram.pending_builds` above 0 means
  the frame is not yet representative.

## Reading the error numbers

- `error_mesh`: the simplifier's error in mesh units; `error_world` is it times the scale. It
  drives switching. It includes normal and UV differences, so a flat coloured wall can report more
  than its surface moved.
- `deviation_world` {max, rms}: sampled surface distance. Use it to answer "how far did it move".
- `image`: against level 0, in the CPU contact sheet (base colours, no textures), so the numbers
  are the same on every machine. `changed_fraction` is the share of pixels that changed visibly.

No threshold fits all assets. As a start: hero models at under 1 mm of deviation at level 1,
background props where `changed_fraction` stays under 1 % at their switch distance.

## Budgets

Project budgets live in `quality.render_budgets` (`set_project_settings`), per platform with
per-scene entries; `budgets` on the call overrides them.

- `triangles` counts **every draw**, shadow and depth passes too; `main_triangles` in the result is
  the camera's. Over budget with a large shadow share? Lower shadow distance before simplifying.
- `source: "measured"` draws frames at the platform's view height and threshold. Measure at a
  representative camera: nothing is measured for a place the camera never goes. For a scene that
  spawns things, use `play` and `wait` first.
- `source: "static"` needs no GPU: every model at level 0, no culling. It is an upper bound.
- No budget is an error, never a pass.

## While playing

`get_render_stats` (one frame), `get_frame_times {monitors: ["Triangles", "Meshes culled"]}`
(series; compare medians), `get_vram_report {filter: "meshes"}`. `set_mesh_import_settings` is
refused while playing. Lua: `Profiler.frameStats()`, `Mesh.lodLevel`, `Mesh.forcedLod`,
`Engine.setMeshLodBias`. Culling is on the CPU: frustum, layers and distance.

## From a shell and in CI

```sh
jasperengine-editor check --project . --meshes --format json          # no display, no GPU
jasperengine-editor call --project . --json --wait-job check_render_budgets \
    --scene scenes/Forest.scene --platform mobile                     # exit 1 when over budget
```

Exit 0 pass; 1 over budget or another tool error (`structured.violations` tells them apart); 3 the
editor could not start or the project is locked. See `docs/mcp/cli.md`.

## Common mistakes

- Judging from one `get_render_stats` frame. Compare medians.
- Reading level 0 everywhere and concluding LOD is broken: check `mesh_lod_enabled` first.
- Comparing `triangles` with a budget meant for the main view.
- Trusting the CPU sheet for normal-mapped models: use `entity`.
- Editing `.meta` or `project.yaml` by hand while an editor is open.
````

---

## 3. Declarative approach

### 3.1 What fits the system

**Idempotent settings tool:** yes, `set_mesh_import_settings`. Equal values write nothing, and there's no new undo step. `[V isNoOp precedent AssetTools.cpp:4543]` A single `set_import_settings` covering textures and meshes isn't worth it: `set_texture_import_settings` can't be renamed without breaking agents, the setting sets are disjoint, and validation differs. `[I]` Note the texture setter isn't annotated `idempotent` even though it behaves so `[V AssetTools.cpp:4707-4720]`; the mesh one should be.

**Presets per platform.** New rows, following `quality.atmosphere_quality_*`. `[V editor/settings/SettingsRows.cpp:1671-1680,2743-2745]` No schema cost, discoverable via `get_project_settings {category: "quality", describe: true}`. `[V ProjectTools.cpp:1456-1463]`

| Key | Type | Default |
|---|---|---|
| `quality.mesh_preset_{desktop,mobile,web}` | enum: none, balanced, mobile, web | none, so old projects cook nothing and ship unchanged |
| `quality.mesh_lod_threshold_{desktop,mobile,web}` | float pixels | follows the scene's `mesh_lod_threshold` until set |
| `quality.mesh_lod_bias_{desktop,mobile,web}` | float | 1 |
| `quality.budget_view_height_{desktop,mobile,web}` | int | 1080, 720, 720 `[A]` |
| `quality.render_budgets` | object | empty |

Default `none` satisfies "Old projects must load: new fields get defaults". `[V CLAUDE.md]` Use three platform groups as atmosphere does. `graphics.shadow_tier.*` separates android and ios `[V SettingsRows.cpp:2738-2740]`; add those only if a real need appears. `[A]`

**Budgets:** in `project.yaml`, not in scene settings:

```yaml
renderBudgets:
  default:
    mobile: {triangles: 300000, drawCalls: 200, instances: 10000}
    web:    {triangles: 200000, drawCalls: 150}
  scenes:
    - {scene: scenes/Forest.scene, sceneGuid: 4b1e..., mobile: {triangles: 150000, drawCalls: 100}}
```

Why `project.yaml`: scene settings go through `SceneSettingsCodec` into games `[I]`, whereas budgets are editor-only, cost a shipped game nothing, and are committed so CI reads them. `[V Generator.cpp:1750 (project.yaml is committed)]` Reference the scene by GUID, since moves don't rewrite referrers. `[V docs/mcp/guide/assets.md:5-6]` Precedence: `budgets` argument, then scene entry, then `default`. Set through `set_project_settings`, never by hand. `[V McpInstructions.cpp:67-68]`

Agent phrasing of the brief's example:

```json
{"tool": "set_project_settings", "arguments": {"values": {"quality.render_budgets": {"scenes": [{"scene": "scenes/Forest.scene", "mobile": {"triangles": 150000, "draw_calls": 100}}]}}}}
```

The exact argument name should follow `set_project_settings`' schema. `[A]` YAML camelCase and MCP snake_case map as textures do. `[V AssetTools.cpp:4266-4277]`

### 3.2 Does this exist in another form?

No render budget gate exists. `[V grep of docs/mcp/guide, tools/mcp/tests and SettingsRows.cpp]` Neighbours:

| Existing | What it is | Why it isn't this |
|---|---|---|
| `tools/mcp/budget.json`, `tool_budget.py --check` | MCP definition sizes `[V budget.json:1-15]` | same shape, wrong subject; a good model for violation messages |
| `tools/perf/check_bench.py`, `bench-thresholds.json` | job system and allocators `[V docs/testing.md:17]` | repo only |
| `harness.over_budget` | test helper `[V tools/mcp/harness.py:586]` | tests only |
| `texture.memory_budget_mb.*` | a runtime cap that acts `[V SettingsRows.cpp:1523, docs/textures.md:120-121]` | no check, not geometry |
| particle world budget | runtime cap `[V docs/mcp/guide/particles.md:153]` | same |
| `cook-textures --check`, `check` | static gates `[V docs/mcp/checks.md:88; textures.md:67-70]` | the model for `check --meshes` |
| `benchmark --out` | measured FPS, draws `[V CommandLine.cpp:354; App.cpp:4467]` | numbers without limits; can be an alternative front end |
| `assert_ui` | declarative layout assertions | a UX precedent only |

### 3.3 CI: does `jasperengine-editor call` work, and what would it take?

**It works today for this.** Nothing in `CallCommand.cpp` needs changing. `[I]`

- Reaches hidden-group tools: `call` and `batch` reach every tool. `[V docs/mcp/cli.md:150-151]`
- Starts a one-shot headless editor with `--project` in a scratch directory, and never touches the user's settings. `[V docs/mcp/cli.md:66-73]`
- Exit codes: 0 ok, 1 tool error, including a failed `--wait-job` job, 2 usage, 3 connection, 4 internal. `[V docs/mcp/cli.md:129-135]`
- `structuredContent` survives an error, and `--json` carries it. `[V CallCommand.cpp:568-571,576-583]`
- Validation against the closed schema catches a bad budget key before anything runs, exit 2. `[V docs/mcp/cli.md:102-105]`
- Images go to `--image-dir`, usable as CI artifacts. `[V docs/mcp/cli.md:46,113-115]`

```yaml
- name: Geometry rules (no display, fast)
  run: jasperengine-editor check --project . --meshes --format json > meshes.json
- name: Render budgets (software GL)
  run: |
    jasperengine-editor call --project . --json --wait-job --timeout 600 \
      check_render_budgets --scene scenes/Forest.scene --platform mobile > budgets.json
```

For a scene whose content appears at runtime, one session via `batch`, whose exit is the highest of its calls `[V docs/mcp/cli.md:139-146]`:

```jsonl
{"tool":"play","arguments":{"action":"start","scene":"scenes/Forest.scene","fixed_fps":60}}
{"tool":"wait","arguments":{"frames":120}}
{"tool":"check_render_budgets","arguments":{"platform":"mobile"}}
```

What it takes, and the limits:

1. **The tool must fail with `isError`** (`job->fail`), not `ok: false` in a success. A one-line decision, and the most important. `[V CallCommand.cpp:569]`
2. **Exit 1 is ambiguous**: it also covers "no camera" or "no budget". CI that needs to tell them apart reads `structured.violations`. `[I]`
3. **Reproducibility**: a measurement needs a fixed camera, view height and finished LOD builds. `check_render_budgets` sets the view height and waits for builds; the scene's main camera is the camera, so a scene whose camera moves needs `play` with `fixed_fps` and `wait` first. `[V editor/mcp/McpInstructions.cpp:137; A]` Draw and triangle counts are the same across GPUs; **frame time isn't**, so `frame_ms` should be advisory on software GL. `[I]`
4. **`--headless` applies only on Linux**, so the measured gate is a Linux CI job. `[V editor/mcp/McpLaunch.cpp:298-299]` Windows and macOS need a display.
5. **It needs software GL or a GPU** and first open compiles shaders, up to `JASPER_CALL_SPAWN_TIMEOUT` = 600 s. `[V docs/mcp/cli.md:68-71]`
6. **`project_locked` exits 3** if an editor has the project open with no HTTP. `[V docs/mcp/cli.md:79-80]`
7. **`static` shouldn't need any of this**, so add `check --meshes`: a ninth flag in `CheckOptions`, same `ProjectChecks` code as MCP, reading scene and prefab YAML as `--normals` does, no engine start. `[V docs/mcp/checks.md:7,24; editor/cli/CommandLine.cpp:2096-2115]` Files gain `.gltf/.glb/.obj/.fbx`. Hook mode: add model extensions to the tracked list so a re-exported model reports problems. `[V docs/mcp/checks.md:335-341]`

Unrelated finding: `checks.md` says `--all` runs "All eight", while `CheckOptions` also has `videos`. `[V docs/mcp/checks.md:25; CommandLine.cpp:2106]` Docs may be a flag behind; update when adding `--meshes`.

### 3.4 Game-project `AGENTS.md`

Generated by `Generator::writeAgentsFile`, **written once when missing and never changed afterwards**. `[V editor/Generator.cpp:1686-1695]` So existing projects won't get the new text: the tools' descriptions and `jasper://docs/meshes` must stand alone, and `jasper://project/AGENTS.md` serves whatever the project has. `[V editor/mcp/tools/DocsTools.cpp:515-531]` It has no size gate, but one bullet is proportionate. `[I; test_generators only checks it names the call CLI: V tools/mcp/tests/test_generators.py:45-50]`

Insert after the "Checks without the editor" bullet `[V Generator.cpp:1720]`:

> - Geometry and render budgets (`jasper://docs/meshes`, `tool_groups {enable: ["meshes"]}`): `inspect_meshes` finds the costliest models from their headers and dry-runs a change; `set_mesh_import_settings` applies it (one undo step, a cook job, stored in the model's `.meta`: commit it); `capture_mesh_lods` shows and measures the levels; `check_render_budgets` holds a scene to `quality.render_budgets` per platform. Mesh LOD is per scene and off by default (`mesh_lod_enabled`). Before calling a 3D scene done for mobile or web, run `check_render_budgets`; in CI, `jasperengine-editor call --project . --wait-job check_render_budgets ...` exits 1 when over budget.

Also add `--meshes` to the same bullet's list of checks. Extend `test_project_agents_file` to assert it names `check_render_budgets`.

---

## 4. Lua API

### 4.1 How bindings are declared

- LuaBridge `.addProperty` and `.addFunction` chains in `engine/core/script/binding/*.cpp`; `tools/lua_api` parses them at build time into `lua-api.json`, stubs, completion and the reference. Nothing generated is committed. `[V docs/scripting.md:238-243, .claude/rules/scripting.md:9-11, tools/lua_api/parser.py:1-9]`
- What C++ signatures can't say goes in `OVERRIDES`. `[V tools/lua_api/parser.py:1925]` A raw-C table function needs a hand-written stub, as `Profiler` does. `[V engine/core/script/binding/ProfilerClassesLua.cpp:4-6]`
- Getters are properties. `[V .claude/rules/scripting.md:12-13]` Lua is main-thread only. `[V .claude/rules/scripting.md:14-15]`
- Docs: `docs/mcp/guide/lua-api.md`, whose blocks are checked by `lua_api doctest`. `[V docs/mcp/guide/README.md (lua-api row)]`

### 4.2 What is already bound

| API | Citation | Note |
|---|---|---|
| `Mesh.lodEnabled`, `Mesh.lodBias` | `[V GameObjectClassesLua.cpp:1842-1843]` | **not in `lua-api.md`** `[V grep]` |
| `Mesh.cullDistance`, `cullInstances`, `setFadeRange` | `[V GameObjectClassesLua.cpp:1870-1874]` | instanced only: logs an error otherwise `[V engine/core/object/Mesh.cpp:739-748]` |
| `scene.meshLodEnabled`, `scene.meshLodThreshold` | `[V CoreClassesLua.cpp:3198-3199]` | not in `lua-api.md` |
| `Profiler.frameStats()` | `[V ProfilerClassesLua.cpp:96-105]` | works in release `[V docs/mcp/guide/lua-api.md:1316-1317]` |
| `DebugOverlay.getStats()` | `[V CoreClassesLua.cpp:1277-1279]` | same three counters |
| `Engine.setTextureQuality` and friends | `[V CoreClassesLua.cpp:2176-2181]` | the pattern for runtime quality |

So first: **document the existing LOD API**. It costs nothing and agents can't find it today.

### 4.3 Worth adding: runtime behaviour

| API | Kind | Why | Cost and note |
|---|---|---|---|
| `Engine.setMeshLodBias(x)`, `getMeshLodBias()` | static fn | Global quality slider for a settings menu (Unity's `QualitySettings.lodBias`); `Engine.setTextureQuality` is the model | One float in `selectMeshLod`; a change must dirty instance views, which `updateMainLodView` already compares `[V RenderSystem.cpp:8211-8220,8226]` |
| `Engine.setMeshLodMax(level)`, `getMeshLodMax()` | static fn | Hard floor on low-end devices (Unity's `maximumLODLevel`) | One clamp |
| `Mesh.forcedLod` | rw int, -1 = auto | Cutscenes, hero close-ups, debugging (Unity's `LODGroup.ForceLOD`); clamps to `lodCount - 1` | One compare in `selectMeshLod`; also powers `capture_mesh_lods {entity}` |
| `Mesh.lodLevel` | ro int | Debugging, gameplay swapping (draw detail off at far level) | The main view's last level. An instanced mesh: -1, as instances differ |
| `Mesh.lodCount` | ro int | `numLods` | `[V MeshComponent.h:521]` |
| `Mesh:getLodError(level)` | fn, world units | Custom transitions | Times world scale |
| `Mesh.cullDistance` on any mesh | rw, 0 = off | Cheap distance culling with no instanced component | **A behaviour change**: today it logs an error if not instanced. One compare when above 0; the field needs a default |
| `Profiler.frameStats()` additions | fields | Game-side HUDs and tests: `meshesDrawn`, `meshesCulled`, `lodDraws` {n,n,n,n}, `meshVertexBytes`, `meshIndexBytes` | Table built only when called, and it "still works" in release |

Not bound as new API because they already exist or need nothing: `meshLodThreshold` is the per-scene knob; `Asset` and `Model` loading are unaffected by cooks, since references keep source paths, as textures do. `[V docs/specs/S14-texture-compression.md R31]`

```lua
-- settings menu
function Settings:applyQuality(level)
    Engine.setMeshLodBias(level == "low" and 0.5 or 1.0)
    Engine.setMeshLodMax(level == "low" and 1 or 0)
end

-- cutscene close-up
function Cutscene:start()  self.hero.forcedLod = 0  end
function Cutscene:stop()   self.hero.forcedLod = -1 end

-- a self-check while playing
local s = Profiler.frameStats()
Log.print(s.triangles .. " tris, " .. s.meshesCulled .. " culled")
```

Reflection: `forcedLod` as a `JE_FIELD` with `.min = -1`, plus `kMeshKnownKeys` and the catalog snapshot. `[V RenderReflect.cpp:190-191,501; editor/tests/reflect/catalog_snapshot.json]` Whether it saves is a choice: I'd leave it runtime-only so a debug value can't ship. `[A]`

### 4.4 Must not be exposed

| Do not expose | Why |
|---|---|
| The simplifier, optimizer, encoder (`meshopt_*`, `MeshLodPool::build`) | Heavy jobs; in single-thread mode (the web, `[I]`) they run from the frame's pump `[V MeshLodPool.cpp:125]`. Cooking is an editor concern |
| Import settings, `.meta`, cook cache paths and state | Editor-only; never ship `[V docs/mcp/guide/assets.md:150-151]` |
| `MeshLodData`, raw indices or buffers, `lodError[]` writes, `numLods` writes | Would break invariants and pin the memory layout |
| Budget checks and their settings | An editor and CI concern; a game has no use for them |
| Per-entity LOD or culling lists | Per-frame allocation; a call an agent would poll. Totals only |
| `MeshLodPool::clear`, `clearUnused`, `Engine::addDrawStats` | Engine lifecycle `[V Engine.cpp:1346,1362]` |
| GPU-only culling or indirect paths | Banned `[V CLAUDE.md engineering values, docs/specs/README.md:23]` |
| Anything per-frame that toggles `meshLodEnabled` | Already bound, and rebuilds every mesh `[V RenderSystem.cpp:14743-14744]`. Don't remove it; document "not per frame" |

Agents already reach runtime values through `eval_lua` `[V tools/mcp/tests/test_profiler.py:238]`, so Lua additions also serve agents without new tools.

---

## 5. Testing

### 5.1 Fixture

**Small deterministic meshes exist, but none is bad on purpose.** `[M from the files]`

| File | Vertices | Triangles | Fits |
|---|---|---|---|
| `tools/fixtures/unified-lighting/assets/sphere.gltf` | 1,225 | 2,304 | LOD. POSITION and NORMAL only, 58 KB |
| `tools/fixtures/unified-lighting/assets/bumpy_box.gltf` | 24 | 12 | at the LOD minimum `[V MeshLodPool.cpp:145]`; UV, colour |
| `tools/fixtures/shadows/assets/skinned_morph.gltf` | 10 | 8 | skinning, morphs |
| `engine/tests/core/fixtures/gltf/unsupported_required.gltf` | 3 | 1 | requires `EXT_meshopt_compression` `[V line 115-118]` |
| `engine/tests/core/fixtures/gltf/two_primitive_counts.gltf` | 3 + 3 | 1 + 2 | submesh sums |
| `editor/tests/fixtures/rigged_cube.fbx` | | | FBX path |

Add **`tools/fixtures/meshes/`** with a stdlib `generate.py`, following `tools/fixtures/textures/generate.py`, "every file is a pure function of this script". `[V tools/fixtures/textures/generate.py:3-5]`

| Asset | Purpose | Known numbers |
|---|---|---|
| `grid_64.gltf` | a clean indexed grid | 8,192 triangles, 4,225 vertices |
| `grid_unwelded.gltf` | same shape, no shared vertices | 24,576 vertices, dedup target 4,225 |
| `grid_shuffled.gltf` | indices shuffled by a fixed seed | high ACMR |
| `grid_u32.gltf` | u32 indices with few vertices | `mesh-index-wider-than-needed` |
| `grid_unindexed.gltf` | no indices | `mesh-unindexed` |
| `tiny.gltf` | 8 triangles | `mesh-lod-too-small` |
| `header_only.glb` | valid JSON chunk, BIN cut short | proves headers don't read buffers |
| `required_meshopt.gltf` | copy of the engine fixture | `gltf-required-extension` |
| `Forest.scene` | one camera, an instanced grid, objects off screen, a far object | exact culled and LOD counts |

Exact counts make assertions equality rather than ranges. Cross-check meshopt's determinism in a unit test. `[I]`

### 5.2 Tests in `tools/mcp/tests/`

New `test_meshes.py` (`CI = True`, `EDITOR = {"project": FIXTURE}`) `[V .claude/rules/mcp.md:26-29]`, `test_cli_meshes.py`, and a `tools/mcp/gpu_lane.json` entry. `[V docs/testing.md:481]`

Every row names its red control: the local revert, shown failing, per `docs/testing.md:610-612`.

**Group and conventions**

| Test | Asserts | Red control |
|---|---|---|
| `test_tools_hidden_until_enabled` | The 4 names aren't in `tools(groups=None)`; `tool_groups {enable: ["meshes"]}` lists them; `_meta jasper/group == "meshes"`; callable by name while hidden (copy of `test_texture_import.py:65-78`, `test_tool_groups.py:40-65`) | Set `group = ""` in the module row: "listed by default" fails |
| `test_budget` | Group ≤ 4,800, each tool ≤ 1,500, via `tool_budget.py` | `JASPER_MCP_SELFTEST_GROUP=2000`, existing red control `[V test_tool_budget.py:7,56]`; plus a padded description |
| existing `test_tool_conventions.py`, `test_tool_coverage.py`, `test_docs.py`, `test_autosync.py` | Names, closed objects, quoted names, regenerated `TOOLS.md`, guide names | Each has its own |
| `test_group_listed_in_guide` | Guide stays within headroom after the clause | Add 60 bytes: `tool_budget --check` names it `[V tool_budget.py check_guide]` |

**Discover**

| Test | Asserts | Red control |
|---|---|---|
| `test_inspect_header_counts` | Exact triangles and vertices per model; ranking order by `sort_by` | Swap index and vertex counts |
| `test_inspect_reads_no_buffers` | `header_only.glb` gets correct numbers with a cut BIN; `get_vram_report filter=meshes` unchanged; `.fbx` not converted | Implement the scan through the model loader: the row errors |
| `test_inspect_findings` | Each code on its fixture, with `suggestion` and valid `apply` (passing `apply` to `set_mesh_import_settings` succeeds) | Drop `apply`, or change a key name |
| `test_required_extension_names_fix` | Message names `EXT_meshopt_compression` and the fix, in both `inspect_meshes` and the console | Restore today's text `MeshSystemGltfPbr.cpp:140` |
| `test_inspect_scene_weight` | `uses.scene_instances` equals the known count | Count entities, not instances |
| `test_inspect_cached` | Second run `cached == scanned`; edit a file: `cached` drops | Disable the mtime check |

**Dry run and apply**

| Test | Asserts | Red control |
|---|---|---|
| `test_dry_run_writes_nothing` | Project tree hash unchanged, no undo step, runs over HTTP with "Allow changes" off (`HttpSession`) | Make the preview call `SetAssetMetaCmd` |
| `test_prediction_equals_result` | Dry-run numbers equal the cook's, and equal a load's real counts (`get_model_info`); second cook `cache_hit` | Compute prediction as `triangles * ratio`, as `LOD_TARGETS` would suggest |
| `test_set_idempotent` | A repeat call: `updated == []`, no new step in `get_undo_history` | Remove the `isNoOp` skip |
| `test_set_undo` | `undo {scope: "project"}` restores the `.meta` bytes and earlier numbers; `structured.undo` set | Write the file directly instead of through the command |
| `test_set_invalid_writes_nothing` | A bad key lists the valid ones; one bad path among several writes none | Validate per file in the write loop |
| `test_set_refused_while_playing` | Refused while playing; `inspect_meshes`, `get_render_stats` fine | `allowedWhilePlaying = true` |
| `test_cook_job` | `wait: false` gives `job_id`, `status: running`, `get_job` ends succeeded with the same report; `cancel_job` cancels | Run the cook inline |
| `test_error_budget_respected` | With small `lod_max_error`, each `error_world <= budget`, with `stopped_by`; larger allows fewer triangles | Keep today's `FLT_MAX` `[V MeshLodPool.cpp:167]` |
| `test_old_project_unchanged` | A project without keys loads, cooks nothing, writes no `.meta`, saves identical scenes (`test_save_determinism`) | Default the preset to balanced |

**Verify**

| Test | Asserts | Red control |
|---|---|---|
| `test_capture_reproducible` | Two calls give identical PNG bytes and numbers | Random seed for sampling |
| `test_capture_metrics_move` | Level 0: PSNR null, `changed_fraction` 0; triangles fall and error rises per level; L3 differs visibly; PNG valid, not `pngdiff.flat` | Ignore `level` when drawing, so every level is identical |
| `test_world_scale` | Scale 2 doubles `error_world`, `deviation_world`, `switch_distance` | Drop `worldScale` |
| `test_switch_distance_matches_selection` | Moving the camera just past `switch_distance` changes `lodLevel`, just before doesn't | Round the formula; omit `constant` |
| `test_capture_entity` | Real shaders, one image per level, restores `forcedLod` afterwards; structural checks only | Don't restore |

**Per-frame, while playing**

| Test | Asserts | Red control |
|---|---|---|
| `test_render_stats_geometry` | Exact on `Forest.scene`: `drawn + culled = total`, known frustum, layer and distance counts, histogram sums to draws (`test_render_stats` style, `test_profiler.py:57-70`) | Don't increment the frustum counter |
| `test_lod_histogram_moves` | Moving the camera shifts levels; LOD off gives `enabled: false` and all level 0 | Report the mesh's highest level, not the drawn one |
| `test_lod_off_costs_nothing` | With LOD off: no `mesh-lod-simplify` task, identical `alive` and `passes` vs baseline | Build levels regardless `[V RenderSystem.cpp:7217-7220]` |
| `test_triangles_by_pass` | With a shadow light, `main + shadow + depth == triangles` | Count shadow draws in main |
| `test_stats_while_playing` | All of the above while `play` runs; `jasper://stats` has `geometry` | Registry flag off |
| `test_monitors` | `get_frame_times {monitors: ["Meshes culled"]}` returns a series | Misspell a monitor |
| `test_vram_usage` | `usage_bytes.vertex + index` ≤ meshes group; falls after cooking | Sum all buffers |

**Budgets and CI**

| Test | Asserts | Red control |
|---|---|---|
| `test_budget_pass_and_fail` | Limit 1 fails with `isError`, exact values, offenders; generous passes | Reply `ok` always |
| `test_no_budget_is_error` | No budgets: error with the fix, never a pass | Return ok when empty |
| `test_budget_precedence` | Argument over scene entry over default | Swap order |
| `test_budget_platform` | `platform: mobile` uses its threshold and view height: the histogram differs from desktop's | Ignore `platform` |
| `test_budget_waits_for_builds` | Straight after enabling LOD, measures after builds finish | Skip the wait: all level 0 |
| `test_budget_no_camera` | Actionable error | Default to the Scene view |
| `test_cli_budget_exit_codes` (`test_cli_meshes.py`) | `call --project <copy> check_render_budgets` exits 1 with `code: tool_error`, `structured.violations` in `--json`; exit 0 when generous; a bad `budgets` key exits 2 before running (`test_call_cli.py` style) | `call->ok` on violation |
| `test_cli_check_meshes` | `check --meshes` with no `DISPLAY`: exit 1, findings in the JSON shape; `--files` works | Report findings as warnings |
| `test_project_agents_file` (extend) | Names `check_render_budgets` | Remove the bullet |

**Lua** (extend `test_lua_api_json.py` run blocks, plus `test_stubs.py`)

| Test | Asserts | Red control |
|---|---|---|
| `test_lua_force_lod` | `forcedLod = 2` makes `lodLevel == 2` and triangles fall; -1 restores; out-of-range clamps | Ignore it in `selectMeshLod` |
| `test_lua_lod_bias_global` | `Engine.setMeshLodBias(0.5)` lowers detail, also on instanced meshes | No instance view dirtying |
| `test_lua_frame_stats` | New fields exist, also in release | Omit one |

**Unit tests** (`docs/testing.md:491-500`)

`MeshLodPool` has no direct test today. `[V grep of engine/tests]` Add `engine/tests/core/pool/MeshLodPoolTest.cpp` for levels shrinking, the 90% stop, the error budget and meshopt determinism, and `editor/tests/mesh/MeshImportSettingsTest.cpp` for parse, migration and canonical serialization, modelled on `TextureImportSettingsTest.cpp`. The first run on existing behaviour is characterization. `[V docs/testing.md:614-615]`

---

## 6. Open questions and risks

1. **Run the gate.** All budget numbers here are static. Run `tool_budget.py --groups all --top 20` and `--check` on a build server first, especially core and the guide. `TOOLS.md` was generated from `d61bed5a5-dirty`, not this commit `[V docs/mcp/TOOLS.md:3]`.
2. **Guide room is first-come.** Other specs can use the same 110 bytes. If something lands first, this clause may need moving detail out of another line, which rules restrict. `[V conventions.md:210-212]`
3. **`anyObject` for what-if settings** is a deliberate deviation from "closed objects", in two of three places, to save about 1,000 chars. It has S14's precedent `[V AssetTools.cpp:4714]`. The closed schema on the setter means an agent still sees the keys. Alternative: drop what-if from `capture_mesh_lods`, and require the dry-run call first.
4. **Dry run in a read-only tool writes a cache.** Decide if that's acceptable under "Allow changes" off. My recommendation is yes, as it's internal, content-addressed and invisible. If no, compute in memory.
5. **`SetAssetMetaCmd`'s single listener** needs a small refactor before a second queue can exist.
6. **The `triangles` semantic.** Budget wording must pin it; I keep `triangles` (all passes, the GPU's real load) as the budget key and report `main_triangles`, which a user's mental "150k" may mean. Confirm with the user.
7. **CPU sheet blind spots.** Lambert plus base colours misses normal maps and UV seams; `entity` mode covers them but only structurally in tests. A visual claim on a real asset still needs a person or a screenshot.
8. **`configure_game_view` is at its limit**, so a persistent "simulate mobile quality" toggle (like `simulate_textures`) doesn't fit. It would need an exception or trimming. I left it out; `platform` on the check and capture tools covers verification.
9. **Skinned and morphed models**: LOD interaction is out of scope here but affects findings. `findLodAttributes` decides what qualifies today. `[V RenderSystem.h:2114]`
10. **Settings semantics** (`quantize` levels, preset contents, `meshopt` decode cost on web) belong to the pipeline researchers; the keys above are placeholders for their result, and the schema budget allows about 450 more chars across the group.
11. **The `quality.render_budgets` shape** must be checked against how settings rows handle object values; `physics.layer_collision_matrix` and `tags_and_layers.layers` suggest it's supported. `[I, V SettingsRows.cpp:2747]`

---

## 7. The workflow, in five steps

1. **Discover:** `tool_groups {enable: ["meshes"]}`, then `inspect_meshes {scene, sort_by: "scene_triangles"}`. Headers only; ranked rows and findings with paste-ready `apply`.
2. **Dry-run:** `inspect_meshes {paths, settings}`. Nothing written; real cook numbers per level, cached.
3. **Apply:** `set_mesh_import_settings {paths, settings}`. One project undo step; a `job_id` if it outlasts `timeout_seconds`; before and after numbers. `set_scene_settings {mesh_lod_enabled: true}` if needed.
4. **Verify:** `capture_mesh_lods {path}` (reproducible sheet, world-unit deviation, PSNR), `{entity}` for normal-mapped models.
5. **Gate and watch:** `check_render_budgets {scene, platform}` (exit 1 in CI through `call`), and `get_render_stats` or `get_frame_times` while playing.

---

## Appendix: scripts

Sizes come from `/tmp/forge-meshopt-research/scratch/draft_sizes.py`. It applies `editor/mcp/McpSchema.cpp`'s output rules (empty descriptions omitted, whole bounds as integers, top-level `additionalProperties` omitted from the listing `[V docs/mcp/conventions.md:64-68]`), and counts `len(name) + len(description) + len(compact JSON)` as `tool_budget.py` does. `[V tools/mcp/tool_budget.py:50-56]`
