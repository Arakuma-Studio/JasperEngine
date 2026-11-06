// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImporterFields.h"
#include "EditorModal.h"

#include "imgui_internal.h"

#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::vector<std::string> splitDotted(const std::string& key){
        std::vector<std::string> parts;
        size_t start = 0;
        while (true){
            const size_t dot = key.find('.', start);
            parts.push_back(key.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
        return parts;
    }

    struct PromptState {
        std::vector<std::pair<const void*, ImporterPrompt::Pending>> views;
        std::function<void()> then;
        bool open = false;
        bool requested = false;
    };

    PromptState& promptState(){
        static PromptState state;
        return state;
    }

    void rowLabel(const char* label, const char* tooltip){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        ImGui::TableSetColumnIndex(1);
    }

}

YAML::Node importer::get(const YAML::Node& root, const std::string& key){
    // (reset rebinds: yaml-cpp's assignment to a bound node overwrites what it holds)
    YAML::Node node;
    node.reset(root);
    for (const std::string& part : splitDotted(key)){
        if (!node || !node.IsMap() || !node[part])
            return YAML::Node();
        node.reset(node[part]);
    }
    return YAML::Clone(node);
}

void importer::set(YAML::Node& root, const std::string& key, const YAML::Node& value){
    const std::vector<std::string> parts = splitDotted(key);
    if (!root || !root.IsMap())
        root = YAML::Node(YAML::NodeType::Map);
    YAML::Node node = root;
    for (size_t i = 0; i + 1 < parts.size(); i++){
        if (!node[parts[i]] || !node[parts[i]].IsMap())
            node[parts[i]] = YAML::Node(YAML::NodeType::Map);
        node.reset(node[parts[i]]);
    }
    if (value && !value.IsNull())
        node[parts.back()] = YAML::Clone(value);
    else
        node.remove(parts.back());
}

void importer::leaves(const YAML::Node& node, const std::string& prefix, std::set<std::string>& out){
    if (!node || !node.IsMap())
        return;
    for (const auto& entry : node){
        const std::string key = prefix + entry.first.as<std::string>();
        if (entry.second.IsMap())
            leaves(entry.second, key + ".", out);
        else
            out.insert(key);
    }
}

std::string importer::dump(const YAML::Node& node){
    return node ? YAML::Dump(node) : std::string();
}

void ImporterForm::reset(const std::vector<YAML::Node>& sections){
    touched.clear();
    mixed.clear();
    working = sections.empty() || !sections[0] ? YAML::Node(YAML::NodeType::Map) : YAML::Clone(sections[0]);
    std::set<std::string> keys;
    for (const YAML::Node& section : sections)
        importer::leaves(section, "", keys);
    for (const std::string& leaf : keys){
        const std::string first = sections.empty() ? std::string() : importer::dump(importer::get(sections[0], leaf));
        for (size_t i = 1; i < sections.size(); i++){
            if (importer::dump(importer::get(sections[i], leaf)) != first){
                mixed.insert(leaf);
                break;
            }
        }
    }
}

void ImporterForm::setValue(const std::string& key, const YAML::Node& value){
    importer::set(working, key, value);
    touched.insert(key);
    mixed.erase(key);
}

YAML::Node ImporterForm::merged(const YAML::Node& section) const{
    YAML::Node node = section && section.IsMap() ? YAML::Clone(section) : YAML::Node(YAML::NodeType::Map);
    for (const std::string& key : touched)
        importer::set(node, key, value(key));
    return node;
}

bool ImporterForm::comboField(const char* label, const std::string& key, const std::vector<std::string>& names,
                              const std::vector<std::string>& labels, const std::string& shown, const char* tooltip){
    rowLabel(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    const bool isMixed = mixed.count(key) > 0;
    size_t index = 0;
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == shown) index = i;
    bool changed = false;
    const std::string id = std::string("##") + key;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::BeginCombo(id.c_str(), isMixed ? "\xE2\x80\x94" : labels[index].c_str())){
        for (size_t i = 0; i < names.size(); i++){
            if (ImGui::Selectable(labels[i].c_str(), i == index && !isMixed)){
                setValue(key, YAML::Node(names[i]));
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (isMixed) ImGui::PopItemFlag();
    return changed;
}

bool ImporterForm::boolField(const char* label, const std::string& key, bool shown, const char* tooltip){
    rowLabel(label, tooltip);
    const bool isMixed = mixed.count(key) > 0;
    bool value = shown;
    const std::string id = std::string("##") + key;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool changed = ImGui::Checkbox(id.c_str(), &value);
    if (isMixed) ImGui::PopItemFlag();
    if (changed)
        setValue(key, YAML::Node(value));
    return changed;
}

bool ImporterForm::intSlider(const char* label, const std::string& key, int shown, int low, int high, const char* tooltip){
    rowLabel(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    int value = shown;
    const bool isMixed = mixed.count(key) > 0;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool changed = ImGui::SliderInt((std::string("##") + key).c_str(), &value, low, high);
    if (isMixed) ImGui::PopItemFlag();
    if (changed)
        setValue(key, YAML::Node(value));
    return changed;
}

bool ImporterForm::floatSlider(const char* label, const std::string& key, float shown, float low, float high, const char* format, const char* tooltip){
    rowLabel(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    float value = shown;
    const bool isMixed = mixed.count(key) > 0;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool changed = ImGui::SliderFloat((std::string("##") + key).c_str(), &value, low, high, format);
    if (isMixed) ImGui::PopItemFlag();
    if (changed)
        setValue(key, YAML::Node(value));
    return changed;
}

void ImporterPrompt::add(const void* owner, Pending pending){
    PromptState& s = promptState();
    for (auto& entry : s.views){
        if (entry.first == owner){
            entry.second = std::move(pending);
            return;
        }
    }
    s.views.emplace_back(owner, std::move(pending));
}

void ImporterPrompt::remove(const void* owner){
    PromptState& s = promptState();
    for (auto it = s.views.begin(); it != s.views.end(); ++it){
        if (it->first == owner){
            s.views.erase(it);
            return;
        }
    }
}

bool ImporterPrompt::hasPending(){
    for (const auto& [owner, pending] : promptState().views)
        if (pending.has && pending.has())
            return true;
    return false;
}

bool ImporterPrompt::guard(std::function<void()> then){
    if (!hasPending())
        return false;
    PromptState& s = promptState();
    s.then = std::move(then);
    s.requested = true;
    return true;
}

void ImporterPrompt::draw(){
    PromptState& s = promptState();
    if (s.requested){
        ImGui::OpenPopup("Unapplied Import Settings##importer_prompt");
        s.requested = false;
        s.open = true;
    }
    if (!s.open)
        return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!EditorModal::begin("Unapplied Import Settings##importer_prompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize)){
        s.open = false;
        return;
    }
    const ImporterPrompt::Pending* pending = nullptr;
    for (const auto& [owner, entry] : s.views)
        if (entry.has && entry.has()){
            pending = &entry;
            break;
        }
    if (!pending){
        // applied or reverted elsewhere meanwhile
        std::function<void()> then = std::move(s.then);
        s.then = nullptr;
        s.open = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (then) then();
        return;
    }
    ImGui::Text("Unapplied import settings for '%s'", pending->name ? pending->name().c_str() : "");
    ImGui::Spacing();
    std::function<void()> next;
    bool close = false;
    if (ImGui::Button("Apply")){
        if (pending->apply) pending->apply();
        next = std::move(s.then);
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")){
        if (pending->revert) pending->revert();
        next = std::move(s.then);
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")){
        close = true;
    }
    if (close){
        s.then = nullptr;
        s.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (next)
        next();
}
