#!/usr/bin/env python3
"""Prefab test projects for the prefab system (editor/project/PREFABS.md).

    tools/prefab-fixtures.py legacy <out-dir>
    tools/prefab-fixtures.py nested <out-dir>     (see nested())
    tools/prefab-fixtures.py variant <out-dir>    (see variant())

`legacy` writes a project (FIXTURE: the TappyPlane fixture project, for its sprites) in the formats written before the real-root model (prefab
files version 1 and 2, wrapper-root instances with whole-component overrides): a
multi-root prefab, prefabs nested three levels deep, instance overrides, added and removed
GameObjects at every depth, instance roots with their own scale, tag and layer, a scene
reference into a prefab instance and one to a scene GameObject whose id a prefab member
would take. Load it with an editor before and after a change and compare the
`scene-dump` outputs (tools/prefab-dump-compare.py).
"""
import copy
import os
import sys

import yaml

IDENTITY = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def transform(x=0.0, y=0.0, sx=1.0, sy=1.0):
    return {
        "position": [x, y, 0],
        "rotation": [1, 0, 0, 0],
        "scale": [sx, sy, 1],
        "visible": True,
        "billboard": False,
        "fakeBillboard": False,
        "cylindricalBillboard": False,
        "billboardRotation": [1, 0, 0, 0],
    }


def sprite_components(texture, width, height, x=0.0, y=0.0, color=(1, 1, 1, 1), sx=1.0, sy=1.0):
    return {
        "transform": transform(x, y, sx, sy),
        "mesh": {
            "castShadows": False,
            "receiveLights": False,
            "receiveShadows": False,
            "receiveIBL": False,
            "transparent": True,
            "submeshes": [{
                "faceCulling": False,
                "hasTextureRect": True,
                "textureRect": [0, 0, 1, 1],
                "primitiveType": "triangles",
                "vertexCount": 6,
                "material": {
                    "baseColorFactor": list(color),
                    "alphaMode": "AUTO",
                    "baseColorTexture": {
                        "textureType": "2D", "source": "path", "path": texture,
                        "minFilter": "linear", "magFilter": "linear",
                        "wrapU": "clamp_to_edge", "wrapV": "clamp_to_edge",
                        "releaseDataAfterLoad": True,
                    },
                },
            }],
        },
        "sprite": {
            "width": width, "height": height, "pivotPreset": "bottom_center",
            "automaticFlipY": True, "flipY": True, "textureScaleFactor": 0,
        },
    }


def entity(eid, name, components, children=None, **state):
    node = {"type": "Entity", "entity": eid, "name": name, "components": components}
    node.update(state)
    if children:
        node["children"] = children
    return node


def prefab_ref(name, path):
    return {"name": name, "path": path}


def write_yaml(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        yaml.safe_dump(data, f, sort_keys=False)


def legacy(out):
    # the sprites and scene settings come from the TappyPlane fixture project
    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.environ.get("FIXTURE", os.path.join(here, "..", "build", "fixture-project"))
    if not os.path.isdir(fixture):
        sys.exit("needs the TappyPlane fixture project (build/fixture-project, or FIXTURE=<dir>)")
    os.makedirs(out, exist_ok=True)
    os.system('cp -r "%s/assets" "%s/"' % (fixture, out))
    project = yaml.safe_load(open(os.path.join(fixture, "project.yaml")))
    project["name"] = "PrefabLegacy"
    project["scenes"] = [{"filepath": "Prefab Scene.scene"}]
    project["startSceneId"] = 1
    project.pop("standalonePrefabs", None)
    write_yaml(os.path.join(out, "project.yaml"), project)

    # Rock: version 1 (no header), two top-level GameObjects
    rock = {"type": "Prefab", "members": [
        entity(1001, "Rock", sprite_components("sprites/rock.png", 140, 303), [
            entity(1002, "Pebble", sprite_components("sprites/rock.png", 40, 90, x=90)),
        ]),
        entity(1003, "Shadow", sprite_components("sprites/rock-down.png", 140, 120, y=-40, color=(0.2, 0.2, 0.2, 1))),
    ]}
    write_yaml(os.path.join(out, "prefabs", "Rock.prefab"), rock)

    # Gem: version 1, one top-level GameObject with an id a scene GameObject has too
    gem = entity(1050, "Gem", sprite_components("sprites/plane-sheet.png", 88, 73))
    write_yaml(os.path.join(out, "prefabs", "Gem.prefab"), gem)

    # Tower: version 2 header, nests Rock
    tower = {"type": "Prefab", "version": 2, "guid": "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1", "nextEntity": 1004, "members": [
        entity(1001, "Base", sprite_components("sprites/ground.png", 300, 70), [
            entity(1002, "Top", sprite_components("sprites/rock-down.png", 100, 120, y=180, color=(0.6, 0.6, 1, 1))),
            entity(1003, "Rock", {"prefab": prefab_ref("Rock", "prefabs/Rock.prefab"),
                                   "transform": transform(-110, 70, 0.5, 0.5)}),
        ]),
    ]}
    write_yaml(os.path.join(out, "prefabs", "Tower.prefab"), tower)

    # Castle: version 2, nests Tower (so Rock is three levels down)
    castle = {"type": "Prefab", "version": 2, "guid": "b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2", "nextEntity": 1003, "members": [
        entity(1001, "Keep", sprite_components("sprites/ground.png", 360, 50, color=(1, 0.8, 0.6, 1)), [
            entity(1002, "Tower", {"prefab": prefab_ref("Tower", "prefabs/Tower.prefab"),
                                    "transform": transform(60, 50, 0.8, 0.8)}),
        ]),
    ]}
    write_yaml(os.path.join(out, "prefabs", "Castle.prefab"), castle)

    red = sprite_components("sprites/rock.png", 40, 90, x=90, color=(1, 0.2, 0.2, 1))
    green = sprite_components("sprites/rock.png", 40, 90, x=90, color=(0.2, 1, 0.2, 1))
    flag = entity(1101, "Flag", sprite_components("sprites/plane-sheet.png", 88, 73, x=-60, y=320, sx=0.6, sy=0.6))
    nested_flag = entity(1102, "Leaf", sprite_components("sprites/plane-sheet.png", 88, 73, y=120, sx=0.5, sy=0.5))

    entities = [
        # A Rock with a whole-component override on a member, an added GameObject and a
        # removed one
        entity(1001, "Rock A", {"prefab": prefab_ref("Rock", "prefabs/Rock.prefab"), "transform": transform(150, 80)}),
        # A Rock whose root has its own scale, tag and layer
        entity(1010, "Rock B", {"prefab": prefab_ref("Rock", "prefabs/Rock.prefab"), "transform": transform(380, 80, 0.7, 1.3)},
               tag="Enemy", layer=3),
        # A Tower with overrides, removals and an added GameObject in its nested Rock
        entity(1020, "Tower A", {"prefab": prefab_ref("Tower", "prefabs/Tower.prefab"), "transform": transform(640, 60)}),
        # A Castle with a removal three levels down
        entity(1030, "Castle A", {"prefab": prefab_ref("Castle", "prefabs/Castle.prefab"), "transform": transform(1000, 40)}),
        # A Gem, whose member would take id 1050 before the scene's own 1050 is decoded
        entity(1040, "Gem A", {"prefab": prefab_ref("Gem", "prefabs/Gem.prefab"), "transform": transform(1180, 600)}),
        entity(1050, "Target", sprite_components("sprites/rock-down.png", 60, 60, x=1180, y=500)),
        # References: to the scene's 1050, and to Rock A's Pebble (a member, by its id then)
        entity(1060, "Pointer", {"action": {"ownedTarget": False, "speed": 1, "state": "Stopped", "target": 1050}}),
        entity(1061, "Pebble Pointer", {"action": {"ownedTarget": False, "speed": 1, "state": "Stopped", "target": 1002}}),
    ]
    entities[0]["prefabOverrides"] = [{"registryEntity": 1002, "components": {"mesh": red["mesh"], "transform": red["transform"]}}]
    entities[0]["prefabLocalEntities"] = [dict(flag, parentRegistryEntity=1001, childIndex=1)]
    entities[0]["prefabRemovedGameObjects"] = [{"registryEntity": 1003}]
    entities[2]["prefabOverrides"] = [
        {"registryEntity": 1002, "components": {"transform": transform(0, 230)}},
        {"registryEntity": 1002, "prefabPath": "prefabs/Rock.prefab", "prefabRootRegistryEntity": 1003,
         "components": {"mesh": green["mesh"]}},
    ]
    entities[2]["prefabRemovedGameObjects"] = [
        {"registryEntity": 1003, "prefabPath": "prefabs/Rock.prefab", "prefabRootRegistryEntity": 1003},
    ]
    entities[2]["prefabLocalEntities"] = [
        dict(nested_flag, parentRegistryEntity=1001, childIndex=1, prefabPath="prefabs/Rock.prefab", prefabRootRegistryEntity=1003),
    ]
    entities[3]["prefabRemovedGameObjects"] = [
        {"prefabPath": "prefabs/Tower.prefab", "prefabRootRegistryEntity": 1002,
         "nested": {"registryEntity": 1003, "prefabPath": "prefabs/Rock.prefab", "prefabRootRegistryEntity": 1003}},
    ]

    scene = yaml.safe_load(open(os.path.join(fixture, "Main Scene.scene")))
    scene["name"] = "Prefab Scene"
    scene["id"] = 1
    scene.pop("childScenes", None)
    scene["prefabs"] = ["prefabs/Rock.prefab", "prefabs/Tower.prefab", "prefabs/Castle.prefab", "prefabs/Gem.prefab"]
    scene["entities"] = entities
    write_yaml(os.path.join(out, "Prefab Scene.scene"), scene)


WATCHER_LUA = """local Watcher = {
    properties = {
        { name = "target", displayName = "Target", type = "Sprite" }
    }
}

-- Logs the GameObject its property points at, for the export checks
function Watcher:init()
    local name = "none"
    local id = 0
    if self.target ~= nil then
        id = self.target.entity
        name = self.scene:getEntityName(id)
    end
    Log.print("WATCH " .. self.scene:getEntityName(self.entity) .. " " .. tostring(self.entity) .. " -> " .. name .. " " .. tostring(id))
end

return Watcher
"""


def camera_components(x=0.0, y=0.0):
    return {
        "transform": transform(x, y),
        "camera": {"type": "camera_2d", "renderToTexture": True, "framebufferWidth": 256, "framebufferHeight": 256},
    }


def action_components(target=0):
    return {"action": {"ownedTarget": False, "speed": 1, "state": "Stopped", "target": target}}


def nested(out):
    """A scene of plain GameObjects four levels deep (Forest > Tree > Branch > Leaf) and the
    scene-dump ops that make prefabs of them bottom-up through the editor: each level is made
    from a selection holding an instance of the one below with overrides of every kind
    (properties, state, added and removed components, added and removed GameObjects) and
    references into it (a camera texture link, action targets, joint bodies), so every level
    of nesting holds asset-level data, stripped objects and added GameObjects. Then more
    instances with scene overrides at every depth, references from the scene into deep
    members and between instances, Create Prefab from a multi-selection of instances, and an
    instance put into a prefab (Add to Prefab). Writes <out>/ (the project) and
    <out>.ops/build.yaml."""
    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.environ.get("FIXTURE", os.path.join(here, "..", "build", "fixture-project"))
    if not os.path.isdir(fixture):
        sys.exit("needs the TappyPlane fixture project (build/fixture-project, or FIXTURE=<dir>)")
    os.makedirs(out, exist_ok=True)
    os.system('cp -r "%s/assets" "%s/"' % (fixture, out))
    os.makedirs(os.path.join(out, "prefabs"), exist_ok=True)
    project = yaml.safe_load(open(os.path.join(fixture, "project.yaml")))
    project["name"] = "PrefabNested"
    project["scenes"] = [{"filepath": "Nested Scene.scene"}]
    project["startSceneId"] = 1
    project.pop("standalonePrefabs", None)
    write_yaml(os.path.join(out, "project.yaml"), project)

    def sprite(texture, w, h, x=0.0, y=0.0, color=(1, 1, 1, 1), sx=1.0, sy=1.0):
        return sprite_components(texture, w, h, x=x, y=y, color=color, sx=sx, sy=sy)

    # A script with an entity property: it logs what it points at when the game starts
    os.makedirs(os.path.join(out, "scripts"), exist_ok=True)
    with open(os.path.join(out, "scripts", "Watcher.lua"), "w") as f:
        f.write(WATCHER_LUA)

    def watcher(x=0.0, y=0.0):
        return {
            "transform": transform(x, y),
            "script": {"scripts": [{
                "type": "lua", "path": "Watcher.lua", "className": "Watcher", "enabled": True,
                "properties": [{"name": "target", "displayName": "Target", "type": "EntityReference",
                                "ptrTypeName": "Sprite", "value": 0, "defaultValue": 0}],
            }]},
        }

    leaf = entity(1009, "Leaf", sprite("sprites/plane-sheet.png", 88, 73, x=0, y=180, color=(0.4, 1, 0.4, 1)), [
        entity(1010, "Petal", sprite("sprites/rock.png", 30, 60, x=60, y=0, color=(1, 0.6, 0.8, 1))),
        entity(1011, "Bud", dict(sprite("sprites/rock.png", 20, 40, x=-60, y=0, color=(1, 1, 0.4, 1)), **action_components())),
        entity(1013, "Thorn", sprite("sprites/rock-down.png", 20, 30, x=0, y=-40)),
        entity(1012, "LeafCam", camera_components(0, 0)),
    ])
    branch = entity(1006, "Branch", sprite("sprites/rock-down.png", 60, 140, x=120, y=150, color=(0.7, 0.5, 0.3, 1)), [
        entity(1007, "Twig", sprite("sprites/rock.png", 30, 80, x=60, y=40)),
        entity(1008, "Screen", sprite("sprites/ground.png", 120, 40, x=-80, y=40)),
        entity(1014, "Eye", watcher(-30, 20)),
        leaf,
    ])
    tree = entity(1003, "Tree", sprite("sprites/rock.png", 120, 260, x=0, y=60), [
        entity(1004, "Trunk", sprite("sprites/ground.png", 80, 40, x=0, y=-20, color=(0.5, 0.35, 0.2, 1))),
        entity(1005, "Rope", {"transform": transform(-60, 100)}),
        entity(1015, "Lens", watcher(40, 20)),
        branch,
    ])
    forest = entity(1001, "Forest", sprite("sprites/ground.png", 400, 60, x=250, y=60), [
        entity(1002, "Wind", dict({"transform": transform(0, 0)}, **action_components())),
        tree,
    ])
    entities = [
        forest,
        entity(1020, "Pointer", action_components()),
        entity(1021, "Cross", action_components()),
        entity(1022, "Field", sprite("sprites/ground.png", 300, 30, x=900, y=30)),
        # scene scripts pointing into instances (deep members, GameObjects prefabs add)
        entity(1023, "Spy", watcher(0, 0)),
        entity(1024, "Spy2", watcher(0, 0)),
    ]

    scene = yaml.safe_load(open(os.path.join(fixture, "Main Scene.scene")))
    scene["name"] = "Nested Scene"
    scene["id"] = 1
    scene.pop("childScenes", None)
    scene.pop("cppScripts", None)
    scene["prefabs"] = []
    scene["gravity2D"] = [0, 0]
    scene["entities"] = entities
    write_yaml(os.path.join(out, "Nested Scene.scene"), scene)

    F, T, B, L = "Forest", "Forest/Tree", "Forest/Tree/Branch", "Forest/Tree/Branch/Leaf"
    color = "submeshes[0].material.baseColorFactor"
    ops = [
        # joint bodies (the scene has no gravity: they stay where they are)
        {"op": "addComponent", "entity": L + "/Petal", "component": "rigidbody2d"},
        {"op": "addComponent", "entity": B + "/Twig", "component": "rigidbody2d"},
        {"op": "addComponent", "entity": T + "/Trunk", "component": "rigidbody2d"},
        # ---- Leaf, then an instance of it inside Branch with overrides of every kind ----
        {"op": "createPrefab", "entity": L, "path": "prefabs/Leaf.prefab"},
        {"op": "dump", "out": "@/0-leaf.yaml"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Leaf.prefab", "below": True},
        {"op": "dump", "out": "@/0-fresh.yaml"},
        {"op": "delete", "entity": "Field/Leaf 2"},
        {"op": "set", "entity": L + "/Petal", "component": "mesh", "property": color, "value": [1, 0, 0, 1], "shared": False},
        {"op": "recordProperty", "entity": L + "/Petal", "component": "mesh", "properties": [color]},
        {"op": "setStateLocal", "entity": L + "/Bud", "layer": 2},
        {"op": "recordState", "entity": L + "/Bud", "state": 8},
        {"op": "removeLocalComponent", "entity": L + "/Bud", "component": "action"},
        {"op": "recordRemovedComponent", "entity": L + "/Bud", "component": "action"},
        {"op": "addLocalComponent", "entity": L + "/Petal", "component": "action"},
        {"op": "recordAddedComponent", "entity": L + "/Petal", "component": "action"},
        {"op": "setRef", "entity": L + "/Petal", "component": "action", "property": "target", "target": L + "/LeafCam", "shared": False},
        {"op": "delete", "entity": L + "/Thorn"},
        {"op": "addObject", "entity": L + "/Petal", "name": "Dew"},
        # references from Branch into the Leaf instance
        {"op": "setRef", "entity": B + "/Screen", "component": "mesh", "property": "submeshes[0].material.baseColorTexture", "target": L + "/LeafCam"},
        {"op": "setRef", "entity": B + "/Eye", "component": "script", "property": "scripts[0].target", "target": L + "/Petal"},
        {"op": "addComponent", "entity": B + "/Twig", "component": "action"},
        {"op": "setRef", "entity": B + "/Twig", "component": "action", "property": "target", "target": L + "/Petal"},
        {"op": "dump", "out": "@/1-before-branch.yaml"},
        {"op": "createPrefab", "entity": B, "path": "prefabs/Branch.prefab"},
        {"op": "dump", "out": "@/2-branch.yaml"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Branch.prefab", "below": True},
        {"op": "dump", "out": "@/2-fresh.yaml"},
        {"op": "delete", "entity": "Field/Branch 2"},

        # ---- an instance of Branch inside Tree with overrides at both of its levels ----
        {"op": "set", "entity": B + "/Twig", "component": "mesh", "property": color, "value": [0, 0, 1, 1], "shared": False},
        {"op": "recordProperty", "entity": B + "/Twig", "component": "mesh", "properties": [color]},
        {"op": "set", "entity": L + "/Petal", "component": "transform", "property": "scale", "value": [2, 2, 1], "shared": False},
        {"op": "recordProperty", "entity": L + "/Petal", "component": "transform", "properties": ["scale"]},
        {"op": "delete", "entity": L + "/Bud"},
        {"op": "addObject", "entity": L + "/Petal/Dew", "name": "Moss"},
        {"op": "addObject", "entity": B + "/Twig", "name": "Knot"},
        {"op": "setRef", "entity": T + "/Lens", "component": "script", "property": "scripts[0].target", "target": L + "/LeafCam"},
        {"op": "addComponent", "entity": T + "/Rope", "component": "joint2d"},
        {"op": "setRef", "entity": T + "/Rope", "component": "joint2d", "property": "bodyA", "target": B + "/Twig"},
        {"op": "setRef", "entity": T + "/Rope", "component": "joint2d", "property": "bodyB", "target": L + "/Petal"},
        {"op": "dump", "out": "@/3-before-tree.yaml"},
        {"op": "createPrefab", "entity": T, "path": "prefabs/Tree.prefab"},
        {"op": "dump", "out": "@/4-tree.yaml"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Tree.prefab", "below": True},
        {"op": "dump", "out": "@/4-fresh.yaml"},
        {"op": "delete", "entity": "Field/Tree 2"},

        # ---- an instance of Tree inside Forest: overrides three levels down ----
        {"op": "setStateLocal", "entity": L + "/Petal", "tag": "Enemy"},
        {"op": "recordState", "entity": L + "/Petal", "state": 4},
        {"op": "setStateLocal", "entity": B + "/Screen", "active": False},
        {"op": "recordState", "entity": B + "/Screen", "state": 2},
        {"op": "set", "entity": T + "/Trunk", "component": "transform", "property": "scale", "value": [1.5, 1, 1], "shared": False},
        {"op": "recordProperty", "entity": T + "/Trunk", "component": "transform", "properties": ["scale"]},
        {"op": "delete", "entity": B + "/Twig/Knot"},
        {"op": "addObject", "entity": L, "name": "Nest"},
        {"op": "setRef", "entity": F + "/Wind", "component": "action", "property": "target", "target": L + "/Petal/Dew/Moss"},
        {"op": "dump", "out": "@/5-before-forest.yaml"},
        {"op": "createPrefab", "entity": F, "path": "prefabs/Forest.prefab"},
        {"op": "dump", "out": "@/6-forest.yaml"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Forest.prefab", "below": True},
        {"op": "dump", "out": "@/6-fresh.yaml"},
        {"op": "delete", "entity": "Field/Forest 2"},

        # ---- the scene: references into deep members, overrides at every depth ----
        {"op": "setRef", "entity": "Pointer", "component": "action", "property": "target", "target": L + "/Petal"},
        {"op": "set", "entity": L + "/LeafCam", "component": "transform", "property": "position", "value": [5, 5, 0], "shared": False},
        {"op": "recordProperty", "entity": L + "/LeafCam", "component": "transform", "properties": ["position"]},
        {"op": "addObject", "entity": T + "/Trunk", "name": "Squirrel"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Forest.prefab", "below": True},
        {"op": "set", "entity": "Field/Forest 2/Tree/Branch/Leaf/Petal", "component": "mesh", "property": color, "value": [0, 1, 1, 1], "shared": False},
        {"op": "recordProperty", "entity": "Field/Forest 2/Tree/Branch/Leaf/Petal", "component": "mesh", "properties": [color]},
        {"op": "delete", "entity": "Field/Forest 2/Tree/Branch/Leaf/Nest"},
        {"op": "setRef", "entity": "Cross", "component": "action", "property": "target", "target": "Field/Forest 2/Tree/Branch/Twig"},
        {"op": "addLocalComponent", "entity": F + "/Wind", "component": "joint2d"},
        {"op": "recordAddedComponent", "entity": F + "/Wind", "component": "joint2d"},
        {"op": "setRef", "entity": F + "/Wind", "component": "joint2d", "property": "bodyA", "target": "Field/Forest 2/Tree/Trunk", "shared": False},
        {"op": "setRef", "entity": F + "/Wind", "component": "joint2d", "property": "bodyB", "target": "Field/Forest 2/Tree/Branch/Twig", "shared": False},

        # ---- Create Prefab from a multi-selection of instances (one original each) ----
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Leaf.prefab"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Branch.prefab"},
        {"op": "set", "entity": "Leaf 2", "component": "transform", "property": "position", "value": [700, 300, 0], "shared": False},
        {"op": "set", "entity": "Branch 2", "component": "transform", "property": "position", "value": [900, 300, 0], "shared": False},
        {"op": "set", "entity": "Branch 2/Leaf/Petal", "component": "mesh", "property": color, "value": [1, 1, 1, 0.5], "shared": False},
        {"op": "recordProperty", "entity": "Branch 2/Leaf/Petal", "component": "mesh", "properties": [color]},
        {"op": "createPrefabs", "entity": "Field", "entities": ["Leaf 2", "Branch 2"], "dir": "prefabs/multi"},

        # ---- an instance put into a prefab (Add to Prefab) ----
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Leaf.prefab"},
        {"op": "set", "entity": "Leaf 3", "component": "transform", "property": "position", "value": [-150, 200, 0], "shared": False},
        {"op": "set", "entity": "Leaf 3/Bud", "component": "transform", "property": "scale", "value": [3, 3, 1], "shared": False},
        {"op": "recordProperty", "entity": "Leaf 3/Bud", "component": "transform", "properties": ["scale"]},
        {"op": "addObject", "entity": "Leaf 3/Petal", "name": "Drop"},
        {"op": "addToPrefab", "entity": "Leaf 3", "parent": T},
        # scene scripts into deep GameObjects: added by Tree / Forest, and by Add to Prefab
        {"op": "setName", "entity": "Field/Forest 2/Tree/Branch/Leaf/Petal/Dew/Moss", "name": "Moss2"},
        {"op": "setRef", "entity": "Spy", "component": "script", "property": "scripts[0].target", "target": "Field/Forest 2/Tree/Branch/Leaf/Petal/Dew/Moss2"},
        {"op": "setRef", "entity": "Spy2", "component": "script", "property": "scripts[0].target", "target": "Forest/Tree/Leaf 3/Petal/Drop"},
        {"op": "dump", "out": "@/7-final.yaml"},
        {"op": "bake", "out": "@/bake"},
        {"op": "save"},
    ]
    os.makedirs(out + ".ops", exist_ok=True)
    write_yaml(os.path.join(out + ".ops", "build.yaml"), ops)


def variant(out):
    """Prefab Variants (PREFABS.md section 18): a scene of plain GameObjects (a Ship holding an
    Engine, with a camera, an action and children) and the scene-dump ops that make, through
    the editor, Engine and Ship (Engine nested in it), a Ship Variant from an instance of Ship
    with overrides of every kind (properties, state, added and removed components, added and
    removed GameObjects, an override of the nested Engine, references into it) and a Ship
    Variant 2 (a variant of the variant) from an instance of that one with overrides of its
    own, a fresh instance of each compared with what it was made from, and a variant made
    from the Project window. Writes <out>/ (the project) and <out>.ops/build.yaml."""
    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.environ.get("FIXTURE", os.path.join(here, "..", "build", "fixture-unity"))
    if not os.path.isdir(fixture):
        sys.exit("needs the TappyPlane fixture project (build/fixture-unity, or FIXTURE=<dir>)")
    os.makedirs(out, exist_ok=True)
    os.system('cp -r "%s/assets" "%s/"' % (fixture, out))
    os.makedirs(os.path.join(out, "prefabs"), exist_ok=True)
    project = yaml.safe_load(open(os.path.join(fixture, "project.yaml")))
    project["name"] = "PrefabVariants"
    project["scenes"] = [{"filepath": "Variant Scene.scene"}]
    project["startSceneId"] = 1
    project.pop("standalonePrefabs", None)
    write_yaml(os.path.join(out, "project.yaml"), project)

    os.makedirs(os.path.join(out, "scripts"), exist_ok=True)
    with open(os.path.join(out, "scripts", "Watcher.lua"), "w") as f:
        f.write(WATCHER_LUA)

    def watcher(x=0.0, y=0.0):
        return {
            "transform": transform(x, y),
            "script": {"scripts": [{
                "type": "lua", "path": "Watcher.lua", "className": "Watcher", "enabled": True,
                "properties": [{"name": "target", "displayName": "Target", "type": "EntityReference",
                                "ptrTypeName": "Sprite", "value": 0, "defaultValue": 0}],
            }]},
        }

    ship = entity(1001, "Ship", sprite_components("sprites/plane-sheet.png", 88, 73, x=300, y=300), [
        entity(1002, "Hull", sprite_components("sprites/rock.png", 60, 120, x=0, y=-60, color=(1, 1, 1, 1))),
        entity(1003, "Wing", sprite_components("sprites/ground.png", 160, 30, x=0, y=20), [
            entity(1004, "Tip", sprite_components("sprites/rock.png", 20, 40, x=80, y=0)),
        ]),
        entity(1005, "Gun", dict({"transform": transform(40, 0)}, **action_components())),
        entity(1006, "Cam", camera_components(0, 0)),
        entity(1007, "Engine", sprite_components("sprites/rock-down.png", 40, 60, x=-60, y=0, color=(0.6, 0.6, 1, 1)), [
            entity(1008, "Flame", sprite_components("sprites/rock.png", 20, 30, x=0, y=-40, color=(1, 0.5, 0, 1))),
        ]),
        entity(1009, "Eye", watcher(0, 40)),
    ])
    entities = [
        ship,
        entity(1020, "Field", sprite_components("sprites/ground.png", 300, 30, x=900, y=30)),
        entity(1021, "Pointer", action_components()),
        entity(1022, "Spy", watcher(0, 0)),
    ]
    scene = yaml.safe_load(open(os.path.join(fixture, "Main Scene.scene")))
    scene["name"] = "Variant Scene"
    scene["id"] = 1
    scene.pop("childScenes", None)
    scene.pop("cppScripts", None)
    scene["prefabs"] = []
    scene["gravity2D"] = [0, 0]
    scene["entities"] = entities
    write_yaml(os.path.join(out, "Variant Scene.scene"), scene)

    color = "submeshes[0].material.baseColorFactor"
    S = "Ship"
    ops = [
        # ---- Engine, then Ship holding an instance of it, a reference into it ----
        {"op": "createPrefab", "entity": S + "/Engine", "path": "prefabs/Engine.prefab"},
        {"op": "setRef", "entity": S + "/Gun", "component": "action", "property": "target", "target": S + "/Engine/Flame"},
        {"op": "setRef", "entity": S + "/Eye", "component": "script", "property": "scripts[0].target", "target": S + "/Hull"},
        {"op": "createPrefab", "entity": S, "path": "prefabs/Ship.prefab"},
        {"op": "dump", "out": "@/0-ship.yaml"},
        # ---- the Ship instance: overrides of every kind ----
        {"op": "set", "entity": S + "/Hull", "component": "mesh", "property": color, "value": [1, 0, 0, 1]},
        {"op": "setState", "entity": S + "/Wing", "tag": "Enemy"},
        {"op": "addComponent", "entity": S + "/Hull", "component": "action"},
        {"op": "setRef", "entity": S + "/Hull", "component": "action", "property": "target", "target": S + "/Engine/Flame"},
        {"op": "removeComponent", "entity": S + "/Cam", "component": "camera"},
        {"op": "delete", "entity": S + "/Wing/Tip"},
        {"op": "addObject", "entity": S + "/Wing", "name": "Flag"},
        {"op": "set", "entity": S + "/Engine/Flame", "component": "transform", "property": "scale", "value": [2, 2, 1]},
        {"op": "set", "entity": S, "component": "transform", "property": "scale", "value": [1.5, 1.5, 1]},
        {"op": "dump", "out": "@/1-before-variant.yaml"},
        # ---- Ship Variant from it ----
        {"op": "createVariant", "entity": S, "path": "prefabs/Ship Variant.prefab"},
        {"op": "levels", "entity": S},
        {"op": "dump", "out": "@/2-variant.yaml"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Ship Variant.prefab", "below": True},
        {"op": "dump", "out": "@/2-fresh.yaml"},
        {"op": "delete", "entity": "Field/Ship 2"},
        # ---- the variant's instance: overrides of its own (its GameObjects and the base's) ----
        {"op": "set", "entity": S + "/Hull", "component": "transform", "property": "scale", "value": [3, 1, 1]},
        {"op": "addObject", "entity": S + "/Wing/Flag", "name": "Pole"},
        {"op": "set", "entity": S + "/Wing/Flag", "component": "transform", "property": "position", "value": [5, 5, 0]},
        {"op": "set", "entity": S + "/Engine/Flame", "component": "mesh", "property": color, "value": [0, 1, 0, 1]},
        {"op": "delete", "entity": S + "/Cam"},
        {"op": "setRef", "entity": S + "/Gun", "component": "action", "property": "target", "target": S + "/Wing/Flag"},
        {"op": "dump", "out": "@/3-before-variant2.yaml"},
        # ---- Ship Variant 2, a variant of the variant ----
        {"op": "createVariant", "entity": S, "path": "prefabs/Ship Variant 2.prefab"},
        {"op": "levels", "entity": S},
        {"op": "dump", "out": "@/4-variant2.yaml"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Ship Variant 2.prefab", "below": True},
        {"op": "dump", "out": "@/4-fresh.yaml"},
        {"op": "delete", "entity": "Field/Ship 2"},
        # ---- a variant made from the Project window: an instance of it is one of the base ----
        {"op": "createVariantAsset", "prefab": "prefabs/Ship.prefab", "path": "prefabs/Ship Plain Variant.prefab"},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Ship.prefab", "below": True},
        {"op": "instantiate", "entity": "Field", "prefab": "prefabs/Ship Plain Variant.prefab", "below": True},
        {"op": "dump", "out": "@/5-plain.yaml"},
        {"op": "delete", "entity": "Field/Ship 2"},
        {"op": "delete", "entity": "Field/Ship 3"},
        # ---- scene references into the variant instance's GameObjects at every level ----
        {"op": "setRef", "entity": "Pointer", "component": "action", "property": "target", "target": S + "/Engine/Flame"},
        {"op": "setRef", "entity": "Spy", "component": "script", "property": "scripts[0].target", "target": S + "/Wing/Flag/Pole"},
        {"op": "variantInfo", "prefab": "prefabs/Ship Variant 2.prefab"},
        {"op": "variantInfo", "prefab": "prefabs/Ship.prefab"},
        {"op": "usage"},
        {"op": "dump", "out": "@/6-final.yaml"},
        {"op": "bake", "out": "@/bake"},
        {"op": "save"},
    ]
    os.makedirs(out + ".ops", exist_ok=True)
    write_yaml(os.path.join(out + ".ops", "build.yaml"), ops)


def replace(out):
    """The legacy project plus prefabs to replace its instances with (Replace Prefab, PREFABS.md
    section 12): Boulder (a Rock under another prefab name, its Pebble at another id, no
    Shadow, a Moss), Bastion (a Tower with its ids shifted, a Rock nested as in Tower and a
    Gem in place of Top) and Pebbles (a Rock whose Pebble has no sprite: a component the
    instance overrides is gone)."""
    legacy(out)
    boulder = {"type": "Prefab", "members": [
        entity(2001, "Rock", sprite_components("sprites/rock.png", 160, 320, color=(0.8, 0.8, 0.8, 1)), [
            entity(2002, "Moss", sprite_components("sprites/rock-down.png", 60, 40, x=-50, color=(0.3, 0.8, 0.3, 1))),
            entity(2005, "Pebble", sprite_components("sprites/rock.png", 30, 60, x=70)),
        ]),
    ]}
    write_yaml(os.path.join(out, "prefabs", "Boulder.prefab"), boulder)

    bastion = {"type": "Prefab", "members": [
        entity(3001, "Base", sprite_components("sprites/ground.png", 320, 80), [
            entity(3002, "Gem", {"prefab": prefab_ref("Gem", "prefabs/Gem.prefab"), "transform": transform(0, 200)}),
            entity(3003, "Rock", {"prefab": prefab_ref("Rock", "prefabs/Rock.prefab"),
                                   "transform": transform(-120, 80, 0.5, 0.5)}),
        ]),
    ]}
    write_yaml(os.path.join(out, "prefabs", "Bastion.prefab"), bastion)

    pebble = {"transform": transform(90, 0)}
    pebbles = {"type": "Prefab", "members": [
        entity(4001, "Rock", sprite_components("sprites/rock.png", 140, 303), [
            entity(4002, "Pebble", pebble),
        ]),
    ]}
    write_yaml(os.path.join(out, "prefabs", "Pebbles.prefab"), pebbles)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("legacy", "replace", "nested", "variant"):
        sys.exit(__doc__)
    globals()[sys.argv[1]](sys.argv[2])
