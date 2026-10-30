"""S30 shipping group: publish_prepare, publish_verify, publish_steam_vdf (R22-R25, R32).

A brand-new project is built for Linux with a prebuilt player runtime the test installs (its
"player" is /bin/true: see test_build_method.py), publishing.json names that build, and a release
is prepared, verified, tampered with (verify fails), and given its SteamPipe scripts. Uploads are a
person's step (no upload tool exists). publish_deploy_deck deploys a release to a "Deck" that is a
directory of this machine, through ssh and scp stand-ins on the editor's PATH (as
editor/tests/publish/PublishingTest.cpp does): copied, checked, swapped in with one previous build
kept, and its smoke run.
"""

import json
import os
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


async def build_and_describe(ed: Editor, releases: Path, deck: dict) -> Path:
    """A Linux build with the test runtime, and the publishing.json of it; returns the build dir."""
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
    (ed.project / "publishing.json").write_text(json.dumps({
        "schemaVersion": 1,
        "releasesDir": str(releases),
        "targets": {"linux": {"platform": "linux", "arch": "x64", "backend": "glcore", "flavour": "release",
                              "buildDir": str(out), "executable": Path(r["result"]["executable"]).name, "depotId": "1001",
                              "deviceProfile": "steamdeck", "requiredFiles": ["game.json"]}},
        "steam": {"appId": "480", "buildDescription": "test"},
        "deck": deck,
    }, indent=2))
    return out


def deck_stubs(bin_dir: Path) -> None:
    """ssh and scp stand-ins: the Deck is FAKE_DECK_HOME, a directory of this machine."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    (bin_dir / "ssh").write_text(
        "#!/bin/sh\nwhile [ $# -gt 0 ]; do case \"$1\" in -o|-p|-i) shift 2 ;; -*) shift ;; *) break ;; esac; done\n"
        "shift\ncd \"$FAKE_DECK_HOME\" && exec sh -c \"$1\"\n")
    (bin_dir / "scp").write_text(
        "#!/bin/sh\nwhile [ $# -gt 0 ]; do case \"$1\" in -o|-P|-i) shift 2 ;; -*) shift ;; *) break ;; esac; done\n"
        "src=\"$1\"; dst=\"${2#*:}\"\nexec cp -R \"$src\" \"$FAKE_DECK_HOME/$dst\"\n")
    for tool in ("ssh", "scp"):
        (bin_dir / tool).chmod(0o755)


async def test_deploy_deck():
    import tempfile
    root = Path(tempfile.mkdtemp(prefix="jasper-deck-"))
    deck_home = root / "deck-home"
    deck_home.mkdir()
    deck_stubs(root / "bin")
    env = {"PATH": f"{root / 'bin'}{os.pathsep}{os.environ.get('PATH', '')}", "FAKE_DECK_HOME": str(deck_home)}
    async with Editor(create=True, name="Deck Test", env=env) as ed:
        await ed.check_error("publish_deploy_deck", release="1.0.0", contains="publishing.json")
        await build_and_describe(ed, ed.scratch.root / "releases", {"host": "steamdeck.local", "user": "deck", "remoteDir": "Games/test"})
        for release in ("1.0.0", "1.0.1"):
            r = await ed.call("publish_prepare", release=release, timeout_seconds=300)
            ed.check_eq(r["status"], "succeeded", f"{r}")
        r = await ed.call("publish_deploy_deck", release="1.0.0", timeout_seconds=300)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        ed.check_eq(r["result"]["remote_dir"], "Games/test/current")
        current = deck_home / "Games" / "test" / "current"
        ed.check((current / "game.json").is_file(), f"the release is on the Deck: {sorted(p.name for p in current.iterdir())}")
        ed.check_in("1.0.0", (current / ".jasper-release").read_text())
        # the next deploy keeps exactly one previous build, and its smoke run plays it there
        r = await ed.call("publish_deploy_deck", release="1.0.1", smoke=True, timeout_seconds=300)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        ed.check_in("1.0.1", (current / ".jasper-release").read_text())
        ed.check_in("1.0.0", (deck_home / "Games" / "test" / "previous" / ".jasper-release").read_text())
        # a release changed after its preparation is not deployed
        (ed.scratch.root / "releases" / "1.0.0" / "content" / "linux" / "game.json").write_text("{}")
        msg = await ed.call("publish_deploy_deck", release="1.0.0", timeout_seconds=300, expect_error=True)
        ed.check_in("changed", msg)


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
