# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S84 AC28 (§6.14.5): goldens of a representative subset of the Title Art library. Two looks
per family (one 2D, one 3D), four schemes on casino/outline-gold, four templates on their `en`
sample, rendered at 0.25 x on a transparent background and compared premultiplied with
Pachislop's limits (max 48/255, mean 0.25/255, at most 0.1 % of pixels over 8/255). Each case
renders twice first (identical bytes). JASPER_TITLE_UPDATE_GOLDEN=1 rewrites them; an update needs
a row in docs/titleart/goldens.md (tools/titleart/check_golden_ledger.py)."""

import os
import time
from pathlib import Path

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Library Goldens"}
TIMEOUT = 2400
GOLDEN = Path(__file__).resolve().parent / "golden" / "title-library"
UPDATE = os.environ.get("JASPER_TITLE_UPDATE_GOLDEN") == "1"

LOOKS = ["casino/outline-gold", "casino/orb-candy-gold-3d", "metal/classic-gold", "metal/chrome-3d", "neon/sign-pink", "neon/tube-3d",
         "elements/ice", "elements/fire-3d", "gems/red-glass", "gems/diamond-3d", "wa/ink-brush", "wa/sakura-pearl-3d",
         "scifi/hud-cyan", "scifi/visor-chrome-3d", "fantasy/engraved-gold", "fantasy/gilded-shield-3d", "horror/blood-drip",
         "horror/bloodstone-3d", "cartoon/bubble-pop", "cartoon/jelly-3d", "retro/chrome-80s", "retro/pixel-block-3d",
         "anime/impact-burst", "anime/candy-pop-3d", "sports/varsity", "sports/trophy-gold-3d", "minimal/duotone", "minimal/matte-3d"]
SCHEMES = ["gold", "rainbow", "zebra", "neon-pink"]
TEMPLATES = ["casino/big-win-3d", "casino/rush-title-fx", "fantasy/kingdom-title", "minimal/clean-title"]
LIMITS = {"max": 48, "mean": 0.25, "fraction_over": 0.001}

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def render(ed: Editor, path: str, width: int) -> bytes:
    args = {"path": path, "max_width": width, "backgrounds": ["transparent"]}
    r = await ed.call_raw("render_title", args)
    job = (r.structured or {}).get("job_id") if not r.is_error else None
    if job:
        deadline = time.monotonic() + 600
        state = await ed.call("get_job", job_id=job)
        while state.get("status") == "running" and time.monotonic() < deadline:
            state = await ed.call("get_job", job_id=job)
        r = await ed.call_raw("render_title", args)
    ed.check(not r.is_error and r.images, f"{path}: {r.text[:300]}")
    return r.images[0][0]


async def case(ed: Editor, name: str, create: dict, ops: list | None, report: list):
    path = f"assets/goldens/{name.replace('/', '_')}.titleart"
    await ed.call("edit_title", path=path, create=create, bake="never")
    if ops:
        await ed.call("edit_title", path=path, ops=ops)
    doc = await ed.call("get_title", path=path)
    width = max(16, int(doc["canvas"]["width"] * 0.25))
    first = await render(ed, path, width)
    second = await render(ed, path, width)
    ed.check_eq(first, second, f"{name}: two renders are identical")
    golden = GOLDEN / (name.replace("/", "_") + ".png")
    if UPDATE or not golden.exists():
        GOLDEN.mkdir(parents=True, exist_ok=True)
        golden.write_bytes(first)
        if not UPDATE:
            ed.check(False, f"{name}: no golden yet; wrote {golden} (add a ledger row)")
        return
    s = pngdiff.stats(pngdiff.decode(golden.read_bytes()), pngdiff.decode(first), premultiplied=True)
    if s is None:
        ed.check(False, f"{name}: the size changed")
        return
    report.append((name, s))
    ok = s["max"] <= LIMITS["max"] and s["mean"] <= LIMITS["mean"] and s["fraction_over"] <= LIMITS["fraction_over"]
    if not ok:
        diff = ed.scratch / f"golden-{name.replace('/', '_')}.png"
        diff.write_bytes(first)
    ed.check(ok, f"{name}: {s} beyond {LIMITS}")


async def test_goldens(ed: Editor):
    await setup(ed)
    report: list = []
    for look in LOOKS:
        detail = await ed.call_raw("list_title_library", {"kind": "looks", "id": look})
        if detail.is_error or not detail.structured.get("available", False):
            ed.check(False, f"{look}: not available: {detail.text[:200]}")
            continue
        await case(ed, look, {"look": look, "text": "HERO"}, None, report)
    for scheme in SCHEMES:
        await case(ed, "scheme/" + scheme, {"look": "casino/outline-gold", "text": "HERO"},
                   [{"op": "apply_color_scheme", "color_scheme": scheme}], report)
    for tmpl in TEMPLATES:
        await case(ed, "template/" + tmpl, {"template": tmpl, "language": "en"}, None, report)
    differing = [r for r in report if r[1]["max"] > 0]
    largest = max((r[1]["max"] for r in report), default=0)
    print(f"[goldens] {len(report)} compared, {len(differing)} differ at all, largest channel difference {largest}")
