// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "OrganisationCommands.h"

#include "Out.h"
#include "Project.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    bool sameEntries(const std::vector<LabelColor>& a, const std::vector<LabelColor>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].name != b[i].name || a[i].color != b[i].color) return false;
        }
        return true;
    }

    // project.yaml, for the shared data (label colours, project saved searches)
    bool saveProjectFile(Project* project, std::vector<std::string>& written) {
        if (project->getProjectPath().empty() || project->isTempProject()) return true;
        if (!project->saveProjectFile()) return false;
        written.push_back("project.yaml");
        return true;
    }

}

// ---- labels

SetAssetLabelsCmd::SetAssetLabelsCmd(Project* project, std::string name, Changes before, Changes after):
    project(project), name(std::move(name)), before(std::move(before)), after(std::move(after)) {
}

void SetAssetLabelsCmd::setEntries(std::vector<LabelColor> beforeEntries, std::vector<LabelColor> afterEntries) {
    entries = true;
    entriesBefore = std::move(beforeEntries);
    entriesAfter = std::move(afterEntries);
}

bool SetAssetLabelsCmd::apply(const Changes& changes, const std::vector<LabelColor>* entryList) {
    written.clear();
    lastError.clear();
    AssetLabels::WriteResult result = project->getAssetLabels().setLabels(changes);
    if (!result.ok) {
        lastError = result.error;
        return false;
    }
    written = result.filesWritten;
    if (entryList) {
        project->getAssetLabels().setEntries(*entryList);
        if (!saveProjectFile(project, written)) {
            lastError = "project.yaml could not be written (see the Console)";
            return false;
        }
    }
    return true;
}

bool SetAssetLabelsCmd::execute() {
    return apply(after, entries ? &entriesAfter : nullptr);
}

void SetAssetLabelsCmd::undo() {
    if (!apply(before, entries ? &entriesBefore : nullptr)) Out::warning("Undo %s: %s", name.c_str(), lastError.c_str());
}

bool SetAssetLabelsCmd::isNoOp() const {
    if (entries && !sameEntries(entriesBefore, entriesAfter)) return false;
    return before == after;
}

// ---- saved searches

SetSavedSearchesCmd::SetSavedSearchesCmd(Project* project, std::string name, std::vector<SavedSearch> userAfter,
                                         std::vector<SavedSearch> projectAfter):
    project(project), name(std::move(name)), userAfter(std::move(userAfter)), projectAfter(std::move(projectAfter)) {
    userBefore = project->getOrganisation().userSearches();
    projectBefore = project->getOrganisation().projectSearches();
}

bool SetSavedSearchesCmd::apply(const std::vector<SavedSearch>& user, const std::vector<SavedSearch>& shared) {
    written.clear();
    Organisation& organisation = project->getOrganisation();
    const bool userChanged = organisation.userSearches() != user;
    const bool projectChanged = organisation.projectSearches() != shared;
    organisation.setUserSearches(user);
    organisation.setProjectSearches(shared);
    if (userChanged) {
        std::string error;
        if (!organisation.save(&error)) {
            Out::warning("Saved searches: %s", error.c_str());
            return false;
        }
        written.push_back(".jasperengine/user/organisation.yaml");
    }
    if (projectChanged && !saveProjectFile(project, written)) return false;
    return true;
}

bool SetSavedSearchesCmd::execute() {
    return apply(userAfter, projectAfter);
}

void SetSavedSearchesCmd::undo() {
    apply(userBefore, projectBefore);
}

bool SetSavedSearchesCmd::isNoOp() const {
    return userBefore == userAfter && projectBefore == projectAfter;
}

// ---- bookmarks

SetBookmarksCmd::SetBookmarksCmd(Project* project, std::string name, std::vector<Bookmark> after):
    project(project), name(std::move(name)), after(std::move(after)) {
    before = project->getOrganisation().bookmarks();
}

bool SetBookmarksCmd::apply(const std::vector<Bookmark>& list) {
    written.clear();
    Organisation& organisation = project->getOrganisation();
    organisation.setBookmarks(list);
    std::string error;
    if (!organisation.save(&error)) {
        Out::warning("Bookmarks: %s", error.c_str());
        return false;
    }
    written.push_back(".jasperengine/user/organisation.yaml");
    return true;
}

bool SetBookmarksCmd::execute() {
    return apply(after);
}

void SetBookmarksCmd::undo() {
    apply(before);
}
