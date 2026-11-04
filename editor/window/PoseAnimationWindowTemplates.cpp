// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pose Animation window's Templates tab (spec: pose animation phase 6, section 4): a grid
// of template cards (the middle pose on the bound character, name, pose count and loop, the
// description as tooltip, a "user" badge for project templates) with a category filter and a
// search field. A click creates a new animation from the template (save dialog), the context
// menu replaces or appends the open animation's poses, and shows or deletes project templates;
// "Save Current as Template..." saves the open animation as a project template. The store is
// PoseTemplates; every change is one project undo step.

#include "PoseAnimationWindow.h"
#include "util/FileUtils.h"

#include "App.h"
#include "Backend.h"
#include "Scene.h"
#include "Theme.h"
#include "component/ModelComponent.h"
#include "external/IconsFontAwesome6.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseSampler.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"
#include "window/EditorModal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    // card geometry: a little smaller than the strip's cards, same 88:116 thumbnails
    constexpr float CARD_WIDTH = 104.0f;
    constexpr float THUMB_HEIGHT = 105.0f;
    constexpr float THUMB_WIDTH = THUMB_HEIGHT * 88.0f / 116.0f;

    ImU32 color(const ImVec4& c, float alpha = 1.0f){
        return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
    }

    void caption(const char* text){
        ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::SubtleText);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }

    std::string trimmed(const std::string& text){
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    bool sameName(const std::string& a, const std::string& b){
        const std::string x = trimmed(a), y = trimmed(b);
        return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [](char p, char q){
            return std::tolower(static_cast<unsigned char>(p)) == std::tolower(static_cast<unsigned char>(q));
        });
    }

    // `text` cut to `width` pixels with "..", whole UTF-8 characters
    std::string fitText(const std::string& text, float width){
        if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
        std::string cut = text;
        while (!cut.empty()){
            cut.pop_back();
            while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
            if (ImGui::CalcTextSize((cut + "..").c_str()).x <= width) return cut + "..";
        }
        return "..";
    }

    // the project's template files (names, sizes, times): a change re-reads the list
    std::string templateFilesStamp(const fs::path& directory){
        std::vector<std::string> parts;
        std::error_code ec;
        if (!fs::is_directory(directory, ec)) return "";
        for (fs::directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec)){
            std::error_code fileError;
            const auto size = fs::file_size(it->path(), fileError);
            const auto time = fs::last_write_time(it->path(), fileError);
            parts.push_back(editor::FileUtils::pathToUtf8(it->path().filename()) + ":" + std::to_string(fileError ? 0 : size) + ":" +
                            std::to_string(fileError ? 0 : static_cast<long long>(time.time_since_epoch().count())));
        }
        std::sort(parts.begin(), parts.end());
        std::string stamp = "dir";
        for (const std::string& part : parts) stamp += "|" + part;
        return stamp;
    }

    void copyTo(char* buffer, size_t size, const std::string& text){
        std::snprintf(buffer, size, "%s", text.c_str());
    }

}

// ---- the list ----

void editor::PoseAnimationWindow::refreshTemplates(bool force){
    const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    if (!force && templatesLoaded && now - templateCheckTime < 0.5) return;
    templateCheckTime = now;
    const fs::path projectPath = project->getProjectPath();
    const std::string stamp = templateFilesStamp(PoseTemplates::userDirectory(projectPath));
    if (!force && templatesLoaded && stamp == templateStamp) return;
    templateStamp = stamp;
    templatesLoaded = true;
    std::vector<TemplateEntry> entries;
    for (const PoseTemplateInfo& info : PoseTemplates::list(projectPath)){
        TemplateEntry entry;
        if (!info.user){
            const auto& builtins = PoseTemplates::builtins();
            auto it = std::find_if(builtins.begin(), builtins.end(), [&info](const PoseTemplate& t){ return sameName(t.info.name, info.name); });
            if (it == builtins.end()) continue;
            entry.tmpl = *it;
        }else if (!PoseTemplates::load(projectPath, info.name, entry.tmpl, nullptr)){
            continue;
        }
        entries.push_back(std::move(entry));
    }
    templateEntries = std::move(entries);
}

editor::PoseAnimationWindow::TemplateEntry* editor::PoseAnimationWindow::findTemplate(const std::string& name){
    // (not while the grid iterates templateEntries: a refresh replaces them)
    for (int attempt = 0; attempt < 2; attempt++){
        if (attempt == 1 || !templatesLoaded) refreshTemplates(true);
        for (TemplateEntry& entry : templateEntries){
            if (sameName(entry.tmpl.info.name, name)) return &entry;
        }
    }
    return nullptr;
}



// ---- actions ----

std::string editor::PoseAnimationWindow::templateAnimationPath(const std::string& name){
    if (!session.bound()) return "";
    const ModelComponent* model = session.scene()->findComponent<ModelComponent>(session.model());
    if (!model) return "";
    // the template's own spelling of its name (names are looked up without case)
    const TemplateEntry* entry = findTemplate(name);
    const fs::path root = project->getProjectPath();
    const fs::path folder = project->resolveAssetPath(model->filename).parent_path();
    std::string directory = ProjectChecks::projectRelative(root, folder);
    return poseui::templateAnimationPath(directory, entry ? entry->tmpl.info.name : trimmed(name), [&root](const std::string& path){
        std::error_code ec;
        return fs::exists(root / editor::FileUtils::pathFromUtf8(path), ec);
    });
}

bool editor::PoseAnimationWindow::createFromTemplate(const std::string& name, const std::string& path, std::string* error){
    auto fail = [error](const std::string& message){
        if (error) *error = message;
        return false;
    };
    if (!session.bound()){
        return fail("bind a character first (the Character field): the new animation previews on its model");
    }
    const ModelComponent* model = session.scene()->findComponent<ModelComponent>(session.model());
    if (!model){
        return fail("the character has no Model component");
    }
    PoseTemplate tmpl;
    if (!PoseTemplates::load(project->getProjectPath(), name, tmpl, error)){
        return false;
    }
    std::string target = trimmed(path).empty() ? templateAnimationPath(tmpl.info.name) : trimmed(path);
    if (!Util::isPoseAnimationFile(target)) target += ".poseanim";   // any case: "Walk.PoseAnim" is one already
    const pose::PoseAnimationData initial = PoseTemplates::newAnimation(tmpl.data, model->filename);
    std::string failure;
    std::shared_ptr<PoseAnimationDocument> created =
        PoseAnimationDocument::create(project, target, initial, "Create pose animation from template", nullptr, &failure);
    if (!created){
        return fail(failure);
    }
    if (!openAnimation(created->path(), error)){
        return false;
    }
    selectTab("Pose");
    setStatus("Created " + created->path() + " from the template '" + tmpl.info.name + "'", false);
    return true;
}

bool editor::PoseAnimationWindow::applyTemplate(const std::string& name, PoseTemplateApply mode, std::string* error){
    auto fail = [error](const std::string& message){
        if (error) *error = message;
        return false;
    };
    const pose::PoseAnimationData* animation = data();
    if (!animation){
        return fail("open a pose animation first");
    }
    PoseTemplate tmpl;
    if (!PoseTemplates::load(project->getProjectPath(), name, tmpl, error)){
        return false;
    }
    const poseui::Edit edit = poseui::applyTemplate(*animation, tmpl.data, mode, selection(), session.primary());
    if (!edit.changed){
        return fail("the template '" + tmpl.info.name + "' has no poses");
    }
    const bool replace = mode == PoseTemplateApply::Replace;
    std::string failure;
    if (!session.commitData(edit.data, replace ? "Replace poses from template" : "Append poses from template", "",
                            &edit.selection, edit.primary, &failure, false)){
        setStatus(failure, true);
        return fail(failure);
    }
    scrollToPrimary = true;
    if (session.bound() && session.mode() == PoseEditSession::Mode::Idle) session.startPosing();
    setStatus(std::string(replace ? "Replaced the poses with" : "Appended") + " the template '" + tmpl.info.name + "' (" +
              std::to_string(tmpl.data.poses.size()) + (tmpl.data.poses.size() == 1 ? " pose)" : " poses)"), false);
    return true;
}

bool editor::PoseAnimationWindow::saveAsTemplate(const std::string& name, const std::string& description, const std::string& category,
                                                 std::string* error){
    const pose::PoseAnimationData* animation = data();
    if (!animation || animation->poses.empty()){
        if (error) *error = "open a pose animation with poses first";
        return false;
    }
    fs::path file;
    const std::string group = trimmed(category).empty() ? std::string("User") : trimmed(category);
    if (!PoseTemplates::save(*project->getProjectCommandHistory(), project->getProjectPath(), name, description, group,
                             *animation, error, &file)){
        return false;
    }
    refreshTemplates(true);
    setStatus("Saved the template '" + trimmed(name) + "' (" + ProjectChecks::projectRelative(project->getProjectPath(), file) + ")", false);
    return true;
}

bool editor::PoseAnimationWindow::deleteTemplate(const std::string& name, std::string* error){
    if (!PoseTemplates::remove(*project->getProjectCommandHistory(), project->getProjectPath(), name, error)){
        return false;
    }
    refreshTemplates(true);
    setStatus("Deleted the template '" + trimmed(name) + "' (Ctrl/Cmd+Z brings it back)", false);
    return true;
}

bool editor::PoseAnimationWindow::showTemplateInProject(const std::string& name, std::string* error){
    const TemplateEntry* entry = findTemplate(name);
    if (!entry || !entry->tmpl.info.user){
        if (error) *error = entry ? "'" + name + "' is a built-in template (it has no project file)" : "no template named '" + name + "'";
        return false;
    }
    ProjectWindow* projectWindow = Backend::getApp().getProjectWindow();
    if (!projectWindow){
        if (error) *error = "the Project window is not available";
        return false;
    }
    projectWindow->setOpen(true);
    projectWindow->revealAsset(entry->tmpl.info.file);
    return true;
}

void editor::PoseAnimationWindow::openTemplateCreateDialog(const std::string& name){
    templateCreateName = name;
    copyTo(templateCreatePath, sizeof(templateCreatePath), templateAnimationPath(name));
    templateCreateError.clear();
    templateCreateRequested = true;
}

// ---- the tab ----

void editor::PoseAnimationWindow::drawTemplatesTab(){
    refreshTemplates(false);
    TemplatesState shown;
    shown.shown = true;
    templatesDrawnFrame = ImGui::GetFrameCount();

    // filter row: category, search, then Save Current as Template... at the right
    std::vector<PoseTemplateInfo> infos;
    for (const TemplateEntry& entry : templateEntries) infos.push_back(entry.tmpl.info);
    const std::vector<std::string> categories = poseui::templateCategories(infos);
    if (!templateCategory.empty() &&
        std::none_of(categories.begin(), categories.end(), [this](const std::string& c){ return sameName(c, templateCategory); })){
        templateCategory.clear();   // its last template went away
    }
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    if (ImGui::BeginCombo("##TemplateCategory", templateCategory.empty() ? "All categories" : templateCategory.c_str())){
        if (ImGui::Selectable("All categories", templateCategory.empty())) templateCategory.clear();
        for (const std::string& category : categories){
            if (ImGui::Selectable(category.c_str(), sameName(category, templateCategory))) templateCategory = category;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Show the templates of one category");
    ImGui::SameLine();
    const char* saveLabel = ICON_FA_FLOPPY_DISK " Save Current as Template...";
    const float saveWidth = ImGui::CalcTextSize(saveLabel).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - saveWidth - ImGui::GetStyle().ItemSpacing.x,
                                       Theme::dpi(80.0f), Theme::dpi(200.0f)));
    ImGui::InputTextWithHint("##TemplateSearch", ICON_FA_MAGNIFYING_GLASS " search", templateSearch, sizeof(templateSearch));
    ImGui::SetItemTooltip("Name, category or description words");
    const pose::PoseAnimationData* animation = data();
    const bool canSave = animation && !animation->poses.empty();
    if (ImGui::GetContentRegionAvail().x > saveWidth + ImGui::GetStyle().ItemSpacing.x){
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - saveWidth));
    }
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button(saveLabel)){
        const auto& doc = session.document();
        copyTo(templateSaveName, sizeof(templateSaveName), doc ? editor::FileUtils::pathToUtf8(editor::FileUtils::pathFromUtf8(doc->path()).stem()) : std::string());
        templateSaveDescription[0] = '\0';
        copyTo(templateSaveCategory, sizeof(templateSaveCategory), "User");
        templateSaveError.clear();
        templateSaveRequested = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(canSave ? "Save the open animation as a project template (pose-templates/, shared with the project)"
                                  : "Open a pose animation with poses to save it as a template");
    shown.category = templateCategory;
    shown.search = templateSearch;

    if (!session.bound()){
        shown.hint = true;
        caption(ICON_FA_CIRCLE_INFO " Bind a character to preview templates");
    }

    // the grid
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float cardWidth = Theme::dpi(CARD_WIDTH);
    const float line = ImGui::GetTextLineHeight();
    const float cardHeight = Theme::dpi(6.0f) + Theme::dpi(THUMB_HEIGHT) + Theme::dpi(4.0f) + line * 2 + Theme::dpi(6.0f);
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cardWidth + spacing)));
    // the middle poses follow the bound rig
    const std::shared_ptr<pose::PoseRig>& rig = session.rig();
    if (templateRig.lock() != rig){
        templateRig = rig;
        for (TemplateEntry& entry : templateEntries) entry.posed = false;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    int count = 0;
    for (TemplateEntry& entry : templateEntries){
        const PoseTemplateInfo& info = entry.tmpl.info;
        if (!poseui::templateMatches(info, templateCategory, templateSearch)) continue;
        const ImVec2 pos(origin.x + (count % columns) * (cardWidth + spacing), origin.y + (count / columns) * (cardHeight + spacing));
        if (ImGui::IsRectVisible(pos, ImVec2(pos.x + cardWidth, pos.y + cardHeight))) shown.visible++;
        if (drawTemplateCard(entry, pos, cardWidth, cardHeight)) shown.thumbnails++;
        shown.cards.push_back(info.name);
        if (info.user) shown.user.push_back(info.name);
        count++;
    }
    const int rows = (count + columns - 1) / columns;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(columns * (cardWidth + spacing), std::max(0, rows) * (cardHeight + spacing)));
    if (count == 0){
        caption(templateEntries.empty() ? "No templates." : "No template matches the filter.");
    }
    templatesShown = shown;
    if (!templateClicked.empty()){
        const std::string name = templateClicked;
        templateClicked.clear();
        if (session.bound()){
            openTemplateCreateDialog(name);
        }else{
            setStatus("Bind a character first (the Character field): a new animation previews on its model", true);
        }
    }
    drawTemplateMenu();
}

bool editor::PoseAnimationWindow::drawTemplateCard(TemplateEntry& entry, const ImVec2& pos, float cardWidth, float cardHeight){
    const PoseTemplateInfo& info = entry.tmpl.info;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID((std::string(info.user ? "user:" : "builtin:") + info.name).c_str());
    const bool clicked = ImGui::InvisibleButton("##template", ImVec2(cardWidth, cardHeight));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        templateMenu = info.name;
        // opened by drawTemplateMenu, in the ID scope its BeginPopup uses (not this card's)
        templateMenuRequested = true;
    }
    if (hovered){
        ImGui::SetItemTooltip("%s (%s, %s)\n%s\n\nClick: a new animation from this template\nRight-click: replace or append the poses of "
                              "the open animation%s", info.name.c_str(), info.category.empty() ? "no category" : info.category.c_str(),
                              info.user ? "project template" : "built-in", info.description.empty() ? "(no description)" : info.description.c_str(),
                              info.user ? ", show in Project, delete" : "");
    }
    ImGui::PopID();
    // handled after the grid (the save dialog opens at the window level)
    if (clicked) templateClicked = info.name;

    const ImVec2 cardMin(pos.x + Theme::dpi(2.0f), pos.y + Theme::dpi(2.0f));
    const ImVec2 cardMax(pos.x + cardWidth - Theme::dpi(2.0f), pos.y + cardHeight - Theme::dpi(2.0f));
    draw->AddRectFilled(cardMin, cardMax, color(hovered ? Theme::Colors::FileCardBackgroundHovered : Theme::Colors::FileCardBackground), 4.0f);
    if (hovered){
        draw->AddRect(cardMin, cardMax, color(Theme::Colors::PrefabText, 0.8f), 4.0f, 0, Theme::dpi(1.5f));
    }

    // the thumbnail: the template's middle pose on the bound character
    const ImVec2 thumbMin(pos.x + (cardWidth - Theme::dpi(THUMB_WIDTH)) * 0.5f, pos.y + Theme::dpi(6.0f));
    const ImVec2 thumbMax(thumbMin.x + Theme::dpi(THUMB_WIDTH), thumbMin.y + Theme::dpi(THUMB_HEIGHT));
    TextureRender* texture = nullptr;
    const std::shared_ptr<pose::PoseRig>& rig = session.rig();
    if (session.bound() && rig && ImGui::IsRectVisible(thumbMin, thumbMax)){
        if (!entry.posed){
            entry.posed = true;
            entry.hash = 0;
            const std::vector<pose::HumanPose> keys = pose::realiseKeys(entry.tmpl.data, *rig, rig->skin());
            if (!keys.empty()){
                entry.middle = keys[keys.size() / 2];
                entry.hash = PoseThumbnails::hashPose(entry.middle);
            }
        }
        if (entry.hash != 0){
            texture = thumbnails.get(entry.hash, entry.middle, static_cast<int>(std::lround(Theme::dpi(THUMB_WIDTH))),
                                     static_cast<int>(std::lround(Theme::dpi(THUMB_HEIGHT))));
        }
    }
    draw->AddRectFilled(thumbMin, thumbMax, IM_COL32(48, 48, 51, 255), 3.0f);
    if (texture){
        draw->AddImage(Backend::getImGuiTexture(texture), thumbMin, thumbMax);
    }else{
        const char* icon = ICON_FA_PERSON_WALKING;
        const ImVec2 size = ImGui::CalcTextSize(icon);
        draw->AddText(ImVec2((thumbMin.x + thumbMax.x - size.x) * 0.5f, (thumbMin.y + thumbMax.y - size.y) * 0.5f),
                      color(Theme::Colors::SubtleText, 0.6f), icon);
    }
    if (info.user){
        // the "user" badge: a project template
        const char* badge = "user";
        const ImVec2 size = ImGui::CalcTextSize(badge);
        const ImVec2 badgeMin(thumbMin.x + Theme::dpi(3.0f), thumbMin.y + Theme::dpi(3.0f));
        draw->AddRectFilled(badgeMin, ImVec2(badgeMin.x + size.x + Theme::dpi(8.0f), badgeMin.y + size.y + Theme::dpi(2.0f)),
                            color(Theme::Colors::Selection, 0.95f), 3.0f);
        draw->AddText(ImVec2(badgeMin.x + Theme::dpi(4.0f), badgeMin.y + Theme::dpi(1.0f)), IM_COL32(235, 235, 235, 255), badge);
    }

    // name, then "N poses · loop"
    const float textX = pos.x + Theme::dpi(7.0f);
    const float maxText = cardWidth - Theme::dpi(14.0f);
    const float nameY = thumbMax.y + Theme::dpi(3.0f);
    draw->AddText(ImVec2(textX, nameY), color(ImGui::GetStyle().Colors[ImGuiCol_Text]), fitText(info.name, maxText).c_str());
    const std::string summary = fitText(poseui::templateSummary(info.poseCount, info.loop), maxText);
    draw->AddText(ImVec2(textX, nameY + ImGui::GetTextLineHeight()), color(Theme::Colors::SubtleText), summary.c_str());
    return texture != nullptr;
}

void editor::PoseAnimationWindow::drawTemplateMenu(){
    if (templateMenuRequested){
        templateMenuRequested = false;
        ImGui::OpenPopup("##PoseTemplateMenu");
    }
    if (!ImGui::BeginPopup("##PoseTemplateMenu")) return;
    menuShownFrame = ImGui::GetFrameCount();
    menuShown = "template:" + templateMenu;
    const TemplateEntry* entry = nullptr;
    for (const TemplateEntry& candidate : templateEntries){
        if (sameName(candidate.tmpl.info.name, templateMenu)) entry = &candidate;
    }
    if (!entry){
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const std::string name = entry->tmpl.info.name;
    const bool user = entry->tmpl.info.user;
    ImGui::TextColored(Theme::Colors::SubtleText, "%s", name.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("New Animation from Template...", nullptr, false, session.bound())) openTemplateCreateDialog(name);
    const bool open = data() != nullptr;
    std::string error;
    if (ImGui::MenuItem("Replace Poses in Current Animation", nullptr, false, open)){
        if (!applyTemplate(name, PoseTemplateApply::Replace, &error)) setStatus(error, true);
    }
    ImGui::SetItemTooltip("The template's poses and feel replace the open animation's (its preview model stays; one undo step)");
    if (ImGui::MenuItem("Append Poses", nullptr, false, open)){
        if (!applyTemplate(name, PoseTemplateApply::Append, &error)) setStatus(error, true);
    }
    ImGui::SetItemTooltip("The template's poses are added after the open animation's (its feel stays; one undo step)");
    if (user){
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Project")){
            if (!showTemplateInProject(name, &error)) setStatus(error, true);
        }
        if (ImGui::MenuItem("Delete...")){
            templateDeleteName = name;
            templateDeleteError.clear();
            templateDeleteRequested = true;
        }
    }
    ImGui::EndPopup();
}

// ---- dialogs (window level) ----

void editor::PoseAnimationWindow::drawTemplatePopups(){
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

    // a card click: where the new animation goes (the save dialog)
    if (templateCreateRequested){
        templateCreateRequested = false;
        ImGui::OpenPopup("New Pose Animation from Template###PoseTemplateCreate");
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin("New Pose Animation from Template###PoseTemplateCreate", nullptr, flags)){
        EditorModal::message("New Pose Animation from the template " + templateCreateName + ": path is where to save it (project folder).");
        const TemplateEntry* entry = nullptr;
        for (const TemplateEntry& candidate : templateEntries){
            if (sameName(candidate.tmpl.info.name, templateCreateName)) entry = &candidate;
        }
        ImGui::Text("Template: %s", templateCreateName.c_str());
        if (entry && !entry->tmpl.info.description.empty()){
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Theme::dpi(420.0f));
            ImGui::TextColored(Theme::Colors::SubtleText, "%s", entry->tmpl.info.description.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Save as (project folder)");
        ImGui::SetNextItemWidth(Theme::dpi(420.0f));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        EditorModal::pathField("path", templateCreatePath, sizeof(templateCreatePath));
        const bool enter = ImGui::InputText("##PoseTemplatePath", templateCreatePath, sizeof(templateCreatePath),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        std::string target = trimmed(templateCreatePath);
        if (!target.empty() && !Util::isPoseAnimationFile(target)) target += ".poseanim";
        std::error_code ec;
        const fs::path absolute = editor::FileUtils::pathFromUtf8(target).is_absolute() ? editor::FileUtils::pathFromUtf8(target) : project->getProjectPath() / editor::FileUtils::pathFromUtf8(target);
        const bool taken = !target.empty() && fs::exists(absolute, ec);
        EditorModal::error(!templateCreateError.empty() ? templateCreateError : taken ? target + " exists: pick another name" :
                           target.empty() ? std::string("The path is empty.") : std::string());
        if (taken){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s exists: pick another name", target.c_str());
        }else if (session.bound()){
            const ModelComponent* model = session.scene()->findComponent<ModelComponent>(session.model());
            ImGui::TextColored(Theme::Colors::SubtleText, "Previews on %s", model ? model->filename.c_str() : "the character's model");
        }
        if (!templateCreateError.empty()){
            ImGui::TextColored(Theme::Colors::ErrorText, "%s", templateCreateError.c_str());
        }
        ImGui::Separator();
        ImGui::BeginDisabled(target.empty() || taken);
        const bool create = EditorModal::button("Create", "ok", ImVec2(Theme::dpi(120.0f), 0)) || (enter && !target.empty() && !taken);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = EditorModal::button("Cancel", "cancel", ImVec2(Theme::dpi(120.0f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (create){
            std::string error;
            if (createFromTemplate(templateCreateName, target, &error)){
                ImGui::CloseCurrentPopup();
            }else{
                templateCreateError = error;
            }
        }else if (cancel){
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Delete...: a confirmation
    if (templateDeleteRequested){
        templateDeleteRequested = false;
        ImGui::OpenPopup("Delete Template###PoseTemplateDelete");
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin("Delete Template###PoseTemplateDelete", nullptr, flags)){
        EditorModal::message("Delete the project template '" + templateDeleteName + "'? (Ctrl/Cmd+Z brings it back)");
        EditorModal::error(templateDeleteError);
        std::string file;
        for (const TemplateEntry& candidate : templateEntries){
            if (sameName(candidate.tmpl.info.name, templateDeleteName)){
                file = ProjectChecks::projectRelative(project->getProjectPath(), candidate.tmpl.info.file);
            }
        }
        ImGui::Text("Delete the project template '%s'?", templateDeleteName.c_str());
        if (!file.empty()) ImGui::TextColored(Theme::Colors::SubtleText, "%s (Ctrl/Cmd+Z brings it back)", file.c_str());
        if (!templateDeleteError.empty()) ImGui::TextColored(Theme::Colors::ErrorText, "%s", templateDeleteError.c_str());
        ImGui::Separator();
        if (EditorModal::button("Delete", "yes", ImVec2(Theme::dpi(120.0f), 0))){
            std::string error;
            if (deleteTemplate(templateDeleteName, &error)) ImGui::CloseCurrentPopup();
            else templateDeleteError = error;
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(Theme::dpi(120.0f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)){
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Save Current as Template...: name, description, category
    if (templateSaveRequested){
        templateSaveRequested = false;
        ImGui::OpenPopup("Save as Template###PoseTemplateSave");
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin("Save as Template###PoseTemplateSave", nullptr, flags)){
        EditorModal::message("Save this animation as a project template (pose-templates/, one undo step).");
        const float labelWidth = Theme::dpi(90.0f);
        const float fieldWidth = Theme::dpi(320.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        EditorModal::inputText("##PoseTemplateName", "name", templateSaveName, sizeof(templateSaveName));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Description");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(fieldWidth);
        EditorModal::inputTextWithHint("##PoseTemplateDescription", "description", "shown as the card's tooltip", templateSaveDescription,
                                       sizeof(templateSaveDescription));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Category");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(fieldWidth - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        EditorModal::inputTextWithHint("##PoseTemplateCategory", "category", "User", templateSaveCategory, sizeof(templateSaveCategory));
        ImGui::SameLine();
        std::vector<PoseTemplateInfo> infos;
        for (const TemplateEntry& entry : templateEntries) infos.push_back(entry.tmpl.info);
        if (ImGui::BeginCombo("##PoseTemplateCategories", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)){
            for (const std::string& category : poseui::templateCategories(infos)){
                if (ImGui::Selectable(category.c_str())) copyTo(templateSaveCategory, sizeof(templateSaveCategory), category);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The categories in use");
        // what a save with this name does
        std::string note;
        std::string nameError;
        const bool valid = PoseTemplates::validName(templateSaveName, &nameError);
        for (const TemplateEntry& entry : templateEntries){
            if (!valid || !sameName(entry.tmpl.info.name, templateSaveName)) continue;
            note = entry.tmpl.info.user ? "Replaces the project template '" + entry.tmpl.info.name + "'"
                                        : "Shadows the built-in template '" + entry.tmpl.info.name + "' in this project";
        }
        EditorModal::error(!templateSaveError.empty() ? templateSaveError : valid ? std::string() : nameError);
        if (!valid){
            ImGui::TextColored(Theme::Colors::WarningText, "%s", nameError.c_str());
        }else if (!note.empty()){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", note.c_str());
        }else{
            ImGui::TextColored(Theme::Colors::SubtleText, "Saved to pose-templates/ (shared with the project; one undo step)");
        }
        if (!templateSaveError.empty()) ImGui::TextColored(Theme::Colors::ErrorText, "%s", templateSaveError.c_str());
        ImGui::Separator();
        ImGui::BeginDisabled(!valid);
        const bool save = EditorModal::button("Save", "ok", ImVec2(Theme::dpi(120.0f), 0));
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = EditorModal::button("Cancel", "cancel", ImVec2(Theme::dpi(120.0f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (save){
            std::string error;
            if (saveAsTemplate(templateSaveName, templateSaveDescription, templateSaveCategory, &error)) ImGui::CloseCurrentPopup();
            else templateSaveError = error;
        }else if (cancel){
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
