# Physics Placement fixture

A project for Physics Placement (the Scene view's Physics toggle, the `place_with_physics` MCP
tool and the `place-physics` CLI; design spec
`docs/superpowers/specs/2026-10-23-physics-placement-design.md`, section 10). Every object is an
engine primitive (boxes, spheres, cylinders, a torus) or a terrain; there are no imported assets.
`generate.py` builds it through the MCP tools (see its docstring); `tools/mcp/tests/test_place_physics_cli.py`
runs on it.

One scene per scenario. Floors are 0.2 m slabs with their top at y = 0. Distances are metres.

| Scene | Entities | What placing them should do |
|---|---|---|
| `TableCrate` | `Table` (`Top`, `Leg1`..`Leg4`: a table of five boxes, top face at y = 1.0); `Props/Crate` (`Bottom`, `WallN/S/E/W`: an open-top crate of five boxes on the table, inside 0.52 x 0.52, floor at y = 1.04, rim at y = 1.42); `Props/Box1`..`Box3` (12-16 cm boxes, tilted, in the air above the crate's opening); `Overlap/A`, `Overlap/B` (unit cubes on the floor, interpenetrating by 0.2 m along x) | `drop` on `Props/Box1..3`: all three land inside the crate, their centres between its floor and its rim, resting on the crate (or on each other). `separate` on `Overlap/A` and `Overlap/B`: they end up no more than 1 mm into each other (centres at least 1 m apart), still on the floor. |
| `RingPeg` | `Peg` (a cylinder, radius 6 cm, 0.8 m tall, standing on the floor); `Ring` (a torus, radius 0.25 m, tube radius 5 cm, inner radius 0.2 m, flat, 1.3 m up, centred over the peg) | `drop` on `Ring`: with a convex decomposition of the ring (`--decompose always`, `decompose: "always"`) it slides down the peg and rests on the floor, encircling it (centre y about 0.05); with a single hull (`--decompose never`) it rests on top of the peg (centre y about 0.85). |
| `Stack` | `Stack/Box01`..`Box20` (0.4 m boxes in a column, 2 mm apart, alternately nudged 1 cm sideways) | `drop` on all twenty: the column settles standing, each box resting on the one below (`Box01` on `Floor`), the top box's centre near y = 7.8. |
| `Plates` | `Plates/Plate1`..`Plate6` (0.5 x 0.35 m plates, 12 mm thick, tilted, 0.25 m apart in the air); `Pencils/Pencil1`..`Pencil4` (cylinders 7 mm thick and 18 cm long, lying, in the air) | `drop` on all of them: thin objects keep their shape and never tunnel; the plates end up flat on the floor or on each other, the pencils on the floor; nothing below y = 0. |
| `Pebbles` | `Ground` (a 40 m terrain of gentle fractal hills, 4 m max height); `Pebbles/Pebble01`..`Pebble30` (spheres at odd numbers, boxes at even ones, 6 to 30 cm, tilted, 1 to 3 m above the ground in a 6 m square) | `drop` on all thirty: each settles on `Ground` (or on another pebble), on the rendered terrain surface. `--jitter` with a `--seed` scatters them reproducibly. With `--max-seconds 0.05` they are still falling: not all settled (CLI exit code 3). |
| `Ramp` | `Ramp` (a 4 x 0.2 x 3.2 m board tilted 20 degrees about z, its high end at +x); `Balls/Small`, `Balls/Medium`, `Balls/Large` (the same 0.5 m-radius sphere at scale 0.35, 0.66 and 1.5, just above the ramp's upper half) | `drop` on the three balls: they roll down toward -x and off the ramp's low end onto the floor; the scale is baked into each ball's shape (radii 0.175, 0.33 and 0.75 m). |
| `Room` | `Room` (`Floor`, `WallN/S/E/W`: a 30 m room under one root, 3 m walls); `Furniture/Box` (a 0.8 m box) and `Furniture/Ball` (radius 0.5 m) in the air inside it | `Room` is larger than the 25 m mover limit: as a mover it is refused with "too large (> 25 m), treated as environment" and stays where it is; it is the environment instead. `drop` on `Furniture/Box` and `Furniture/Ball`: they land on the room's floor. |

Each scene has a `View` camera aimed at its scenario (the main camera) for screenshots.

Example (from the repository root):

    jasperengine-editor place-physics --project tools/fixtures/physics-placement --scene TableCrate \
        --entity Props/Box1 --entity Props/Box2 --entity Props/Box3 --format json

Run it on a copy when using `--save`: it writes the scene file.
