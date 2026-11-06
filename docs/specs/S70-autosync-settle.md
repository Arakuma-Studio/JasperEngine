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

## Deviations

- **Large files keep a partial content check.** R4 records "a content hash". Files up to 1 MB are hashed whole; of a larger one the
  first and last 64 KB are, because hashing a fresh 64 MB texture whole on every scan cost about 0.5 s per call around it (the
  reference's own finding, §5.6 of its design). So a same-tick, same-size rewrite of a file over 1 MB that differs only in its middle
  is not seen. The reference gave up on such files altogether.
- **Verification has a time budget.** Racy files are verified newest first within the scan's budget (at least 16 files), so a
  checkout that leaves thousands of files racy does not slow every call for 2 s. What the budget leaves stays racy, and a later call
  reports it.
- **The settle scans where the barrier is not a mere drain.** R6 is met on Linux and Windows: the settle reads the journal behind a
  new barrier, `FileWatchService::flushJournal`, which delivers nothing to main-thread subscribers. On macOS (FSEvents' marker round
  trip) and with the polling watcher (a walk of the project) the settle scans instead, to hold R5; the watcher's later report of the
  same writes finds them handled. A tool writing past the atomic writer to an asset the 1 ms settle budget does not reach can there
  be reported once as external. Windows and macOS were not run (inferred from the code).
- **More completions get a task.** R2 lists "a task posted from a job's completion". Any completion with no handler on its stack gets
  one, a main-thread task's included, so such a call does not depend on a frame being drawn.
- **Editor writes.** The journal's origin-Editor changes now move the baseline (before, a later settle scan could report a UI save
  stamped outside the call as external). The call-window filter on the journal is gone.
- **The same-stat rewrite reloads too.** A rewrite found by its hash also clears the hot reload's applied stamp
  (`HotReloadService::forgetApplied`); otherwise it was reported but not reloaded once that stamp had stopped being racy (100 ms).
- **AC1's same-tick new file forges more than the reference's test.** It also gives the new file the tool's mtime, the stamp a real
  same-tick write has, which is what the journal path must survive.
- **A file picker comes before the settle (S69).** A picker holds the main thread until it is answered, and the client may have to answer
  it first, so a result completed while one is open goes out at once and its settle runs when the thread is back. A client's write in
  that gap counts as written during the call (R7).
