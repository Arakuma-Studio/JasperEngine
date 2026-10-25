#!/usr/bin/env python3
"""Compares two `jasperengine-editor scene-dump` outputs GameObject by GameObject.

    tools/prefab-dump-compare.py <a.yaml> <b.yaml> [--ignore-prefab-info] [--ids] [--ignore-derived]

GameObjects are matched by hierarchy path. Compared: name, state, parent, components (as
Stream encodes them, matrices and other derived fields included) and references (by target
path, so entity ids may differ). --ignore-prefab-info leaves out the prefab link and the
override bookkeeping (`prefab`, `overrides`, the PrefabComponent), for a dump of an older
editor or a GameObject made a prefab's root; entity ids are
compared only with --ids (member ids persisted across a save and load).
--subtree=A:B compares the GameObjects below path A (A included) in the first dump with the
ones below B in the second (two instances of one prefab): the roots' name, position,
rotation and derived fields are left out, paths are relative to A and B. Exit 1 on any
difference.
"""
import copy
import sys

import yaml

# --ignore-derived: what the engine computes when it runs (a dump of a project saved by the
# GUI against one that never ran)
DERIVED_FIELDS = {("mesh", k) for k in ("aabb", "verticesAABB", "worldAABB", "transparent")} | \
    {("transform", k) for k in ("localMatrix", "modelMatrix", "normalMatrix", "worldPosition", "worldRotation", "worldScale")}


# Material and texture inputs remain observable even when runtime mesh metadata is ignored.
DERIVED_SUBMESH_FIELDS = {"vertexCount", "indexCount", "textureShadow", "hasTexCoord1", "hasTexCoord2",
                          "hasNormalMap", "hasTangent", "hasVertexColor3", "hasVertexColor4",
                          "hasTextureRect", "hasSkinning", "hasMorphTarget", "hasMorphNormal", "hasMorphTangent"}


def component_data(component, value, ignored):
    if not isinstance(value, dict):
        return value
    value = {k: v for k, v in value.items() if (component, k) not in ignored}
    if component == "mesh" and ignored and isinstance(value.get("submeshes"), list):
        value["submeshes"] = [{k: v for k, v in submesh.items() if k not in DERIVED_SUBMESH_FIELDS}
                              for submesh in value["submeshes"]]
    return value


def load(path):
    with open(path) as f:
        return {scene["name"]: scene.get("entities") or {} for scene in yaml.safe_load(f)["scenes"]}


def strip(node, ignore_prefab, ids):
    node = dict(node)
    if not ids:
        node.pop("id", None)
    if ignore_prefab:
        node.pop("prefab", None)
        node.pop("overrides", None)
        if isinstance(node.get("components"), dict) and "prefab" in node["components"]:
            node["components"] = {k: v for k, v in node["components"].items() if k != "prefab"}
    return node


def drop_reference(components, key):
    """Takes the raw id a `refs` key ("action.target", "mesh.submeshes[0].material.baseColorTexture") names."""
    if key.startswith("script.scripts["):
        # a script property: scripts[i].<name> is the value of the property of that name
        index, _, name = key[len("script.scripts["):].partition("].")
        try:
            for prop in components["script"]["scripts"][int(index)]["properties"]:
                if prop.get("name") == name:
                    prop.pop("value", None)
        except (KeyError, IndexError, TypeError, ValueError):
            pass
        return
    steps = []
    for part in key.split("."):
        name, _, rest = part.partition("[")
        steps.append(name)
        while rest:
            index, _, rest = rest.partition("]")
            steps.append(int(index))
            rest = rest[1:] if rest.startswith("[") else rest
    node = components
    for step in steps[:-1]:
        try:
            node = node[step]
        except (KeyError, IndexError, TypeError):
            return
    last = steps[-1]
    if isinstance(node, dict) and last in node:
        value = node[last]
        if isinstance(value, dict) and "camera" in value:
            value = dict(value)
            value.pop("camera")
            node[last] = value
        else:
            node.pop(last)


def subtree(entities, root):
    """The GameObjects below `root` (included), paths made relative to it ("<root>")."""
    def rel(path):
        if path == root:
            return "<root>"
        if isinstance(path, str) and path.startswith(root + "/"):
            return "<root>" + path[len(root):]
        return path
    result = {}
    for path, node in entities.items():
        if path != root and not path.startswith(root + "/"):
            continue
        node = dict(node)
        if "parent" in node:
            node["parent"] = rel(node["parent"])
        if isinstance(node.get("refs"), dict):
            node["refs"] = {k: rel(v) for k, v in node["refs"].items()}
            # the raw ids of what `refs` compares by path differ between two instances
            comps = node["components"] = copy.deepcopy(node.get("components") or {})
            for key in node["refs"]:
                drop_reference(comps, key)
        if path == root:
            node.pop("name", None)
            node.pop("parent", None)
            comps = dict(node.get("components") or {})
            if isinstance(comps.get("transform"), dict):
                comps["transform"] = {k: v for k, v in comps["transform"].items() if k not in ("position", "rotation")}
            node["components"] = comps
        result[rel(path)] = node
    return result


def main():
    pair = None
    for a in sys.argv[1:]:
        if a.startswith("--subtree="):
            pair = a[len("--subtree="):].split(":", 1)
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ignore_prefab = "--ignore-prefab-info" in sys.argv
    ids = "--ids" in sys.argv
    ignored = DERIVED_FIELDS if "--ignore-derived" in sys.argv else set()
    if len(args) != 2:
        sys.exit(__doc__)
    a, b = load(args[0]), load(args[1])
    if pair:
        for label, scenes, root in ((args[0], a, pair[0]), (args[1], b, pair[1])):
            if not any(root in entities for entities in scenes.values()):
                print("subtree %s is missing from %s" % (root, label))
                sys.exit(1)
        ignored = ignored | DERIVED_FIELDS
        a = {scene: subtree(entities, pair[0]) for scene, entities in a.items()}
        b = {scene: subtree(entities, pair[1]) for scene, entities in b.items()}
    differences = 0
    for scene in sorted(set(a) | set(b)):
        ea, eb = a.get(scene), b.get(scene)
        if ea is None or eb is None:
            print("scene %s only in %s" % (scene, args[0] if eb is None else args[1]))
            differences += 1
            continue
        for path in sorted(set(ea) | set(eb)):
            if path not in ea or path not in eb:
                print("[%s] %s only in %s" % (scene, path, args[0] if path not in eb else args[1]))
                differences += 1
                continue
            na, nb = strip(ea[path], ignore_prefab, ids), strip(eb[path], ignore_prefab, ids)
            for key in sorted(set(na) | set(nb)):
                if key == "components":
                    ca, cb = na.get(key) or {}, nb.get(key) or {}
                    for comp in sorted(set(ca) | set(cb)):
                        va, vb = ca.get(comp), cb.get(comp)
                        if isinstance(va, dict) and isinstance(vb, dict):
                            va = component_data(comp, va, ignored)
                            vb = component_data(comp, vb, ignored)
                        if va != vb:
                            detail = ""
                            if isinstance(va, dict) and isinstance(vb, dict):
                                keys = [k for k in sorted(set(va) | set(vb)) if va.get(k) != vb.get(k)]
                                detail = " fields " + ", ".join("%s: %r -> %r" % (k, va.get(k), vb.get(k)) for k in keys[:6])
                            elif va is None or vb is None:
                                detail = " missing in " + (args[0] if va is None else args[1])
                            print("[%s] %s: component %s differs%s" % (scene, path, comp, detail))
                            differences += 1
                elif na.get(key) != nb.get(key):
                    print("[%s] %s: %s %r -> %r" % (scene, path, key, na.get(key), nb.get(key)))
                    differences += 1
    print("%d difference(s)" % differences)
    sys.exit(1 if differences else 0)


if __name__ == "__main__":
    main()
