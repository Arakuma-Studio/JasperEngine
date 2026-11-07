# Lighting after The Forge: tiles, capsules, contact shadows, OIT and hair

Five lighting features of the forward renderer (spec `docs/specs/S57-forge-lighting.md`). Each is
opt-in or follows the device tier, and costs nothing while it is off: no pass, no texture, no
job and no shader variant.

| Feature | Where | Turned on by | What it costs when on |
|---|---|---|---|
| Light Mode Tiled | Project Settings > Graphics > Light Mode | Automatic: the Medium tier and above, when the scene has a point or spot light | a CPU job a camera pass, one RGBA32F data texture (48 KB at 16x9, 112 KB at 32x18), 4-6 texel fetches per light per pixel |
| Capsule shadows | Scene settings > Capsule Shadows; the Capsule Shadow Caster component | the scene setting, with a caster and a shadowed directional light | up to 32 capsule tests per lit pixel (culled per object), no pass |
| Contact shadows | Light > Shadows > Contact Shadows | a directional or spot light with Shadows on; off on the Low tier | the depth pre-pass (turned on), a fullscreen R8 pass of 8-16 depth reads a pixel, full resolution on High, half on Medium |
| Order Independent transparency | Camera > Transparency Mode | Order Independent on a camera | one extra pass into RGBA16F + R16F targets sharing the camera's depth, a fullscreen resolve |
| Hair shading model | Material > Shading Model > Hair | the material | a few ALU per light over the Standard model (the `PBR_HAIR` variant) |

## Light Mode: Per Object and Tiled

- **Per Object** (today's forward path): each lit object takes its `Max Lights Per Object` (4, 8
  or 16) most important lights, and folds the rest into its spherical-harmonics ambient. A large
  mesh (terrain, a level block) spends its slots on lights far from most of its pixels.
- **Tiled** (Forward+): the camera's view is cut into a grid of screen tiles (16x9, 32x18 on
  High and Ultra). A job projects every point and spot light's range sphere to the screen and lists
  it in the tiles it covers, up to 32 lights a tile and 256 a view (past that the nearest to the
  camera win). Directional lights and lights of unlimited range are in every tile. Each pixel
  lights itself with its tile's lights, so a light never pops as objects pick other lights.
- **Automatic** (default) is Tiled on the Medium tier and above and Per Object on Low. The tier is
  the platform's until the device tiers (S56) resolve the device's: desktop High, web Medium,
  Android and iOS Low.
- A scene whose lights are all directional keeps Per Object (no texture, no job): the tiles would
  hold the same lights.
- A scene with no more lights reaching an object than its cap lights it identically in both modes:
  the tiles add the same lights in the same (scene) order.
- The vertex lights of a Mixed material stay per object. A light's Culling Mask still leaves out
  the layers it excludes.
- Kept per object: a painted terrain (its variant is at the 16-texture limit), forks written
  before the per-object lights block, and passes drawing meshes outside a camera.

### For shader authors

`includes/lights.glsl` keeps its contract: `pixelLightCount()` and `getLight(i, ...)` read the
tile's lights in a Tiled variant (`USE_TILED_LIGHTS`), and `LIGHT_CAPACITY` (the loop bound) is
the tile's cap, 32. A library shader written against those functions needs no change. The tiles
start on even pixels, so a 2x2 quad never straddles two tiles and the loop's trip count is uniform
per quad (the derivatives `lightShadow` takes stay defined).

A shader near GLES3 and WebGL2's 224 fragment uniform vectors can `#define LIGHTS_FS_RECORDS` with
`LIGHTS_FS` (Per Object only; `library:water` and the built-in Lit's painted Terrain do): the block then holds two floats a pixel light
instead of four vectors, and the lights' data comes from the frame's `u_lightRecords` (four texel
fetches a light, one upload a frame while such a mesh is loaded). It draws the same lights the same.

## Capsule shadows

A **Capsule Shadow Caster** holds up to 8 capsules on a character's bones (Bone, Radius, Length,
Offset, Direction; sizes follow the bone's scale, as a Unity collider's). **Fit To Skeleton** finds
the humanoid bones of the GameObject and its children (the same auto-detection as the pose tools)
and fits The Forge's eight capsules: the head (neck to the head's end), the spine (hips to the
upper chest), and each leg's upper leg, lower leg and foot, their radii shares of the body's height.

With Scene settings > Capsule Shadows on, every lit receiver evaluates the main directional
light's soft occlusion by the capsules of the 4 casters nearest the camera: the closest approach of
the ray towards the light to each capsule, smoothstepped over a penumbra that widens by
**Softness** per metre along the ray. **Intensity** fades it. The light must have Shadows on.

**Cast Shadows: Capsules Only** takes the caster's meshes (its GameObject and children) out of
the shadow maps: the capsules are their whole shadow, so a moving character keeps the static
shadow cache (S10) valid and costs no shadow pass. **On** keeps the meshes' own Cast Shadows.

The Scene view draws a selected caster's capsules, with handles on their ends and sides to drag
their length and radius (undoable).

## Contact shadows

A directional or spot light with Shadows on can add **Contact Shadows** (Light > Shadows): after
the depth pre-pass (which they turn on), a fullscreen pass marches from each pixel towards the
light through the depth, as Bend Studio's screen-space shadows do (ported from Sony's
`bend_sss_gpu.h`, Apache-2.0, in `engine/shaderlib/contact_shadows.frag`). Its R8 mask multiplies
the light's shadow term in the opaque pass. One light a frame takes them: the main directional
light when it asks, else the first spot light asking.

- **Length**: metres the march reaches towards the light (0.15).
- **Thickness**: metres a surface is taken to be thick, what it hides behind it (0.05).
- **Sample Count**: 8 to 16 depth samples a pixel, jittered with blue noise (12).
- Full resolution on High and Ultra, half on Medium, off on Low. The main camera only (other cameras
  and reflections take none).

## Order Independent transparency

A camera's **Transparency Mode**: **Sorted** (back to front, one renderer at a time) or **Order
Independent**: its alpha-blended materials go through weighted blended OIT (McGuire and Bavoil,
JCGT 2013), so layered glass, foliage cards and hair look the same whatever their order.

- The accumulation pass draws them into an RGBA16F target (colour times alpha times weight, the
  revealage in its alpha) and an R16F one (the weight), both with one blend state (RGB ONE, ONE;
  alpha ZERO, ONE_MINUS_SRC_ALPHA: GL and GLES3 have no per-target blending), the opaque depth
  loaded and not written. A fullscreen resolve lays their weighted average over the scene.
- **OIT Weight Scale** multiplies the view distances McGuire's depth weight takes: lower it for
  large scenes, raise it for small ones.
- Only straight alpha blending: Additive, Multiply and Premultiplied stay sorted, as do transmissive
  materials, materials with an extra pass, terrains and forks. The built-in Lit and the library
  shaders get the variant (`USE_OIT`: the compiler wraps their `main` and accumulates their
  `g_finalColor`, `includes/oit.glsl`).
- A device without renderable and blendable half-float targets (`sg_query_pixelformat`; with MSAA,
  multisampled ones too) draws the camera Sorted and warns once.

## Hair shading model

**Shading Model: Hair** (the Lit material) lights strands with Kajiya and Kay's model and two
highlights along tangents shifted towards the normal (Scheuermann 2004, as The Forge's UT06): the
**Primary** highlight (its colour, Shift and Width) and the **Secondary** one (tinted by the
albedo). The strands run along the mesh's tangent turned by **Strand Rotation**, or along a **Flow
Map**'s direction (RG in tangent space). Use it with alpha clip, or alpha blending and an Order
Independent camera, for hair cards.

## Lua and MCP

| Inspector | Lua | MCP (`set_properties`, `set_scene_settings`, `set_project_settings`) |
|---|---|---|
| Light > Contact Shadows, Length, Thickness, Sample Count | `light.contactShadows`, `contactShadowLength`, `contactShadowThickness`, `contactShadowSampleCount` | `light`: `contactShadows`, `contactShadowLength`, `contactShadowThickness`, `contactShadowSampleCount` |
| Camera > Transparency Mode, OIT Weight Scale | `camera.transparencyMode` (`TransparencyMode.SORTED`, `ORDER_INDEPENDENT`), `oitWeightScale` | `camera`: `transparencyMode`, `oitWeightScale` |
| Material > Shading Model and the hair values | `material.shadingModel` (`ShadingModel.STANDARD`, `HAIR`), `hairPrimaryColor` ... `hairRotation`, `hairFlowTexture` | the material's `pbr.hair` |
| Capsule Shadow Caster | `CapsuleShadowCaster`: `castShadows`, `addCapsule`, `fitToSkeleton`, ... | `capsuleshadowcaster`: `castShadows`, `capsules` |
| Scene > Capsule Shadows | `scene.capsuleShadowsEnabled`, `capsuleShadowIntensity`, `capsuleShadowSoftness` | `capsule_shadows_enabled`, `capsule_shadow_intensity`, `capsule_shadow_softness` |
| Project Settings > Graphics > Light Mode | (project setting) | `graphics.light_mode`: automatic, per_object, tiled |

`get_render_stats` reports `lighting`: the Light Mode and tier, the tiles' figures (lights in the
view, the most and the average a tile, the lights dropped), the capsules, the contact shadows'
mask size, OIT (whether it draws, the fallback, its draws) and the `contact_shadows_ms` and
`oit_accumulation_ms` GPU times of the pass list (`get_gpu_passes` turns the timing on).
