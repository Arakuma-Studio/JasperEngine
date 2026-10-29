"""S30 shipping group: publish_prepare, publish_verify, publish_steam_vdf (R22-R25, R32).

A brand-new project is built for Linux with a prebuilt player runtime the test installs (its
"player" is /bin/true: see test_build_method.py), publishing.json names that build, and a release
is prepared, verified, tampered with (verify fails), and given its SteamPipe scripts. Uploads and
Deck deploys are a person's steps (no upload tool exists; the Deck deploy is unit-tested with ssh
stand-ins: editor/tests/publish/PublishingTest.cpp).
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from harness import Editor  # noqa: E402
from test_build_method import RUNTIME, runtime_archive  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Publish Test"}


async def test_shipping_group(ed: Editor):
    groups = await ed.call("tool_groups")
    names = json.dumps(groups)
    ed.check_in("shipping", names)
    ed.check(not ed.has_tool("publish_steam_upload"), "no upload tool: a person uploads")


async def test_release_flow(ed: Editor):
    await ed.call("create_entity", name="Box", type="box")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
    info = await ed.call("get_build_info")
    archive = runtime_archive(ed.scratch.root / f"jasperengine-player-{RUNTIME}.zip", info["editor_version"])
    r = await ed.call("install_player_runtime", archive=str(archive), timeout_seconds=120)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    out = ed.scratch.root / "game-linux"
    r = await ed.call("build_project", mode="player", platform="linux", output_dir=str(out), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    executable = Path(r["result"]["executable"]).name

    msg = await ed.call("publish_prepare", release="1.0.0", timeout_seconds=120, expect_error=True)
    ed.check_in("publishing.json", msg)

    releases = ed.scratch.root / "releases"
    (ed.project / "publishing.json").write_text(json.dumps({
        "schemaVersion": 1,
        "releasesDir": str(releases),
        "targets": {"linux": {"platform": "linux", "arch": "x64", "backend": "glcore", "flavour": "release",
                              "buildDir": str(out), "executable": executable, "depotId": "1001",
                              "deviceProfile": "steamdeck", "requiredFiles": ["game.json"]}},
        "steam": {"appId": "480", "buildDescription": "test"},
        "deck": {"host": "steamdeck.local", "user": "deck", "remoteDir": "Games/test"},
    }, indent=2))

    await ed.check_error("publish_prepare", release="../escape", timeout_seconds=60, contains="release name")
    r = await ed.call("publish_prepare", release="1.0.0", timeout_seconds=300)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    release = Path(r["result"]["release_dir"])
    ed.check_eq(release, releases / "1.0.0")
    manifest = json.loads((release / "release.json").read_text())
    ed.check_eq(manifest["release"], "1.0.0")
    target = manifest["targets"]["linux"]
    ed.check_eq(target["playerId"], RUNTIME)
    ed.check_eq(target["method"], "prebuilt")
    ed.check(target.get("glibcFloor"), f"the player's glibc floor: {target}")
    ed.check_in("game.json", target["files"])
    await ed.check_error("publish_prepare", release="1.0.0", timeout_seconds=60, contains="exists already")

    r = await ed.call("publish_verify", release="1.0.0", timeout_seconds=300)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    ed.check_eq(r["result"]["targets"]["linux"]["player_id"], RUNTIME)

    r = await ed.call("publish_steam_vdf", release="1.0.0", preview=True)
    app = Path(r["app_build"])
    ed.check_eq(app.name, "app_build_480.vdf")
    ed.check_in('"Preview" "1"', app.read_text())
    ed.check_eq([Path(p).name for p in r["depot_builds"]], ["depot_build_1001.vdf"])
    await ed.check_error("publish_steam_vdf", release="1.0.0", preview=False, branch="public", contains="Steamworks")

    # a change after prepare is caught, by name
    (release / "content" / "linux" / "game.json").write_text("{}")
    msg = await ed.call("publish_verify", release="1.0.0", timeout_seconds=300, expect_error=True)
    ed.check_in("Release content changed after preparation: linux/game.json", msg)
    await ed.check_error("publish_steam_vdf", release="1.0.0", preview=True, contains="changed")
