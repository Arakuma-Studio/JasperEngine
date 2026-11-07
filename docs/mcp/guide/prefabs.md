# Prefabs

Served as `jasper://docs/prefabs`. JasperEngine follows Unity's prefab model; the developer
contracts are in `editor/project/PREFABS.md`.

## When to make a Prefab

Anything placed more than once, in one scene or across scenes (the player, an enemy, a pickup, a
door, a recurring prop), and anything a script spawns: build it once in a scene, `create_prefab`
from it, and place the others with `instantiate_prefab`. Copies from `duplicate_entities` are
independent GameObjects: an edit then has to be repeated on each. More: `jasper://docs/authoring`.

## Assets and instances

- A Prefab is a `.prefab` file (YAML, version 3) with a GUID; references survive renames and
  moves. Its instance root in a scene carries a `PrefabComponent` (`prefab:` in scene files).
- **An edit on a GameObject of an instance is an override of that instance only.** Other
  instances and scenes never see it. The asset changes through **Apply** (per property,
  component or GameObject, or Apply All) or by editing it in **Prefab Mode**. **Revert** drops
  overrides. A value set back equal to the prefab's stays overridden until Revert.
- Overrides cover properties, whole components (added/removed), name/active/tag/layer, and
  GameObjects added to or removed from the instance.
- **Instances cannot be restructured**: reparenting, reordering or moving a member out of an
  instance is refused ("Open the Prefab to restructure it"). Open the prefab (Prefab Mode),
  restructure there, save. Instance roots and GameObjects you added to an instance move freely.
- **Unpack** turns an instance into plain GameObjects (Unpack Completely also unpacks nested
  instances).
- **Nested prefabs**: a prefab may contain instances of other prefabs, with its own overrides
  of them.
- **Variants**: a prefab based on another prefab (its base) plus overrides and GameObjects of
  its own; base edits reach every variant except where overridden. Chains of any length.
- A **Missing Prefab** (deleted or unresolvable asset) shows as a red row; its instance keeps
  its data until fixed or unpacked.
- Edits while playing record overrides like at edit time but are discarded at Stop; Apply is
  refused during play.

## Runtime: Instantiate and Destroy

Lua and C++ alike (Lua uses the dot: these are static functions):

```lua
-- a property { name = "enemy", type = "Prefab" } assigned in the Inspector, or:
local prefab = PrefabAsset.load("enemies/EnemyShip")      -- path (no extension), name or id
local go = GameObject.instantiate(prefab, self.scene, Vector3(0, 5, 0), Quaternion.IDENTITY)
-- also: instantiate(prefab [, scene]), instantiate(prefab, parentGameObject [, worldSpace]),
--       instantiate(someGameObject) to copy a GameObject
GameObject.destroy(go, 2.0)          -- after 2 seconds; destroyImmediate(go) right away
```

- The new root is named `<name>(Clone)`; the scripts of every new active GameObject have already
  run `init` and `onEnable` (Lua and C++); `start` runs at the next frame. Without a scene or
  parent it goes to the active scene.
- A copy keeps components and script property values; references among the copied
  GameObjects point to the copies.
- `destroy` removes the GameObject and its children after the frame's update (or after
  `delay`), calling `onDisable` and `onDestroy` (Lua and C++), then the C++ destructors, first.
- `instantiate` returns nil when the prefab is not in the build.
- Into the persistent scene (survives scene loads, `DontDestroyOnLoad`):
  `GameObject.instantiate(prefab, SceneManager.persistentScene)` once it exists (it is nil
  until the first `DontDestroyOnLoad`), or `DontDestroyOnLoad(go)` on the new root. See
  [lua-api.md](lua-api.md) 2.8.1.

## Which prefabs are in the build

A build (Play or export) contains the prefabs a scene instantiates, the ones a script
property of type Prefab references, the ones listed as **standalone** (`standalonePrefabs` in
project.yaml; Assets > Prefabs), and whatever those nest or reference. A prefab only ever
loaded by path with `PrefabAsset.load` must be marked standalone, otherwise it is "not found".
Prefer a Prefab script property: it is tracked through renames and needs no list.
