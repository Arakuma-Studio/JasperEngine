// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The FBX import batch (S98 REQ-32, REQ-33: Project's FbxImportQueue) as a job agents see in
// editor_status and get_job: kind "model_import", progress done/total, finished when the batch ends.

#pragma once

#include "McpJobs.h"

#include <string>

namespace jasperengine::editor::mcp {

    class McpContext;

    // Brings the job up to date and returns it while the batch runs (null otherwise). `ensure`
    // makes it before the batch has counted its conversions: a tool that just queued a missing
    // import names it. Main thread; McpServer::tick calls it every frame.
    JobPtr syncModelImportJob(McpContext& ctx, bool ensure = false);

}
