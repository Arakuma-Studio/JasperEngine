#!/usr/bin/env python3
"""Compares two `jasperengine-editor scene-dump` outputs GameObject by GameObject.

    tools/prefab-dump-compare.py <a.yaml> <b.yaml> [--ignore-prefab-info] [--ids]

GameObjects are matched by hierarchy path. Compared: name, state, parent, components (as
Stream encodes them, matrices and other derived fields included) and references (by target
path, so entity ids may differ). --ignore-prefab-info leaves out the prefab link and the
override bookkeeping (`prefab`, `overrides`), for a dump of an older editor; entity ids are
compared only with --ids (member ids persisted across a save and load). Exit 1 on any
difference.
"""
import sys

import yaml

# MeshComponent::morphWeights is uninitialized memory when a file does not hold it
IGNORED_FIELDS = {("mesh", "morphWeights")}


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
    return node


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ignore_prefab = "--ignore-prefab-info" in sys.argv
    ids = "--ids" in sys.argv
    if len(args) != 2:
        sys.exit(__doc__)
    a, b = load(args[0]), load(args[1])
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
                            va = {k: v for k, v in va.items() if (comp, k) not in IGNORED_FIELDS}
                            vb = {k: v for k, v in vb.items() if (comp, k) not in IGNORED_FIELDS}
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
