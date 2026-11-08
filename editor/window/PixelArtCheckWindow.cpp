// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelArtCheckWindow.h"

#include "Project.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/PixelArtFix.h"
#include "pixelart/PixelArtInput.h"
#include "shortcuts/CommandCatalog.h"
#include "util/EntityPath.h"
#include "window/EditorModal.h"
#include "window/ProjectWindow.h"

#include "imgui.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace px = jasperengine::editor::pixelart;

namespace {

    constexpr std::chrono::milliseconds kImportSettle(1000);
    constexpr const char* kPreviewName = "Fix All###PixelArtFixAll";

    ImVec4 statusColor(px::RuleStatus status){
        switch (status){
            case px::RuleStatus::Fail: return Theme::Colors::ErrorText;
            case px::RuleStatus::Warn: return Theme::Colors::WarningText;
            case px::RuleStatus::Pass: default: return Theme::Colors::DisabledGreenText;
        }
    }

    const char* statusIcon(px::RuleStatus status){
        switch (status){
            case px::RuleStatus::Fail: return ICON_FA_CIRCLE_XMARK;
            case px::RuleStatus::Warn: return ICON_FA_TRIANGLE_EXCLAMATION;
            case px::RuleStatus::Pass: default: return ICON_FA_CIRCLE_CHECK;
        }
    }

    std::string targetText(const px::Finding& finding){
        const std::string kind = finding.target.value("kind", "");
        if (kind == "project")
            return "Project Settings";
        if (!finding.target.value("entity", "").empty())
            return finding.target.value("scene", finding.target.value("path", "")) + "  " + finding.target.value("entity", "");
        return finding.target.value("path", finding.target.value("scene", ""));
    }

    std::string changesText(const px::FixOption& option){
        std::string text = option.label;
        for (const px::FixChange& change : option.changes)
            text += "\n" + change.target + "  " + change.field + ": " + change.from.dump() + " -> " + change.to.dump();
        return text;
    }

}

PixelArtCheckWindow::PixelArtCheckWindow(Project* project, ProjectWindow* projectWindow): project(project), projectWindow(projectWindow){
}

void PixelArtCheckWindow::open(){
    windowOpen = true;
    focusRequested = true;
    if (!hasResult && !running.isValid())
        run();
}

bool PixelArtCheckWindow::takesKeys() const{
    return windowOpen && focused && !ImGui::GetIO().WantTextInput;
}

void PixelArtCheckWindow::resetForProjectSwitch(){
    hasResult = false;
    current = px::CheckResult();
    selectedKey.clear();
    pendingFocus.clear();
    notice.clear();
}

void PixelArtCheckWindow::bindCommands(CommandCatalog& catalog){
    auto bind = [&catalog, this](const char* id, std::function<void()> action, std::function<bool()> enabled){
        const CommandDef* existing = catalog.find(id);
        if (!existing)
            return;
        CommandDef def = *existing;
        def.run = [action](const CommandArgs&){ action(); };
        def.enabled = std::move(enabled);
        def.keyGate = [this](){ return takesKeys() && !ImGui::IsAnyItemActive(); };
        catalog.add(std::move(def));
    };
    bind("pixelArtCheck.run", [this](){ open(); run(); }, [](){ return true; });
    bind("pixelArtCheck.fixSelected", [this](){ fixFinding(selectedKey); }, [this](){
        const px::Finding* finding = hasResult ? current.find(selectedKey) : nullptr;
        return finding && !finding->options.empty();
    });
    bind("pixelArtCheck.fixAll", [this](){ open(); previewOpen = true; }, [this](){ return hasResult && fixAllChangeCount() > 0; });
}

void PixelArtCheckWindow::run(){
    if (running.isValid() && !running.isReady()){
        rerun = true;
        return;
    }
    px::Scope input;
    switch (scope){
        case Scope::OpenScenes:
            for (const SceneProject& scene : project->getScenes())
                if (scene.opened && scene.scene)
                    input.scenes.push_back(px::scenePathOf(*project, scene));
            input.paths = {};
            break;
        case Scope::Selection:
            input.selectionOnly = true;
            for (const SceneProject& scene : project->getScenes()){
                if (!scene.opened || !scene.scene)
                    continue;
                bool any = false;
                for (Entity entity : project->getSelectedEntities(scene.id)){
                    input.selection.push_back({scene.id, (uint32_t)entity});
                    any = true;
                }
                if (any)
                    input.scenes.push_back(px::scenePathOf(*project, scene));
            }
            if (input.scenes.empty())
                input.scenes.push_back("__none__");
            break;
        case Scope::BuildScenes:
        case Scope::Project:
            break;
    }
    std::vector<px::PixelProblem> errors;
    if (scope == Scope::Selection && input.scenes.size() == 1 && input.scenes[0] == "__none__"){
        hasResult = true;
        current = px::CheckResult();
        notice = "Nothing selected.";
        return;
    }
    auto data = std::make_shared<px::PixelArtCheckInput>(px::inputFromProject(*project, input, errors));
    notice = errors.empty() ? std::string() : errors.front().message;
    const std::vector<std::string> rules(ruleFilter.begin(), ruleFilter.end());
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "pixel-art-check-window";
    running = jobs::submit([data, rules](){ return std::make_shared<px::CheckResult>(px::run(*data, rules)); }, options);
}

void PixelArtCheckWindow::collectResult(){
    if (!running.isValid() || !running.isReady())
        return;
    std::shared_ptr<px::CheckResult> result;
    try{
        result = running.get();
    }catch (const std::exception& e){
        notice = std::string("The check failed: ") + e.what();
    }
    running = jobs::Task<std::shared_ptr<px::CheckResult>>();
    if (result){
        current = *result;
        hasResult = true;
        if (!pendingFocus.empty()){
            selectedKey = pendingFocus;
            pendingFocus.clear();
        }
    }
    if (rerun){
        rerun = false;
        run();
    }
}

void PixelArtCheckWindow::focusFinding(const std::string& key){
    pendingFocus = key;
    open();
    run();
}

void PixelArtCheckWindow::assetsChanged(){
    if (!windowOpen)
        return;
    assetsDirty = true;
    assetsChangedAt = std::chrono::steady_clock::now();
}

bool PixelArtCheckWindow::applyOptions(const std::vector<std::pair<std::string, px::FixOption>>& fixes, const std::string& stepName){
    px::FixPlan plan;
    plan.fixes = fixes;
    px::FixApplyReport report;
    std::string error;
    if (!px::applyFixPlan(*project, plan, stepName, report, error)){
        notice = "Nothing was fixed: " + error;
        return false;
    }
    notice.clear();
    run();
    return true;
}

bool PixelArtCheckWindow::fixFinding(const std::string& key, const std::string& option){
    const px::Finding* finding = hasResult ? current.find(key) : nullptr;
    if (!finding || finding->options.empty())
        return false;
    const px::FixOption* chosen = &finding->options.front();
    for (const px::FixOption& candidate : finding->options)
        if (!option.empty() && candidate.id == option)
            chosen = &candidate;
    return applyOptions({{finding->key, *chosen}}, "Fix Pixel Art (1)");
}

size_t PixelArtCheckWindow::fixAllChangeCount() const{
    if (!hasResult)
        return 0;
    return px::plan(current, {"all"}).changeCount();
}

bool PixelArtCheckWindow::fixAll(){
    if (!hasResult)
        return false;
    const px::FixPlan plan = px::plan(current, {"all"});
    if (plan.fixes.empty())
        return false;
    return applyOptions(plan.fixes, px::fixStepName(plan));
}

void PixelArtCheckWindow::selectTarget(const px::Finding& finding, bool frame){
    const std::string entity = finding.target.value("entity", "");
    const std::string scenePath = finding.target.value("scene", finding.target.value("path", ""));
    if (!entity.empty()){
        for (SceneProject& scene : project->getScenes()){
            if (px::scenePathOf(*project, scene) != scenePath || !scene.scene)
                continue;
            const Entity e = EntityPath::find(scene, entity.size() > 0 && entity[0] == '/' ? entity.substr(1) : entity);
            if (e == NULL_ENTITY)
                return;
            project->setSelectedSceneId(scene.id);
            project->replaceSelectedEntities(scene.id, {e});
            if (frame){
                const CommandDef* def = CommandCatalog::editor().find("edit.frameSelected");
                if (def && def->run)
                    def->run(CommandArgs());
            }
            return;
        }
        return;
    }
    const std::string kind = finding.target.value("kind", "");
    if ((kind == "texture" || kind == "font") && projectWindow)
        projectWindow->revealAsset(project->getAssetsPath() / finding.target.value("path", ""));
}

void PixelArtCheckWindow::show(){
    collectResult();
    if (assetsDirty && std::chrono::steady_clock::now() - assetsChangedAt >= kImportSettle){
        assetsDirty = false;
        run();
    }
    if (!windowOpen)
        return;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(720.0f), Theme::dpi(480.0f)), ImGuiCond_FirstUseEver);
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    bool keepOpen = true;
    if (ImGui::Begin(WINDOW_NAME, &keepOpen)){
        focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        drawToolbar();
        if (!notice.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped("%s", notice.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        drawFindings();
        drawFixAllPreview();
    }else{
        focused = false;
    }
    ImGui::End();
    if (!keepOpen)
        windowOpen = false;
}

void PixelArtCheckWindow::drawToolbar(){
    const bool busy = running.isValid() && !running.isReady();
    if (ImGui::Button(busy ? ICON_FA_SPINNER " Running" : ICON_FA_PLAY " Run"))
        run();
    ImGui::SetItemTooltip("Check again (F5)");
    ImGui::SameLine();
    static const char* const scopes[] = {"Open Scenes", "Build Scenes", "Selection", "Project"};
    int index = (int)scope;
    ImGui::SetNextItemWidth(Theme::dpi(130.0f));
    if (ImGui::Combo("##PixelArtScope", &index, scopes, 4)){
        scope = (Scope)index;
        run();
    }
    ImGui::SetItemTooltip("Scope: the open scenes, the build list, the selected GameObjects, or the build list and every pixel-art texture");
    ImGui::SameLine();
    const std::string rulesLabel = ruleFilter.empty() ? std::string("All Rules") : std::to_string(ruleFilter.size()) + " Rules";
    if (ImGui::Button((rulesLabel + "##PixelArtRules").c_str()))
        ImGui::OpenPopup("##PixelArtRulesPopup");
    if (ImGui::BeginPopup("##PixelArtRulesPopup")){
        bool changed = false;
        for (const std::string& id : px::ruleIds()){
            if (!px::isKnownRule(id))
                continue;
            bool on = ruleFilter.empty() || ruleFilter.count(id);
            if (ImGui::Checkbox(id.c_str(), &on)){
                if (ruleFilter.empty())
                    for (const std::string& other : px::ruleIds())
                        if (px::isKnownRule(other)) ruleFilter.insert(other);
                if (on) ruleFilter.insert(id);
                else ruleFilter.erase(id);
                changed = true;
            }
        }
        if (ImGui::SmallButton("All")){
            ruleFilter.clear();
            changed = true;
        }
        if (changed)
            run();
        ImGui::EndPopup();
    }
    if (hasResult){
        ImGui::SameLine();
        const px::Verdict verdict = current.verdict;
        const ImVec4 color = verdict == px::Verdict::Clean ? Theme::Colors::DisabledGreenText
                           : verdict == px::Verdict::Fixable ? Theme::Colors::WarningText : Theme::Colors::ErrorText;
        ImGui::TextColored(color, "%s", px::verdictName(verdict));
        ImGui::SameLine();
        ImGui::TextDisabled("%d FAIL, %d WARN", current.count(px::RuleStatus::Fail), current.count(px::RuleStatus::Warn));
        const size_t fixable = px::plan(current, {"all"}).fixes.size();
        ImGui::SameLine();
        ImGui::BeginDisabled(fixable == 0);
        if (ImGui::Button(("Fix All (" + std::to_string(fixable) + ")...").c_str()))
            previewOpen = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY " Copy Report"))
            ImGui::SetClipboardText(current.toJson().dump(1).c_str());
    }
}

void PixelArtCheckWindow::drawFindings(){
    if (!hasResult){
        ImGui::TextDisabled("Checking...");
        return;
    }
    if (current.findings.empty()){
        ImGui::TextColored(Theme::Colors::DisabledGreenText, ICON_FA_CIRCLE_CHECK " Clean: every rule passes.");
    }
    std::string fixKey, fixOption;
    ImGui::BeginChild("##PixelArtFindings");
    for (const px::RuleResult& rule : current.rules){
        ImGui::PushStyleColor(ImGuiCol_Text, statusColor(rule.status));
        const std::string header = std::string(statusIcon(rule.status)) + " " + rule.id + "  (" + std::to_string(rule.findings) + " of " +
                                   std::to_string(rule.checked) + ")###rule_" + rule.id;
        const bool openRule = ImGui::CollapsingHeader(header.c_str(), rule.findings > 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleColor();
        if (!openRule || rule.findings == 0)
            continue;
        if (!ImGui::BeginTable(("##findings_" + rule.id).c_str(), 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
            continue;
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Measured", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Expected", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Fix", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        for (const px::Finding& finding : current.findings){
            if (finding.rule != rule.id)
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, statusColor(finding.status));
            const std::string label = std::string(statusIcon(finding.status)) + " " + finding.id + " " + targetText(finding) + "##" + finding.key;
            if (ImGui::Selectable(label.c_str(), selectedKey == finding.key, ImGuiSelectableFlags_AllowDoubleClick)){
                selectedKey = finding.key;
                selectTarget(finding, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", finding.message.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", finding.measured.dump().c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", finding.expected.dump().c_str());
            ImGui::TableNextColumn();
            if (finding.options.empty()){
                ImGui::TextColored(Theme::Colors::SubtleText, "Needs a person: %s", finding.needsPerson.c_str());
                continue;
            }
            if (ImGui::SmallButton(("Fix##" + finding.key).c_str())){
                fixKey = finding.key;
                fixOption.clear();
            }
            ImGui::SetItemTooltip("%s", changesText(finding.options.front()).c_str());
            if (finding.options.size() > 1){
                ImGui::SameLine();
                if (ImGui::SmallButton((ICON_FA_CARET_DOWN "##more_" + finding.key).c_str()))
                    ImGui::OpenPopup(("##options_" + finding.key).c_str());
                if (ImGui::BeginPopup(("##options_" + finding.key).c_str())){
                    for (const px::FixOption& option : finding.options){
                        if (ImGui::MenuItem(option.label.c_str())){
                            fixKey = finding.key;
                            fixOption = option.id;
                        }
                        ImGui::SetItemTooltip("%s", changesText(option).c_str());
                    }
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (!fixKey.empty())
        fixFinding(fixKey, fixOption);
}

void PixelArtCheckWindow::drawFixAllPreview(){
    if (previewOpen){
        ImGui::OpenPopup(kPreviewName);
        previewOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(640.0f), Theme::dpi(420.0f)), ImGuiCond_Appearing);
    if (!EditorModal::begin(kPreviewName, nullptr, 0))
        return;
    const px::FixPlan plan = hasResult ? px::plan(current, {"all"}) : px::FixPlan();
    const std::string summary = "Fix All applies " + std::to_string(plan.changeCount()) + " change(s) of " + std::to_string(plan.fixes.size()) +
                                " finding(s) as one undo step.";
    EditorModal::message(summary);
    ImGui::TextWrapped("%s", summary.c_str());
    ImGui::BeginChild("##FixAllChanges", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_Borders);
    for (const auto& [key, option] : plan.fixes){
        ImGui::TextUnformatted(option.label.c_str());
        for (const px::FixChange& change : option.changes)
            ImGui::TextDisabled("    %s  %s: %s -> %s", change.target.c_str(), change.field.c_str(), change.from.dump().c_str(), change.to.dump().c_str());
    }
    ImGui::EndChild();
    if (EditorModal::button("Apply", "ok")){
        fixAll();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
