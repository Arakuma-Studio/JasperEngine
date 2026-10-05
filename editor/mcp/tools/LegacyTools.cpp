// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every action of the former in-editor agent (mcp/legacy) as an MCP tool, until the
// workstream modules replace them by name.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/legacy/EditorActionExecutor.h"
#include "mcp/legacy/EditorActionRegistry.h"

#include "Exporter.h"
#include "Project.h"

#include <cctype>
#include <memory>

namespace jasperengine::editor::mcp {

namespace {

std::string titleFromName(const std::string& name) {
    std::string title;
    bool upper = true;
    for (char c : name) {
        if (c == '_') {
            title.push_back(' ');
            upper = true;
            continue;
        }
        title.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        upper = false;
    }
    return title;
}

bool startsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

ToolResult toToolResult(const legacy::ActionResult& action) {
    ToolResult result;
    result.text = action.message;
    result.isError = !action.success;
    if (action.data.is_object() && !action.data.empty()) {
        result.structured = action.data;
    } else if (!action.data.is_null() && !action.data.is_object()) {
        result.structured = Json{{"value", action.data}};
    }
    return result;
}

// export_project on the Exporter's own thread, so the editor keeps drawing while it builds
void exportProjectAsync(const CallPtr& call) {
    ExportConfig config;
    std::string error;
    if (!call->ctx().legacyExecutor().prepareExport(call->args(), config, error)) {
        call->fail(error);
        return;
    }
    auto exporter = std::make_shared<Exporter>();
    exporter->startExport(call->ctx().project(), config);
    const std::string targetDir = config.targetDir.generic_string();
    auto lastStep = std::make_shared<std::string>();

    call->waitUntil([call, exporter, lastStep]() {
        if (call->cancelled()) exporter->cancelExport();
        const ExportProgress progress = exporter->getProgress();
        if (progress.currentStep != *lastStep) {
            *lastStep = progress.currentStep;
            call->progress(progress.overallProgress, 1.0, progress.currentStep);
        }
        return !exporter->isRunning() || progress.finished || progress.failed;
    }, 0, [call, exporter, targetDir](bool) {
        const ExportProgress progress = exporter->getProgress();
        if (progress.failed || !progress.finished) {
            call->fail(progress.errorMessage.empty() ? "Build failed." : progress.errorMessage,
                       Json{{"target_dir", targetDir}, {"step", progress.currentStep}, {"detail", progress.detailLine}});
        } else {
            call->ok("Built project.", Json{{"target_dir", targetDir}});
        }
    });
}

} // namespace

void registerLegacyTools(McpRegistry& registry) {
    for (const legacy::ToolDefinition& definition : legacy::EditorActionRegistry::tools()) {
        ToolDef tool;
        tool.name = definition.name;
        tool.title = titleFromName(definition.name);
        tool.description = definition.description;
        tool.inputSchema = definition.parameters;
        tool.annotations.readOnly = definition.readOnly;
        tool.annotations.destructive = !definition.readOnly &&
            (startsWith(definition.name, "delete_") || startsWith(definition.name, "remove_"));
        tool.annotations.idempotent = definition.readOnly;

        const std::string name = definition.name;
        if (name == "export_project") {
            tool.handler = exportProjectAsync;
        } else {
            tool.handler = [name](CallPtr call) {
                call->reply(toToolResult(call->ctx().legacyExecutor().execute(name, call->args())));
            };
        }
        registry.addTool(std::move(tool));
    }
}

}
