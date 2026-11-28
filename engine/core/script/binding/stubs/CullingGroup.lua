---@meta _
-- Hand-written shapes of the Culling Group and Simulation Culling callbacks
-- (engine/core/script/binding/CullingClassesLua.cpp, docs/scripting/culling-groups.md), appended to
-- the generated stubs.

---A Culling Group's onStateChanged: one call per sphere whose visibility or band changed, in
---ascending index order after the whole group was evaluated. `index` is 1-based; bands are
---0-based (0 nearest) and -1 is unknown (the first evaluation reports every sphere with
---previousBand -1).
---@alias CullingGroupStateChanged fun(index: integer, isVisible: boolean, wasVisible: boolean, band: integer, previousBand: integer)

---SimulationCulling.setGroundCheck's function: whether the ground under the sphere at (x, y, z)
---of `radius` metres is loaded (false freezes the GameObject with reason "ground_not_loaded").
---@alias SimulationGroundCheck fun(x: number, y: number, z: number, radius: number): boolean

---CullingGroup:queryIndices fills `out[1..n]` with the matching 1-based sphere indices and
---returns n; entries past n are left as they were, so a table kept between calls makes no garbage.
---@alias CullingGroupIndices integer[]
