# S101 — Vapor: Fog Profiles, Fog Zones, lit fog and shadowed fog lights

**Wave / Depends on.** Atmosphere follow-up to S12. It builds on S12 (the fog itself), S10 (the
shadow atlas), S20 (particle variants), S04 (asset identity), S05 (hot reload), S24 (undo) and
S31 (the MCP budget). No other spec depends on it.

**Owner area.**
- `engine/core/render/FogProfile.*`: the asset, the built-ins, the resolver.
- `engine/core/component/FogComponent.h`, `FogZoneComponent.h`.
- `engine/core/subsystem/AtmosphereRenderer.*`, `engine/core/render/AtmosphereMath.*`,
  `engine/core/render/AtmosphereQuality.cpp`.
- `engine/shaderlib/volumetric_fog.frag`, `includes/shadow_sampling.glsl`, the particle fog path.
- `editor/command/type/FogProfileAssetCmd.*`, `LinkFogProfileCmd.*`.
- The Fog and Fog Zone inspectors.
- `editor/mcp/tools/AtmosphereTools.cpp`, `docs/mcp/guide/atmosphere.md`.

**Origin.** These are ideas taken from Vapor (github.com/ArthurBrussee/Vapor, MIT, 2017), a Unity
froxel fog. Its analysis is summarised in §4. No Vapor code is copied.

## 1. Goal

Today a fog look is a handful of numbers on one scene's Fog. The five built-in looks are hard-coded
in C++ and copied in once. Nothing blends, and a cave cannot have its own fog. This spec makes the
look a shared asset that people and agents edit in one place, blend at run time and switch by
place. The fog's colour can follow the scene's lighting, so one look holds from noon to night.
Point and spot lights scatter on every quality tier, and their shadows reach the fog the way the
sun's already do. Agents can learn why fog is not showing and how dense it is, without
screenshots.

## 2. Scope

**In scope**

- **The `FogProfile` asset** (`.fogprofile`). Each look field is either set by the profile (an
  override) or left to whatever is beneath it, as in Unity's Volume Profiles.
  - There are built-ins: the five existing looks plus four new ones (R4).
  - A built-in is copied into the project on first use, the way palettes are.
- **Fog linked to a profile.**
  - The Fog component gets a `profile` reference.
  - A field the profile overrides comes from the profile and is shared by every user of the
    profile.
  - Any other field comes from that Fog's own value.
  - The Fog inspector edits both kinds of field inline.
- **Fog Zones**: a new component with a box or sphere, a blend distance, a priority, a weight and a
  profile. Its overrides blend over the scene's look according to where the camera is.
- **Runtime blending**: `fog:blendTo(profile, seconds)` in Lua and C++.
- **The Lit colour mode** (`FogColorMode::LIT`): fog colour = albedo × (sun + scene ambient) +
  emissive.
- **Fog lights**:
  - point and spot lights scatter on every tier: Low 1, Medium 2, High 4;
  - a light that casts shadows casts them in the fog;
  - lights are picked with a frustum test.
- **Softer light shafts**: two rotated sun shadow taps on Medium and High, and a 5×5 denoise on
  Low, all still deterministic.
- **Fix: particles take the height fog** in a Height fog scene.
- **One diagnostics source** with reason codes. It drives inspector help boxes and the MCP tools.
- **CPU opacity estimates** in `get_atmosphere_info`.
- **MCP**:
  - extend `list_atmosphere_presets`, `apply_atmosphere_preset`, `get_atmosphere_info` and
    `create_entity`;
  - add one new tool, `edit_fog_profile`.

**Out of scope**

- Froxel or 3D-texture fog, and temporal accumulation (do-not-copy list, `docs/specs/README.md`;
  S12 R12). sokol also rejects rendering into 3D-texture slices (`sokol_gfx.h:23494`).
- Volumetric fog on transparents. They keep the analytic fog (S12 limitation, documented).
- ESM or other filtered fog shadow maps (S10 excludes new filter families; a later spec).
- Cloud presets (they stay as they are), and profiles for Local Volumetric Fog.
- Drag handles for zone shapes (fields and Transform only, like Local Volumetric Fog).
- Zones driving anything but fog (post-processing, sky).
- Physics colliders for zones. A zone is an analytic shape, tested against the camera position.

## 3. Current state (read on `main` 82ccd7c6a)

- **`FogComponent`** (`engine/core/component/FogComponent.h:30-64`).
  - Fields: type, colour, distance-fog fields, the height-fog medium, sky fill, and the volumetric
    switches.
  - Colour is added light: `L = color + sun·inscatter·HG·visibility` (`includes/height_fog.glsl:133`).
  - The global fog's albedo is a fixed 1 (`volumetric_fog.frag:177`).
  - The first Fog in the scene is used. A second Fog is ignored without a warning.
- **Presets** (`engine/core/render/AtmospherePresets.cpp:12-21`).
  - Five ids are written in C++: `morning_haze`, `ground_mist`, `mountain_valley`,
    `horizon_haze`, `night`.
  - Applying one sets fields (`AtmospherePresetCmd`). It forces Height, and leaves the colour mode
    and the volumetric fields alone.
  - The inspector's menu passes `current=nullptr` (`editor/window/InspectorWindow.cpp:11676`), so
    it never shows the active look. It is hidden unless the type is already Height (`:11675`).
- **Local Volumetric Fog** (`LocalVolumetricFogComponent.h`).
  - Each volume has density (`attenuationDistance`), its own `albedo`, noise, a falloff and a
    scroll speed. Volumes add together; there is no priority.
  - The march mixes albedo per step as `(fogDepth + volumeScatter) / tau`, with the global fog
    counting as albedo 1 (`volumetric_fog.frag:162-180`).
  - Its reflected fields have no tooltips (`editor/reflect/components/LightingReflect.cpp:263-273`).
- **Fog lights** (`AtmosphereRenderer.cpp:636-669`).
  - On High only, up to 4 point or spot lights are picked, nearest the camera, with no frustum test.
  - They have no shadows (`volumetric_fog.frag:80-105`).
  - Tiers are 0/0/4 (`engine/core/render/AtmosphereQuality.cpp`), so mobile and web, which default
    to Low, get none.
- **Sun shafts**:
  - one hardware comparison tap per step (`sampleDirectionalShadow1`,
    `includes/shadow_sampling.glsl:384-411`);
  - a 3×3 bilateral denoise.
- **Shadow data**:
  - The fog pass already binds `u_fs_shadow_data` (all 9 projective slots, so spot slots are
    present).
  - It binds the real `u_shadowAtlas` only when sun shafts run (`lightShafts` on, scene shadows
    on, a shadowed directional light). Otherwise it binds the empty depth texture
    (`AtmosphereRenderer.cpp:616`, `:688-694`).
  - Point shadows need `SHADOW_DATA_POINT` and `u_shadowPointAtlas`.
- **Particles**:
  - Particle variants define `HAS_FOG` but never `HAS_HEIGHT_FOG`
    (`engine/core/shader/ShaderVariantCompiler.cpp:560`; meshes at `:382-387`).
  - `RenderSystem.cpp:2657` keeps `fs_fog` as a linear fog for Height.
  - Inferred from code, not rendered: under Height fog, particles take a linear fog from the
    Fog's own `color`, `linearStart` and `linearEnd` (`RenderSystem.cpp:2665-2666`), not the
    height fog. With default values that is 10–60 m grey; after a preset it carries the preset's
    colour.
- **Agents**:
  - `get_atmosphere_info` reports what ran, not why something did not.
  - The two "why" checks (`InspectorDefault.cpp:373-381`, `InspectorWindow.cpp:11731`) are
    inspector-only.
  - The eval case `guard_atmosphere_preset` (`tools/mcp/eval/cases.py:1335-1359`) mocks preset
    ids that do not exist.
- **Precedents to build on**:
  - `.particles` editing: `ParticleEffectAssetCmd` keeps the canonical JSON before and after, and
    every user rebinds at once.
  - Material link and unlink: `LinkMaterialCmd`, `UnlinkMaterialCmd`.
  - Palette built-ins copied on first use: `builtin:<id>`, `docs/mcp/guide/palettes.md`.
  - Audio snapshot transitions: `AudioMixer::transitionTo`, `engine/core/audio/AudioMixer.h:136`.

## 4. Reference (Vapor, `ArthurBrussee/Vapor`, MIT)

Read for ideas only. The architecture is not taken.

**Take:**

- **The medium as an asset, edited inline from where it is used.** Vapor: `VaporSetting.cs`,
  `Editor/VaporEditor.cs:10-51`.
- **Albedo × incoming light, plus ambient and emissive as light sources.** Vapor:
  `VaporSim.compute:256`, `VaporSetting.cs:64-71`. So a look keeps its character as the lighting
  changes.
- **Setting → BlendTo over time** (`Vapor.cs:26-37`).
  - Improve on it: Vapor has no setters, and lerps albedo on its own.
  - Ours re-targets freely, and blends extinction in extinction space.
- **Zones that change the look by place** (`VaporZone.cs`, `VaporSim.compute:452-478`).
  - Improve on it: Vapor overwrites inside a hard edge, has no priority, and costs one dispatch per
    zone.
  - Ours blend by camera position with a linear weight and priorities, and cost nothing on the GPU.
- **Spot lights shadowed in the fog** (`VaporSim.compute:359-370`).
  - Improve on it: Vapor's ESM is unfinished and its point lights leak light.
  - Ours shadow every light that casts shadows.
- **The looks of its presets**:
  - Bright: thin, sky-blue ambient.
  - Murky: low albedo, high ambient.
  - Hell: dense, red emissive.
  - Poison: green emissive, thinning with height.
  - These inspire four new built-ins (R4). Values are re-tuned for our medium, not copied.

**Do not take:**

- The froxel/compute pipeline and the 3D history. Vapor's own versions also have bugs: depth
  conventions that disagree, and reprojection with no divide by w.
- The separate VaporLight component. `Light.volumetricIntensity` already exists.
- The custom light.
- Rayleigh sky parameters on the fog.
- Mutating shared assets from script (`Examples/TimeOfDay.cs:69`).

## 5. Requirements

### Functional: the profile asset

- **R1** `AssetType::FogProfile = 41` (`"fog_profile"`, extension `.fogprofile`).
  - It is versioned, canonical JSON, with its GUID in `.meta` (S04).
  - A field present in `fields` is an override. An absent field is not.
  - Loading ignores unknown fields with one console warning per file.
  - Writing goes through `AtomicFile` in canonical field order.
- **R2** The overridable fields are every look field of `FogComponent`:
  - `color`, `density`, `linearStart`, `linearEnd`;
  - `colorMode`, `attenuationDistance`, `baseHeight`, `maximumHeight`, `startDistance`,
    `maxFogDistance`, `maxOpacity`;
  - `directionalInscattering`, `anisotropy`, `skyFill`, `skyFillAngle`;
  - `noiseIntensity`, `noiseScale`, `wind`;
  - the new `albedo`, `ambientMultiplier`, `emissive` (R12).

  Fields that set cost or technique are never in a profile: `type`, `volumetric`,
  `volumetricDistance`, `lightShafts`, `qualityOverride`.
- **R3** The asset has a `name` and an optional `description`.
- **R4** Built-ins are embedded in the engine and addressed as `builtin:<id>`.
  - They are the five existing ids, re-expressed as profiles with the same values, so applying one
    renders exactly as today.
  - Plus four new ids that use noise, wind and Lit: `bright_haze`, `murky`, `infernal`, `toxic`.
  - The editor copies a built-in to `assets/FogProfiles/<id>.fogprofile` the first time it is
    linked, and reuses that copy afterwards.
  - A game can link `builtin:<id>` directly from Lua without a file.
- **R5** Editing a profile, whether from the editor, the file watcher (S05) or MCP, rebinds every
  Fog and Fog Zone using it on the next frame.
- **R6** A missing or unreadable profile is treated as an empty profile.
  - The Fog renders its own values, a Fog Zone has no effect.
  - The diagnostic `profile_missing` names the path.

### Functional: resolving the look

- **R7** Each frame, each view resolves one set of fog values. Layers apply in this order, each
  only for fields it overrides:
  1. The Fog's own fields.
  2. Its linked profile.
  3. The scripted blend (R10).
  4. Fog Zones in ascending priority, ties broken by entity id.
  5. Runtime writes from Lua and animation (R43).
- **R8** Blend rules:
  - Each layer mixes as `v = lerp(v, layerValue, w)`.
  - Colours mix in linear RGB.
  - `attenuationDistance` mixes as extinction (1/d), then converts back.
  - Enums and bools switch when `w ≥ 0.5`.
  - The renderer sees only the resolved values. No shader changes for profiles or zones.
- **R9** Fog Zone weight:
  - `w = weight × clamp(1 − d / blendDistance, 0, 1)`, where `d` is the camera's distance to the
    shape (0 inside).
  - `blendDistance = 0` gives a hard switch at the surface.
  - The Scene view uses the scene camera, the Game view the main camera.
  - Shape and size follow Local Volumetric Fog: a box or sphere in local space, scaled and rotated
    by the Transform.
- **R10** `blendTo(profile, seconds)`:
  - It cross-fades the base layer from the current resolved base to the base with the new profile,
    over `seconds` of game time.
  - At the end it becomes the Fog's runtime profile. This is not saved.
  - `blendTo(nil, s)` fades back to the Fog's own fields.
  - `seconds ≤ 0` applies at once.
  - Calling it again mid-blend starts from the current mixed values, with no jump.
  - Setting `fog.profile` directly applies at once and cancels a running blend.
- **R11** With no profile, no zones and no blend, the resolve is skipped and the look fields
  reach the renderer exactly as today.
  - Scenes without volumetric fog render identically.
  - Volumetric scenes change only through the deliberate rendering improvements R13-R15: fog
    lights on Low and Medium, shadows in the fog, softer shafts. Their goldens are regenerated
    in the same commits, with the reason stated (AC6, AC7).

### Functional: rendering

- **R12** `FogColorMode::LIT` (appended), with new fields:
  - `albedo` (linear RGB, default 1,1,1);
  - `ambientMultiplier` (default 1);
  - `emissive` (linear RGB HDR, default 0).

  In Lit mode:
  - The fog's light is `albedo × (sun·inscatter·HG·visibility + sceneAmbient × ambientMultiplier)
    + emissive`.
  - The same applies to the analytic fog used by meshes, transparents, particles and sky fill.
  - `sceneAmbient` is the scene ambient colour × intensity, the value meshes receive.

  - The analytic fog (meshes, transparents, particles, sky fill) gets Lit on the CPU: the fog
    colour and sun terms it already reads become `albedo × sceneAmbient × ambientMultiplier +
    emissive` and `albedo × sun`.
  - The volumetric march lights it physically, per step:
    - the global medium scatters with `albedo` and each Local Volumetric Fog volume with its own
      albedo, mixed by their share of the extinction, as today;
    - sun, ambient and local lights are the light that is scattered, with no fog albedo folded in
      beforehand, so a volume is never tinted twice;
    - `emissive` is added in proportion to the global medium's share of the extinction only.
  - Where only the global medium is present, the march equals the analytic fog. S12 R6, the
    handoff equality, still holds and its test covers Lit.
  - In COLOR and SKY modes the march is unchanged.
- **R13** Fog light counts per tier: Low 1, Medium 2, High 4. `ATMOSPHERE_MAX_FOG_LIGHTS` stays 4.
  - Lights are chosen in this order:
    1. intensity > 0 and `volumetricIntensity > 0`;
    2. bounding sphere intersects the view frustum, within `volumetricDistance`;
    3. nearest first.
- **R14** Shadowed fog lights:
  - A point or spot light with shadows on scatters shadowed light in the fog, whenever the Fog's
    `lightShafts` is on.
  - Each step does one comparison tap, through new single-tap helpers `sampleSpotShadow1` and
    `samplePointShadow1`. They take no normals or derivatives and use explicit LOD (S12 R15).
  - The fog pass binds the real shadow atlas whenever sun shafts run or a selected fog light is a
    shadowed spot light. It binds the point shadow block and atlas only when a selected light is
    a shadowed point light. Otherwise it binds what it binds today.
- **R15** Softer sun shafts:
  - On Medium and High, two sun shadow taps per step, offset on a disk of 1.5 shadow texels and
    rotated by the static blue noise (still static per pixel, S12 R12).
  - On Low, one tap and a 5×5 denoise in place of the 3×3.
  - Spot and point lights stay at one tap on every tier.
- **R16** Particles take the same height fog as meshes when the Fog is Height.
  - Additive, multiply and premultiplied blend modes keep their current fog handling
    (`output.glsl:63-125`).

### Functional: diagnostics

- **R17** `AtmosphereDiagnostics` (engine) returns `{code, severity, message, entity?, fix}`.
  - The same strings feed the inspector help boxes and `get_atmosphere_info`.
  - The codes:
    - `fog_missing`: Fog Zones or local volumes exist but there is no Fog.
    - `fog_second_ignored`.
    - `volumetric_needs_height`.
    - `volumetric_quality_off` (names the setting: project quality, device tier, Game-view
      preview, or runtime override).
    - `volumetric_main_camera_only`: Scene view, or a non-main camera.
    - `shafts_off`.
    - `shafts_sun_no_shadows`.
    - `light_not_scattered` (per light: tier cap, out of frustum, out of distance, zero
      intensity).
    - `volume_culled` (per volume: cap, distance, frustum).
    - `scene_view_fog_disabled`.
    - `sky_mode_no_horizon` (existing).
    - `lit_no_light`: Lit mode with neither sun nor ambient, so the fog is black.
    - `profile_missing`.
    - `profile_no_effect`: the profile overrides only fields the current type ignores.
    - `zone_no_effect`: weight 0, no profile, or no overrides.
    - `fog_invisible`: estimated opacity < 2% at 200 m.
    - `fog_opaque`: > 95% at 10 m.
- **R18** Opacity estimates:
  - The CPU integrates the resolved analytic medium along the main camera's forward ray, at 10,
    50 and 200 m, using `AtmosphereMath`.
  - It is exact for the analytic fog, within 0.02 of a GPU readback of the analytic fog in tests.
  - Volumes and noise are not included, and the result says so.

### Platform & backends

- **R19** Everything runs on GL 4.1, GLES3/WebGL2, Metal, D3D11 and Vulkan.
  - Profiles, zones and blending are CPU only.
  - Shadow taps use the existing comparison sampler.
  - Point shadows on GLES3 must fit within 16 fragment texture units in the fog pass. That pass
    uses far fewer, but count them in a test.

### Performance (S12 §5 machines and bench)

- **R20** Zero cost when unused:
  - no profile, zone or blend means no resolve, no allocation and no upload change (R11);
  - no fog lights selected means no shadow bindings or loops beyond today's.
- **R21** CPU:
  - the resolve with 16 zones and a blend is ≤ 0.02 ms on the mid desktop and ≤ 0.05 ms on mid
    mobile;
  - diagnostics run only when asked for (inspector open, MCP call), never per frame in a player.
- **R22** GPU, measured with `tools/performance/atmosphere_bench.py`, with new scenes for 1, 2 and
  4 shadowed spot lights and 1 shadowed point light:
  - Low with 1 shadowed light adds ≤ 0.2 ms on the mid mobile reference (Adreno 650 / Mali-G78 at
    720p).
  - Medium with 2 adds ≤ 0.25 ms, and High with 4 adds ≤ 0.4 ms, on the desktop reference.
  - Two sun taps add ≤ 0.1 ms on Medium (desktop).
  - S12 R18's totals are reported with these counts. High's known overrun (S12 Deviations) is
    tracked, not widened.

### Editor UX (human)

- **R23** Fog inspector, at the top: a **Profile** row. It replaces the old fog Preset menu
  (`drawAtmospherePresetsMenu` stays for clouds only).
  - It has an asset field, plus a menu listing built-ins and project profiles. The menu shows the
    current one, or "None (this Fog's values)", and is visible for every fog type.
  - Badge: "Used by N" (scenes and zones referencing it).
  - Buttons:
    - **New** saves this Fog's look fields as a new profile, all overridden, and links it.
    - **Duplicate** links a copy.
    - **Unlink** copies the resolved values into the Fog and clears the link.
  - Linking a profile that overrides height fields to a non-Height fog sets Type to Height in the
    same undo step, as the presets do today.
- **R24** While linked, every look row gets a leading checkbox, "Set by profile".
  - Checked rows edit the profile: one `FogProfileAssetCmd` per edit, drags merged.
  - Unchecked rows edit this Fog (`PropertyCmd`).
  - Ticking a row copies the current value into the profile. Unticking removes the override.
  - **All** and **None** buttons per section.
  - Every change is one undo step and undoes the file too.
- **R25** In Play, or with zones in view, an "Effective look" line names the active layers, e.g.
  "Profile Murky · blending to Night 40% · Zone Cave 0.6". Rows whose effective value differs
  show it in the tooltip.
- **R26** A `.fogprofile` opens in the Inspector with every field.
  - Unticked rows read "not set (each Fog's own value)".
  - Shown: name, description, and a "Used by" list that selects users on click.
  - Create > Fog Profile in the Project window.
- **R27** Fog Zone:
  - Hierarchy: Effects > Fog Zone.
  - Inspector: Shape, Size, Blend Distance, Priority, Weight, and Profile with the same inline
    rows. A zone's own values are its profile's overrides, so a zone shows only ticked rows plus
    an add-field menu.
  - Gizmo: the shape, plus a fainter outer blend shell, drawn when selected.
- **R28** Help boxes from R17 appear on Fog, Fog Zone, Local Volumetric Fog and Light (beside
  Volumetric Intensity). They use the same text as MCP.
- **R29** Every new field has a tooltip, Local Volumetric Fog's existing fields included.
- **R30** The Lit mode's rows (Albedo, Ambient Multiplier, Emissive) show only in Lit mode.
  Fields a mode ignores are hidden, as today.

### Agent UX (MCP, Lua)

- **R31** `list_atmosphere_presets {kind: "fog"}` returns built-ins and project profiles: id or
  path, name, description, override count, used-by count.
- **R32** `apply_atmosphere_preset {entity, preset}`:
  - `preset` is a built-in id or a `.fogprofile` path.
  - On a Fog, it links (copying a built-in on first use). On a Fog Zone, it sets the zone's
    profile. Clouds are unchanged.
  - `unlink: true` (instead of `preset`) unlinks as R23.
  - An unknown preset errors and lists the valid ids and paths.
  - The result's `next` points at `edit_fog_profile` when the profile is used by more than one
    scene or zone.
- **R33** New `edit_fog_profile {path, from?, set?, clear?}`:
  - `from` is `{builtin}`, `{entity}` or `{profile}`, and creates the file.
  - `set` maps fields to values, and also ticks them.
  - `clear` lists fields to untick.
  - It returns the profile's fields, its users, and the resolved look of the open scene.
  - It is one undoable step through `FogProfileAssetCmd`. It is refused in Play.
- **R34** `get_atmosphere_info` adds:
  - `fog.profile`;
  - `zones` (entity, priority, current weight);
  - `blend` (target, progress);
  - `diagnostics` (R17);
  - `opacity {at10, at50, at200, includes: "analytic"}` (R18);
  - with `resolved: true`, each look field's value and the layer it came from
    (`fog | profile | blend | zone:<entity>`).
  - Hints go in `next`, per `docs/mcp/conventions.md`.
- **R35** `create_entity` accepts `fog_zone`. `set_properties` covers every Fog Zone field.
- **R36** Lua (S23 `tools/lua_api`):
  - `FogProfile.load(path)`, `FogProfile.builtin(id)`.
  - `profile:get(field)`, `profile:isSet(field)`.
  - `profile:clone()` returns a runtime-only, mutable copy with `set` and `clear`. Loaded and
    built-in profiles are read-only from Lua.
  - `Fog.profile`, `fog:blendTo(profile|nil, seconds)`, `fog.blending`, `fog:getResolved(field)`.
  - A `FogZone` with every field.
  - `Fog:applyPreset(id)` keeps its current behaviour (it writes the Fog's fields), and the docs
    point to profiles.
- **R37** Eval (`tools/mcp/eval/cases.py`):
  - Fix `guard_atmosphere_preset` to mock the real ids, and check the link.
  - Add cases that fail without the new text:
    - a fog for one cave only goes through a Fog Zone, not the global Fog;
    - "the fog isn't showing" reads `diagnostics` and fixes the named cause;
    - changing a profile used by several scenes duplicates it first when asked to change only one
      scene.
- **R38** `docs/mcp/guide/atmosphere.md` covers profiles, zones, Lit, fog lights and diagnostics.
  - `docs/mcp/TOOLS.md` is regenerated.
  - `tools/mcp/budget.json` is lowered where text was cut and raised only for `edit_fog_profile`.

### Data & compatibility

- **R39** New optional fields with defaults and `OmitIfDefault`, so there is no version bump:
  - Fog: `profile` (asset ref), `albedo`, `ambientMultiplier`, `emissive`;
  - `FogColorMode::LIT` appended.
- **R40** New component `FogZoneComponent` (stream key `fogzone`):
  - `shape` BOX|SPHERE, BOX by default;
  - `size` (10,10,10);
  - `blendDistance` 5;
  - `priority` 0;
  - `weight` 1;
  - `profile`.
- **R41** Old scenes load without migration. They render unchanged except for R11's deliberate
  volumetric improvements. A Fog with a preset applied the old way keeps its fields and stays
  unlinked.
- **R43** Writes to a look field that the linked profile sets:
  - **MCP** `set_properties` refuses, with an error naming the profile, its user count and both
    fixes: change it for every user with `edit_fog_profile {set}`, or for this scene only by
    duplicating (`edit_fog_profile {from: {profile}}`) and linking the copy. Fields the profile
    does not set write normally.
  - **Lua and animation** writes in Play go to a runtime layer at the top of R7's order, above
    zones, so they always take effect. `fog:clearOverride(field)` and `fog:clearOverrides()`
    remove them. The layer is never saved and is cleared on Stop.
  - **The inspector** routes by the row's checkbox (R24), so it never meets the conflict.
- **R42** `.fogprofile` files cook into players like other JSON assets. Built-ins ship in the
  engine.

## 6. Design constraints & decisions

### 6.1 Decisions

- **Override model (chosen by the owner).**
  - Per-field overrides, as in Unity HDRP Volume Profiles.
  - The Fog's own fields are the bottom layer, so a scene keeps its own values for anything the
    profile leaves unset.
  - Old scenes are that bottom layer alone.
- **Linked, not copied (owner).** Editing a profile changes every user. Duplicate and Unlink are
  one click away, and the "Used by N" badge warns.
- **Zones blend by camera position (owner).** Zones are CPU-only, so zero GPU cost. Local
  Volumetric Fog stays the tool for density you see from outside.
- **Fog lights (owner delegated, decided here).**
  - One rule for every light type: shadows on the light means shadows in the fog. The sun already
    works this way, and an unshadowed point light would shine through walls.
  - Counts 1/2/4. Low gets one, because mobile and web get none today and one light costs about
    what the sun costs.
  - High gains shadows, not lights, because it is already over its S12 budget estimate on the
    reference GPU.
- **Profiles don't hold technique.** Volumetric on/off, distance, quality and shafts stay on the
  Fog. A profile can't make a mobile scene expensive.
- **Script safety.** Shared profiles are read-only at run time. Lua mutates clones. This avoids
  Vapor's shared-asset mutation.
- **No temporal history**, so a profile switch needs no history reset. Vapor needed one.
- **Existing volumetric scenes change on purpose.** Fog lights on Low and Medium, shadows in the
  fog and softer shafts are corrections, not options. A light that shines through walls is a bug,
  and mobile and web getting no fog lights is a gap. So they apply without a new opt-in field.
  A scene that wants the old cost sets `volumetricIntensity` to 0 on its lights.
- **Profile-set fields are protected from silent writes** (R43). An agent's tweak after linking
  must not vanish beneath the profile; a script's write must always show.

### 6.2 `.fogprofile` (version 1)

```json
{
  "version": 1,
  "name": "Murky",
  "description": "Dark, soupy fog that drinks the light",
  "fields": {
    "colorMode": "lit",
    "albedo": [0.05, 0.05, 0.05],
    "ambientMultiplier": 3.0,
    "attenuationDistance": 25.0,
    "noiseIntensity": 0.4,
    "wind": [0.5, 0.0, 0.2]
  }
}
```

### 6.3 C++ sketch

```cpp
namespace jasperengine {
    enum class FogField : uint8_t { Color, Density, LinearStart, /* ... R2 order ... */ Emissive, Count };

    struct FogProfileData {
        std::string name, description;
        std::bitset<size_t(FogField::Count)> set;
        FogComponent values;        // only `set` fields are meaningful
    };

    // One view's resolved look (R7-R10); the bottom layer is the Fog's own fields.
    struct FogResolve {
        FogComponent resolved;
        std::array<FogLayerSource, size_t(FogField::Count)> source;   // for R34 `resolved`
    };
    FogResolve resolveFog(const FogComponent& fog, const FogProfileData* profile,
                          const FogBlendState* blend, std::span<const FogZoneSample> zones);
}
```

### 6.4 Lua sketch

```lua
local night = FogProfile.load("FogProfiles/night.fogprofile")
scene.fog:blendTo(night, 8.0)          -- eight seconds of game time
caveZone.weight = 0.5                  -- fade a zone by script
local p = scene.fog.profile:clone()
p:set("emissive", Vector3(0.6, 1.4, 0.0))
scene.fog.profile = p                  -- runtime only
```

## 7. Acceptance criteria

- **AC1** Asset round trip (R1-R3, R39).
  - Unit test: `FogProfileTest.RoundTripCanonical`, `.UnknownFieldWarnsOnce`,
    `.AbsentFieldIsNotOverride`.
  - Stream test: `StreamAtmosphereTest.FogProfileFieldsOmitIfDefault`.
  - `python3 tools/check_stream_keys.py` passes.
- **AC2** Built-ins match today (R4, R41). `FogProfileTest.BuiltinsMatchLegacyPresets`: each of the
  five built-ins resolves to the field values `AtmospherePresets::applyFogPreset` sets. The existing
  atmosphere goldens are unchanged.
- **AC3** Resolve order and blend rules (R7-R10).
  - `FogResolveTest.LayerOrder`, `.ExtinctionSpaceBlend`, `.EnumSwitchAtHalf`,
    `.ZoneWeightLinearInBlendDistance`, `.PriorityThenEntityId`, `.BlendToRetargetNoJump`,
    `.BlendToNilReturnsToOwnFields`.
  - These fail on main, where the API is absent, and pass with the change.
- **AC4** Zero cost (R11, R20). `AtmosphereQualityTest.NoProfileNoResolve` plus the S15 pass list:
  identical passes and uploads with no profile, zone or blend.
- **AC5** Lit mode (R12).
  - Golden `atmosphere_lit_day` and `atmosphere_lit_night`: the same profile under two suns.
  - Unit test `AtmosphereMathTest.LitColourFromAmbientAndSun`.
- **AC6** Fog lights (R13-R14).
  - Goldens `fog_spot_shadowed` and `fog_point_shadowed`, with a wall between the light and the
    camera: no light behind the wall.
  - The red control is the same scene on main, with light through the wall.
  - `AtmosphereQualityTest.FogLightTiers`: 1/2/4.
  - `AtmosphereRendererTest.FogLightFrustumCull`.
- **AC7** Softer shafts (R15). The goldens `atmosphere_shafts_low`, `_medium` and `_high`
  (`engine/tests/core/render/AtmosphereGpuTest.cpp:422`) are regenerated one suite at a time, each
  image looked at, with the reason stated in the commit. The bit-identical two-frame check
  (S12 R12) still passes.
- **AC8** Particles (R16). Golden `particles_height_fog`: particles inside a ground mist match the
  mesh fog at the same depth within 3/255. The red control is main, where they take the Fog's
  linear fields instead.
- **AC9** Diagnostics (R17). `AtmosphereDiagnosticsTest.<Code>`, one per code: each builds the
  state, expects the code, then fixes it and expects no code.
- **AC10** Opacity (R18). `AtmosphereMathTest.OpacityMatchesGpuAnalytic`, within 0.02 over five
  profiles.
- **AC11** Editor (R23-R30).
  - Unit tests `FogProfileAssetCmdTest.EditUndoRestoresFile` and
    `LinkFogProfileCmdTest.UnlinkCopiesResolved`.
  - Screenshots from a scratch-project editor of the linked Fog inspector, the profile inspector,
    and a Fog Zone with its gizmo.
- **AC12** MCP (R31-R35). `tools/mcp/tests/test_fog_profiles.py` (CI = True):
  - link a built-in, which creates its project copy;
  - edit through `edit_fog_profile`, then undo;
  - create a zone and check its weight in `get_atmosphere_info` as the camera moves;
  - check the diagnostics round trip;
  - check that unlink keeps the look.
- **AC13** Lua (R36, R43). An MCP Play test: a script runs `blendTo` and samples `getResolved` at
  0, 50 and 100% of game time. `clone():set` does not change the asset file. A Lua write to a
  profile-set field takes effect and is gone after Stop. `set_properties` on a profile-set field
  errors with both fixes named.
- **AC17** Lit with volumes (R12). `AtmosphereGpuTest.LitVolumeNotDoubleTinted`: a white Local
  Volumetric Fog inside green Lit fog renders the volume's own scattering untinted by the fog's
  albedo. The S12 handoff-equality test passes in Lit mode.
- **AC14** Eval (R37). The new cases fail without the guide and description text and pass with it,
  run on Orquester-managed accounts.
- **AC15** Performance (R21-R22). Bench numbers are recorded in this spec's Deviations, with the
  machine named.
- **AC16** Platforms (R19).
  - `tools/build.sh web`, `tools/build.sh android`, and a Vulkan run of the goldens.
  - `SMOKE_METHOD=compile tools/smoke-test.sh` on a scratch project with a linked profile and a
    zone.

## 8. Dependencies & conflicts

- **Depends on:** S12, S10, S20, S04, S05, S24, S31. All have landed.
- **Merge hotspots:**
  - `AtmosphereRenderer.cpp`, `volumetric_fog.frag`, `shadow_sampling.glsl`;
  - `InspectorWindow.cpp` (the Fog section);
  - `ShaderVariantCompiler.cpp` (the particle bits);
  - `AssetType.h` (value 35);
  - `AtmosphereTools.cpp`, `docs/mcp/guide/atmosphere.md`, `tools/mcp/budget.json`.

## 9. Risks & defaults

- **The particle variant may lack a free bit for height fog.**
  - Default: reuse the mesh scheme, where height fog is chosen by a uniform at run time under
    `HAS_FOG`.
  - If that is impossible, record it as a deviation.
- **Point shadows in the fog pass on GLES3** (texture units, uniform vectors).
  - Default: bind only when needed (R14).
  - If the budget fails, points stay unshadowed on GLES3. Record it as a deviation, and show the
    `light_not_scattered` diagnostic as an info message.
- **The R22 budgets are estimates.** No phone or reference-GPU runs exist yet (S12 Deviations).
  - Default: measure on what is available, scale as S12 did, and record it.
- **"Used by N" needs the asset reference index.**
  - Default: the existing reference index behind `RetargetAssetRefsCmd`.
  - If scenes not loaded are not indexed, the badge says "in open scenes".
- **Lit ambient source.** The default is the scene ambient colour × intensity, which is what meshes
  add. SH ambient (L0) is a later refinement.

## Milestones

1. **M1**: the particle fix, diagnostics, opacity, and the eval fix (R16-R18, R28, R29, R37 first
   case). The old fog preset menu is not touched; M2 replaces it.
2. **M2**: the profile asset, built-ins, link and unlink, the inspectors, and the MCP extensions
   (R1-R6, R23-R26, R31-R34).
3. **M3**: Lit mode (R12, R30).
4. **M4**: Fog Zones, the resolve, `blendTo`, runtime writes, and Lua (R7-R11, R27, R35, R36,
   R43).
5. **M5**: fog lights, shadows, and softer shafts (R13-R15, R19-R22).

## Deviations

- **AC10's test is `AtmosphereMathGpu.OpacityMatchesGpuAnalytic`**, not `AtmosphereMathTest.…`: it reads
  back the GPU, and a suite that needs a GPU ends in `Gpu` (`docs/testing.md`, Naming), which decides
  how it runs. The CPU side is `AtmosphereMath.FogOpacityFollowsEachFogType`.
- **`volumetric_needs_height` also covers volumes with the volumetric part off**: Local Volumetric Fog
  in a scene whose Height fog has Volumetric off (R17 lists no code of its own for it); the message
  and fix say which.
- **Profiles, zones and the resolve (R1-R11, R43).**
  - The Fog handle's C++ setters (`Fog::setColor`, ...) take the runtime layer too while the game
    runs, not only Lua and animation: Lua binds that handle, and a C++ script's write must show
    over a profile as a Lua one does. Outside a running game they write the Fog as before.
  - `Fog:applyPreset` writes the Fog's own fields in Play as well (R36: unchanged).
  - A Fog Zone's profile set from Lua with a clone names it by its `runtime:<n>` key; runtime
    profiles live until Stop (`FogProfileLibrary::resetRuntime`), so a script clones once.
  - Test names: the stream test is `StreamAtmosphere.FogProfileFieldsOmitIfDefault` (the
    file's suite); AC4 is `FogResolveTest.NothingToLayerNeedsNoResolve` (CPU, no allocation)
    and `AtmosphereGpu.NoProfileNoResolve` (the same frame and passes through the Fog itself).
- **MCP (R31-R35, R43).**
  - `apply_atmosphere_preset` on a Fog links a built-in id (the five old presets included) instead of
    writing its fields: the old ids are built-in profiles, and a link is R32's behaviour. Clouds keep
    their presets. `unlink: true` on a Fog Zone is refused: a zone's look is its profile.
  - `edit_fog_profile`'s `set` also takes `name` and `description` (R3's fields), and needs `from` to
    create a file: a path that does not exist is refused rather than made empty, so a typo never
    makes a new profile.
  - `edit_fog_profile` is a step of the project history, as an edit in the `.fogprofile` Inspector
    is; `apply_atmosphere_preset`'s link or unlink (a built-in's copy included) is a step of the
    scene's history, as the Profile row's is. Both run the Inspector's commands
    (`FogProfileAssetCmd`, `LinkFogProfileCmd`).
  - `set_properties` refuses a profile-set Fog field in Play too (the running scene's write would not
    show either), and lets a call through that changes `profile` itself in the same call.
  - `profile_no_effect` judges by the Fog's Type only (distance types read Color, Density and the
    Linear range; Height every other field and Color), not by its Color Mode.
  - R37's first new case is a guard (`guard_cave_fog_through_a_zone`), not a `bad` case: with the
    sentence on Fog Zones cut from `apply_atmosphere_preset`'s description, agents still created a
    Fog Zone in 3/3 samples (through `create_entity`'s types and `list_component_types`), so no new
    text needs it and the sentence was cut.
  - R37's fixed `guard_atmosphere_preset` needed new text: asked for haze, agents opened
    `get_scene_settings` first (Unity keeps fog in Lighting), found none and tinted the background
    (1/4 twice; main's case scored 2/3 on main's atmosphere text and 1/3 on this one). In a 3D
    scene without a Fog, `get_scene_settings` now says in `next` that fog is a Fog GameObject and
    names `create_entity` and `apply_atmosphere_preset` (4/4 twice; 0/4 twice without the
    `apply_atmosphere_preset` part).
- **R22, AC15: the fog-light budgets are not measured on the reference hardware.** The bench has
  the scenes (`tools/performance/atmosphere_bench.py`: 1, 2 and 4 shadowed spot lights on Low,
  Medium and High and 1 shadowed point light on Low, each against the same lights with Volumetric
  Intensity 0). They ran once, `--relative --frames 60`, on the shared build host (12-core AMD EPYC
  Genoa, no GPU: Mesa llvmpipe in the Linux image, an 867×488 Game view, other builds running).
  It scattered 1, 2, 4 and 1 lights as configured. The software timings are noise at this scale
  (fog-light cost +3.3, -1.1, +74 and -43 ms on 250-690 ms frames) and say nothing about a GPU, so
  no budget is claimed met or missed. The two sun taps' Medium cost (R22's 0.1 ms) has no bench
  configuration of its own: the bench cannot switch them off, so it needs the S15 pass timers
  (`--gpu`) before and after on a reference GPU. Still to run: `--gpu --runs 5` on the desktop
  reference and `--mobile` on an Adreno 650 / Mali-G78 phone.
- **R19: GLES3 limits are counted, not measured on a device.** The fog pass with the point shadows
  ('Pnt' variant) takes 5 fragment texture units and 217 fragment uniform vectors on GLES3 (with
  the Lit mode's two vec4), under the 16 and 224 every GLES3 device has
  (`AtmosphereShaderBuild.FogPointShadowsFitGles3Limits`); the plain variant 4 and 177. 217 of 224
  leaves little room: a later field in the fog's block
  must replace an unused component, as the shadow slot does (`lightCone.z`).
- **Lit test names (AC5, AC17)** follow the existing suites: `AtmosphereMath.LitColourFromAmbientAndSun`,
  `AtmosphereGpu.LitFogDay`, `AtmosphereGpu.LitFogNight` (goldens `atmosphere_lit_day`,
  `atmosphere_lit_night`) and `AtmosphereGpu.LitVolumeNotDoubleTinted`.
- **The editor UI (R23-R28).**
  - A profile edit made in the Fog or Fog Zone Inspector (a ticked row, a tick, All/None, Add Field,
    and New, Duplicate or a built-in's copy with the link) is a step of that scene's history, as a
    `.particles` edit from the Particle System Inspector is, so the link and its file stay one
    step; the `.fogprofile` Inspector and Create > Fog Profile use the project history.
  - While a scene plays, the Profile row and the profile's rows are read only (R33 refuses
    `edit_fog_profile` in Play too); the Fog's own rows edit as any Play edit does. A Fog named
    `builtin:<id>` directly (Lua, `set_properties`) has no file: its rows are read only and
    Duplicate makes one.
  - "Used by N" counts the Fogs and Fog Zones of the open scenes and each closed scene or prefab
    the reference index lists once; until the index is ready it reads "in open scenes".
  - A row whose effective value differs (R25) shows a layer icon before its label, whose tooltip
    gives the value and its layer.
