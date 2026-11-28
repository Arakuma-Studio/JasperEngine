// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueWindow.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "dialogue/DialogueEditor.h"
#include "dialogue/DialogueExpression.h"
#include "dialogue/DialogueKind.h"
#include "dialogue/DialogueProgram.h"
#include "dialogue/DialogueSettingsCmd.h"
#include "dialogue/DialogueVariables.h"
#include "graph/GraphEditorHooks.h"
#include "localization/MessageFormat.h"
#include "localization/StringTableDocument.h"
#include "localization/StringTableEditCmd.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"

#include "external/IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace dlg = jasperengine::dialogue;
using GraphJson = graph::Json;

namespace {

    DialogueWindow*& current() {
        static DialogueWindow* window = nullptr;
        return window;
    }

    std::string stringOf(const graph::Node& node, const char* name) {
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    const GraphJson& arrayOf(const graph::Node& node, const char* name) {
        static const GraphJson empty = GraphJson::array();
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_array() ? *it : empty;
    }

    ImU32 headerColor(const std::string& type) {
        if (type == "entry") return IM_COL32(52, 120, 72, 255);
        if (type == "end") return IM_COL32(140, 52, 52, 255);
        if (type == "jump") return IM_COL32(110, 80, 150, 255);
        if (type == "line") return IM_COL32(56, 86, 130, 255);
        return IM_COL32(80, 80, 88, 255);
    }

    std::string joinTags(const GraphJson& tags) {
        std::string out;
        if (tags.is_array()) {
            for (const GraphJson& tag : tags) {
                if (tag.is_string()) out += (out.empty() ? "" : ", ") + tag.get<std::string>();
            }
        } else if (tags.is_string()) {
            out = tags.get<std::string>();
        }
        return out;
    }

    GraphJson splitTags(const std::string& text) {
        GraphJson out = GraphJson::array();
        std::string current;
        for (char c : text + ",") {
            if (c == ',') {
                while (!current.empty() && current.back() == ' ') current.pop_back();
                if (!current.empty()) out.push_back(current);
                current.clear();
            } else if (c != ' ' || !current.empty()) {
                current += c;
            }
        }
        return out;
    }

    // the name a suggestion "declare variable gold" / "declare prefix quest" names
    std::string suggested(const std::string& message, const std::string& what) {
        const std::string marker = "(declare " + what + " ";
        const size_t at = message.find(marker);
        if (at == std::string::npos) return {};
        const size_t start = at + marker.size();
        const size_t end = message.find(')', start);
        return message.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    size_t codePoints(const std::string& text) {
        size_t count = 0;
        for (unsigned char c : text) count += (c & 0xC0) != 0x80;
        return count;
    }

}

DialogueWindow::DialogueWindow(Project* project): GraphEditorWindow(project, WINDOW_NAME, dlg::KIND_ID){
    dlg::registerDialogueKind();
    current() = this;
    // a translation edited anywhere (the Localization Tables window, edit_string_table) shows on the cards
    translationsListener = StringTableEditCmd::addChangeListener([this](const std::vector<fs::path>&) { loadTranslations(); });
}

DialogueWindow::~DialogueWindow(){
    StringTableEditCmd::removeChangeListener(translationsListener);
    if (current() == this) current() = nullptr;
}

DialogueWindow* DialogueWindow::instance(){
    return current();
}

bool DialogueWindow::reveal(const std::string& path, const std::string& nodeId){
    std::string target = path;
    if (!fs::path(path).is_absolute() && project) {
        const fs::path asset = project->resolveAssetPath(path);
        std::error_code ec;
        if (fs::exists(asset, ec)) target = asset.string();
    }
    if (!openGraph(target)) return false;
    setOpen(true);
    requestFocus();
    if (!nodeId.empty() && getDocument() && getDocument()->graph().findNode(nodeId)) {
        selectNodes({nodeId});
        frameSelected();
    }
    return true;
}

// ---------------------------------------------------------------- translations

void DialogueWindow::loadTranslations(){
    translations.clear();
    if (!getDocument()) return;
    for (const auto& [code, path] : l10n::translationSiblings(getDocument()->absolutePath())) {
        std::string text;
        l10n::TranslationDoc doc;
        if (!ProjectChecks::readFile(path, text) || !l10n::parseTranslation(text, doc)) continue;
        std::map<std::string, Translation>& entries = translations[code];
        for (const auto& [key, entry] : doc.entries) entries[key] = {entry.text, entry.status};
    }
    if (!language_.empty() && !translations.count(language_)) language_.clear();
}

std::vector<std::string> DialogueWindow::languages() const{
    std::vector<std::string> out;
    for (const auto& [code, entries] : translations) out.push_back(code);
    return out;
}

std::string DialogueWindow::displayText(const graph::Node& node) const{
    const std::string source = stringOf(node, "text");
    if (language_.empty()) return source;
    auto table = translations.find(language_);
    if (table == translations.end()) return source;
    auto entry = table->second.find(node.id);
    return entry == table->second.end() || entry->second.text.empty() ? source : entry->second.text;
}

std::vector<std::pair<std::string, std::string>> DialogueWindow::translationBadges(const graph::Node& node) const{
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::string> keys;
    if (!stringOf(node, "text").empty()) keys.push_back(node.id);
    for (const GraphJson& item : arrayOf(node, "variants")) {
        if (item.is_object() && !item.value("text", std::string()).empty()) keys.push_back(node.id + "." + item.value("id", std::string()));
    }
    for (const GraphJson& item : arrayOf(node, "options")) {
        if (item.is_object() && !item.value("text", std::string()).empty()) keys.push_back(node.id + "." + item.value("id", std::string()));
    }
    for (const auto& [code, entries] : translations) {
        std::string state = "complete";
        for (const std::string& key : keys) {
            auto entry = entries.find(key);
            if (entry == entries.end() || entry->second.text.empty()) {
                state = "missing";
                break;
            }
            if (entry->second.status == "needs_review") state = "review";
        }
        out.emplace_back(code, state);
    }
    return out;
}

// ---------------------------------------------------------------- the canvas

bool DialogueWindow::nodeHasThumbnail(const graph::Node& node) const{
    return node.type != "entry";
}

void DialogueWindow::drawNodeThumbnail(const graph::Node& node, ImDrawList* drawList, ImVec2 min, ImVec2 max){
    const float scale = std::max(0.2f, (max.y - min.y) / 112.0f);
    const float size = std::max(6.0f, 13.0f * scale);
    const float wrap = std::max(1.0f, max.x - min.x - 6.0f * scale);
    drawList->AddRectFilled(min, max, IM_COL32(28, 30, 36, 255), 4.0f * scale);
    drawList->AddRectFilled(min, ImVec2(max.x, min.y + 3.0f * scale), headerColor(node.type));
    ImFont* font = ImGui::GetFont();
    ImVec2 at(min.x + 3.0f * scale, min.y + 5.0f * scale);
    drawList->PushClipRect(min, max, true);
    auto line = [&](const std::string& text, ImU32 color, bool wrapped = true) {
        if (text.empty()) return;
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, wrapped ? wrap : 0.0f, text.c_str());
        drawList->AddText(font, size, at, color, text.c_str(), nullptr, wrapped ? wrap : 0.0f);
        at.y += extent.y + 2.0f * scale;
    };
    if (node.type == "line") {
        const std::string speaker = stringOf(node, "speaker");
        line(speaker.empty() ? std::string("(no speaker)") : speaker, IM_COL32(150, 190, 255, 255), false);
        // the first three lines of the text, in the window's language
        std::string text = displayText(node);
        const ImVec2 full = font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
        if (full.y > size * 3.4f) {
            size_t cut = text.size();
            while (cut > 0 && font->CalcTextSizeA(size, FLT_MAX, wrap, text.substr(0, cut).c_str()).y > size * 3.4f) cut = cut * 3 / 4;
            text = text.substr(0, cut) + "...";
        }
        line(text.empty() ? std::string("(empty)") : text, text.empty() ? IM_COL32(120, 120, 120, 255) : IM_COL32(230, 230, 230, 255));
        // mood and tag chips
        std::string chips;
        if (!stringOf(node, "mood").empty()) chips += "[" + stringOf(node, "mood") + "] ";
        auto tags = node.params.find("tags");
        if (tags != node.params.end()) {
            const std::string list = joinTags(*tags);
            if (!list.empty()) chips += "#" + list;
        }
        line(chips, IM_COL32(200, 170, 110, 255));
        // a badge per translation
        float x = min.x + 3.0f * scale;
        for (const auto& [code, state] : translationBadges(node)) {
            const ImU32 color = state == "complete" ? IM_COL32(60, 150, 80, 255) : state == "review" ? IM_COL32(190, 150, 40, 255) : IM_COL32(150, 60, 60, 255);
            const ImVec2 extent = font->CalcTextSizeA(size * 0.85f, FLT_MAX, 0.0f, code.c_str());
            drawList->AddRectFilled(ImVec2(x, at.y), ImVec2(x + extent.x + 4.0f * scale, at.y + extent.y + 2.0f * scale), color, 2.0f * scale);
            drawList->AddText(font, size * 0.85f, ImVec2(x + 2.0f * scale, at.y + 1.0f * scale), IM_COL32(255, 255, 255, 255), code.c_str());
            x += extent.x + 8.0f * scale;
        }
    } else if (node.type == "branch") {
        line("if " + stringOf(node, "condition"), IM_COL32(220, 220, 220, 255));
    } else if (node.type == "set") {
        line(stringOf(node, "assign"), IM_COL32(220, 220, 220, 255));
    } else if (node.type == "action") {
        line(stringOf(node, "action"), IM_COL32(220, 220, 220, 255));
        if (node.params.value("wait", false)) line("waits for resume()", IM_COL32(160, 160, 160, 255));
    } else if (node.type == "jump") {
        auto dialogue = node.params.find("dialogue");
        const std::string file = dialogue != node.params.end() && dialogue->is_object() ? dialogue->value("path", std::string()) : std::string();
        line("to " + (file.empty() ? std::string("this dialogue") : file), IM_COL32(220, 220, 220, 255));
        line("at " + stringOf(node, "entry"), IM_COL32(200, 200, 200, 255));
    } else if (node.type == "end") {
        const std::string result = stringOf(node, "result");
        line(result.empty() ? std::string("end") : "result: " + result, IM_COL32(230, 200, 200, 255));
    }
    drawList->PopClipRect();
    // the preview's walk: the path taken, the node it is at
    if (previewActive && walked.count(node.id)) {
        drawList->AddCircleFilled(ImVec2(max.x - 6.0f * scale, min.y + 8.0f * scale), 3.0f * scale, IM_COL32(90, 200, 110, 255));
    }
    if (previewActive && !preview.node.empty() && preview.node == node.id) {
        drawList->AddRect(min, max, IM_COL32(250, 210, 60, 255), 4.0f * scale, 0, 2.5f * scale);
    }
}

// ---------------------------------------------------------------- toolbar and problems

void DialogueWindow::drawToolbarExtras(){
    if (!getDocument()) return;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("Language", language_.empty() ? "Source" : language_.c_str())) {
        if (ImGui::Selectable("Source", language_.empty())) language_.clear();
        for (const std::string& code : languages()) {
            if (ImGui::Selectable(code.c_str(), code == language_)) language_ = code;
        }
        ImGui::EndCombo();
    }
    UIProbe::item("dialogue.language");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    std::vector<std::pair<std::string, std::string>> entries;
    for (const graph::Node& node : getDocument()->graph().nodes) {
        if (node.type == "entry") entries.emplace_back(stringOf(node, "name"), node.id);
    }
    if (ImGui::BeginCombo("Entry", entry.empty() ? "(default)" : entry.c_str())) {
        if (ImGui::Selectable("(default)", entry.empty())) entry.clear();
        for (const auto& [name, id] : entries) {
            if (ImGui::Selectable((name + "##" + id).c_str(), name == entry)) {
                entry = name;
                selectNodes({id});
                frameSelected();
            }
        }
        ImGui::EndCombo();
    }
    UIProbe::item("dialogue.entry");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLAY " Play")) previewStart(entry);
    UIProbe::item("dialogue.play");
}

void DialogueWindow::collectProblems(std::vector<Problem>& problems) const{
    if (!getDocument()) return;
    // the codes the project decides, linted again when the graph or the settings changed
    static const std::set<std::string> projectCodes = {"unknown-action-prefix", "unknown-condition-prefix", "unknown-variable",
                                                        "unknown-argument", "jump-target", "unreachable"};
    if (project && (projectIssuesRevision != getDocument()->revision() || !projectIssuesSettings ||
                    *projectIssuesSettings != project->getDialogueSettings())) {
        projectIssues.clear();
        std::vector<graph::Issue> fresh;
        dlg::lint(getDocument()->graph(), fresh);
        for (graph::Issue& issue : fresh) {
            if (projectCodes.count(issue.code)) projectIssues.push_back(std::move(issue));
        }
        projectIssuesRevision = getDocument()->revision();
        projectIssuesSettings = std::make_shared<const DialogueSettings>(project->getDialogueSettings());
    }
    if (project) {
        problems.erase(std::remove_if(problems.begin(), problems.end(), [](const Problem& p) { return projectCodes.count(p.code) > 0; }), problems.end());
        for (const graph::Issue& issue : projectIssues) {
            Problem problem{issue.severity, issue.code, issue.message, issue.nodeId, {}, ""};
            if (!issue.suggestion.empty()) problem.message += " (" + issue.suggestion + ")";
            problem.revision = getDocument()->revision();
            problems.push_back(std::move(problem));
        }
    }
    const std::vector<graph::Issue>& issues = getDocument()->issues();
    for (Problem& problem : problems) {
        if (problem.code == "fan-out") {
            for (const graph::Issue& issue : issues) {
                if (issue.code == "fan-out" && issue.nodeId == problem.nodeId && !issue.edgeId.empty()) {
                    problem.fix = {{"disconnect", {{"id", issue.edgeId}}}};
                    problem.fixLabel = "Remove Extra Wire";
                    break;
                }
            }
        } else if (problem.code == "unknown-variable" || problem.code == "unknown-argument") {
            const std::string name = suggested(problem.message, "variable");
            if (name.empty() || getDocument()->graph().findVariable(name)) continue;
            problem.fix = {{"addVariable", {{"variable", {{"name", name}, {"type", "number"}, {"default", 0}}}}}};
            problem.fixLabel = "Declare Variable $" + name;
        } else if (problem.code == "unknown-action-prefix" || problem.code == "unknown-condition-prefix") {
            const std::string prefix = suggested(problem.message, "prefix");
            if (prefix.empty()) continue;
            const bool action = problem.code == "unknown-action-prefix";
            Project* owner = project;
            problem.fixLabel = "Declare " + std::string(action ? "Action" : "Condition") + " Prefix " + prefix;
            const std::string label = problem.fixLabel;
            problem.fixAction = [owner, prefix, action, label]() {
                DialogueSettings after = owner->getDialogueSettings();
                (action ? after.actionPrefixes : after.conditionPrefixes).push_back({prefix, "", ""});
                return owner->getProjectCommandHistory()->addCommand(new DialogueSettingsCmd(owner, owner->getDialogueSettings(), after, label));
            };
        }
    }
}

void DialogueWindow::documentChanged(){
    loadTranslations();
    if (previewActive) replay();
}

void DialogueWindow::documentClosed(){
    translations.clear();
    previewActive = false;
    steps.clear();
    startValues.clear();
    answered.clear();
    preview = dialogues::WalkResult();
    walked.clear();
    buffers.clear();
    entry.clear();
    language_.clear();
}

// ---------------------------------------------------------------- the preview

void DialogueWindow::replay(){
    if (!getDocument()) return;
    const std::string path = project ? project->normalizeToAssetsRelative(getDocument()->absolutePath()).generic_string() : getDocument()->projectPath();
    const std::shared_ptr<const dlg::Program> program = dlg::Program::compile(getDocument()->graph(), path);
    dialogues::WalkOptions options;
    options.entry = entry;
    options.stepwise = true;
    options.steps = steps;
    for (const auto& [name, value] : startValues) options.variables.emplace_back(name, value);
    for (const auto& [prefix, mode] : conditionModes) {
        if (mode == "true" || mode == "false") options.conditions[prefix + ".*"] = dlg::Value(mode == "true");
    }
    for (const auto& [call, value] : answered) options.conditions[call] = dlg::Value(value);
    if (!language_.empty()) options.language = language_;
    if (project) {
        // the project's declared defaults, as Play has them
        dlg::Variables::setProjectDefaults(dialogues::projectDefaults(project->getDialogueSettings()));
    }
    preview = dialogues::walk(program, options);
    walked = std::set<std::string>(preview.nodes.begin(), preview.nodes.end());
}

void DialogueWindow::previewStart(const std::string& from){
    entry = from;
    previewActive = true;
    steps.clear();
    replay();
}

bool DialogueWindow::previewAdvance(){
    if (!previewActive) return false;
    steps.push_back(0);
    replay();
    if (!preview.error.empty()) {
        steps.pop_back();
        replay();
        return false;
    }
    return true;
}

bool DialogueWindow::previewChoose(int index){
    if (!previewActive) return false;
    steps.push_back(index);
    replay();
    if (!preview.error.empty()) {
        steps.pop_back();
        replay();
        return false;
    }
    return true;
}

bool DialogueWindow::previewBack(){
    if (!previewActive || steps.empty()) return false;
    steps.pop_back();
    replay();
    return true;
}

void DialogueWindow::previewRestart(){
    steps.clear();
    previewActive = true;
    replay();
}

void DialogueWindow::setPreviewVariable(const std::string& name, const dlg::Value& value){
    startValues[name] = value;
    if (previewActive) replay();
}

void DialogueWindow::setConditionMode(const std::string& prefix, const std::string& mode){
    conditionModes[prefix] = mode;
    if (previewActive) replay();
}

void DialogueWindow::answerCondition(const std::string& call, bool value){
    answered[call] = value;
    if (previewActive) replay();
}

std::vector<std::string> DialogueWindow::previewActions() const{
    std::vector<std::string> out;
    for (const dialogues::WalkStep& step : preview.transcript) out.insert(out.end(), step.actions.begin(), step.actions.end());
    out.insert(out.end(), preview.endActions.begin(), preview.endActions.end());
    return out;
}

void DialogueWindow::drawPreviewPane(){
    ImGui::TextUnformatted(ICON_FA_COMMENTS " Preview");
    if (!getDocument()) {
        ImGui::TextDisabled("Open a .dialogue.");
        return;
    }
    if (!previewActive) {
        ImGui::TextWrapped("Plays the dialogue here without Play: handler calls never run.");
        if (ImGui::Button(ICON_FA_PLAY " Play")) previewStart(entry);
        UIProbe::item("dialogue.preview.play");
        return;
    }
    if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT " Restart")) previewRestart();
    UIProbe::item("dialogue.preview.restart");
    ImGui::SameLine();
    ImGui::BeginDisabled(steps.empty());
    if (ImGui::SmallButton(ICON_FA_BACKWARD_STEP " Back")) previewBack();
    UIProbe::item("dialogue.preview.back");
    ImGui::EndDisabled();
    ImGui::Separator();
    if (preview.state == "line" && !preview.transcript.empty()) {
        const dialogues::WalkStep& line = preview.transcript.back();
        if (!line.speaker.empty()) ImGui::TextColored(ImVec4(0.6f, 0.75f, 1.0f, 1.0f), "%s", line.speaker.c_str());
        ImGui::TextWrapped("%s", line.text.c_str());
        bool anyEnabled = false;
        const graph::Node* node = getDocument()->graph().findNode(line.node);
        for (const dlg::Option& option : line.options) {
            anyEnabled = anyEnabled || option.enabled;
            ImGui::PushID(option.index);
            ImGui::BeginDisabled(!option.enabled);
            if (ImGui::Button((std::to_string(option.index) + ". " + option.text).c_str())) previewChoose(option.index);
            UIProbe::item("dialogue.preview.option." + std::to_string(option.index));
            ImGui::EndDisabled();
            if (!option.enabled && node) {
                for (const GraphJson& item : arrayOf(*node, "options")) {
                    if (item.is_object() && item.value("id", std::string()) == option.id) ImGui::SetItemTooltip("if %s", item.value("condition", std::string()).c_str());
                }
            }
            ImGui::PopID();
        }
        if (!anyEnabled) {
            if (ImGui::Button("Advance")) previewAdvance();
            UIProbe::item("dialogue.preview.advance");
        }
    } else if (preview.state == "ended") {
        ImGui::TextDisabled("Ended%s", preview.result.empty() ? "" : (": " + preview.result).c_str());
    }
    if (!preview.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", preview.error.c_str());

    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::map<std::string, dlg::Value> now;
        for (const auto& [name, value] : preview.variables) now[name] = value;
        std::set<std::string> names;
        for (const graph::Variable& variable : getDocument()->graph().variables) names.insert(variable.name);
        if (project) {
            for (const DialogueSettings::Variable& variable : project->getDialogueSettings().variables) names.insert(variable.name);
        }
        for (const auto& [name, value] : now) names.insert(name);
        if (ImGui::BeginTable("##dialogue_variables", 3, ImGuiTableFlags_SizingStretchProp)) {
            for (const std::string& name : names) {
                ImGui::PushID(name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("$%s", name.c_str());
                ImGui::TableNextColumn();
                auto start = startValues.find(name);
                const dlg::Value shown = start != startValues.end() ? start->second : (now.count(name) ? now[name] : dlg::Value());
                std::string typed;
                if (textField("var." + name, "##start", shown.toText(), typed)) {
                    const nlohmann::json parsed = nlohmann::json::parse(typed, nullptr, false);
                    if (parsed.is_boolean()) setPreviewVariable(name, dlg::Value(parsed.get<bool>()));
                    else if (parsed.is_number()) setPreviewVariable(name, dlg::Value(parsed.get<double>()));
                    else setPreviewVariable(name, dlg::Value(typed));
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", now.count(name) ? now[name].describe().c_str() : "-");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Conditions", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::set<std::string> prefixes;
        const std::shared_ptr<const dlg::Program> program = dlg::Program::compile(getDocument()->graph(), getDocument()->projectPath());
        for (const dlg::CallSite& call : program->exprs.calls) prefixes.insert(call.prefix);
        static const char* modes[] = {"false", "true", "ask"};
        for (const std::string& prefix : prefixes) {
            ImGui::PushID(prefix.c_str());
            const std::string mode = conditionModes.count(prefix) ? conditionModes[prefix] : std::string("ask");
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::BeginCombo(("##" + prefix).c_str(), mode.c_str())) {
                for (const char* option : modes) {
                    if (ImGui::Selectable(option, mode == option)) setConditionMode(prefix, option);
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::Text("%s.*", prefix.c_str());
            ImGui::PopID();
        }
        for (const std::string& call : preview.unknownCalls) {
            ImGui::PushID(call.c_str());
            ImGui::TextDisabled("asked: %s -> false", call.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Make True")) answerCondition(call, true);
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::vector<std::string> actions = previewActions();
        if (actions.empty()) ImGui::TextDisabled("None yet.");
        for (const std::string& action : actions) ImGui::BulletText("%s", action.c_str());
    }
}

// ---------------------------------------------------------------- line edits

std::string DialogueWindow::nextSubId(const graph::Node& node, const char* prefix, std::vector<graph::GraphOp>& ops) const{
    int next = node.params.value("next_sub_id", 1);
    // ids already used (a file written by hand) are skipped: never reused
    std::set<std::string> used;
    for (const char* list : {"options", "variants"}) {
        for (const GraphJson& item : arrayOf(node, list)) {
            if (item.is_object()) used.insert(item.value("id", std::string()));
        }
    }
    std::string id;
    do {
        id = prefix + std::to_string(next++);
    } while (used.count(id));
    ops.push_back({"setParam", {{"node", node.id}, {"name", "next_sub_id"}, {"value", next}}});
    return id;
}

bool DialogueWindow::applyOptions(const std::string& nodeId, const GraphJson& options, const std::vector<graph::GraphOp>& extra, const std::string& label){
    std::vector<graph::GraphOp> ops = extra;
    ops.push_back({"setParam", {{"node", nodeId}, {"name", "options"}, {"value", options}}});
    return apply(ops, label);
}

bool DialogueWindow::applyVariants(const std::string& nodeId, const GraphJson& variants, const std::string& label){
    return apply({{"setParam", {{"node", nodeId}, {"name", "variants"}, {"value", variants}}}}, label);
}

bool DialogueWindow::addOption(const std::string& nodeId, const std::string& text){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node || node->type != "line") return false;
    std::vector<graph::GraphOp> ops;
    GraphJson options = arrayOf(*node, "options");
    options.push_back({{"id", nextSubId(*node, "o", ops)}, {"text", text}});
    return applyOptions(nodeId, options, ops, "Add Option");
}

bool DialogueWindow::removeOption(const std::string& nodeId, const std::string& optionId){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node) return false;
    GraphJson options = GraphJson::array();
    bool found = false;
    for (const GraphJson& item : arrayOf(*node, "options")) {
        if (item.is_object() && item.value("id", std::string()) == optionId) found = true;
        else options.push_back(item);
    }
    if (!found) return false;
    // its wire goes with it, in the same step
    std::vector<graph::GraphOp> ops;
    for (const graph::Edge& edge : getDocument()->graph().edges) {
        if (edge.from.node == nodeId && edge.from.port == optionId) ops.push_back({"disconnect", {{"id", edge.id}}});
    }
    return applyOptions(nodeId, options, ops, "Remove Option");
}

bool DialogueWindow::moveOption(const std::string& nodeId, size_t from, size_t to){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node) return false;
    GraphJson options = arrayOf(*node, "options");
    if (from >= options.size() || to >= options.size() || from == to) return false;
    GraphJson moved = options[from];
    options.erase(options.begin() + static_cast<std::ptrdiff_t>(from));
    options.insert(options.begin() + static_cast<std::ptrdiff_t>(to), moved);
    return applyOptions(nodeId, options, {}, "Reorder Options");
}

bool DialogueWindow::setOptionField(const std::string& nodeId, const std::string& optionId, const std::string& field, const GraphJson& value){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node) return false;
    GraphJson options = arrayOf(*node, "options");
    for (GraphJson& item : options) {
        if (!item.is_object() || item.value("id", std::string()) != optionId) continue;
        if (item.contains(field) && item[field] == value) return true;
        if (value.is_null() || (value.is_string() && value.get<std::string>().empty() && field != "text")) item.erase(field);
        else item[field] = value;
        return applyOptions(nodeId, options, {}, "Set Option " + paramLabel(field));
    }
    return false;
}

bool DialogueWindow::addVariant(const std::string& nodeId, const std::string& text){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node || node->type != "line") return false;
    std::vector<graph::GraphOp> ops;
    GraphJson variants = arrayOf(*node, "variants");
    variants.push_back({{"id", nextSubId(*node, "v", ops)}, {"text", text}});
    ops.push_back({"setParam", {{"node", nodeId}, {"name", "variants"}, {"value", variants}}});
    return apply(ops, "Add Variant");
}

bool DialogueWindow::removeVariant(const std::string& nodeId, const std::string& variantId){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node) return false;
    GraphJson variants = GraphJson::array();
    for (const GraphJson& item : arrayOf(*node, "variants")) {
        if (!item.is_object() || item.value("id", std::string()) != variantId) variants.push_back(item);
    }
    return applyVariants(nodeId, variants, "Remove Variant");
}

bool DialogueWindow::moveVariant(const std::string& nodeId, size_t from, size_t to){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node) return false;
    GraphJson variants = arrayOf(*node, "variants");
    if (from >= variants.size() || to >= variants.size() || from == to) return false;
    GraphJson moved = variants[from];
    variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(from));
    variants.insert(variants.begin() + static_cast<std::ptrdiff_t>(to), moved);
    return applyVariants(nodeId, variants, "Reorder Variants");
}

bool DialogueWindow::setVariantText(const std::string& nodeId, const std::string& variantId, const std::string& text){
    const graph::Node* node = getDocument() ? getDocument()->graph().findNode(nodeId) : nullptr;
    if (!node) return false;
    GraphJson variants = arrayOf(*node, "variants");
    for (GraphJson& item : variants) {
        if (item.is_object() && item.value("id", std::string()) == variantId) {
            item["text"] = text;
            return applyVariants(nodeId, variants, "Set Variant Text");
        }
    }
    return false;
}

// ---------------------------------------------------------------- the Inspector

bool DialogueWindow::textField(const std::string& id, const char* label, const std::string& value, std::string& out, bool multiline){
    std::string& text = buffers.try_emplace(id, value).first->second;
    char buffer[4096];
    std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
    ImGui::PushID(id.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = multiline ? ImGui::InputTextMultiline(label, buffer, sizeof(buffer), ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f))
                                   : ImGui::InputText(label, buffer, sizeof(buffer));
    if (changed) text = buffer;
    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
    const bool active = ImGui::IsItemActive();
    ImGui::PopID();
    if (commit) {
        out = text;
        buffers.erase(id);
        return out != value;
    }
    // not typing: the field follows the document
    if (!active) buffers.erase(id);
    return false;
}

void DialogueWindow::expressionField(const graph::Node& node, const char* param, const char* label, bool statements){
    ImGui::TextUnformatted(label);
    const std::string value = stringOf(node, param);
    std::string typed;
    if (textField(node.id + "." + param, "##expr", value, typed)) setParam(node.id, param, typed);
    const std::string shown = buffers.count(node.id + "." + param) ? buffers[node.id + "." + param] : value;
    dlg::ExprPool pool;
    dlg::Expr expr;
    dlg::ParseError error;
    if (!shown.empty() && !dlg::compileExpr(shown, statements ? dlg::ExprMode::Statements : dlg::ExprMode::Condition, pool, expr, &error)) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "column %d: %s", error.column, error.message.c_str());
    }
    // completion: what the word being typed can become
    size_t start = shown.size();
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(shown[start - 1])) || shown[start - 1] == '_' || shown[start - 1] == '$' || shown[start - 1] == '.')) start--;
    const std::string word = shown.substr(start);
    if (word.empty()) return;
    std::vector<std::string> names = {"visited(\"\")", "visits(\"\")", "random()", "chance(0.5)", "not", "and", "or"};
    for (const graph::Variable& variable : getDocument()->graph().variables) names.push_back("$" + variable.name);
    if (project) {
        const DialogueSettings& settings = project->getDialogueSettings();
        for (const DialogueSettings::Variable& variable : settings.variables) names.push_back("$" + variable.name);
        for (const DialogueSettings::Prefix& prefix : statements ? settings.actionPrefixes : settings.conditionPrefixes) names.push_back(prefix.name + ".");
    }
    int shownCount = 0;
    for (const std::string& name : names) {
        if (name.size() <= word.size() || name.compare(0, word.size(), word) != 0 || shownCount >= 6) continue;
        shownCount++;
        ImGui::SameLine();
        if (ImGui::SmallButton(name.c_str())) {
            const std::string completed = shown.substr(0, start) + name;
            setParam(node.id, param, completed);
            buffers.erase(node.id + "." + param);
        }
    }
}

void DialogueWindow::drawVariantRows(const graph::Node& node){
    const GraphJson& variants = arrayOf(node, "variants");
    for (size_t i = 0; i < variants.size(); i++) {
        if (!variants[i].is_object()) continue;
        const std::string id = variants[i].value("id", std::string());
        ImGui::PushID(id.c_str());
        ImGui::TextDisabled("%s", id.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROW_UP) && i > 0) moveVariant(node.id, i, i - 1);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROW_DOWN) && i + 1 < variants.size()) moveVariant(node.id, i, i + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) removeVariant(node.id, id);
        std::string typed;
        if (textField(node.id + ".variant." + id, "##text", variants[i].value("text", std::string()), typed)) setVariantText(node.id, id, typed);
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add Variant")) addVariant(node.id);
    UIProbe::item("dialogue.inspector.add_variant");
}

void DialogueWindow::drawOptionRows(const graph::Node& node){
    const GraphJson& options = arrayOf(node, "options");
    static const char* styles[] = {"", "accept", "danger", "exit"};
    for (size_t i = 0; i < options.size(); i++) {
        if (!options[i].is_object()) continue;
        const GraphJson& option = options[i];
        const std::string id = option.value("id", std::string());
        ImGui::PushID(id.c_str());
        ImGui::Separator();
        ImGui::Text("%zu. %s", i + 1, id.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROW_UP) && i > 0) moveOption(node.id, i, i - 1);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROW_DOWN) && i + 1 < options.size()) moveOption(node.id, i, i + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            removeOption(node.id, id);
            ImGui::PopID();
            break;
        }
        std::string typed;
        ImGui::TextUnformatted("Text");
        if (textField(node.id + ".option." + id + ".text", "##text", option.value("text", std::string()), typed)) setOptionField(node.id, id, "text", typed);
        ImGui::TextUnformatted("Condition");
        if (textField(node.id + ".option." + id + ".condition", "##condition", option.value("condition", std::string()), typed)) setOptionField(node.id, id, "condition", typed);
        ImGui::TextUnformatted("Action");
        if (textField(node.id + ".option." + id + ".action", "##action", option.value("action", std::string()), typed)) setOptionField(node.id, id, "action", typed);
        const std::string style = option.value("style", std::string());
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("Style", style.empty() ? "(none)" : style.c_str())) {
            for (const char* candidate : styles) {
                if (ImGui::Selectable(*candidate ? candidate : "(none)", style == candidate)) setOptionField(node.id, id, "style", std::string(candidate));
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool once = option.value("once", false);
        if (ImGui::Checkbox("Once", &once)) setOptionField(node.id, id, "once", once ? GraphJson(true) : GraphJson());
        ImGui::SameLine();
        const std::string whenFalse = option.value("when_false", std::string("hide"));
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::BeginCombo("When False", whenFalse.c_str())) {
            for (const char* candidate : {"hide", "disable"}) {
                if (ImGui::Selectable(candidate, whenFalse == candidate)) setOptionField(node.id, id, "when_false", std::string(candidate) == "hide" ? GraphJson() : GraphJson(candidate));
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add Option")) addOption(node.id);
    UIProbe::item("dialogue.inspector.add_option");
}

void DialogueWindow::drawLineInspector(const graph::Node& node){
    std::string typed;
    ImGui::TextUnformatted("Speaker");
    if (textField(node.id + ".speaker", "##speaker", stringOf(node, "speaker"), typed)) setParam(node.id, "speaker", typed);
    if (project && !project->getDialogueSettings().speakers.empty()) {
        if (ImGui::BeginCombo("##speakers", "Speakers")) {
            for (const DialogueSettings::Speaker& speaker : project->getDialogueSettings().speakers) {
                if (ImGui::Selectable(speaker.id.c_str())) setParam(node.id, "speaker", speaker.id);
            }
            ImGui::EndCombo();
        }
    }
    ImGui::TextUnformatted("Text");
    const std::string text = stringOf(node, "text");
    if (textField(node.id + ".text", "##text", text, typed, true)) setParam(node.id, "text", typed);
    UIProbe::item("dialogue.inspector.text");
    const std::string shown = buffers.count(node.id + ".text") ? buffers[node.id + ".text"] : text;
    jasperengine::l10n::MessageError messageError;
    if (!shown.empty() && !jasperengine::l10n::MessageFormat::validate(shown, &messageError)) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "column %zu: %s", jasperengine::l10n::MessageFormat::column(shown, messageError.offset),
                           messageError.message.c_str());
    }
    const int maxChars = node.params.value("max_chars", 0);
    if (maxChars > 0) {
        const size_t count = codePoints(shown);
        ImGui::TextColored(count > static_cast<size_t>(maxChars) ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%zu / %d characters", count, maxChars);
    }
    if (ImGui::CollapsingHeader("Variants")) {
        const std::string mode = stringOf(node, "variant_mode").empty() ? std::string("random") : stringOf(node, "variant_mode");
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("Variant Mode", mode.c_str())) {
            for (const char* candidate : {"random", "sequence", "once"}) {
                if (ImGui::Selectable(candidate, mode == candidate)) setParam(node.id, "variant_mode", std::string(candidate));
            }
            ImGui::EndCombo();
        }
        drawVariantRows(node);
    }
    ImGui::TextUnformatted("Mood");
    if (textField(node.id + ".mood", "##mood", stringOf(node, "mood"), typed)) setParam(node.id, "mood", typed);
    ImGui::TextUnformatted("Tags");
    auto tags = node.params.find("tags");
    if (textField(node.id + ".tags", "##tags", tags != node.params.end() ? joinTags(*tags) : std::string(), typed)) setParam(node.id, "tags", splitTags(typed));
    ImGui::TextUnformatted("Voice");
    auto voice = node.params.find("voice");
    const std::string voicePath = voice != node.params.end() && voice->is_object() ? voice->value("path", std::string()) : std::string();
    if (textField(node.id + ".voice", "##voice", voicePath, typed)) {
        setParam(node.id, "voice", typed.empty() ? GraphJson() : GraphJson{{"guid", ""}, {"path", typed}});
    }
    if (ImGui::CollapsingHeader("Options", ImGuiTreeNodeFlags_DefaultOpen)) drawOptionRows(node);
    expressionField(node, "action", "Line Action", true);
}

bool DialogueWindow::drawKindNodeInspector(const graph::Node& node){
    if (node.type == "line") {
        drawLineInspector(node);
        return true;
    }
    if (node.type == "branch") {
        expressionField(node, "condition", "Condition", false);
        return true;
    }
    if (node.type == "set") {
        expressionField(node, "assign", "Assign", true);
        return true;
    }
    if (node.type == "action") {
        expressionField(node, "action", "Action", true);
        bool wait = node.params.value("wait", false);
        if (ImGui::Checkbox("Wait", &wait)) setParam(node.id, "wait", wait);
        ImGui::SetItemTooltip("Pauses the runner after the action until runner:resume(), whatever its handlers return.");
        return true;
    }
    return false;
}
