# pose-animation fixture

The project the pose animation tests run on (phase 2 to 6): two procedurally generated, skinned
glTF humanoids standing 1.5 m apart in `Main Scene.scene`, with a camera framing both, a
directional light from the front, a skybox and a ground plane.

```bash
export JASPER_FIXTURE=$PWD/tools/fixtures/pose-animation    # or Editor(project=...) in a test
```

## Models (`assets/models/`, written by `make_humanoids.py`)

| | `hero_tpose.gltf` | `robot_apose.gltf` |
|---|---|---|
| Entity in the scene | `Hero` at x = -0.75 | `Robot` at x = +0.75 |
| Bone names | Mixamo (`mixamorig:Hips`, `mixamorig:LeftArm` ...) | Blender/Rigify-like (`pelvis`, `upper_arm.L` ...) |
| Bones | 43: hips, 3 spine bones, neck, head, head-top end; per side shoulder, arm, forearm, hand, thumb/index/middle with 3 joints each, up leg, leg, foot, toe base, toe end | 23: pelvis, spine, spine.001, neck, head; per side shoulder, upper_arm, upper_arm_twist (between upper arm and forearm), forearm, hand, thigh, shin, foot, toe |
| Rest pose | T-pose | A-pose, arms 40 degrees below horizontal |
| Proportions | 1.75 m tall | arms 1.2x, legs 0.85x the hero's lengths (1.627 m tall) |
| Rest bone rotations | identity (translations only) | Blender-style: each bone's local +Y points along the bone |
| Humanoid slots left empty | none | `upper_chest` (two spine bones: spine = Spine, spine.001 = Chest) |

Both face +Z with their left side on +X, the feet's lowest vertices at y = 0. The figure is made
of tubes and boxes (hero: round tubes and an ellipsoid head with a nose; robot: square tubes and a
box head with a visor), skinned rigidly per segment with smooth two-bone blends at the joints (up
to 4 weights, normalised). Three materials: centre neutral, left limbs warm (orange), right limbs
cool (blue), so screenshots read left/right. glTF layout: one scene root node (identity) with the
root bone and the skinned mesh node as children; one skin over every bone, skeleton = the root bone.

## Regenerating

```bash
python3 -I tools/fixtures/pose-animation/make_humanoids.py            # rewrite both models
python3 -I tools/fixtures/pose-animation/make_humanoids.py --check    # validate them
python3 -I tools/fixtures/pose-animation/make_humanoids.py --check --verbose   # + bone rest positions
```

Standard library only. The output is deterministic (fixed rounding, fixed JSON order): rerunning
rewrites the same bytes, and `--check` fails when the committed files differ from a fresh
generation. `--check` also validates buffers, buffer views and accessors, POSITION min/max,
normals, joint indices and weights (normalised), the inverse bind matrices (invertible, the
inverse of each bone's rest world matrix from the node TRS), the hierarchy (the mesh node beside
the root bone), the rest-pose skinning (identity) and the ground contact.

### `hero_animated.gltf` (written by `make_animated_hero.py`)

The hero (same nodes, mesh and skin) with five glTF animations for the animation clips tests' model
sub-clips (`hero_animated.gltf#Walk`): `Walk` (LINEAR), `Idle` (CUBICSPLINE translation and
rotation, LINEAR head scale), an unnamed one (STEP, sub-clip `Animation 2`, also moving the scene
root node `Hero`) and two named `Wave` (sub-clips `Wave` and `Wave 2`). Not in `Main Scene.scene`.

```bash
python3 -I tools/fixtures/pose-animation/make_animated_hero.py            # rewrite it
python3 -I tools/fixtures/pose-animation/make_animated_hero.py --check    # validate it
```

`--check` runs make_humanoids.py's validation on it, validates the animations (names, samplers,
accessor types and counts, increasing times, unit rotations, target nodes) and compares the bytes
with a fresh generation.

`project.yaml` and `Main Scene.scene` are made by the editor through MCP with `make_scene.py`
(a new project, `editor.assets_dir` = `assets`, `set_model` both models, the light turned to the
front, `create_entity` the ground and the camera, `set_main_camera`, `save_scene`), so they are in
the editor's own format. Run it after any model change: the scene stores each model's materials,
submesh sizes, bounds and bone entities.

```bash
JASPER_EDITOR=$PWD/build/linux/editor-install/bin/jasperengine-editor \
    uv run --no-project --with mcp --with pyyaml python tools/fixtures/pose-animation/make_scene.py
```
