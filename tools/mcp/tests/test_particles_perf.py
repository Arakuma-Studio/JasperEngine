"""create_particles costs the same in a big scene (S98 REQ-25, REQ-42, AC23): 300 calls into a scene of
1,000 GameObjects, the mean of the last 50 at most 1.5 times the mean of the first 50. A wall-clock
budget, so reported through harness.over_budget (enforced by the benchmark run)."""

from __future__ import annotations

import statistics
import time

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Particles perf"}

GAMEOBJECTS = 1000
CALLS = 300
WINDOW = 50


async def test_create_particles_flat_cost(ed: Editor):
    sid = (await ed.call("create_scene", name="Big", type="3D", path="scenes/Big"))["scene"]["id"]
    for start in range(0, GAMEOBJECTS, 200):
        await ed.batch([("create_entity", {"scene": sid, "type": "cube", "name": f"Box {i}", "position": [i % 40, 0, i // 40]})
                        for i in range(start, min(start + 200, GAMEOBJECTS))])
    made = await ed.call("batch", calls=[{"tool": "find_entities", "arguments": {"scene": sid, "name": "Box 999"}}])
    ed.check("Box 999" in str(made), "the scene holds its GameObjects")

    times: list[float] = []
    for i in range(CALLS):
        t0 = time.perf_counter()
        made = await ed.call("create_particles", scene=sid, preset="fire", name=f"Fire {i}", position=[i % 30, 2, i // 30])
        times.append(time.perf_counter() - t0)
        ed.check(made["created"], f"call {i} made a system")
    first, last = statistics.mean(times[:WINDOW]), statistics.mean(times[-WINDOW:])
    ratio = last / first
    print(f"     [bench] create_particles: first {WINDOW} mean {first * 1000:.1f} ms, last {WINDOW} mean {last * 1000:.1f} ms, "
          f"ratio {ratio:.2f}", flush=True)
    over = harness.over_budget("create_particles last/first mean", {"ratio": ratio}, 1.5, unit="x")
    ed.check(not over, f"the cost grows with the scene: {over}")
