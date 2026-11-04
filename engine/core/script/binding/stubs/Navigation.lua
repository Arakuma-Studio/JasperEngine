---@meta _
-- Hand-written: the tables NavigationClassesLua.cpp builds with the Lua C API (paths, hits,
-- grid paths, query filters, query options), which the generator does not read. docs/navigation.md.

---What calculatePath returns (Unity's NavMeshPath)
---@class NavMeshPath
---@field status integer # NavMeshPathStatus.Complete, Partial or Invalid
---@field corners Vector3[] # world points from the start to the end (or to the reachable point closest to it)
---@field length number
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

---NavMeshAgent.currentOffMeshLinkData (Unity's OffMeshLinkData)
---@class OffMeshLinkData
---@field valid boolean
---@field activated boolean
---@field startPos Vector3
---@field endPos Vector3
---@field link integer # the NavMeshLink's entity
