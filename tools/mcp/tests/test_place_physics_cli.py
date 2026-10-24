"""`jasperengine-editor place-physics`: Physics Placement from a shell (editor/physics/PlacementCli.cpp),
on copies of the physics-placement fixture (tools/fixtures/physics-placement, README.md there).

No window: each run loads the project and one scene headlessly and runs the place_with_physics
simulation straight through.

- usage errors exit 2 with the reason on stderr (unknown option, bad values, options of another
  mode, a mode without what it needs, --save with --dry-run, no project.yaml, an unknown scene or
  GameObject, an ambiguous name, what checkPlacementSpec refuses); --help exits 0 and the top-level
  help lists the command;
- a dry run with --format json -q prints one object of the documented shape (spec section 8.1)
  and nothing on stderr, and leaves every file of the project byte-identical; text mode prints a
  line per object; the scene is found by name, file name or path;
- drop on the table and crate: the three boxes land inside the crate (their centres between its
  floor and its rim), resting on it;
- separate pushes the two interpenetrating cubes apart (at most 1 mm into each other);
- the 30 m room is refused as a mover ("too large (> 25 m), treated as environment") and is the
  environment the furniture lands on; alone nothing can move: exit 2 with the result object;
- --max-seconds 0.05 on falling pebbles: exit 3, the result still printed, all_settled false;
- the same arguments twice give the same state_hash (with jitter and a seed); another seed another;
- every mode runs from the command line (move, snap, gather, distribute, the tuning flags);
- --decompose always computes the ring's decomposition first, so it slides down over the peg;
  --decompose never keeps a single hull, which rests on the peg's top;
- --save writes the scene (applied: true) and a re-run starts from the saved poses;
- the CLI's dry-run result equals the place_with_physics preview of the same scene and arguments.
"""

from __future__ import annotations

import json
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import TestFailure

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools" / "fixtures" / "physics-placement"
EDITOR = {"project": str(FIXTURE)}   # only the last test drives an editor (the MCP preview)
TIMEOUT = 900

CRATE_BOXES = ["Props/Box1", "Props/Box2", "Props/Box3"]
CRATE_FLOOR, CRATE_RIM, CRATE_INSIDE = 1.04, 1.42, 0.26   # y of the crate's floor and rim, half its inside

S: dict = {}


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def tmp() -> Path:
    if "tmp" not in S:
        S["tmp"] = Path(tempfile.mkdtemp(prefix="jasper-place-physics-"))
    return S["tmp"]


def project(name: str = "shared") -> Path:
    """A copy of the fixture: "shared" for the runs that change nothing, a fresh one otherwise"""
    path = tmp() / name
    if name == "shared" and path.exists():
        return path
    if path.exists():
        shutil.rmtree(path)
    shutil.copytree(FIXTURE, path, ignore=shutil.ignore_patterns(".jasperengine"))
    return path


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(harness.editor_command() + ["place-physics", *args], cwd=str(cwd or tmp()),
                          capture_output=True, text=True, timeout=600)


def place(proj: Path, scene: str, entities: list[str], *args: str, want: tuple[int, ...] = (0,), quiet_stderr: bool = True) -> dict:
    """A --format json -q run; its result object (quiet_stderr: nothing on stderr, the run succeeded)"""
    argv = ["--project", str(proj), "--scene", scene]
    for e in entities:
        argv += ["--entity", e]
    r = run(*argv, *args, "--format", "json", "-q")
    check(r.returncode in want, f"{scene} {entities} {args}: exit {r.returncode}, want {want}\n"
                                f"stdout: {r.stdout[-3000:]}\nstderr: {r.stderr[-3000:]}")
    if quiet_stderr:
        check(r.stderr == "", f"-q: stderr is not empty: {r.stderr[-2000:]}")
    else:
        check(r.stderr.strip() != "", "a failed run said nothing on stderr")
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError as e:
        fail(f"stdout is not one JSON object ({e}):\n{r.stdout[-3000:]}")
    return {}


def by_path(result: dict) -> dict:
    return {e["path"]: e for e in result["entities"]}


def after(entity: dict) -> list[float]:
    return entity["after"]["position"]


def tree_bytes(root: Path) -> dict:
    return {str(p.relative_to(root)): p.read_bytes() for p in sorted(root.rglob("*"))
            if p.is_file() and ".jasperengine" not in p.relative_to(root).parts}


def check_vec(value, n: int, what: str) -> None:
    check(isinstance(value, list) and len(value) == n and all(isinstance(v, (int, float)) for v in value),
          f"{what}: not {n} numbers: {value}")


# ---------------------------------------------------------------------------- tests

async def test_usage_errors():
    proj = project()
    p = ["--project", str(proj), "--scene", "TableCrate", "--entity", "Props/Box1"]
    cases = [
        ([], "--project is required"),
        (["--project", str(proj)], "--scene is required"),
        (["--project", str(proj), "--scene", "TableCrate"], "--entity is required"),
        (p + ["--bogus"], "unknown option --bogus"),
        (p + ["stray"], "unexpected argument stray"),
        (p + ["--mode"], "missing value for --mode"),
        (p + ["--mode", "spin"], "unknown mode 'spin'"),
        (p + ["--mode", "move"], "move mode needs --target, --delta or --rotation-euler"),
        (p + ["--mode", "move", "--target", "1,2"], "invalid --target '1,2'"),
        (p + ["--mode", "move", "--target", "1,2,3", "--delta", "0,1,0"], "give one"),
        (p + ["--mode", "move", "--rotation-euler", "0,x,0"], "invalid --rotation-euler"),
        (p + ["--target", "1,2,3"], "only used in move mode"),
        (p + ["--delta", "1,2,3"], "only used in move mode"),
        (p + ["--mode", "snap"], "snap mode needs --ray"),
        (p + ["--mode", "snap", "--ray", "0,3,0,0,0,0"], "invalid --ray"),
        (p + ["--ray", "0,3,0,0,-1,0"], "--ray is only used in snap mode"),
        (p + ["--mode", "distribute"], "distribute mode needs --axis"),
        (p + ["--mode", "distribute", "--axis", "w"], "invalid --axis 'w'"),
        (p + ["--axis", "x"], "--axis is only used in distribute mode"),
        (p + ["--mode", "gather", "--amount", "2"], "invalid --amount '2'"),
        (p + ["--mode", "gather", "--amount", "-0.5"], "invalid --amount"),
        (p + ["--amount", "0.5"], "--amount is only used in gather mode"),
        (p + ["--mode", "separate", "--no-settle"], "--no-settle is not used in separate mode"),
        (p + ["--gravity", "0,-9.81"], "invalid --gravity"),
        (p + ["--gravity", "0,nan,0"], "invalid --gravity"),
        (p + ["--seed", "-1"], "invalid --seed '-1'"),
        (p + ["--seed", "4294967296"], "invalid --seed"),
        (p + ["--max-seconds", "0"], "invalid --max-seconds '0'"),
        (p + ["--max-seconds", "soon"], "invalid --max-seconds"),
        (p + ["--jitter", "0.1"], "invalid --jitter '0.1'"),
        (p + ["--jitter", "0.1,-5"], "invalid --jitter"),
        (p + ["--feel", "slow"], "unknown feel 'slow'"),
        (p + ["--colliders", "mesh"], "unknown colliders 'mesh'"),
        (p + ["--decompose", "sometimes"], "unknown decompose 'sometimes'"),
        (p + ["--context-margin", "-1"], "invalid --context-margin"),
        (p + ["--format", "yaml"], "--format must be text or json"),
        (p + ["--save", "--dry-run"], "--save and --dry-run exclude each other"),
        (["--project", str(tmp()), "--scene", "TableCrate", "--entity", "Props/Box1"], "no project.yaml"),
    ]
    for args, expected in cases:
        r = run(*args)
        check(r.returncode == 2, f"{args}: exit {r.returncode}, want 2\nstderr: {r.stderr[-1500:]}")
        check(expected in r.stderr, f"{args}: stderr lacks {expected!r}:\n{r.stderr[-1500:]}")
        check(r.stdout == "", f"{args}: a usage error printed on stdout: {r.stdout[-500:]}")

    # the ones the project answers: an unknown scene or GameObject, an ambiguous name (a copy
    # whose Overlap/B is renamed Box1, as Props/Box1)
    loaded = [
        (["--project", str(proj), "--scene", "Kitchen", "--entity", "Props/Box1"], "no scene 'Kitchen' in the project (scenes: TableCrate"),
        (["--project", str(proj), "--scene", "TableCrate", "--entity", "Props/Nothing"], "no GameObject 'Props/Nothing' in scene TableCrate"),
        # what checkPlacementSpec refuses
        (p + ["--max-seconds", "500"], "max_seconds must be above 0 and at most 120"),
        (p + ["--jitter", "0,200"], "jitter.rotation_deg must be between 0 and 180"),
    ]
    twin = project("twin")
    scene = twin / "TableCrate.scene"
    text = scene.read_text()
    check(text.count("name: B\n") == 1, "the fixture's Overlap/B is not named B")
    scene.write_text(text.replace("name: B\n", "name: Box1\n"))
    loaded.append((["--project", str(twin), "--scene", "TableCrate", "--entity", "Box1"],
                   "'Box1' names 2 GameObjects in scene TableCrate: "))
    for args, expected in loaded:
        r = run(*args, "-q", "--format", "json")
        check(r.returncode == 2, f"{args}: exit {r.returncode}, want 2\nstderr: {r.stderr[-1500:]}")
        check(expected in r.stderr, f"{args}: stderr lacks {expected!r}:\n{r.stderr[-1500:]}")
        check(r.stdout == "", f"{args}: printed before any run: {r.stdout[-500:]}")
    r = run("--project", str(twin), "--scene", "TableCrate", "--entity", "Box1", "-q")
    check("Overlap/Box1" in r.stderr and "Props/Box1" in r.stderr and "(give the path)" in r.stderr,
          f"the ambiguous name's candidates: {r.stderr}")
    # the paths still tell them apart
    result = place(twin, "TableCrate", ["Overlap/Box1"], "--mode", "separate")
    check([e["path"] for e in result["entities"]] == ["Overlap/Box1"], f"Overlap/Box1: {result['entities']}")


async def test_help():
    r = run("--help")
    check(r.returncode == 0, f"--help: exit {r.returncode}: {r.stderr}")
    for word in ("place-physics", "--entity", "--mode", "separate", "distribute", "--rotation-euler", "--ray", "--jitter",
                 "--decompose", "--push-neighbours", "--context-margin", "--save", "--dry-run", "--format", "Exit code", "3 it ran"):
        check(word in r.stdout, f"--help lacks {word!r}")
    r = run("--project", "x", "-h")
    check(r.returncode == 0 and "Usage:" in r.stdout, "-h anywhere shows the help")
    main = subprocess.run(harness.editor_command() + ["--help"], capture_output=True, text=True, timeout=60)
    check("place-physics" in main.stdout, f"the main usage lacks place-physics:\n{main.stdout}")


async def test_dry_run_json_and_crate_drop():
    proj = project()
    before = tree_bytes(proj)
    result = place(proj, "TableCrate", CRATE_BOXES, "--dry-run")
    # the shape (spec section 8.1)
    for key in ("applied", "mode", "seed", "state_hash", "sim_seconds", "steps", "all_settled", "entities", "context",
                "refused", "warnings", "hint"):
        check(key in result, f"the result lacks {key!r}: {sorted(result)}")
    check(result["applied"] is False and result["mode"] == "drop" and result["seed"] == 1, f"result: {result}")
    check(isinstance(result["state_hash"], str) and len(result["state_hash"]) >= 8, f"state_hash: {result['state_hash']}")
    check(result["steps"] > 0 and result["sim_seconds"] > 0, f"steps {result['steps']}, sim_seconds {result['sim_seconds']}")
    check(result["all_settled"] is True, f"not all settled: {result['warnings']}")
    check(result["context"]["bodies"] > 0, f"no context: {result['context']}")
    check_vec(result["context"]["region"]["min"], 3, "context.region.min")
    check_vec(result["context"]["region"]["max"], 3, "context.region.max")
    check(isinstance(result["warnings"], list) and isinstance(result["hint"], str), "warnings and hint")
    entities = by_path(result)
    check(sorted(entities) == CRATE_BOXES, f"entities: {sorted(entities)}")
    for path, e in entities.items():
        for key in ("id", "name", "before", "after", "moved_m", "rotated_deg", "settled", "settle_step", "resting_on",
                    "max_penetration_m", "collider", "warnings"):
            check(key in e, f"{path} lacks {key!r}: {sorted(e)}")
        check(e["name"] == path.split("/")[-1], f"{path}: name {e['name']}")
        check_vec(e["before"]["position"], 3, f"{path} before.position")
        check_vec(e["before"]["rotation"], 4, f"{path} before.rotation")
        check_vec(e["after"]["position"], 3, f"{path} after.position")
        check_vec(e["after"]["rotation"], 4, f"{path} after.rotation")
        check_vec(e["after"]["local"]["position"], 3, f"{path} after.local.position")
        check_vec(e["after"]["local"]["rotation"], 4, f"{path} after.local.rotation")
        # inside the crate: between its floor and its rim, within its walls
        x, y, z = after(e)
        check(CRATE_FLOOR < y < CRATE_RIM, f"{path} came to rest at y {y:.3f}, not inside the crate ({CRATE_FLOOR}..{CRATE_RIM})")
        check(abs(x) < CRATE_INSIDE and abs(z) < CRATE_INSIDE, f"{path} came to rest at x {x:.3f}, z {z:.3f}: outside the walls")
        check(e["settled"] is True and e["moved_m"] > 0.3, f"{path}: settled {e['settled']}, moved {e['moved_m']}")
        resting = [r["path"] for r in e["resting_on"]]
        check(resting and all(r.startswith("Props/") for r in resting), f"{path} rests on {resting}, not the crate or a box")
        check(e["max_penetration_m"] < 0.01, f"{path}: penetration {e['max_penetration_m']}")
    check(tree_bytes(proj) == before, "a dry run changed a file of the project")

    # text, the default dry run, the scene by its file name and by a path relative to the
    # working directory
    for scene, cwd in (("TableCrate.scene", None), ("shared/TableCrate.scene", tmp()), ("../TableCrate.scene", proj / "terrain_maps")):
        r = run("--project", str(proj), "--scene", scene, *sum([["--entity", b] for b in CRATE_BOXES], []), cwd=cwd)
        check(r.returncode == 0, f"text run ({scene}): exit {r.returncode}\n{r.stderr[-2000:]}")
        check("place-physics drop on TableCrate: 3 movers" in r.stdout and "all settled" in r.stdout, f"text summary:\n{r.stdout}")
        check(f"state {result['state_hash']}" in r.stdout, f"text: another state than the JSON's:\n{r.stdout}")
        for path in CRATE_BOXES:
            check(f"  {path}: moved " in r.stdout, f"text lacks {path}:\n{r.stdout}")
        check("resting on Props/" in r.stdout and "dry run: TableCrate.scene not changed" in r.stdout, f"text:\n{r.stdout}")
        check("[INFO]" not in r.stdout, f"the engine log went to stdout:\n{r.stdout}")
        check("place-physics: simulating drop (3 GameObjects)" in r.stderr, f"progress on stderr: {r.stderr[-1000:]}")
    check(tree_bytes(proj) == before, "a dry run changed a file of the project")


async def test_separate_resolves_overlap():
    result = place(project(), "TableCrate", ["Overlap/A", "Overlap/B"], "--mode", "separate")
    check(result["mode"] == "separate", f"mode {result['mode']}")
    a, b = by_path(result)["Overlap/A"], by_path(result)["Overlap/B"]
    distance = math.dist(after(a), after(b))
    # unit cubes: centres at least 1 m apart unless they still overlap (separating axis)
    check(distance >= 0.999 - 0.001, f"still overlapping: centres {distance:.4f} m apart")
    check(math.dist(a["before"]["position"], b["before"]["position"]) < 0.81, "the fixture's cubes do not overlap")
    for e in (a, b):
        check(e["max_penetration_m"] <= 0.0011, f"{e['path']}: penetration {e['max_penetration_m']}")
        check(abs(after(e)[1] - 0.5) < 0.02, f"{e['path']} left the floor: y {after(e)[1]:.4f}")


async def test_large_room_is_environment():
    # alone, nothing can move: a usage error, the refusal in the result object
    lone = place(project(), "Room", ["Room"], want=(2,), quiet_stderr=False)
    check(lone["error"].startswith("nothing can move (Room: too large (> 25 m)"), f"error: {lone.get('error')}")
    check(lone["entities"] == [] and [r["path"] for r in lone["refused"]] == ["Room"], f"refused: {lone['refused']}")
    result = place(project(), "Room", ["Room", "Furniture/Box", "Furniture/Ball"])
    refused = {r["path"]: r["reason"] for r in result["refused"]}
    check("Room" in refused, f"the room was not refused: {result['refused']}")
    check("too large (> 25 m)" in refused["Room"] and "environment" in refused["Room"], f"the reason: {refused['Room']}")
    entities = by_path(result)
    check("Room" not in entities and sorted(entities) == ["Furniture/Ball", "Furniture/Box"], f"entities: {sorted(entities)}")
    box, ball = entities["Furniture/Box"], entities["Furniture/Ball"]
    check(abs(after(box)[1] - 0.4) < 0.01, f"the box rests at y {after(box)[1]:.4f}, not on the room's floor (0.4)")
    check(abs(after(ball)[1] - 0.5) < 0.01, f"the ball rests at y {after(ball)[1]:.4f}, not on the room's floor (0.5)")
    for e in (box, ball):
        check([r["path"] for r in e["resting_on"]] == ["Room/Floor"], f"{e['path']} rests on {e['resting_on']}")


async def test_not_settled_exits_3():
    result = place(project(), "Pebbles", [f"Pebbles/Pebble{i:02d}" for i in range(1, 31)], "--max-seconds", "0.05", want=(3,))
    check(result["all_settled"] is False, "all_settled after 0.05 s of falling")
    check(result["sim_seconds"] <= 0.05 + 1e-6, f"sim_seconds {result['sim_seconds']} past --max-seconds")
    check(len(result["entities"]) == 30 and not any(e["settled"] for e in result["entities"]), "a pebble settled in mid-air")
    check(result["warnings"], "no warning about the bodies that never came to rest")


async def test_pebbles_deterministic():
    proj = project()
    pebbles = [f"Pebbles/Pebble{i:02d}" for i in range(1, 31)]
    first = place(proj, "Pebbles", pebbles, "--jitter", "0.2,30", "--seed", "11")
    again = place(proj, "Pebbles", pebbles, "--jitter", "0.2,30", "--seed", "11")
    check(first["state_hash"] == again["state_hash"], f"state_hash {first['state_hash']} then {again['state_hash']}")
    check(first == again, "the same arguments gave another result")
    other = place(proj, "Pebbles", pebbles, "--jitter", "0.2,30", "--seed", "12")
    check(other["state_hash"] != first["state_hash"] and other["seed"] == 12, "another seed gave the same state")
    for e in first["entities"]:
        check(e["settled"], f"{e['path']} never came to rest")
        resting = [r["path"] for r in e["resting_on"]]
        check(resting and all(r == "Ground" or r.startswith("Pebbles/") for r in resting), f"{e['path']} rests on {resting}")


async def test_every_mode_runs():
    proj = project()
    # move: the box goes 2 m along +x across the room's floor
    moved = place(proj, "Room", ["Furniture/Box"], "--mode", "move", "--delta", "2,0,0", "--feel", "precise")
    box = by_path(moved)["Furniture/Box"]
    check(moved["mode"] == "move" and abs(after(box)[0] - 4.0) < 0.3, f"move: x {after(box)[0]:.3f}, want about 4")
    turned = place(proj, "Room", ["Furniture/Box"], "--mode", "move", "--target", "-2,1.5,3", "--rotation-euler", "0,45,0")
    box = by_path(turned)["Furniture/Box"]
    check(abs(after(box)[0] + 2.0) < 0.3 and box["rotated_deg"] > 10, f"move to a target, turned: {box}")
    # snap: the ball lands where a ray from above hits the room's floor
    snapped = place(proj, "Room", ["Furniture/Ball"], "--mode", "snap", "--ray", "5,5,5,0,-10,0", "--colliders", "generated")
    ball = by_path(snapped)["Furniture/Ball"]
    check(snapped["mode"] == "snap" and math.dist(after(ball), [5.0, 0.5, 5.0]) < 0.3, f"snap: at {after(ball)}")
    # gather and distribute: the furniture toward each other, then along z
    gathered = place(proj, "Room", ["Furniture/Box", "Furniture/Ball"], "--mode", "gather", "--amount", "0.5")
    spread = by_path(gathered)
    check(gathered["mode"] == "gather", f"mode {gathered['mode']}")
    check(math.dist(after(spread["Furniture/Box"]), after(spread["Furniture/Ball"]))
          < math.dist(spread["Furniture/Box"]["before"]["position"], spread["Furniture/Ball"]["before"]["position"]),
          "gather did not bring them closer")
    distributed = place(proj, "Stack", [f"Stack/Box{i:02d}" for i in range(1, 4)], "--mode", "distribute", "--axis", "x", want=(0, 3))
    check(distributed["mode"] == "distribute" and len(distributed["entities"]) == 3, f"distribute: {distributed['mode']}")
    # no gravity, no settle: a drop moves nothing
    still = place(proj, "Room", ["Furniture/Ball"], "--gravity", "0,0,0", "--no-settle", "--max-seconds", "0.5", want=(0, 3))
    check(by_path(still)["Furniture/Ball"]["moved_m"] < 1e-3, f"moved without gravity: {by_path(still)['Furniture/Ball']}")
    # the other tuning flags are accepted and reported the same way
    pushed = place(proj, "TableCrate", CRATE_BOXES, "--push-neighbours", "--context-margin", "0.5", "--feel", "fast",
                   "--colliders", "auto", "--decompose", "never")
    check(len([e for e in pushed["entities"] if e["path"] in CRATE_BOXES]) == 3, f"push neighbours: {pushed['entities']}")


async def test_ring_decomposition():
    proj = project("ring")
    hull = by_path(place(proj, "RingPeg", ["Ring"], "--decompose", "never"))["Ring"]
    check(after(hull)[1] > 0.7, f"a single hull came to rest at y {after(hull)[1]:.3f}, not on the peg's top")
    r = run("--project", str(proj), "--scene", "RingPeg", "--entity", "Ring", "--decompose", "always", "--format", "json")
    check(r.returncode == 0, f"--decompose always: exit {r.returncode}\n{r.stderr[-2000:]}")
    check("decomposing" in r.stderr, f"no decomposition progress on stderr: {r.stderr[-1000:]}")
    ring = by_path(json.loads(r.stdout))["Ring"]
    check(ring["collider"].startswith("decomposed("), f"collider {ring['collider']}")
    check(after(ring)[1] < 0.2 and math.hypot(after(ring)[0], after(ring)[2]) < 0.2,
          f"the decomposed ring came to rest at {after(ring)}, not around the peg on the floor")
    # cached now: a plain run uses the decomposition too, with no wait
    again = place(proj, "RingPeg", ["Ring"])
    check(by_path(again)["Ring"]["collider"] == ring["collider"], f"the cache: {by_path(again)['Ring']['collider']}")


async def test_save_writes_the_scene():
    proj = project("save")
    scene = proj / "TableCrate.scene"
    before = scene.read_bytes()
    others = {k: v for k, v in tree_bytes(proj).items() if k != "TableCrate.scene"}
    saved = place(proj, "TableCrate", CRATE_BOXES, "--save")
    check(saved["applied"] is True, "applied is not true after --save")
    check(scene.read_bytes() != before, "--save did not write the scene file")
    check({k: v for k, v in tree_bytes(proj).items() if k != "TableCrate.scene"} == others, "--save changed another file")
    # the saved poses are where the next run starts, and they stay put
    again = place(proj, "TableCrate", CRATE_BOXES)
    for path, e in by_path(saved).items():
        start = by_path(again)[path]["before"]["position"]
        check(math.dist(start, after(e)) < 1e-3, f"{path}: the re-run starts at {start}, not the saved {after(e)}")
        check(by_path(again)[path]["moved_m"] < 0.01, f"{path} moved {by_path(again)[path]['moved_m']} m after the save")
    r = run("--project", str(proj), "--scene", "TableCrate", "--entity", "Props/Box1", "--save")
    check(r.returncode == 0 and "saved TableCrate.scene" in r.stdout, f"text --save: {r.stdout}\n{r.stderr[-1000:]}")


async def test_matches_the_mcp_preview(ed):
    ed.require_tools("place_with_physics")
    cli = place(project(), "TableCrate", CRATE_BOXES, "--jitter", "0.05,15", "--seed", "4")
    mcp = await ed.call("place_with_physics", scene="TableCrate", entities=CRATE_BOXES, jitter={"position": 0.05, "rotation_deg": 15},
                        seed=4)
    mcp = {k: v for k, v in mcp.items() if k != "external_changes"}
    check(mcp["state_hash"] == cli["state_hash"], f"MCP state {mcp['state_hash']}, CLI {cli['state_hash']}")
    check(mcp == cli, f"the MCP preview and the CLI dry run differ:\nMCP {json.dumps(mcp)[:3000]}\nCLI {json.dumps(cli)[:3000]}")
