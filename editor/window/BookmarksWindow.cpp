// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BookmarksWindow.h"
#include "window/HierarchyWindow.h"
#include "window/ProjectWindow.h"

#include "Backend.h"
#include "App.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "organisation/Bookmarks.h"
#include "organisation/OrganisationCommands.h"
#include "util/EntityPayload.h"
#include "util/Util.h"

#include "imgui.h"

#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    const char* kindIcon(Bookmark::Kind kind) {
        switch (kind) {
            case Bookmark::Kind::Asset: return ICON_FA_FILE;
            case Bookmark::Kind::Folder: return ICON_FA_FOLDER;
            case Bookmark::Kind::Scene: return ICON_FA_CUBES;
            case Bookmark::Kind::GameObject: return ICON_FA_CUBE;
        }
        return ICON_FA_FILE;
    }

    constexpr const char* ROW_PAYLOAD = "bookmark_index";

}

BookmarksWindow::BookmarksWindow(Project* project) : project(project) {
}

void BookmarksWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

bool BookmarksWindow::commit(std::vector<Bookmark> list, const char* name, std::string* error) {
    if (!project->getOrganisation().isOpen()) {
        if (error) *error = "Bookmarks belong to a saved project.";
        return false;
    }
    auto* command = new SetBookmarksCmd(project, name, std::move(list));
    command->setNoMerge();
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        if (error) *error = "The bookmarks could not be saved (see the Console).";
        return false;
    }
    return true;
}

bool BookmarksWindow::add(const Bookmark& bookmark, std::string* error) {
    std::vector<Bookmark> list = project->getOrganisation().bookmarks();
    for (const Bookmark& existing : list) {
        if (existing == bookmark) {
            if (error) *error = bookmark.name + " is bookmarked already.";
            return false;
        }
    }
    list.push_back(bookmark);
    return commit(std::move(list), "Add Bookmark", error);
}

bool BookmarksWindow::removeAt(size_t index, std::string* error) {
    std::vector<Bookmark> list = project->getOrganisation().bookmarks();
    if (index >= list.size()) {
        if (error) *error = "No bookmark " + std::to_string(index) + ".";
        return false;
    }
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
    return commit(std::move(list), "Remove Bookmark", error);
}

bool BookmarksWindow::move(size_t from, size_t to, std::string* error) {
    std::vector<Bookmark> list = project->getOrganisation().bookmarks();
    if (from >= list.size() || to >= list.size()) {
        if (error) *error = "A bookmark index is 0 to " + std::to_string(list.empty() ? 0 : list.size() - 1) + ".";
        return false;
    }
    if (from == to) return true;
    Bookmark moved = list[from];
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(from));
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    return commit(std::move(list), "Reorder Bookmarks", error);
}

bool BookmarksWindow::navigate(size_t index, std::string* error) {
    const std::vector<Bookmark>& list = project->getOrganisation().bookmarks();
    if (index >= list.size()) {
        if (error) *error = "No bookmark " + std::to_string(index) + ".";
        return false;
    }
    const Bookmark bookmark = list[index];
    const BookmarkLookups lookups = Bookmarks::projectLookups(project);
    BookmarkTarget target = Bookmarks::resolve(bookmark, lookups);
    App& app = Backend::getApp();
    if (bookmark.kind == Bookmark::Kind::GameObject && target.sceneClosed) {
        // the scene opens first, then the GameObject resolves in it
        project->openScene(project->getProjectPath() / target.path);
        target = Bookmarks::resolve(bookmark, lookups);
    }
    if (!target.found) {
        if (error) *error = bookmark.name + " is missing.";
        return false;
    }
    switch (bookmark.kind) {
        case Bookmark::Kind::Asset:
        case Bookmark::Kind::Folder:
            app.getProjectWindow()->revealAsset(project->getProjectPath() / target.path);
            app.getProjectWindow()->pingAsset(target.path);
            break;
        case Bookmark::Kind::Scene:
            project->openScene(project->getProjectPath() / target.path);
            break;
        case Bookmark::Kind::GameObject:
            project->setSelectedSceneId(target.sceneId);
            project->replaceSelectedEntities(target.sceneId, {static_cast<Entity>(target.entity)});
            app.getHierarchyWindow()->revealEntity(target.sceneId, static_cast<Entity>(target.entity));
            break;
    }
    return true;
}

void BookmarksWindow::locate(size_t index) {
    // Locate: the folder the bookmark's hint names (where it was last)
    const std::vector<Bookmark>& list = project->getOrganisation().bookmarks();
    if (index >= list.size()) return;
    fs::path folder = project->getProjectPath() / list[index].path;
    std::error_code ec;
    while (!folder.empty() && folder != project->getProjectPath() && !fs::is_directory(folder, ec)) folder = folder.parent_path();
    Backend::getApp().getProjectWindow()->revealAsset(folder);
}

void BookmarksWindow::acceptDrops() {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
        // absolute paths, '\0' separated (the Project window's)
        const char* data = static_cast<const char*>(payload->Data);
        size_t at = 0;
        while (at < static_cast<size_t>(payload->DataSize)) {
            const std::string path(data + at);
            at += path.size() + 1;
            if (path.empty()) continue;
            const std::string relative = project->normalizeToProjectRelative(path).generic_string();
            std::string error;
            if (!add(Bookmarks::forPath(project, relative), &error)) Out::warning("Bookmarks: %s", error.c_str());
        }
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity")) {
        if (payload->DataSize >= static_cast<int>(sizeof(EntityPayload))) {
            EntityPayload entity;
            std::memcpy(&entity, payload->Data, sizeof(EntityPayload));
            const uint32_t sceneId = payloadSceneId(entity, project->getSelectedSceneId());
            std::string error;
            if (!add(Bookmarks::forGameObject(project, sceneId, entity.entity), &error)) Out::warning("Bookmarks: %s", error.c_str());
        }
    }
    ImGui::EndDragDropTarget();
}

void BookmarksWindow::show() {
    if (!windowOpen) return;
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    const std::vector<Bookmark> list = project->getOrganisation().bookmarks();
    const BookmarkLookups lookups = Bookmarks::projectLookups(project);
    if (list.empty()) {
        ImGui::TextDisabled("Drag assets, folders, scenes or GameObjects here,");
        ImGui::TextDisabled("or use Add to Bookmarks in the Project window and the Hierarchy.");
    }

    int removeIndex = -1;
    int navigateIndex = -1;
    int moveFrom = -1;
    int moveTo = -1;
    int locateIndex = -1;
    for (size_t i = 0; i < list.size(); i++) {
        const Bookmark& bookmark = list[i];
        const BookmarkTarget target = Bookmarks::resolve(bookmark, lookups);
        const bool missing = !target.found && !target.sceneClosed;
        ImGui::PushID(static_cast<int>(i));
        if (missing) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const std::string label = std::string(kindIcon(bookmark.kind)) + "  " + bookmark.name + (missing ? "  (Missing)" : "");
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) navigateIndex = static_cast<int>(i);
        }
        if (missing) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            const std::string where = target.path.empty() ? bookmark.path : target.path;
            ImGui::SetTooltip("%s%s%s\nDouble-click to go there", where.c_str(),
                              bookmark.kind == Bookmark::Kind::GameObject ? "\n" : "",
                              bookmark.kind == Bookmark::Kind::GameObject ? bookmark.entityPath.c_str() : "");
        }

        // drag: an asset, folder or scene bookmark is the file itself (the Scene view and the
        // Hierarchy take it as from the Project window); dropped on another row it moves there
        if (ImGui::BeginDragDropSource()) {
            if (!missing && bookmark.kind != Bookmark::Kind::GameObject) {
                const std::string absolute = (project->getProjectPath() / target.path).string();
                std::vector<char> buffer(absolute.begin(), absolute.end());
                buffer.push_back('\0');
                ImGui::SetDragDropPayload("resource_files", buffer.data(), buffer.size());
            } else {
                const int index = static_cast<int>(i);
                ImGui::SetDragDropPayload(ROW_PAYLOAD, &index, sizeof(index));
            }
            ImGui::TextUnformatted(bookmark.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ROW_PAYLOAD)) {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = static_cast<int>(i);
            } else if (const ImGuiPayload* files = ImGui::AcceptDragDropPayload("resource_files")) {
                // a bookmarked file dropped on a row moves its bookmark there, a new one is added
                const std::string relative = project->normalizeToProjectRelative(static_cast<const char*>(files->Data)).generic_string();
                for (size_t j = 0; j < list.size(); j++) {
                    if (Bookmarks::resolve(list[j], lookups).path == relative) {
                        moveFrom = static_cast<int>(j);
                        moveTo = static_cast<int>(i);
                    }
                }
                if (moveFrom < 0) {
                    std::string error;
                    if (!add(Bookmarks::forPath(project, relative), &error)) Out::warning("Bookmarks: %s", error.c_str());
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (missing) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Locate")) locateIndex = static_cast<int>(i);
            ImGui::SetItemTooltip("Show the folder it was last seen in");
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeIndex = static_cast<int>(i);
            ImGui::SetItemTooltip("Remove this bookmark");
        }
        if (ImGui::BeginPopupContextItem("##BookmarkMenu")) {
            if (ImGui::MenuItem("Go There", nullptr, false, !missing)) navigateIndex = static_cast<int>(i);
            if (ImGui::MenuItem("Remove")) removeIndex = static_cast<int>(i);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // the rest of the window takes drops too
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight())));
    acceptDrops();

    std::string error;
    if (moveFrom >= 0 && moveTo >= 0 && !move(static_cast<size_t>(moveFrom), static_cast<size_t>(moveTo), &error)) {
        Out::warning("Bookmarks: %s", error.c_str());
    }
    if (removeIndex >= 0 && !removeAt(static_cast<size_t>(removeIndex), &error)) Out::warning("Bookmarks: %s", error.c_str());
    if (navigateIndex >= 0 && !navigate(static_cast<size_t>(navigateIndex), &error)) Out::warning("Bookmarks: %s", error.c_str());
    if (locateIndex >= 0) locate(static_cast<size_t>(locateIndex));
    ImGui::End();
}
