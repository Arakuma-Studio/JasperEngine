# Shader library, phase 3: scene depth and scene colour

Status: approved scope; implementation details are refined against the merged phase 1 and
phase 2 code before work starts.
Depends on: phases 1 and 2. Extends their contracts; does not redefine them.

## 1. Goal

Let mesh shaders read the opaque scene's depth and colour, so the library gains the
effects that need them (soft particles, depth-fade foam water, glass, heat haze, projected
decals, intersection highlights), while scenes that use none of them pay nothing.

## 2. Requirements

### 2.1 Scene inputs

- Manifest `@require sceneDepth` and `@require sceneColor` (phase 1 section 4 directive,
  new values).
- `includes/scene.glsl` API for library shaders:

  ```glsl
  vec2  screenUV();                       // 0..1, correct orientation on every backend
  float sceneLinearDepth(vec2 uv);        // view-space distance of the opaque surface
  float fragmentLinearDepth();            // view-space distance of the current fragment
  vec3  sceneWorldPosition(vec2 uv);      // reconstructed from depth
  vec3  sceneColor(vec2 uv);              // linear colour of the opaque scene
  ```

- The engine produces scene depth only when a visible draw in the current camera pass
  requires it, and scene colour only when one requires `sceneColor`. Producing them:
  - Depth: reuse the existing opaque depth pre-pass when it is already running (SSAO,
    depth-sampling post passes, depth prepass setting); otherwise run it on demand. The
    format and decode must be correct on GLES3 (no float targets) and match
    `depth_util.glsl`.
  - Colour: one copy of the opaque + sky result taken after the opaque queue and before
    the transparent queue, at a configurable scale (full or half resolution, half by
    default), linear filtering.
- Requiring materials are drawn in the transparent queue; an opaque material that requires
  a scene input is reported as a manifest/state error in the inspector and check tooling.
- Works with fixed-resolution and render-to-texture cameras; the editor Scene view shows
  the effects too.
- Mobile cost is visible to the user: the shader picker marks shaders that require scene
  colour or depth, with a tooltip about the extra pass.

### 2.2 Library shaders and features

| Name / feature | Technique |
|---|---|
| `particle` `SOFT` and `unlit` `SOFT` | Soft particles: fade alpha by `saturate((sceneDepth - fragmentDepth) / softness)`; also camera near fade. Works on points and instanced-mesh particles. |
| `water` `DEPTH_FOAM`, `ABSORPTION`, `REFRACTION` | Shoreline/intersection foam from depth difference, depth-based colour absorption, refraction of the scene colour by the water normals. |
| `glass` | Refraction (scene colour offset by normal map and IOR-like strength), tint, fresnel reflection from the reflection probe or a cubemap, optional blur by sampling a mip/half-res copy. |
| `heat_haze` | Screen distortion of the scene colour by scrolling noise, masked by a texture/vertex alpha. |
| `rim` `INTERSECTION` | Force-field highlight where the surface intersects geometry. |
| `projected_decal` | Box projector: reconstruct the world position from scene depth, project into the decal box, clip outside, sample texture; angle fade; drawn as a cube with front-face culling off-camera-safe. |
| `depth_fade` | Fog cards and light shafts: fade by depth intersection and by camera distance (replaces the owner's platform-dependent clip-z fade). |

## 3. Verification

- Compile matrix covers the new requirements and shaders on all six backends.
- Fixture scene group with soft particles intersecting geometry, depth-foam water on a
  shore, glass in front of geometry, heat haze, projected decals on uneven ground and a
  force field intersecting the floor; smoke test passes and the screenshot shows each.
- A scene with none of these shaders runs no depth pre-pass or colour copy it did not run
  before (verified through a pass-count debug log or counter).
- Phase 1 and 2 regression checks still pass.

## 4. Definition of done

All requirements implemented, all checks pass, merged into `shader-library`, review pass
found no open correctness bugs.
