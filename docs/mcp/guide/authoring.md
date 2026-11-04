# Building scenes and games

Served as `jasper://docs/authoring`. How a game is assembled in JasperEngine, the way Unity
projects are: the editor holds the world, scripts drive it. The agent guide's "Building scenes"
section is the digest of this page.

## Assemble scenes in the editor

A scene opened in the editor shows every GameObject it uses, in place and with its real values:
the level, the player, the camera, the lights, the pickups, the door that opens later. Build them
with the tools (`create_entity`, `instantiate_prefab`, `set_properties`, `edit_colliders`,
`create_ui`), then let scripts drive what is there.

Why: the person watching sees and edits the game in the Scene view and the Inspector; your own
Scene view screenshots show the level before Play; Prefab overrides, undo, Find References and the
build's asset list all work on scene objects and on nothing a script assembles at runtime.

Scripts reach scene objects through **script properties** that reference GameObjects, set with
`set_script_properties`:

```lua
---@class Door : ScriptInstance
local Door = {
    properties = {
        { name = "label", displayName = "Label", type = "Text" },        -- a GameObject with a Text
        { name = "reward", displayName = "Reward", type = "GameObject" },
        { name = "coin", displayName = "Coin", type = "Prefab" },        -- a .prefab asset
    },
}

function Door:open()
    self.reward:setActive(true)                 -- placed inactive in the editor, shown now
    self.label.text = "Open"
end

return Door
```

In C++ the same are `DPROPERTY("Label") jasperengine::Text* label = nullptr;` pointer members.

Something that appears later (a door, a reward, a boss) is placed in the editor and made inactive
(`set_entity_state {active: false}`), then activated from the script (`go:setActive(true)`).
Do not merely hide it (`visible = false`): a hidden GameObject keeps its body and scripts running.

## Spawn only what cannot be placed ahead

Create GameObjects from a script only when their number or timing is not known in advance:
bullets, enemy waves, drops, procedural content. Spawn them from a Prefab, never by assembling
components in code:

```lua
local go = GameObject.instantiate(self.coin, self.scene, Vector3(0, 2, 0), Quaternion.IDENTITY)
GameObject.destroy(go, 5.0)
```

A Prefab referenced by a script property is in the build; one only loaded by path
(`PrefabAsset.load`) must be standalone (`set_standalone_prefab`), or the build lacks it.

## Build once, as a Prefab

When the same object or group appears more than once, in one scene or in several (the player, an
enemy, a pickup, a door, a recurring prop), build it once, make it a Prefab (`create_prefab` from
the finished GameObject) and place the others with `instantiate_prefab`. One edit of the asset
(`prefab_mode`, or Apply of an instance's overrides) then changes every copy in every scene. An
instance that differs keeps its own values as overrides; a family of differing objects is a
variant (`create_prefab_variant`). `duplicate_entities` makes independent copies: its result says
so when the source is not a Prefab instance. Details: `jasper://docs/prefabs`.

## HUDs and menus

A HUD (score, health, timers, on-screen controls) and menus go in a UI that does not move with the
gameplay camera: an HTML UI document (`create_ui_document`, `jasper://docs/html-ui`) or a UI scene
attached to every gameplay scene that shows it (`set_child_scene`). Component UI widgets
(`create_ui`) placed inside a 3D scene sit in its world and move with its camera, and every level
would need its own copy; `create_ui` says so in its result. A pause menu is a child scene with
`start_active: false`, shown and hidden with `SceneManager.addChildScene` /
`SceneManager.removeChildScene`. The HUD's script reads the game's state from what both scenes
share: a Lua module both scripts `require`, or a C++ static.

## Animate natively

Motion that can be authored ahead of time (spinning, bobbing, patrols, opening doors, fades,
flashes, UI transitions, sprite frame cycles) is an animation, not script code:

- an Animation clip (`.anim`) played by an Animation Player, or an entity Animation with tweens:
  `edit_animation` (`jasper://docs/animation`); an Animator for state machines
  (`jasper://docs/animator`); sprite animations for frame cycles;
- the Camera Follow component and camera shakes for cameras (`jasper://docs/camera`);
- a Canvas Group's alpha in a tween for UI fades (`jasper://docs/ui`); CSS transitions and
  `anime` in HTML UI.

A script only starts, stops or restarts them when the game calls for it:

```lua
self.door:getAnimationPlayer():play("Open")
```

It never moves a transform, a colour or an alpha a little every frame, or steps sprite frames on a
timer, to play such motion: that motion does not preview in the editor, cannot be tuned in the
Animation window and runs at the frame rate. Per-frame script movement is for what follows live
input, physics or game state: a player controller, a camera following a target without the
component.

## Clicks and input

A button click is the UI event `onClick`: it fires when the pointer is released over the button
while it is interactable. `onPress` and `onRelease` are the button's pressed state; `onRelease`
fires on every release, wherever the pointer is, so it is not a click.

```lua
RegisterEvent(self, Image(self.scene, self.entity):getUIComponent().onClick, "onClicked")
```

C++: `REGISTER_UI_EVENT(onClick, onClicked)`. Forces and held input that drive bodies go in
`onFixedUpdate` (`jasper://docs/physics`).

## Choosing Lua or C++

Keep the language of the project's existing scripts. A project with none gets Lua unless the user
asks for C++: Lua runs in the prebuilt players and needs no compiler, while C++ makes every export a
compiled build (`jasper://docs/shipping`). Before the first C++ script, read
`get_build_info`'s `cpp_scripts`: `can_build` is the check Play itself runs. Switch to Lua, and
tell the user why, when the tools are missing, when a build fails for reasons outside your code
(CMake does not configure, the compiler or linker does not run, the editor's compatibility check
fails), or when your code still does not compile after two fixes. Details:
`jasper://docs/cpp-scripting`.

## Check what you built

- `get_scene_tree` lists every GameObject the scene uses before you press Play.
- A Scene view screenshot in edit mode shows the level as the person will see it.
- Play, look at the Game view, and read the Console since your last cursor.

## Anti-patterns

| In a script | In the editor |
|---|---|
| `init` builds the level: `GameObject(self.scene)` and components in a loop | the level placed with tools, saved in the scene |
| twenty copies made with `duplicate_entities`, each edited by hand | one Prefab, twenty instances |
| `self.t = self.t + Engine.deltatime` moving a coin up and down | a tween or clip (`edit_animation`), looping |
| score Text created in the 3D level scene | a HUD UI scene (`set_child_scene`) or an HTML UI document |
| `onRelease` handling a button click | `onClick` |
