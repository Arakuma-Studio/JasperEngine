# S70 — Auto-sync settles before the reply, so external changes are never lost

**Depends on.** S05's journal and watcher, `editor/mcp/McpAutoSync.*`, `McpServer.cpp`. Reference: sound-layer
`2026-10-19-sound-layer-cleanup-b-design.md` §2.2, `test_autosync_race.py`, in `je-wt/snd-ro` (re-derive on today's code).

## 1. Goal
A file an agent's other tools write right after a tool call is reported as an external change by the next call, even 0 ms
later and even when stamped in the same clock tick. Today the reply goes out first and the settle scan runs a frame later,
so such a write can be absorbed as the tool's own, and the agent silently works on stale data.

## 2. Requirements
- R1. Red test first, on main, with a same-tick write through the existing hooks (or a stress loop if no hook can force it).
- R2. A call that may write files sends its result after its settle has run on the main thread. The queue runs where no handler
  is on the stack: after the handler returns, at the end of `McpServer::tick`, in a task posted from a job's completion, and in
  `McpServer::stop`. A synchronous call gains no added frame.
- R3. The settle takes every change it finds as the tool's own, except changes stamped before the call started.
- R4. Racy stamps are verified by content: a file or folder stamp not older than the scan minus 2 s is recorded with a content hash
  (or relisted for folders) and checked on later scans, until its stamp is old enough.
- R5. Tool results, schemas and report formats do not change. Write-tool latency adds under 2 ms typical on the fixture.
- R6. Use S05's journal where it already gives the answer rather than duplicating scans.
- R7. A program writing during a call stays out of scope and is documented as such.

## 3. Acceptance
- AC1. Ported `test_autosync_race.py` (0 ms new file, same-tick new file in the tool's folder, same-tick same-size rewrite),
  failing on main first, 20 runs clean under load.
- AC2. `test_autosync.py` and watcher tests green; latency measured before and after and reported.
- AC3. `tools/check-changed` green.
