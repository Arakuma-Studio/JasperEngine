// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "hub/ProjectHubWindow.h"
#include "util/UiItemLocator.h"
#include "player/LaunchOptions.h"

#include "AppSettings.h"
#include "Backend.h"
#include "Platform.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "hub/HubPaths.h"
#include "texture/TextureData.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "widget/ColorMath.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    constexpr float kCardWidth = 240.0f;
    constexpr float kCardHeight = 135.0f;

    void copyText(std::array<char, 128>& out, const std::string& text) {
        std::snprintf(out.data(), out.size(), "%s", text.c_str());
    }

    template <size_t N>
    void copyPath(std::array<char, N>& out, const fs::path& path) {
        std::snprintf(out.data(), out.size(), "%s", FileUtils::pathToUtf8(path).c_str());
    }

    fs::path homeDirectory() {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        return home && *home ? fs::path(home) : fs::current_path();
    }

    ImU32 accent(const std::string& hex) {
        Vector4 srgb;
        if (!colormath::fromHex(hex, srgb)) return IM_COL32(90, 103, 167, 255);
        return IM_COL32(colormath::toByte(srgb.x), colormath::toByte(srgb.y), colormath::toByte(srgb.z), 255);
    }

    // a key over a file's thumbnail, refreshed when the file changes
    std::string fileKey(const fs::path& file) {
        std::error_code ec;
        const auto time = fs::last_write_time(file, ec);
        return file.generic_string() + "@" + std::to_string(ec ? 0 : (long long)time.time_since_epoch().count());
    }

}

hub::LaunchOptions& hub::launchOptions() {
    static LaunchOptions options;
    return options;
}

fs::path ProjectHubWindow::defaultLocation() {
    const fs::path last = AppSettings::getHubLastLocation();
    if (!last.empty()) return last;
    return homeDirectory() / "JasperEngine Projects";
}

std::string ProjectHubWindow::createProblem(const std::string& name, const fs::path& parent) {
    const std::string folder = hub::folderName(name);
    if (folder.empty()) return "The project needs a name.";
    if (parent.empty()) return "Pick a location.";
    const std::string problem = hub::targetProblem(parent / folder);
    return problem.empty() ? std::string() : problem + ".";
}

void ProjectHubWindow::open(Page which) {
    visible = true;
    page = which;
    templatesLoaded = false;
    projectsMessage.clear();
    createMessage.clear();
    openMessage.clear();
    if (projectName[0] == '\0') copyText(projectName, "New Project");
    if (location[0] == '\0') copyPath(location, defaultLocation());
    if (importLocation[0] == '\0') copyPath(importLocation, defaultLocation());
    startSweep(true);
}

void ProjectHubWindow::close() {
    visible = false;
}

hub::staging::Scope ProjectHubWindow::leftoverScope() {
    hub::staging::Scope scope;
    std::set<fs::path> parents = {defaultLocation(), hub::templates::userDirectory(AppSettings::getConfigDirectory()) / ".staging"};
    for (const hub::RecentProject& project : AppSettings::getRecentProjectRecords()) {
        parents.insert(project.path.parent_path());
        scope.projects.push_back(project.path);
    }
    scope.parents.assign(parents.begin(), parents.end());
    return scope;
}

void ProjectHubWindow::startSweep(bool knownParents) {
    if (sweepTask.valid()) {
        sweepAgain = true;
        return;
    }
    const hub::staging::Scope scope = knownParents ? leftoverScope() : hub::staging::Scope();
    sweepTask = std::async(std::launch::async, [scope, removals = std::move(removalRequests)]() {
        SweepDone done;
        for (const fs::path& folder : removals) hub::staging::removeLeftover(folder, done.removalError);
        done.leftovers = hub::staging::sweep(scope).leftovers;
        return done;
    });
    removalRequests.clear();
}

bool ProjectHubWindow::pollSweep() {
    if (!sweepTask.valid() || sweepTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    SweepDone done = sweepTask.get();
    leftovers = std::move(done.leftovers);
    leftoverMessage = std::move(done.removalError);
    if (sweepAgain) {
        sweepAgain = false;
        startSweep(true);
    }
    return true;
}

Texture* ProjectHubWindow::thumbnail(const std::string& key, const fs::path& file) {
    auto it = thumbnails.find(key);
    if (it == thumbnails.end()) {
        std::error_code ec;
        if (file.empty() || !fs::exists(file, ec)) return nullptr;
        Thumbnail& t = thumbnails[key];
        const std::string path = file.string();
        t.pending = std::async(std::launch::async, [path]() {
            auto data = std::make_shared<TextureData>();
            if (!data->loadTextureFromFile(path.c_str())) data.reset();
            return data;
        });
        return nullptr;
    }
    Thumbnail& t = it->second;
    if (!t.texture && !t.failed && t.pending.valid() && t.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::shared_ptr<TextureData> data = t.pending.get();
        if (data) {
            t.texture = std::make_unique<Texture>();
            t.texture->setData("editor:hub:" + key, *data);
            t.texture->load();
        } else {
            t.failed = true;
        }
    }
    return t.texture.get();
}

Texture* ProjectHubWindow::thumbnailFromBytes(const std::string& key, const std::vector<uint8_t>& png) {
    if (png.empty()) return nullptr;
    auto it = thumbnails.find(key);
    if (it == thumbnails.end()) {
        Thumbnail& t = thumbnails[key];
        std::vector<uint8_t> bytes = png;
        t.pending = std::async(std::launch::async, [bytes]() mutable {
            auto data = std::make_shared<TextureData>();
            if (!data->loadTextureFromMemory(bytes.data(), (unsigned int)bytes.size())) data.reset();
            return data;
        });
        return nullptr;
    }
    return thumbnail(key, fs::path());
}

void ProjectHubWindow::show() {
    if (!visible) return;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin(WINDOW_NAME, nullptr, flags)) {
        // Esc: back to the project open behind the hub
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() && actions.hasProject && actions.hasProject() &&
            !import.running()) {
            close();
        }
        drawNavigation();
        ImGui::SameLine();
        ImGui::BeginChild("##hubPage", ImVec2(0, 0), ImGuiChildFlags_Borders);
        switch (page) {
            case Page::Projects: drawProjects(); break;
            case Page::NewProject: drawNewProject(); break;
            case Page::Open: drawOpen(); break;
            case Page::Import: drawImport(); break;
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void ProjectHubWindow::drawNavigation() {
    ImGui::BeginChild("##hubNavigation", ImVec2(Theme::dpi(190.0f), 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("JasperEngine");
    ImGui::TextDisabled("Project Hub");
    ImGui::Separator();
    struct Item {
        const char* label;
        Page page;
    };
    for (const Item& item : {Item{ICON_FA_FOLDER_OPEN "  Projects", Page::Projects}, Item{ICON_FA_PLUS "  New Project", Page::NewProject},
                             Item{ICON_FA_FOLDER "  Open", Page::Open}, Item{ICON_FA_DOWNLOAD "  Import", Page::Import}}) {
        if (ImGui::Selectable(item.label, page == item.page, 0, ImVec2(0, ImGui::GetFrameHeight()))) page = item.page;
    }
    if (actions.hasProject && actions.hasProject()) {
        ImGui::Separator();
        if (ImGui::Selectable(ICON_FA_ARROW_LEFT "  Back to Project (Esc)")) close();
    }
    ImGui::EndChild();
}

void ProjectHubWindow::drawLeftovers() {
    if (leftovers.empty()) {
        if (!leftoverMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", leftoverMessage.c_str());
        return;
    }
    const std::string header = "Leftover Folders (" + std::to_string(leftovers.size()) + ")###hubLeftovers";
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("A stopped import or template copy may have left these, and they may hold your files, so the Hub does not remove them by itself.");
    if (!leftoverMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", leftoverMessage.c_str());
    const bool working = sweepTask.valid();
    for (size_t i = 0; i < leftovers.size(); i++) {
        const hub::staging::Leftover& leftover = leftovers[i];
        const std::string probe = "hub.leftover." + std::to_string(i);
        ImGui::PushID(leftover.folder.generic_string().c_str());
        ImGui::BeginGroup();
        ImGui::TextUnformatted(FileUtils::pathToUtf8(leftover.folder).c_str());
        ImGui::TextDisabled("%s", leftover.reason.c_str());
        ImGui::EndGroup();
        ImGui::BeginDisabled(working);
        if (ImGui::SmallButton("Show in Folder")) {
            std::string error;
            if (!Platform::openWithSystemDefault(FileUtils::pathToUtf8(leftover.folder), &error)) leftoverMessage = error;
        }
        UIProbe::item(probe + ".show");
        ImGui::SameLine();
        if (removalToConfirm == leftover.folder) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Delete this folder and all its files?");
            ImGui::SameLine();
            const bool remove = ImGui::SmallButton("Delete");
            UIProbe::item(probe + ".delete");
            ImGui::SameLine();
            const bool keep = ImGui::SmallButton("Keep");
            UIProbe::item(probe + ".keep");
            if (remove) {
                removalRequests.push_back(leftover.folder);
                startSweep(true);
            }
            if (remove || keep) removalToConfirm.clear();
        } else {
            if (ImGui::SmallButton("Remove")) removalToConfirm = leftover.folder;
            UIProbe::item(probe + ".remove");
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::Separator();
}

void ProjectHubWindow::drawProjects() {
    ImGui::TextUnformatted("Projects");
    ImGui::SetNextItemWidth(Theme::dpi(260.0f));
    ImGui::InputTextWithHint("##hubSearch", ICON_FA_MAGNIFYING_GLASS " Search", search.data(), search.size());
    ImGui::SameLine();
    static const char* const kSorts[] = {"Last Opened", "Name"};
    int sortIndex = (int)sort;
    ImGui::SetNextItemWidth(Theme::dpi(130.0f));
    if (ImGui::Combo("##hubSort", &sortIndex, kSorts, 2)) sort = (hub::recent::Sort)sortIndex;
    if (!projectsMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", projectsMessage.c_str());
    ImGui::Separator();
    drawLeftovers();

    std::vector<hub::RecentProject> records = AppSettings::getRecentProjectRecords();
    const std::vector<hub::RecentProject> rows = hub::recent::listed(records, sort, search.data());
    if (rows.empty()) {
        ImGui::TextDisabled(records.empty() ? "No recent projects: make one in New Project, or Open or Import one." : "No project matches.");
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const float thumbWidth = Theme::dpi(160.0f), thumbHeight = Theme::dpi(90.0f);
    bool changed = false;
    for (const hub::RecentProject& project : rows) {
        std::error_code ec;
        const bool missing = !fs::exists(project.path / "project.yaml", ec);
        ImGui::PushID(project.path.generic_string().c_str());
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                                               ImVec2(0, thumbHeight + ImGui::GetStyle().ItemSpacing.y));
        const bool open = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (ImGui::BeginPopupContextItem("##projectMenu")) {
            if (ImGui::MenuItem(
#ifdef __APPLE__
                    "Show in Finder"
#else
                    "Show in Explorer"
#endif
                    , nullptr, false, !missing)) {
                std::string error;
                if (!Platform::openWithSystemDefault(FileUtils::pathToUtf8(project.path), &error)) projectsMessage = error;
            }
            if (ImGui::MenuItem("Open Without Last Scene", nullptr, false, !missing) && actions.openProject) actions.openProject(project.path, true);
            if (ImGui::MenuItem(project.pinned ? "Unpin" : "Pin")) {
                hub::recent::setPinned(records, project.path, !project.pinned);
                changed = true;
            }
            if (ImGui::MenuItem("Remove from List")) {
                hub::recent::remove(records, project.path);
                changed = true;
            }
            ImGui::EndPopup();
        }
        // the row's content over the selectable
        ImGui::SetCursorScreenPos(rowMin);
        if (missing) ImGui::BeginDisabled();
        const fs::path thumbFile = project.thumbnail.empty() ? project.path / ".jasperengine" / "user" / "hub-thumbnail.png" : project.thumbnail;
        Texture* texture = missing ? nullptr : thumbnail(fileKey(thumbFile), thumbFile);
        if (texture && texture->getRender()) {
            ImGui::Image(Backend::getImGuiTexture(texture->getRender()), ImVec2(thumbWidth, thumbHeight));
        } else {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + thumbWidth, at.y + thumbHeight), IM_COL32(45, 45, 50, 255), 4.0f);
            ImGui::Dummy(ImVec2(thumbWidth, thumbHeight));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s%s", project.pinned ? ICON_FA_THUMBTACK " " : "", project.name.c_str());
        ImGui::TextDisabled("%s", FileUtils::pathToUtf8(project.path).c_str());
        ImGui::TextDisabled("%s", missing ? "Missing: moved or deleted" : hub::recent::openedAgo(project.lastOpened, now).c_str());
        ImGui::EndGroup();
        if (missing) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Locate...")) {
                const std::string picked = FileDialogs::openFileDialog(FileUtils::pathToUtf8(project.path.parent_path()), FILE_DIALOG_ALL, true);
                if (!picked.empty()) {
                    const fs::path dir = FileUtils::pathFromUtf8(picked);
                    if (fs::exists(dir / "project.yaml", ec)) {
                        hub::recent::relocate(records, project.path, dir);
                        changed = true;
                    } else {
                        projectsMessage = "Not a JasperEngine project: " + picked;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove from List")) {
                hub::recent::remove(records, project.path);
                changed = true;
            }
        } else {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - Theme::dpi(60.0f));
            if (ImGui::SmallButton(project.pinned ? ICON_FA_THUMBTACK "##unpin" : ICON_FA_THUMBTACK "##pin")) {
                hub::recent::setPinned(records, project.path, !project.pinned);
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(project.pinned ? "Unpin" : "Pin to the top");
        }
        ImGui::PopID();
        if (open && !missing && actions.openProject) actions.openProject(project.path, false);
        if (changed) break;
    }
    if (changed) AppSettings::setRecentProjectRecords(records);
}

void ProjectHubWindow::drawNewProject() {
    if (!templatesLoaded) {
        std::vector<std::string> errors;
        templateList = hub::templates::all(hub::templates::userDirectory(AppSettings::getConfigDirectory()), &errors);
        templatesLoaded = true;
        createMessage = errors.empty() ? std::string() : errors.front();
        if (!hub::templates::find(templateList, selectedTemplate) && !templateList.empty()) selectedTemplate = templateList.front().id;
    }
    ImGui::TextUnformatted("New Project");
    ImGui::Separator();
    const float cardWidth = Theme::dpi(kCardWidth), cardHeight = Theme::dpi(kCardHeight);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, (int)((ImGui::GetContentRegionAvail().x + spacing) / (cardWidth + spacing)));
    for (int section = 0; section < 2; section++) {
        const bool builtIn = section == 0;
        const bool any = std::any_of(templateList.begin(), templateList.end(), [&](const hub::ProjectTemplate& t) { return t.builtIn == builtIn; });
        if (!any && builtIn) continue;
        ImGui::SeparatorText(builtIn ? "Built-in" : "My Templates");
        if (!any) {
            ImGui::TextDisabled("File > Save Project as Template... adds the open project here.");
            continue;
        }
        int column = 0;
        for (const hub::ProjectTemplate& t : templateList) {
            if (t.builtIn != builtIn) continue;
            if (column > 0) ImGui::SameLine();
            ImGui::PushID(t.id.c_str());
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float textHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
            if (ImGui::InvisibleButton("##card", ImVec2(cardWidth, cardHeight + textHeight))) selectedTemplate = t.id;
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 max(min.x + cardWidth, min.y + cardHeight + textHeight);
            const bool selected = selectedTemplate == t.id;
            drawList->AddRectFilled(min, max, selected ? IM_COL32(60, 70, 110, 255) : (hovered ? IM_COL32(50, 50, 58, 255) : IM_COL32(38, 38, 44, 255)), 6.0f);
            Texture* texture = thumbnailFromBytes("template:" + t.id + (t.builtIn ? ":builtin" : ":user"), t.thumbnailPng);
            if (texture && texture->getRender()) {
                drawList->AddImage(Backend::getImGuiTexture(texture->getRender()), min, ImVec2(max.x, min.y + cardHeight));
            }
            // the accent bar
            drawList->AddRectFilled(ImVec2(min.x, min.y + cardHeight - Theme::dpi(4.0f)), ImVec2(max.x, min.y + cardHeight), accent(t.accentColor));
            drawList->AddText(ImVec2(min.x + 6.0f, min.y + cardHeight + 2.0f), IM_COL32(240, 240, 240, 255), t.name.c_str());
            drawList->AddText(nullptr, 0.0f, ImVec2(min.x + 6.0f, min.y + cardHeight + 2.0f + ImGui::GetTextLineHeight()), IM_COL32(170, 170, 170, 255),
                              t.description.c_str(), nullptr, cardWidth - 12.0f);
            if (selected) drawList->AddRect(min, max, IM_COL32(120, 160, 255, 255), 6.0f, 0, 2.0f);
            if (hovered && !t.description.empty()) ImGui::SetTooltip("%s", t.description.c_str());
            ImGui::PopID();
            column = (column + 1) % columns;
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Project Name");
    ImGui::SetNextItemWidth(Theme::dpi(320.0f));
    ImGui::InputText("##hubProjectName", projectName.data(), projectName.size());
    ImGui::TextUnformatted("Location");
    ImGui::SetNextItemWidth(Theme::dpi(420.0f));
    ImGui::InputText("##hubLocation", location.data(), location.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...##location")) {
        const std::string picked = FileDialogs::openFileDialog(location.data(), FILE_DIALOG_ALL, true);
        if (!picked.empty()) copyPath(location, FileUtils::pathFromUtf8(picked));
    }
    const fs::path parent = FileUtils::pathFromUtf8(location.data());
    const std::string problem = createProblem(projectName.data(), parent);
    const hub::ProjectTemplate* chosen = hub::templates::find(templateList, selectedTemplate);
    if (!problem.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", problem.c_str());
    else ImGui::TextDisabled("%s", FileUtils::pathToUtf8(parent / hub::folderName(projectName.data())).c_str());
    ImGui::BeginDisabled(!problem.empty() || !chosen);
    if (ImGui::Button("Create", ImVec2(Theme::dpi(120.0f), 0)) && chosen && actions.createProject) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        AppSettings::setHubLastLocation(parent);
        actions.createProject(*chosen, projectName.data(), parent);
    }
    ImGui::EndDisabled();
    if (!createMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", createMessage.c_str());
}

void ProjectHubWindow::openFolder(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir / "project.yaml", ec)) {
        openMessage = "Not a JasperEngine project: " + FileUtils::pathToUtf8(dir) + " has no project.yaml.";
        return;
    }
    openMessage.clear();
    if (actions.openProject) actions.openProject(dir, false);
}

void ProjectHubWindow::drawOpen() {
    ImGui::TextUnformatted("Open");
    ImGui::Separator();
    ImGui::TextWrapped("A JasperEngine project is a folder with a project.yaml.");
    if (ImGui::Button("Open Folder...", ImVec2(Theme::dpi(160.0f), 0))) {
        const std::string picked = FileDialogs::openFileDialog(FileUtils::pathToUtf8(defaultLocation()), FILE_DIALOG_ALL, true);
        if (!picked.empty()) openFolder(FileUtils::pathFromUtf8(picked));
    }
    if (!openMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", openMessage.c_str());
}

void ProjectHubWindow::drawImport() {
    ImGui::TextUnformatted("Import");
    ImGui::Separator();
    drawLeftovers();
    const bool running = import.running();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("From Zip...")) {
        const std::string picked = FileDialogs::openFileDialogFiltered(FileUtils::pathToUtf8(defaultLocation()), "Zip archives", "zip");
        if (!picked.empty()) std::snprintf(importSource.data(), importSource.size(), "%s", picked.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("or a Git URL (https://, ssh://, user@host:path):");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##importSource", "https://github.com/team/game.git or a .zip", importSource.data(), importSource.size());
    ImGui::TextUnformatted("Into");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(420.0f));
    ImGui::InputText("##importLocation", importLocation.data(), importLocation.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...##importLocation")) {
        const std::string picked = FileDialogs::openFileDialog(importLocation.data(), FILE_DIALOG_ALL, true);
        if (!picked.empty()) copyPath(importLocation, FileUtils::pathFromUtf8(picked));
    }
    const std::string source = importSource.data();
    std::string why;
    const bool sourceOk = hub::ProjectImporter::isZipSource(source) || hub::isAcceptedGitUrl(source, why);
    if (!source.empty() && !sourceOk) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", why.c_str());
    ImGui::BeginDisabled(source.empty() || !sourceOk);
    if (ImGui::Button("Import", ImVec2(Theme::dpi(120.0f), 0))) {
        const fs::path parent = FileUtils::pathFromUtf8(importLocation.data());
        std::error_code ec;
        fs::create_directories(parent, ec);
        AppSettings::setHubLastLocation(parent);
        std::string error;
        importOpened = false;
        if (!import.start(source, parent, AppSettings::getVersionControlSettings().gitPath, error)) openMessage = error;
        startSweep(true);   // what stays in that folder shows here
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (running || import.done()) {
        ImGui::Separator();
        ImGui::ProgressBar(import.done() ? 1.0f : import.progress(), ImVec2(-FLT_MIN, 0));
        if (running) {
            if (ImGui::Button("Cancel")) import.cancel();
        } else if (!import.succeeded()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Import failed: %s", import.error().c_str());
        }
        ImGui::BeginChild("##importLog", ImVec2(0, Theme::dpi(160.0f)), ImGuiChildFlags_Borders);
        for (const std::string& line : import.logTail(12)) ImGui::TextUnformatted(line.c_str());
        ImGui::EndChild();
        if (import.succeeded() && !importOpened) {
            importOpened = true;
            if (actions.openProject) actions.openProject(import.projectDir(), false);
        }
    }
}
