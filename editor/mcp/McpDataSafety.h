// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The JSON of the data-safety features for the MCP tools (docs/data-safety.md): load reports,
// the degraded-save refusal, crash backups.

#include "McpJson.h"

#include "LoadReport.h"
#include "SceneBackup.h"

#include <string>

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
    class PreservedStore;
}

namespace jasperengine::editor::mcp {

    class McpCall;

    Json loadIssueJson(const LoadIssue& issue, bool outstanding);
    // {skipped, preserved, dropped, newer_format}: preserved counts what is still kept
    Json loadReportSummary(const LoadReport& report, const PreservedStore* store);
    // The whole report, each issue with its outstanding state
    Json loadReportJson(const LoadReport& report, const PreservedStore* store);
    // {preserved, dropped} of a scene (every save result carries it)
    Json degradedJson(const SceneProject& scene);
    Json degradedJson(const LoadReport& report, const PreservedStore* store);

    // The refusal of a save that would drop data: code degraded_document, the dropped issues
    // (at most 50) and the hint
    Json degradedDocumentError(const std::string& document, const LoadReport& report);
    void failDegraded(McpCall& call, const std::string& document, const LoadReport& report);

    const char* backupStatusName(BackupStatus status);
    Json backupJson(const Project* project, const SceneBackupInfo& backup);

}
