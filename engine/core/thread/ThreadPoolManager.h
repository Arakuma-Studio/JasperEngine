// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef threadpoolmanager_h
#define threadpoolmanager_h

// Kept one release so projects that included it get a clear error. The engine's job system
// replaces the pool: jobs::submit, jobs::parallelFor, jobs::JobCounter, SerialQueue and
// JobChannel (see docs/jobs-memory.md).
#error "ThreadPoolManager was removed: include \"jobs/JobSystem.h\" and use jasperengine::jobs::submit / jobs::parallelFor"

#endif /* threadpoolmanager_h */
