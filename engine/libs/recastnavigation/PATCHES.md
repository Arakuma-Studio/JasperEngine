# Local patches to recastnavigation

The zlib licence asks that altered sources are plainly marked: each changed line carries a
`JasperEngine patch` comment, and each patch is listed here.

## 1. Avoidance priority (DetourCrowd)

Unity's `NavMeshAgent.avoidancePriority`: 0-99, lower is more important. An agent steers around
neighbours whose priority is less than or equal to its own and ignores those whose priority is
greater, so an important agent keeps its line and the others make way (spec S19 R13, D7).

- `DetourCrowd/Include/DetourCrowd.h`: `dtCrowdAgentParams::avoidancePriority` (`unsigned char`).
  Every caller fills it: the engine's `NavCrowd` sets it from the agent (50 by default), and
  non-carving obstacles get 0 so that every agent avoids them.
- `DetourCrowd/Source/DetourCrowd.cpp`, `dtCrowd::update`: the separation loop and the loop that
  adds neighbours to the obstacle avoidance query as circles skip a neighbour whose priority is
  greater than the agent's. The collision resolution that pushes overlapping agents apart is
  unchanged, so agents never interpenetrate whatever their priorities.

Covered by `NavCrowdTest.PriorityYields` (`engine/tests/core/navigation/NavCrowdTest.cpp`).
