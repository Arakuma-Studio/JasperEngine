"""Scene histories for the save, order and undo tests: a scene of tools/fixtures/scene-inputs
(Directional Light, Skybox) whose saves are compared byte for byte, and with the live Hierarchy.

Oracles independent of the encoder: the scene file on disk (bytes, PyYAML: every id it writes),
get_scene_tree (every GameObject, its parent and its place), and the scene opened again from the
file.
"""

from __future__ import annotations

import difflib
import json
from pathlib import Path

import yaml

from harness import Editor, ToolError

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures"
SCENE_INPUTS = FIXTURES / "scene-inputs"
# Column (a skinned mesh) with the bones Root and Tip: a model whose load makes three GameObjects
MODEL_SOURCE = FIXTURES / "extra-pass" / "assets" / "models" / "skinned_column.gltf"
MODEL = "assets/models/skinned_column.gltf"
# The GameObjects without a Transform the order tests place among the others
LOOSE = ("Skybox", "Fog", "Loose")


def first_difference(a: bytes, b: bytes, limit: int = 30) -> str:
    return "\n".join(list(difflib.unified_diff(a.decode(errors="replace").splitlines(), b.decode(errors="replace").splitlines(),
                                               "expected", "saved", lineterm="", n=2))[:limit])


def written_ids(data: bytes) -> set:
    """Every GameObject id the file writes: each node's entity (children, added GameObjects and
    nested blocks included) and each [registry entity, local id] pair of a memberIds list"""
    ids: set = set()

    def walk(node) -> None:
        if isinstance(node, dict):
            if isinstance(node.get("entity"), int) and "name" in node:
                ids.add(node["entity"])
            for key, value in node.items():
                if key == "memberIds" and isinstance(value, list):
                    ids.update(pair[1] for pair in value if isinstance(pair, list) and len(pair) == 2)
                else:
                    walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(yaml.safe_load(data))
    return ids


def flatten(nodes: list, parent=None, out: list | None = None) -> list:
    """get_scene_tree json nodes as [(id, name, parent id)], in Hierarchy order"""
    out = [] if out is None else out
    for node in nodes:
        out.append((node["id"], node["name"], parent))
        flatten(node.get("children", []), node["id"], out)
    return out


def top_level(data: bytes) -> list:
    """The scene file's top-level GameObjects, in its order"""
    return [e.get("name") for e in yaml.safe_load(data)["entities"]]


def hierarchy_roots(data: bytes) -> list:
    """The Hierarchy window's root order of a scene file: the top-level GameObjects without a
    Transform first, in the file's order, then the Transform roots in the file's order"""
    entities = yaml.safe_load(data)["entities"]
    loose = [e.get("name") for e in entities if "transform" not in (e.get("components") or {})]
    rooted = [e.get("name") for e in entities if "transform" in (e.get("components") or {})]
    return loose + rooted


def common_order(a: list, b: list) -> tuple:
    """Both lists restricted to the names they share: equal when nothing they share moved"""
    return [n for n in a if n in b], [n for n in b if n in a]


class History:
    """A 3D scene of scene-inputs and the problems its rounds found: every case is run and
    reported before the test fails"""

    def __init__(self, ed: Editor, sid, rel: str, tag: str):
        self.ed, self.sid, self.rel, self.tag = ed, sid, rel, tag
        self.problems: list = []

    @classmethod
    async def create(cls, ed: Editor, name: str, tag: str) -> "History":
        rel = f"{name}.scene"
        sid = (await ed.call("create_scene", name=name, type="3D", path=rel))["scene"]["id"]
        return cls(ed, sid, rel, tag)

    async def entity(self, kind: str, name: str, **args) -> int:
        return (await self.ed.call("create_entity", scene=self.sid, type=kind, name=name, select=False, **args))["entity_id"]

    async def save(self) -> bytes:
        # drawn first, so derived values (a mesh's vertexCount) are in every save alike
        await self.ed.call("wait", frames=3)
        await self.ed.call("save_scene", scene=self.sid)
        return (self.ed.project / self.rel).read_bytes()

    async def below(self, entity, name: str):
        """The id of the GameObject called name in entity's branch (None when there is none)"""
        tree = await self.ed.call("get_scene_tree", scene=self.sid, format="json", root=entity, components=False)
        pending = list(tree["nodes"])
        while pending:
            node = pending.pop()
            if node["name"] == name:
                return node["id"]
            pending.extend(node.get("children", []))
        return None

    async def tree(self) -> list:
        tree = await self.ed.call("get_scene_tree", scene=self.sid, format="json", components=False, limit=100000)
        return flatten(tree["nodes"])

    async def roots(self) -> list:
        tree = await self.ed.call("get_scene_tree", scene=self.sid, format="json", depth=0, components=False)
        return [node["name"] for node in tree["nodes"]]

    def problem(self, message: str, files: dict | None = None) -> None:
        print(f"[{self.tag}] FAILS: {message}")
        self.problems.append(message)
        # kept in the scratch dir (kept when the test fails) for a look
        for name, data in (files or {}).items():
            out = self.ed.scratch.root / "history-files" / f"{self.tag}-{len(self.problems)}-{name}".replace(" ", "-")
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(data)

    async def rounds(self, label: str, steps: list, rounds: int = 2) -> None:
        """Runs the steps (each one undo step), saves; then `rounds` times: undo of all (the file
        before them, byte for byte), redo of all (the file after them)"""
        ed = self.ed
        start = await self.save()
        for step in steps:
            await step()
        after = await self.save()
        for round_ in range(1, rounds + 1):
            undone = await ed.call("undo", scene=self.sid, steps=len(steps))
            if undone.get("steps") != len(steps):
                self.problem(f"{label}, round {round_}: undo did {undone.get('steps')} of {len(steps)} steps: {undone}")
            undone_file = await self.save()
            if undone_file != start:
                self.problem(f"{label}, round {round_}: undo gives back the file:\n{first_difference(start, undone_file)}",
                             {"start.scene": start, "undone.scene": undone_file})
            redone = await ed.call("redo", scene=self.sid, steps=len(steps))
            if redone.get("steps") != len(steps):
                self.problem(f"{label}, round {round_}: redo did {redone.get('steps')} of {len(steps)} steps: {redone}")
            redone_file = await self.save()
            if redone_file != after:
                self.problem(f"{label}, round {round_}: redo saves the steps' bytes:\n{first_difference(after, redone_file)}",
                             {"after.scene": after, "redone.scene": redone_file})

    def done(self) -> None:
        self.ed.check(not self.problems, f"{len(self.problems)} problem(s):\n" + "\n".join(self.problems))


class Checked(History):
    """A History whose saves are checked against the live Hierarchy and a reopen of the file"""

    async def check_save(self, label: str, reopen: bool = True) -> bytes | None:
        """Saves; every GameObject of the Hierarchy is in the file and no other, and (reopen) the
        file opened again gives the same Hierarchy. Returns the file (None when the save was
        refused)"""
        live = await self.tree()
        try:
            data = await self.save()
        except ToolError as e:
            self.problem(f"{label}: the save was refused: {str(e)[:600]}")
            return None
        written = written_ids(data)
        name = label.replace(" ", "-")
        missing = [(i, n) for i, n, _ in live if i not in written]
        if missing:
            self.problem(f"{label}: the saved file leaves out {len(missing)} GameObject(s) of the Hierarchy: {missing[:12]}",
                         {f"{name}.scene": data})
        extra = sorted(written - {i for i, _, _ in live})
        if extra:
            self.problem(f"{label}: the saved file names {len(extra)} GameObject(s) the Hierarchy does not have: {extra[:12]}",
                         {f"{name}.scene": data})
        if reopen:
            await self.ed.call("close_scene", scene=self.sid, unsaved="discard")
            self.sid = (await self.ed.call("open_scene", scene=self.rel))["scene"]["id"]
            again = await self.tree()
            if again != live:
                gone = [e for e in live if e not in again]
                new = [e for e in again if e not in live]
                if gone or new:
                    self.problem(f"{label}: opened again, the Hierarchy differs: lost {gone[:12]}, now {new[:12]}",
                                 {f"{name}.scene": data, f"{name}-trees.json":
                                  json.dumps({"live": live, "opened": again}, indent=0).encode()})
                else:
                    at = next(i for i, (a, b) in enumerate(zip(live, again)) if a != b)
                    self.problem(f"{label}: opened again, the same GameObjects in another order from place {at}: "
                                 f"live {live[max(0, at - 2):at + 6]}, opened {again[max(0, at - 2):at + 6]}",
                                 {f"{name}.scene": data})
        return data


async def make_prefab(h: History, name: str, child_kind: str = "sphere", grandchild: bool = False) -> str:
    """name.prefab: a cube "<name> Source" with a "<name> Child" (and its "<name> Leaf"), made
    from GameObjects that are then deleted"""
    await h.entity("cube", f"{name} Source")
    await h.entity(child_kind, f"{name} Child", parent=f"{name} Source")
    if grandchild:
        await h.entity("cube", f"{name} Leaf", parent=f"{name} Child")
    await h.ed.call("create_prefab", scene=h.sid, entity=f"{name} Source", path=f"{name}.prefab")
    await h.ed.call("delete_entities", scene=h.sid, entity=f"{name} Source")
    return f"{name}.prefab"


async def two_level_prefab(h: History, name: str, root_kind: str, child_kind: str) -> str:
    """A prefab of a <root_kind> with a <child_kind> child, made from a GameObject that is then deleted"""
    root = await h.entity(root_kind, f"{name} Source")
    await h.entity(child_kind, f"{name} Child", parent=root)
    path = f"{name}.prefab"
    await h.ed.call("create_prefab", scene=h.sid, entity=root, path=path)
    await h.ed.call("delete_entities", scene=h.sid, entity=root)
    return path
