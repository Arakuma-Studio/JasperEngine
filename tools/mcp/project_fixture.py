"""Small authored project inputs for CLI checks; no imported game or generated API snapshot."""

from pathlib import Path

import yaml


def write_project(root: Path, *, scenes: bool = False) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    (root / "assets").mkdir(exist_ok=True)
    (root / "scripts").mkdir(exist_ok=True)
    (root / "scripts/Probe.lua").write_text("local Probe = {}\nreturn Probe\n", encoding="utf-8")
    config = {"name": "CLI Probe", "assetsDir": "assets", "luaDir": "scripts", "scenes": []}
    if scenes:
        config.update(canvasWidth=320, canvasHeight=180, scalingMode="letterbox", textureStrategy="none")
        for scene_id, name, scene_type in [(11, "World", "scene_2d"), (12, "Overlay", "scene_ui")]:
            filename = name + ".scene"
            (root / filename).write_text(yaml.safe_dump({
                "id": scene_id, "name": name, "sceneType": scene_type,
                "scene": {"backgroundColor": [0.1, 0.2, 0.3, 1]},
                "entities": [{"type": "Entity", "entity": 1001, "name": name + " Marker",
                              "components": {"transform": {"position": [0, 0, 0], "scale": [1, 1, 1],
                                                            "rotation": [1, 0, 0, 0]}}}],
            }, sort_keys=False), encoding="utf-8")
            config["scenes"].append({"filepath": filename})
        (root / "assets/payload.txt").write_text("project-relative export payload\n", encoding="utf-8")
    (root / "project.yaml").write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")
    return root
