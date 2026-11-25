# b-rig fixture

`assets/models/b_rig_proxy.gltf` (written by `make_b_rig.py`): a small humanoid named like Kevin
Iglesias' rigs, for the Mesh Space Rotation tests (`docs/specs/mesh-space-rotation.md`).

Not an editor project: every model in a project the editor opens is cooked before the scene's models
reload, and the tests that drive `tools/fixtures/pose-animation` with real input wait for that. The
tests copy the model into their scratch copy of that project, at `assets/models/b_rig_proxy.gltf`.

- Nodes: scene root `Rig` with `B-root` and the skinned mesh `RigMesh`; `B-root` holds `B-hips` and
  `B-spineProxy`. Joints (22): `B-root`, `B-hips`, `B-spine`, `B-chest`, `B-neck`, `B-head`, and per
  side `B-shoulder`, `B-upperArm`, `B-forearm`, `B-hand`, `B-thigh`, `B-shin`, `B-foot`, `B-toe`
  (`.L`, `.R`).
- `B-spineProxy` (path `Rig/B-root/B-spineProxy`) is Unity's Spine Proxy bone: a plain node, no
  joint, no weights, at the spine's rest pose relative to `B-root`.
- T-pose, 1.75 m, Blender-style rest rotations (local +Y along the bone), one rigid box per bone.
- Animations, LINEAR, 1 s, keys every 0.25 s: `Run` (`B-hips` turned 90 degrees about +Y, thighs
  swinging 30 degrees, shins bending) and `Attack` (`B-hips` turned -20 degrees about +Y, `B-spine`
  leaning forward to 30 degrees, `B-upperArm.R` swinging forward to 80 degrees). Both key
  `B-spineProxy` with the spine's rotation relative to `B-root`.

```bash
python3 -I tools/fixtures/b-rig/make_b_rig.py            # rewrite it
python3 -I tools/fixtures/b-rig/make_b_rig.py --check    # validate it
```

The generator reuses `make_humanoids.py` and `make_animated_hero.py` from
`tools/fixtures/pose-animation`. `--check` runs make_humanoids.py's validation and
make_animated_hero.py's animation checks on it, checks the proxy node and compares the bytes with a
fresh generation.
