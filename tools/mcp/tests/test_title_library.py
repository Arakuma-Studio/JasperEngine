# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S84 AC14, AC15 (R23-R27, R36): the Title Art library's content and its quality gate.

test_content: 195 looks with §6.6's per-family counts, 33 color schemes, 73 templates, at least two
3D looks per family, ids ending in -3d exactly for 3D looks.

test_gate: every available item on its sample texts (looks: HERO, 大当り, 1234; schemes on the
preview look; templates in en and ja) renders twice at 0.25 x the canvas with identical bytes,
is not flat, and passes every default check_title check (its check_allow excepted) and the style
lint (its lint_allow excepted). Allowlists carry reasons and at most 10 % of a kind carries any.
Unavailable items are counted by reason, never skipped silently. Shard with
JASPER_TITLE_LIBRARY_SHARD=<i>/<n> (the harness's -j runs one shard per editor)."""

import hashlib
import os
import time

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Library Gate"}
TIMEOUT = 3600

FAMILY_LOOKS = {"casino": 44, "metal": 31, "neon": 10, "elements": 10, "gems": 10, "wa": 10, "scifi": 10, "fantasy": 10,
                "horror": 10, "cartoon": 10, "retro": 10, "anime": 10, "sports": 10, "minimal": 10}
FAMILY_TEMPLATES = {"casino": 33, "metal": 4}

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def rows(ed: Editor, kind: str) -> list:
    out, offset = [], 0
    while True:
        page = await ed.call("list_title_library", kind=kind, limit=48, offset=offset)
        out += page["rows"]
        if "next_offset" not in page:
            return out
        offset = page["next_offset"]


async def test_content(ed: Editor):
    await setup(ed)
    looks = [r for r in await rows(ed, "looks") if "/" in r["id"] and not r["id"].startswith(("project/", "user/"))]
    schemes = [r for r in await rows(ed, "color_schemes") if not r["id"].startswith(("project/", "user/"))]
    templates = [r for r in await rows(ed, "templates") if not r["id"].startswith(("project/", "user/"))]
    ed.check_eq(len(looks), 195, "195 looks")
    ed.check_eq(len(schemes), 33, "33 color schemes")
    ed.check_eq(len(templates), 73, "73 templates")
    for family, n in FAMILY_LOOKS.items():
        fam = [r for r in looks if r["family"] == family]
        ed.check_eq(len(fam), n, f"{family}: looks")
        ed.check(sum(1 for r in fam if r.get("dim") == "3d") >= 2, f"{family}: at least two 3D looks")
        ed.check(sum(1 for r in templates if r["family"] == family) >= FAMILY_TEMPLATES.get(family, 3), f"{family}: templates")
    for r in looks:
        ed.check_eq(r["id"].endswith("-3d"), r.get("dim") == "3d", f"{r['id']}: -3d <-> dimension 3d")


def shard(items: list) -> list:
    spec = os.environ.get("JASPER_TITLE_LIBRARY_SHARD", "")
    if "/" not in spec:
        return items
    i, n = (int(x) for x in spec.split("/"))
    return [item for k, item in enumerate(items) if k % n == i]


async def render_once(ed: Editor, path: str) -> bytes:
    args = {"path": path, "max_width": 480, "backgrounds": ["transparent"]}
    r = await ed.call_raw("render_title", args)
    job = (r.structured or {}).get("job_id") if not r.is_error else None
    if job:
        deadline = time.monotonic() + 600
        state = await ed.call("get_job", job_id=job)
        while state.get("status") == "running" and time.monotonic() < deadline:
            state = await ed.call("get_job", job_id=job)
        r = await ed.call_raw("render_title", args)
    ed.check(not r.is_error and r.images, f"{path}: renders: {r.text[:300]}")
    metrics = (r.structured or {}).get("metrics") or (r.structured or {}).get("cells", [{}])[0].get("metrics") or {}
    ed.check(not metrics.get("flat", False), f"{path}: the render is not flat")
    return r.images[0][0] if r.images else b""


async def test_gate(ed: Editor):
    await setup(ed)
    counts = {"looks": [0, 0, 0], "color_schemes": [0, 0, 0], "templates": [0, 0, 0]}   # checked, unavailable, allowlisted
    reasons: dict = {}
    work = []
    for kind in ("looks", "color_schemes", "templates"):
        for r in await rows(ed, kind):
            if r["id"].startswith(("project/", "user/")):
                continue
            if not r["available"]:
                counts[kind][1] += 1
                reasons[r.get("reason", "?")] = reasons.get(r.get("reason", "?"), 0) + 1
                continue
            samples = [("text", None)] if kind == "templates" else [("HERO", None), ("大当り", None), ("1234", None)]
            if kind == "templates":
                samples = [(None, "en"), (None, "ja")]
            for text, language in samples:
                work.append((kind, r["id"], text, language))
    n = 0
    for kind, ref, text, language in shard(work):
        n += 1
        path = f"assets/gate/{n}.titleart"
        create = {"template": ref, "language": language} if kind == "templates" else {"look": ref, "text": text}
        if kind == "color_schemes":
            create = {"look": "casino/outline-gold", "text": text}
        await ed.call("edit_title", path=path, create=create, bake="never")
        if kind == "color_schemes":
            await ed.call("edit_title", path=path, ops=[{"op": "apply_color_scheme", "color_scheme": ref}])
        detail = await ed.call("list_title_library", kind=kind, id=ref)
        allow_checks = {a["check"] for a in detail.get("check_allow", [])}
        allow_rules = {a["rule"] for a in detail.get("lint_allow", [])}
        if allow_checks or allow_rules:
            counts[kind][2] += 1
        a = await render_once(ed, path)
        b = await render_once(ed, path)
        ed.check_eq(hashlib.sha256(a).hexdigest(), hashlib.sha256(b).hexdigest(), f"{ref} ({text or language}): two renders are identical")
        report = await ed.call("check_title", path=path, backgrounds=["black", "white", "checker"])
        for check in report["checks"]:
            cid = check["id"]
            if check["status"] not in ("FAIL",):
                continue
            base = cid.split(".")[0]
            rule = cid.split(".", 1)[1] if cid.startswith("style_lint.") else ""
            if base in allow_checks or cid in allow_checks or rule in allow_rules:
                continue
            ed.check(False, f"{ref} ({text or language}): {cid} {check['status']}: {check.get('message', '')} measured {check.get('measured')}")
        counts[kind][0] += 1
    print(f"[gate] checked/unavailable/allowlisted per kind: {counts}; unavailable by reason: {reasons}")
    for kind, (checked, unavailable, allowed) in counts.items():
        total = checked + unavailable
        ed.check(allowed * 10 <= max(total, 10), f"{kind}: allowlists on at most 10 % of the items ({allowed} of {total})")


RED = __import__("pathlib").Path(__file__).resolve().parent / "fixtures" / "title-library-red"


async def failing(ed: Editor, path: str) -> dict:
    report = await ed.call("check_title", path=path, backgrounds=["black", "white", "checker"])
    return {c["id"]: c for c in report["checks"] if c["status"] in ("FAIL", "WARN")}


async def test_red_controls(ed: Editor):
    """AC15's red controls (§6.14.4): every bad fixture fails exactly its check against its twin,
    and the twin (or the bad one with the check's fix applied) passes it."""
    import json
    await setup(ed)
    index = json.loads((RED / "index.json").read_text())
    for name, entry in index.items():
        check = entry["check"]
        bad_path = f"assets/red/{entry['bad']}"
        (ed.project / "assets" / "red").mkdir(parents=True, exist_ok=True)
        (ed.project / bad_path).write_text((RED / entry["bad"]).read_text())
        bad = await failing(ed, bad_path)
        ed.check(check in bad, f"{name}: the bad fixture fails {check}: {sorted(bad)}")
        if entry.get("good"):
            good_path = f"assets/red/{entry['good']}"
            (ed.project / good_path).write_text((RED / entry["good"]).read_text())
        else:
            fix = (bad.get(check) or {}).get("fix") or {}
            ed.check(fix.get("ops"), f"{name}: {check} offers a fix")
            good_path = f"assets/red/{name}-fixed.titleart"
            (ed.project / good_path).write_text((RED / entry["bad"]).read_text())
            await ed.call("edit_title", path=good_path, ops=fix.get("ops", []))
        good = await failing(ed, good_path)
        ed.check(check not in good, f"{name}: the twin passes {check}: {good.get(check)}")
        extra = set(bad) - set(good) - {check}
        ed.check(not {e for e in extra if e.split(".")[0] in ("style_lint", "color_scheme_match", "metal_range") or e == check.split(".")[0]},
                 f"{name}: the bad fixture fails only {check} beyond its twin: {sorted(extra)}")
