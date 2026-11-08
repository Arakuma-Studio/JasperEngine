---@meta _
-- Hand-written: the tables NavigationClassesLua.cpp builds with the Lua C API (paths, hits,
-- grid paths, query filters, query options), which the generator does not read. docs/navigation.md.

---What calculatePath returns (Unity's NavMeshPath)
---@class NavMeshPath
---@field status integer # NavMeshPathStatus.Complete, Partial or Invalid
---@field corners Vector3[] # world points from the start to the end (or to the reachable point closest to it)
---@field length number
---@field reason string # why it is not Complete: "none", "no_surface", "not_baked", "start_off_mesh", "end_off_mesh", "start_filtered", "end_filtered", "disconnected", "blocked_by_area", "out_of_nodes"
---@field surface integer # the NavMeshSurface that answered (0: none)
---@field closestPoint Vector3 # the last corner (the snapped start when there are none)
---@field gap number # from closestPoint to the end (0 when Complete)
---@field cost number # each stretch's length times its area's cost (a link at its own cost)
---@field cornerAreas integer[] # per corner: the area of the stretch that starts there (the last: the area at the end)
---@field cornerFlags integer[] # per corner: NavCornerFlags bits (START, END, OFF_MESH_LINK)
---@field areaLengths table<integer, number> # area index (0-15) -> the length walked in it, areas walked only
---@field corridor NavMeshPathCorridor|nil # what NavMeshAgent:setPath reads; nil on a path built by a script

---calculatePath options
---@class NavMeshPathOptions
---@field areaCrossings? boolean # a corner wherever the path changes area

---randomPoint options: none is anywhere (uniform by area)
---@class NavRandomPointOptions
---@field center? Vector3 # with radius: a point reachable from it within radius (not uniform)
---@field radius? number
---@field sameIslandAs? Vector3 # only the island (connected part) of the navmesh under this point
---@field seed? integer # draws from a stream of this seed, leaving NavigationSystem.randomSeed's alone

---What NavigationSystem:locate returns
---@class NavLocation
---@field onMesh boolean # a polygon is where a path from here would start
---@field found boolean # a polygon within maxDistance
---@field position Vector3
---@field snapDistance number # -1 when none was found
---@field island integer # -1: none
---@field area integer
---@field surface integer

---What NavigationSystem:getCoverage returns (m²)
---@class NavCoverage
---@field walkableArea number
---@field islandCount integer
---@field largestIslandArea number
---@field tilesArea number
---@field fraction number # walkableArea / tilesArea

---One item of NavigationSystem:getIslands
---@class NavIsland
---@field id integer
---@field area number # m²
---@field polys integer
---@field boundsMin Vector3
---@field boundsMax Vector3

---What a bake collected (NavBakeReport.sources)
---@class NavBakeSources
---@field meshes integer
---@field instances integer
---@field terrains integer
---@field colliders integer
---@field colliders2D integer
---@field volumes integer
---@field links integer
---@field excluded integer
---@field triangles integer

---One of NavBakeReport.slowestTiles
---@class NavBakeTile
---@field x integer
---@field y integer
---@field ms number
---@field polys integer
---@field kb number

---NavMeshSurface.lastBakeResult (bake_navigation's per-surface result, camelCase)
---@class NavBakeReport
---@field ok boolean
---@field cancelled boolean
---@field error string
---@field errorCode string # "none", "no_surface", "unavailable", "busy", "nothing_collected", "too_large", "tile_failed", "navmesh_failed", "cancelled", "save_failed"
---@field hint string
---@field tilesBuilt integer
---@field tilesTotal integer
---@field polys integer
---@field milliseconds number
---@field warnings string[]
---@field sources NavBakeSources
---@field walkableArea number # m²
---@field islandCount integer
---@field islands NavIsland[] # the 16 largest
---@field slowestTiles NavBakeTile[] # at most 5

---What samplePosition, raycast and findClosestEdge return with their boolean (Unity's NavMeshHit)
---@class NavMeshHit
---@field position Vector3
---@field normal Vector3
---@field distance number
---@field area integer # 0-15 (NavAreas)
---@field mask integer # 1 << area
---@field hit boolean
---@field cost number # raycast: the cost of the travelled part

---What NavGrid:findPath returns
---@class NavGridPath
---@field status integer # NavMeshPathStatus.Complete, Partial or Invalid
---@field cells GridCell[] # every cell stepped through, start and end included
---@field points Vector3[] # the world waypoints (smoothed, or every cell centre)
---@field cost number

---NavGrid:findPath options
---@class NavGridQuery
---@field allowPartial? boolean # an unreachable goal gives the path to the closest reachable cell
---@field areaMask? integer
---@field cornerCutting? boolean

---NavMeshAgent.currentOffMeshLinkData and nextOffMeshLinkData (Unity's OffMeshLinkData)
---@class OffMeshLinkData
---@field valid boolean
---@field activated boolean
---@field occupied boolean # an agent is crossing it (nextOffMeshLinkData)
---@field startPos Vector3
---@field endPos Vector3
---@field link integer # the NavMeshLink's entity
