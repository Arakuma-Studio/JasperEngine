#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Asset database benchmark (docs/specs/S04-asset-identity.md AC13, R21-R24).

Generates a project with --assets assets (textures, models, audio) and --referrers YAML
referrers (scenes and prefabs, each referencing --refs assets), lets the editor give every asset
its identity once, then runs `jasperengine-editor assets bench`, which times on the asset
database and the dependency index alone:

    cold_identity_ms        open() with no cache (the identity pass)          budget  1000
    cold_index_ms           the full dependency index after it                budget  3000 (with the identity pass)
    warm_open_ms            open() with an intact cache (stat-only)           budget   250
    single_update_ms        one referrer saved: its entry and edges updated   budget     5 + its YAML parse
    refresh_update_ms       the same found by a full refresh() (informational)
    poll_walk_ms            the poll's disk walk, a background thread         (informational)
    poll_apply_ms           the main thread's part of a poll, every >= 2 s    budget     2 per frame amortized (/120)
    find_references_worst_ms  transitive Find References, worst of 200        budget    50
    resolve_ns              one GUID resolution                               budget   200

    tools/performance/assetdb_bench.py [--editor <cmd>] [--assets 10000] [--referrers 1000] [--check]

--check exits 1 when a budget is exceeded (the nightly job); the budgets are for the desktop SSD
reference machine.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

BUDGETS = {
    "cold_identity_ms": 1000.0,
    "warm_open_ms": 250.0,
    "find_references_worst_ms": 50.0,
    "resolve_ns": 200.0,
}


def editor_command(given: str | None) -> list[str]:
    if given:
        return shlex.split(given)
    env = os.environ.get("JASPER_EDITOR")
    if env:
        return shlex.split(env)
    return [str(REPO / "build/linux/editor-install/bin/jasperengine-editor")]


def texture_node(path: str) -> str:
    return ("{textureType: 2D, source: path, path: %s, minFilter: linear, magFilter: linear, "
            "wrapU: repeat, wrapV: repeat, releaseDataAfterLoad: true}" % path)


def generate(root: Path, assets: int, referrers: int, refs: int) -> None:
    rng = random.Random(4)
    (root / "assets").mkdir(parents=True)
    paths: list[str] = []
    for i in range(assets):
        folder = f"pack{i // 500:03d}"
        kind = i % 10
        if kind < 8:
            rel = f"{folder}/tex{i:05d}.png"
            data = b"\x89PNG\r\n\x1a\n" + i.to_bytes(4, "little") * 8
        elif kind == 8:
            rel = f"{folder}/model{i:05d}.gltf"
            data = json.dumps({"asset": {"version": "2.0"}, "meshes": [{"name": f"M{i}"}, {"name": "Body"}]}).encode()
        else:
            rel = f"{folder}/sound{i:05d}.wav"
            data = b"RIFF" + i.to_bytes(4, "little") * 4
        out = root / "assets" / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        paths.append(rel)
    textures = [p for p in paths if p.endswith(".png")]
    scenes = []
    for r in range(referrers):
        entities = []
        for e in range(refs):
            tex = rng.choice(textures)
            entities.append(f"""  - type: Entity
    entity: {1001 + e}
    name: Light{e}
    components:
      transform: {{position: [0, 0, 0], rotation: [1, 0, 0, 0], scale: [1, 1, 1]}}
      light:
        type: spot
        spotMask: {texture_node(tex)}
""")
        if r % 2 == 0:
            name = f"scenes/Level{r:04d}.scene"
            scenes.append(name)
            text = f"id: {r + 1}\nname: Level{r:04d}\nsceneType: scene_3d\nentities:\n" + "".join(entities)
        else:
            name = f"prefabs/Prefab{r:04d}.prefab"
            text = "type: Prefab\nversion: 3\nnextEntity: 2000\nroot: 1001\nmembers:\n" + "".join(entities)
        out = root / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
    (root / "project.yaml").write_text("name: AssetDbBench\nassetsDir: assets\nluaDir: assets\nstartSceneId: 1\nscenes:\n" +
                                       "".join(f"  - filepath: {s}\n" for s in scenes[:1]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--editor")
    parser.add_argument("--assets", type=int, default=10000)
    parser.add_argument("--referrers", type=int, default=1000)
    parser.add_argument("--refs", type=int, default=10, help="references per referrer")
    parser.add_argument("--check", action="store_true", help="exit 1 when a budget is exceeded")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix="jasper-assetdb-bench-"))
    try:
        project = work / "project"
        generate(project, args.assets, args.referrers, args.refs)
        command = editor_command(args.editor)
        # the first open gives every asset its identity (metas, guid lines): not timed
        first = subprocess.run(command + ["assets", "bench", "--project", str(project)], capture_output=True, text=True)
        if first.returncode != 0:
            print(first.stdout + first.stderr, file=sys.stderr)
            return 2
        run = subprocess.run(command + ["assets", "bench", "--project", str(project)], capture_output=True, text=True)
        if run.returncode != 0:
            print(run.stdout + run.stderr, file=sys.stderr)
            return 2
        result = json.loads(run.stdout[run.stdout.index("{"):])
        full_index = result["cold_identity_ms"] + result["cold_index_ms"]
        result["cold_full_index_ms"] = full_index
        print(json.dumps(result, indent=2))
        failures = [f"{key} {result[key]:.2f} > {limit}" for key, limit in BUDGETS.items() if result.get(key, 0) > limit]
        if full_index > 3000.0:
            failures.append(f"cold_full_index_ms {full_index:.1f} > 3000")
        # the update budget is 5 ms plus the referrer's YAML parse: the parse of one 10-entity file
        if result.get("single_update_ms", 0) > 5.0 + 10.0:
            failures.append(f"single_update_ms {result['single_update_ms']:.2f} > 5 + parse")
        # a poll every 2 s at 60 fps: its main-thread part spread over 120 frames
        if result.get("poll_apply_ms", 0) / 120.0 > 2.0:
            failures.append(f"poll_apply_ms {result['poll_apply_ms']:.1f} > 2 ms per frame over 120 frames")
        for failure in failures:
            print("over budget: " + failure, file=sys.stderr)
        return 1 if failures and args.check else 0
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
