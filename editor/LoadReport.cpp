// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LoadReport.h"

#include "Out.h"
#include "StreamPreserved.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    thread_local LoadReport* currentReport = nullptr;
    thread_local uint32_t installedSceneId = 0;

    std::string suggestionText(const std::string& suggestion) {
        return suggestion.empty() ? std::string() : " (did you mean \"" + suggestion + "\"?)";
    }

    std::string fileName(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

}

const char* editor::loadIssueKindName(LoadIssueKind kind) {
    switch (kind) {
        case LoadIssueKind::UnknownComponent: return "unknown_component";
        case LoadIssueKind::UnknownField: return "unknown_field";
        case LoadIssueKind::UnknownEnumValue: return "unknown_enum_value";
        case LoadIssueKind::InvalidValue: return "invalid_value";
        case LoadIssueKind::UnknownEntityType: return "unknown_entity_type";
        case LoadIssueKind::UnknownRootKey: return "unknown_root_key";
        case LoadIssueKind::NewerFormat: return "newer_format";
        case LoadIssueKind::Migrated: return "migrated";
        case LoadIssueKind::Fallback: return "fallback";
    }
    return "unknown";
}

const char* editor::loadIssueFateName(LoadIssueFate fate) {
    switch (fate) {
        case LoadIssueFate::Preserved: return "preserved";
        case LoadIssueFate::Dropped: return "dropped";
        case LoadIssueFate::Info: return "info";
    }
    return "info";
}

std::string LoadIssue::location() const {
    if (component.empty())
        return field;
    const std::string shown = component == kSceneSettingsBlock ? std::string("Scene") : component;
    return field.empty() ? shown : shown + "." + field;
}

std::string LoadIssue::describe() const {
    std::string text;
    if (!entityPath.empty()) {
        std::string path = entityPath;
        for (size_t at = path.find('/'); at != std::string::npos; at = path.find('/', at + 3))
            path.replace(at, 1, " \xE2\x80\xBA ");
        text += path + " \xE2\x80\xBA ";
    }
    text += location();
    if (!value.empty())
        text += " = \"" + value + "\"";
    text += ": " + message + suggestionText(suggestion);
    if (!file.empty()) {
        text += " \xE2\x80\x94 " + fileName(file);
        if (line > 0)
            text += ":" + std::to_string(line);
    }
    return text;
}

std::string LoadIssue::consoleLine() const {
    std::string text = file;
    if (line > 0)
        text += ":" + std::to_string(line);
    text += ": ";
    const std::string where = location();
    if (!where.empty())
        text += where + ": ";
    text += message;
    if (!value.empty())
        text += " \"" + value + "\"";
    text += suggestionText(suggestion);
    if (!entityPath.empty())
        text += " (in " + entityPath + ")";
    if (fate != LoadIssueFate::Info)
        text += fate == LoadIssueFate::Preserved ? " \xE2\x80\x94 kept" : " \xE2\x80\x94 dropped";
    return text;
}

size_t LoadReport::dropped() const {
    return (size_t)std::count_if(issues.begin(), issues.end(), [](const LoadIssue& issue) { return issue.fate == LoadIssueFate::Dropped; });
}

size_t LoadReport::preserved() const {
    return (size_t)std::count_if(issues.begin(), issues.end(), [](const LoadIssue& issue) { return issue.fate == LoadIssueFate::Preserved; });
}

size_t LoadReport::infos() const {
    return (size_t)std::count_if(issues.begin(), issues.end(), [](const LoadIssue& issue) { return issue.fate == LoadIssueFate::Info; });
}

bool LoadReport::newerFormat() const {
    return std::any_of(issues.begin(), issues.end(), [](const LoadIssue& issue) { return issue.kind == LoadIssueKind::NewerFormat; });
}

std::vector<const LoadIssue*> LoadReport::outstanding(const PreservedStore* store) const {
    std::vector<const LoadIssue*> result;
    for (const LoadIssue& issue : issues) {
        if (issue.fate == LoadIssueFate::Dropped) {
            result.push_back(&issue);
        } else if (issue.fate == LoadIssueFate::Preserved) {
            // kept by a file overlay (no store), or still held by the store
            if (!store || store->holds(issue.entity, issue.component, issue.field))
                result.push_back(&issue);
        }
    }
    return result;
}

size_t LoadReport::outstandingPreserved(const PreservedStore* store) const {
    size_t count = 0;
    for (const LoadIssue* issue : outstanding(store)) {
        if (issue->fate == LoadIssueFate::Preserved)
            count++;
    }
    return count;
}

void LoadReport::clearDropped() {
    issues.erase(std::remove_if(issues.begin(), issues.end(), [](const LoadIssue& issue) {
        return issue.fate == LoadIssueFate::Dropped;
    }), issues.end());
}

void LoadReport::clear() {
    issues.clear();
    versionRead = 0;
    versionCurrent = 0;
}

bool LoadReport::empty() const {
    return std::all_of(issues.begin(), issues.end(), [](const LoadIssue& issue) { return issue.fate == LoadIssueFate::Info; });
}

ScopedLoadReport::ScopedLoadReport(LoadReport& report, uint32_t sceneId):
        previous(currentReport), previousSceneId(installedSceneId) {
    currentReport = &report;
    installedSceneId = sceneId;
}

ScopedLoadReport::~ScopedLoadReport() {
    currentReport = previous;
    installedSceneId = previousSceneId;
}

LoadReport* ScopedLoadReport::current() {
    return currentReport;
}

uint32_t ScopedLoadReport::currentSceneId() {
    return installedSceneId;
}

void editor::recordLoadIssue(LoadIssue issue) {
    LoadReport* report = currentReport;
    if (!report)
        return;
    if (issue.sceneId == 0)
        issue.sceneId = installedSceneId;
    if (issue.file.empty())
        issue.file = report->documentPath;
    report->issues.push_back(std::move(issue));
}

void editor::logLoadReport(const LoadReport& report) {
    size_t problems = 0;
    for (const LoadIssue& issue : report.issues) {
        if (issue.fate == LoadIssueFate::Info) {
            Out::info(issue.consoleLine());
            continue;
        }
        problems++;
        if (issue.fate == LoadIssueFate::Dropped)
            Out::error(issue.consoleLine());
        else
            Out::warning(issue.consoleLine());
    }
    if (problems == 0)
        return;
    const size_t kept = report.preserved();
    const size_t lost = report.dropped();
    const std::string summary = "Loaded " + fileName(report.documentPath) + " with " + std::to_string(problems) +
            (problems == 1 ? " issue (" : " issues (") + std::to_string(kept) + " kept, " + std::to_string(lost) + " dropped).";
    if (lost > 0)
        Out::error(summary);
    else
        Out::warning(summary);
}
