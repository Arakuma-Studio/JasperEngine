---@meta _
-- Hand-written: the tables NavigationClassesLua.cpp builds with the Lua C API (paths, hits,
-- grid paths, query filters), which the generator does not read. docs/navigation.md.

---What calculatePath returns (Unity's NavMeshPath)
---@class NavMeshPath
---@field status integer # NavMeshPathStatus.Complete, Partial or Invalid
---@field corners Vector3[] # world points from the start to the end (or to the reachable point closest to it)
---@field length number

---What samplePosition, raycast and findClosestEdge return with their boolean (Unity's NavMeshHit)
---@class NavMeshHit
---@field position Vector3
---@field normal Vector3
---@field distance number
---@field area integer # 0-15 (NavAreas)
---@field mask integer # 1 << area
---@field hit boolean

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
