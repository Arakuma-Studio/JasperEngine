// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpModelImport.h"

#include "McpContext.h"
#include "Project.h"

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {
    JobPtr trackedImportJob;
    uint64_t trackedImportBatch = 0;

    Json trackedImportCounts(const Project::ModelImportProgress& progress) {
        return Json{{"done", progress.done}, {"total", progress.total}, {"failed", progress.failed}};
    }
}

JobPtr mcp::syncModelImportJob(McpContext& ctx, bool ensure) {
    Project* project = ctx.project();
    if (!project) return nullptr;
    const Project::ModelImportProgress progress = project->getModelImportProgress();
    if (trackedImportJob && !trackedImportJob->finished() && (trackedImportBatch != progress.batch || !progress.active)) {
        std::string message = "Imported " + std::to_string(progress.done) + "/" + std::to_string(progress.total) + " model(s).";
        if (progress.failed > 0) message += " " + std::to_string(progress.failed) + " failed (read_console has why).";
        trackedImportJob->succeed(message, trackedImportCounts(progress));
    }
    if (!progress.active || (progress.total == 0 && !ensure)) {
        return nullptr;
    }
    if (!trackedImportJob || trackedImportJob->finished()) {
        if (trackedImportJob && trackedImportBatch == progress.batch) return nullptr;
        trackedImportJob = ctx.jobs().create("model_import", "Importing FBX models");
        trackedImportBatch = progress.batch;
        // the conversions end on their own: a cancel leaves them running
        trackedImportJob->setCancelHandler([]() {});
    }
    trackedImportJob->setProgress(progress.total > 0 ? static_cast<double>(progress.done) / static_cast<double>(progress.total) : -1.0,
                           std::to_string(progress.done) + "/" + std::to_string(progress.total) + " models imported");
    return trackedImportJob;
}
