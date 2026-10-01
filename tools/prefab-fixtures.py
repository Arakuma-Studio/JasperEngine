#!/usr/bin/env python3
"""Prefab test projects for the prefab system (editor/project/PREFABS.md).

    tools/prefab-fixtures.py legacy <out-dir>

`legacy` writes a project in the formats written before the real-root model (prefab
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
    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.path.join(here, "..", "build", "a2", "fixture")
    if not os.path.isdir(fixture):
        sys.exit("needs the fixture project at build/a2/fixture (a copy of the TappyPlane fixture)")
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


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("legacy",):
        sys.exit(__doc__)
    globals()[sys.argv[1]](sys.argv[2])
