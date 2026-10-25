// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpDataSafety.h"

#include "McpCall.h"
#include "Project.h"
#include "StreamPreserved.h"
#include "util/FileUtils.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

    constexpr size_t kMaxListedIssues = 50;

    std::string generic(const std::filesystem::path& path) {
        std::string text = FileUtils::pathToUtf8(path);
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
    }

}

Json mcp::loadIssueJson(const LoadIssue& issue, bool outstanding) {
    Json item = {
        {"kind", loadIssueKindName(issue.kind)},
        {"fate", loadIssueFateName(issue.fate)},
        {"message", issue.message},
        {"text", issue.describe()},
        {"outstanding", outstanding},
    };
    if (issue.entity != 0) item["entity"] = issue.entity;
    if (!issue.entityPath.empty()) item["entity_path"] = issue.entityPath;
    if (!issue.component.empty()) item["component"] = issue.component;
    if (!issue.field.empty()) item["field"] = issue.field;
    if (!issue.value.empty()) item["value"] = issue.value;
    if (!issue.suggestion.empty()) item["suggestion"] = issue.suggestion;
    if (!issue.file.empty()) item["file"] = issue.file;
    if (issue.line > 0) {
        item["line"] = issue.line;
        item["column"] = issue.column;
    }
    return item;
}

Json mcp::loadReportSummary(const LoadReport& report, const PreservedStore* store) {
    const size_t preserved = report.outstandingPreserved(store);
    const size_t dropped = report.dropped();
    return {{"skipped", preserved + dropped}, {"preserved", preserved}, {"dropped", dropped}, {"newer_format", report.newerFormat()}};
}

Json mcp::loadReportJson(const LoadReport& report, const PreservedStore* store) {
    const std::vector<const LoadIssue*> outstanding = report.outstanding(store);
    Json issues = Json::array();
    for (const LoadIssue& issue : report.issues) {
        const bool open = std::find(outstanding.begin(), outstanding.end(), &issue) != outstanding.end();
        issues.push_back(loadIssueJson(issue, open));
    }
    Json result = loadReportSummary(report, store);
    result["document"] = report.documentPath;
    result["version_read"] = report.versionRead;
    result["version_current"] = report.versionCurrent;
    result["issues"] = issues;
    return result;
}

Json mcp::degradedJson(const LoadReport& report, const PreservedStore* store) {
    return {{"preserved", report.outstandingPreserved(store)}, {"dropped", report.dropped()}};
}

Json mcp::degradedJson(const SceneProject& scene) {
    return degradedJson(scene.loadReport, findPreservedStore(scene.scene));
}

Json mcp::degradedDocumentError(const std::string& document, const LoadReport& report) {
    Json dropped = Json::array();
    for (const LoadIssue& issue : report.issues) {
        if (issue.fate == LoadIssueFate::Dropped && dropped.size() < kMaxListedIssues)
            dropped.push_back(loadIssueJson(issue, true));
    }
    return {
        {"code", "degraded_document"},
        {"document", document},
        {"dropped", dropped},
        {"dropped_count", report.dropped()},
        {"hint", "Pass allow_data_loss=true to write anyway, or path=<new file> to Save As."},
    };
}

void mcp::failDegraded(McpCall& call, const std::string& document, const LoadReport& report) {
    call.fail("\"" + document + "\" did not load completely: saving it would permanently remove " + std::to_string(report.dropped()) +
              " value(s) this editor could not read. Pass allow_data_loss=true to write anyway, or path=<new file> to Save As.",
              degradedDocumentError(document, report));
}

const char* mcp::backupStatusName(BackupStatus status) {
    switch (status) {
        case BackupStatus::Usable: return "usable";
        case BackupStatus::Stale: return "stale";
        case BackupStatus::None: return "none";
    }
    return "none";
}

Json mcp::backupJson(const Project* project, const SceneBackupInfo& backup) {
    const char* kind = backup.kind == BackupKind::Untitled ? "untitled" : backup.kind == BackupKind::Prefab ? "prefab" : "scene";
    Json item = {
        {"kind", kind},
        {"source", generic(backup.source)},
        {"backup", project ? generic(backup.backup.lexically_relative(project->getProjectPath())) : generic(backup.backup)},
        {"status", backupStatusName(backup.status)},
        {"autosaved_at", backup.savedAt},
    };
    if (backup.kind == BackupKind::Untitled) item["scene_id"] = backup.sceneId;
    return item;
}
