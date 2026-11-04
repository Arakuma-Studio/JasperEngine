# S72 — Build hygiene: fewer rebuilds, and sanitizer builds install

**Depends on.** S33. Touches headers across `editor/` and `engine/core/`; merge hotspot. Land fast and merge `main` often.

## 1. Goal
Editing a component header should not rebuild 521 editor files. That wait is paid by every engine change. Sanitizer Linux builds
should also produce an installed editor as normal builds do, so sanitizer MCP runs need no special case.

## 2. Requirements
- R1. Measure: for each `engine/core/component/*.h` and the top 20 most-included editor headers, how many translation units
  rebuild (ninja `-t` queries or depfiles). Save before and after tables in Deviations.
- R2. Cut the fan-out at its causes. Remove umbrella includes (e.g. a header that pulls in every component), use forward
  declarations, split headers, move code out of headers. Goal: editing a typical component header rebuilds at most 80 editor files;
  the worst case at most 150. If a limit proves impossible without harming the code, state why and give the achieved number.
- R3. No runtime change: no behaviour, no ABI of generated code, exported project template and prebuilt players still build. Each
  edited header still compiles alone (a header self-sufficiency check in `tools/`, run by check-changed).
- R4. Keep the portable C++20 subset; Windows and macOS code gets the same include fixes by careful reading, and the Mac build
  proves its side.
- R5. Sanitizer `linux` builds: `cmake --install --strip` at `tools/build.sh:413` installs nothing for `editor-address-undefined`.
  Find why, fix it, and make MCP tests able to run on that install. Strip is skipped in sanitizer builds.

## 3. Acceptance
- AC1. Before/after tables. A check in `tools/` fails if fan-out passes the new worst-case limit.
- AC2. `tools/build.sh linux`, `web`, `android`, unit tests, unity build, Mac editor build, all green.
- AC3. `SANITIZE=address,undefined tools/build.sh linux` then an MCP test file run against its install.
- AC4. `tools/check-changed` and docs (`docs/testing.md`).

## Deviations

- **The umbrella stays for code outside the engine.** R2 says remove it. `EntityRegistry.h` keeps its
  component includes behind `#ifndef JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES` (`GameObject.h` likewise for
  the classes its getters return), and only the engine's and the editor's own targets define the macro. A
  game's scripts have always had every component through `ScriptBase.h` and `Scene.h`, so removing it
  outright would break projects that build today (R3, "old projects must load"). The template, the test
  projects, exports, the Xcode workspace and the core tests build as before.
- **The editor's tests build with the macro, the core tests without it.** The former inherit the editor's
  definitions; the latter see the headers as a game does, which keeps that mode compiled by the unit gate.
  An edit of a component still rebuilds the core tests that reach `Scene.h`.
- **The limits are for component headers.** R2's 80 and 150 count the units of the editor's targets for a file
  of `engine/core/component/`. `tools/check_fanout.py` fails past them. The editor's own hubs (`Catalog.h`,
  `Project.h`) stay around 400: they are the editor's vocabulary, the tables only report them.
- **`Transform.h` is close to the limit by what it is:** 143 units against 150, and about as many sources read
  its fields, so the number is uses, not a chain of includes. A wave that adds many Transform users may reach
  150; the check then asks for a decision, which can be a higher limit stated here with its reason.
- **`MeshComponent.h` was left alone** (`dc/mesh-memory` rewrites it): only its includers changed.
- **Unity (AC2):** sources that came with main since S33 no longer shared a batch (file-local helpers named
  alike, a `using namespace` reaching a later source, Xlib in `ScreenColorX11.cpp`, `LaunchKeysX11.cpp`,
  `HostServicesLinux.cpp`). They are listed in `jasperengine_skip_unity_build` rather than renamed, to touch no
  source a branch in flight edits. That they predate this work is inferred from the errors; main's own unity
  build was not run.
- **Every component header now compiles alone** (`check_headers.py engine/core/component/*.h`, 90 of 90): six did
  not, and a source that includes one first now would hit it.
- **`tools/check-changed --no-mcp`** is new: a change of the root CMake or `tools/build.sh` selects the whole MCP
  suite, which the integrator runs once per wave. It says so in the summary and caches no pass.
- **R5, cause:** every install rule said `CONFIGURATIONS Release MinSizeRel`, and a sanitizer tree is Debug
  by default, so `cmake --install` found nothing to do. The rules now take `JASPERENGINE_INSTALL_CONFIGURATIONS`,
  which adds Debug and RelWithDebInfo under `JASPERENGINE_SANITIZE`. Beyond the spec, that build also skips
  the release symbol checks (at -O0 the engine exports std templates on RmlUi types: 11 of them, none of
  RmlUi itself), and `SANITIZE=... tools/build.sh mcp-tests` turns leak detection off: the editor's exit reports
  frames of an unmapped module, which no suppression can name (the first run: `test_stdio_raw`, exit -6;
  that the module is the GL driver is inferred, from `tools/sanitizers/lsan.supp` and the frames).

### Fan-out, measured with `tools/check_fanout.py --table`

Before: the base of this work, d677b386a, `build/linux/editor` on server H (worst 454, median 451; the check
exits 1 there, which is its red control). After: this branch merged with `main` (worst 143, median 29).

#### Before: component headers

| Header | Editor units | Other units |
|---|---:|---:|
| `engine/core/component/LatticeComponent.h` | 454 | 215 |
| `engine/core/component/LatticeModifierComponent.h` | 454 | 215 |
| `engine/core/component/AnimationPlayerComponent.h` | 452 | 216 |
| `engine/core/component/KeyframeTracksComponent.h` | 452 | 213 |
| `engine/core/component/LatticeTracksComponent.h` | 452 | 213 |
| `engine/core/component/ActionComponent.h` | 451 | 213 |
| `engine/core/component/AlphaTweenComponent.h` | 451 | 213 |
| `engine/core/component/AnimationComponent.h` | 451 | 213 |
| `engine/core/component/AnimatorComponent.h` | 451 | 213 |
| `engine/core/component/AudioSourceComponent.h` | 451 | 217 |
| `engine/core/component/BoneComponent.h` | 451 | 213 |
| `engine/core/component/BuoyancyComponent.h` | 451 | 213 |
| `engine/core/component/ButtonComponent.h` | 451 | 213 |
| `engine/core/component/CameraComponent.h` | 451 | 221 |
| `engine/core/component/CameraFollowComponent.h` | 451 | 213 |
| `engine/core/component/CameraShakerComponent.h` | 451 | 213 |
| `engine/core/component/CanvasGroupComponent.h` | 451 | 213 |
| `engine/core/component/CharacterCollision.h` | 451 | 213 |
| `engine/core/component/CharacterController2DComponent.h` | 451 | 213 |
| `engine/core/component/CharacterControllerComponent.h` | 451 | 213 |
| `engine/core/component/ColorTweenComponent.h` | 451 | 213 |
| `engine/core/component/FluidSimulationComponent.h` | 451 | 215 |
| `engine/core/component/FogComponent.h` | 451 | 215 |
| `engine/core/component/ForceFieldComponent.h` | 451 | 213 |
| `engine/core/component/ImageComponent.h` | 451 | 213 |
| `engine/core/component/InputFieldComponent.h` | 451 | 213 |
| `engine/core/component/InstancedMeshComponent.h` | 451 | 219 |
| `engine/core/component/Joint2DComponent.h` | 451 | 213 |
| `engine/core/component/Joint3DComponent.h` | 451 | 213 |
| `engine/core/component/LatticeFollowerComponent.h` | 451 | 213 |
| `engine/core/component/LayoutGroupComponent.h` | 451 | 213 |
| `engine/core/component/Light2DComponent.h` | 451 | 213 |
| `engine/core/component/LightComponent.h` | 451 | 213 |
| `engine/core/component/LineRendererComponent.h` | 451 | 213 |
| `engine/core/component/LocalVolumetricFogComponent.h` | 451 | 214 |
| `engine/core/component/MeshComponent.h` | 451 | 222 |
| `engine/core/component/MeshPolygonComponent.h` | 451 | 219 |
| `engine/core/component/MirrorComponent.h` | 451 | 213 |
| `engine/core/component/ModelComponent.h` | 451 | 219 |
| `engine/core/component/MorphTracksComponent.h` | 451 | 213 |
| `engine/core/component/NavGridComponent.h` | 451 | 213 |
| `engine/core/component/NavMeshAgentComponent.h` | 451 | 213 |
| `engine/core/component/NavMeshLinkComponent.h` | 451 | 213 |
| `engine/core/component/NavMeshModifierComponent.h` | 451 | 213 |
| `engine/core/component/NavMeshObstacleComponent.h` | 451 | 213 |
| `engine/core/component/NavMeshSurfaceComponent.h` | 451 | 213 |
| `engine/core/component/OnScreenControlComponents.h` | 451 | 213 |
| `engine/core/component/PanelComponent.h` | 451 | 213 |
| `engine/core/component/ParticleSystemComponent.h` | 451 | 213 |
| `engine/core/component/PhysicsCommon.h` | 451 | 215 |
| `engine/core/component/PointsComponent.h` | 451 | 213 |
| `engine/core/component/PolygonComponent.h` | 451 | 219 |
| `engine/core/component/PoseAnimatorComponent.h` | 451 | 213 |
| `engine/core/component/PositionTweenComponent.h` | 451 | 213 |
| `engine/core/component/PrefabComponent.h` | 451 | 213 |
| `engine/core/component/ProgressbarComponent.h` | 451 | 213 |
| `engine/core/component/RagdollComponent.h` | 451 | 213 |
| `engine/core/component/RectTransformComponent.h` | 451 | 213 |
| `engine/core/component/ReflectionProbeComponent.h` | 451 | 213 |
| `engine/core/component/Rigidbody2DComponent.h` | 451 | 213 |
| `engine/core/component/RigidbodyComponent.h` | 451 | 213 |
| `engine/core/component/RotateTracksComponent.h` | 451 | 213 |
| `engine/core/component/RotationTweenComponent.h` | 451 | 213 |
| `engine/core/component/SafeAreaComponent.h` | 451 | 213 |
| `engine/core/component/ScaleTracksComponent.h` | 451 | 213 |
| `engine/core/component/ScaleTweenComponent.h` | 451 | 213 |
| `engine/core/component/ScriptComponent.h` | 451 | 213 |
| `engine/core/component/ScrollbarComponent.h` | 451 | 213 |
| `engine/core/component/ShadowCaster2DComponent.h` | 451 | 213 |
| `engine/core/component/SkyboxComponent.h` | 451 | 213 |
| `engine/core/component/SplineAnimateComponent.h` | 451 | 213 |
| `engine/core/component/SplineComponent.h` | 451 | 213 |
| `engine/core/component/SplineExtrudeComponent.h` | 451 | 213 |
| `engine/core/component/SplineInstantiateComponent.h` | 451 | 213 |
| `engine/core/component/SpriteAnimationComponent.h` | 451 | 213 |
| `engine/core/component/SpriteComponent.h` | 451 | 219 |
| `engine/core/component/TerrainComponent.h` | 451 | 220 |
| `engine/core/component/TextComponent.h` | 451 | 213 |
| `engine/core/component/TextEffectComponent.h` | 451 | 213 |
| `engine/core/component/TilemapComponent.h` | 451 | 219 |
| `engine/core/component/TrailRendererComponent.h` | 451 | 213 |
| `engine/core/component/Transform.h` | 451 | 220 |
| `engine/core/component/TranslateTracksComponent.h` | 451 | 213 |
| `engine/core/component/TweenComponent.h` | 451 | 213 |
| `engine/core/component/UIComponent.h` | 451 | 213 |
| `engine/core/component/UIDocumentComponent.h` | 451 | 216 |
| `engine/core/component/VertexAnimatorComponent.h` | 451 | 214 |
| `engine/core/component/VideoPlayerComponent.h` | 451 | 214 |
| `engine/core/component/VolumetricCloudsComponent.h` | 451 | 214 |
| `engine/core/component/WaterVolumeComponent.h` | 451 | 213 |

#### Before: the 20 most included editor headers

| Header | Editor units | Other units |
|---|---:|---:|
| `editor/Catalog.h` | 428 | 0 |
| `editor/util/SafeWrite.h` | 413 | 0 |
| `editor/LoadReport.h` | 404 | 0 |
| `editor/command/UndoOrigin.h` | 403 | 0 |
| `editor/command/Command.h` | 401 | 0 |
| `editor/Factory.h` | 398 | 0 |
| `editor/texsynth/SynthImage.h` | 398 | 0 |
| `editor/util/Prefab.h` | 398 | 0 |
| `editor/texture/TextureImportSettings.h` | 397 | 0 |
| `editor/StreamMigrations.h` | 396 | 0 |
| `editor/util/CommandRunner.h` | 396 | 0 |
| `editor/DebugSettings.h` | 394 | 0 |
| `editor/asset/DepEdge.h` | 394 | 0 |
| `editor/util/Util.h` | 394 | 0 |
| `editor/asset/AssetTypes.h` | 393 | 0 |
| `editor/render/RenderUtil.h` | 393 | 0 |
| `editor/spritenormals/SpriteNormals.h` | 393 | 0 |
| `editor/Generator.h` | 392 | 0 |
| `editor/command/CommandHistory.h` | 392 | 0 |
| `editor/vcs/VcsPathKey.h` | 392 | 0 |

#### After: component headers

| Header | Editor units | Other units |
|---|---:|---:|
| `engine/core/component/Transform.h` | 143 | 91 |
| `engine/core/component/MeshComponent.h` | 127 | 80 |
| `engine/core/component/TerrainComponent.h` | 124 | 67 |
| `engine/core/component/RectTransformComponent.h` | 98 | 77 |
| `engine/core/component/ParticleSystemComponent.h` | 93 | 87 |
| `engine/core/component/LineRendererComponent.h` | 88 | 59 |
| `engine/core/component/ModelComponent.h` | 88 | 59 |
| `engine/core/component/TextComponent.h` | 80 | 16 |
| `engine/core/component/FogComponent.h` | 79 | 61 |
| `engine/core/component/SpriteComponent.h` | 79 | 64 |
| `engine/core/component/AudioSourceComponent.h` | 78 | 37 |
| `engine/core/component/AnimationComponent.h` | 77 | 22 |
| `engine/core/component/UIComponent.h` | 70 | 59 |
| `engine/core/component/LightComponent.h` | 67 | 57 |
| `engine/core/component/AnimationPlayerComponent.h` | 66 | 37 |
| `engine/core/component/CameraComponent.h` | 66 | 65 |
| `engine/core/component/PointsComponent.h` | 64 | 47 |
| `engine/core/component/TilemapComponent.h` | 64 | 48 |
| `engine/core/component/PhysicsCommon.h` | 63 | 63 |
| `engine/core/component/PoseAnimatorComponent.h` | 62 | 15 |
| `engine/core/component/ScriptComponent.h` | 62 | 24 |
| `engine/core/component/InstancedMeshComponent.h` | 60 | 47 |
| `engine/core/component/Light2DComponent.h` | 59 | 45 |
| `engine/core/component/RigidbodyComponent.h` | 59 | 59 |
| `engine/core/component/TrailRendererComponent.h` | 58 | 46 |
| `engine/core/component/ImageComponent.h` | 57 | 51 |
| `engine/core/component/Rigidbody2DComponent.h` | 57 | 55 |
| `engine/core/component/SkyboxComponent.h` | 57 | 45 |
| `engine/core/component/ReflectionProbeComponent.h` | 56 | 57 |
| `engine/core/component/LatticeComponent.h` | 54 | 29 |
| `engine/core/component/LocalVolumetricFogComponent.h` | 53 | 58 |
| `engine/core/component/VolumetricCloudsComponent.h` | 47 | 26 |
| `engine/core/component/ProgressbarComponent.h` | 45 | 25 |
| `engine/core/component/ActionComponent.h` | 43 | 38 |
| `engine/core/component/LatticeModifierComponent.h` | 43 | 22 |
| `engine/core/component/UIDocumentComponent.h` | 36 | 48 |
| `engine/core/component/PrefabComponent.h` | 35 | 6 |
| `engine/core/component/TranslateTracksComponent.h` | 33 | 17 |
| `engine/core/component/PolygonComponent.h` | 32 | 16 |
| `engine/core/component/KeyframeTracksComponent.h` | 30 | 22 |
| `engine/core/component/LatticeTracksComponent.h` | 30 | 16 |
| `engine/core/component/MeshPolygonComponent.h` | 30 | 11 |
| `engine/core/component/RotateTracksComponent.h` | 30 | 17 |
| `engine/core/component/SpriteAnimationComponent.h` | 30 | 17 |
| `engine/core/component/AlphaTweenComponent.h` | 29 | 31 |
| `engine/core/component/MorphTracksComponent.h` | 29 | 17 |
| `engine/core/component/ScaleTracksComponent.h` | 29 | 17 |
| `engine/core/component/ColorTweenComponent.h` | 27 | 16 |
| `engine/core/component/PositionTweenComponent.h` | 27 | 16 |
| `engine/core/component/RotationTweenComponent.h` | 27 | 16 |
| `engine/core/component/ScaleTweenComponent.h` | 27 | 16 |
| `engine/core/component/SplineComponent.h` | 26 | 20 |
| `engine/core/component/TweenComponent.h` | 26 | 19 |
| `engine/core/component/FluidSimulationComponent.h` | 25 | 29 |
| `engine/core/component/Joint3DComponent.h` | 25 | 30 |
| `engine/core/component/VertexAnimatorComponent.h` | 24 | 17 |
| `engine/core/component/ForceFieldComponent.h` | 23 | 14 |
| `engine/core/component/Joint2DComponent.h` | 23 | 25 |
| `engine/core/component/BoneComponent.h` | 22 | 11 |
| `engine/core/component/VideoPlayerComponent.h` | 22 | 29 |
| `engine/core/component/SplineExtrudeComponent.h` | 21 | 33 |
| `engine/core/component/CanvasGroupComponent.h` | 19 | 27 |
| `engine/core/component/ShadowCaster2DComponent.h` | 19 | 27 |
| `engine/core/component/SplineInstantiateComponent.h` | 19 | 32 |
| `engine/core/component/AnimatorComponent.h` | 18 | 16 |
| `engine/core/component/PanelComponent.h` | 17 | 14 |
| `engine/core/component/CameraShakerComponent.h` | 16 | 15 |
| `engine/core/component/CharacterCollision.h` | 16 | 33 |
| `engine/core/component/LayoutGroupComponent.h` | 16 | 26 |
| `engine/core/component/NavGridComponent.h` | 16 | 10 |
| `engine/core/component/OnScreenControlComponents.h` | 16 | 16 |
| `engine/core/component/SplineAnimateComponent.h` | 16 | 32 |
| `engine/core/component/BuoyancyComponent.h` | 15 | 27 |
| `engine/core/component/ButtonComponent.h` | 15 | 17 |
| `engine/core/component/CameraFollowComponent.h` | 15 | 15 |
| `engine/core/component/NavMeshSurfaceComponent.h` | 15 | 12 |
| `engine/core/component/CharacterController2DComponent.h` | 14 | 15 |
| `engine/core/component/CharacterControllerComponent.h` | 14 | 17 |
| `engine/core/component/NavMeshAgentComponent.h` | 14 | 12 |
| `engine/core/component/NavMeshLinkComponent.h` | 14 | 9 |
| `engine/core/component/ScrollbarComponent.h` | 14 | 27 |
| `engine/core/component/WaterVolumeComponent.h` | 14 | 26 |
| `engine/core/component/InputFieldComponent.h` | 13 | 15 |
| `engine/core/component/MirrorComponent.h` | 13 | 10 |
| `engine/core/component/LatticeFollowerComponent.h` | 12 | 12 |
| `engine/core/component/NavMeshModifierComponent.h` | 12 | 9 |
| `engine/core/component/NavMeshObstacleComponent.h` | 12 | 9 |
| `engine/core/component/SafeAreaComponent.h` | 12 | 10 |
| `engine/core/component/TextEffectComponent.h` | 12 | 10 |
| `engine/core/component/RagdollComponent.h` | 11 | 12 |

#### After: the 20 most included editor headers

| Header | Editor units | Other units |
|---|---:|---:|
| `editor/Catalog.h` | 429 | 0 |
| `editor/util/SafeWrite.h` | 413 | 0 |
| `editor/LoadReport.h` | 404 | 0 |
| `editor/command/UndoOrigin.h` | 403 | 0 |
| `editor/command/Command.h` | 401 | 0 |
| `editor/Factory.h` | 398 | 0 |
| `editor/texsynth/SynthImage.h` | 398 | 0 |
| `editor/util/Prefab.h` | 398 | 0 |
| `editor/texture/TextureImportSettings.h` | 397 | 0 |
| `editor/StreamMigrations.h` | 396 | 0 |
| `editor/util/CommandRunner.h` | 396 | 0 |
| `editor/DebugSettings.h` | 394 | 0 |
| `editor/asset/DepEdge.h` | 394 | 0 |
| `editor/util/Util.h` | 394 | 0 |
| `editor/asset/AssetTypes.h` | 393 | 0 |
| `editor/render/RenderUtil.h` | 393 | 0 |
| `editor/spritenormals/SpriteNormals.h` | 393 | 0 |
| `editor/Generator.h` | 392 | 0 |
| `editor/command/CommandHistory.h` | 392 | 0 |
| `editor/vcs/VcsPathKey.h` | 392 | 0 |
