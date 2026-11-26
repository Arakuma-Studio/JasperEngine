// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueEditor.h"

#include "Project.h"
#include "command/type/GraphEditCmd.h"
#include "dialogue/DialogueProgram.h"
#include "dialogue/DialogueVariables.h"
#include "graph/GraphEditorHooks.h"
#include "graph/GraphRegistry.h"
#include "localization/StringTableCook.h"
#include "localization/StringTableDocument.h"
#include "localization/StringTableEditCmd.h"
#include "util/AssetDatabase.h"
#include "util/ProjectChecks.h"

#include "json.hpp"

#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace dlg = jasperengine::dialogue;

namespace {

    struct FileFacts {
        fs::file_time_type stamp;
        std::string guid;
        std::set<std::string> entries;
        std::set<std::string> nodes;
        struct Jump {
            std::string path;
            std::string guid;
            std::string target;
        };
        std::vector<Jump> jumps;
    };

    struct State {
        Project* project = nullptr;
        uint64_t graphListener = 0;
        std::map<fs::path, FileFacts> facts;    // by absolute path
    };

    State& state() {
        static State instance;
        return instance;
    }

    // a jump's or a component's path: asset-relative, as the runtime loads it (FileData)
    fs::path absoluteOf(Project* project, const std::string& path) {
        const fs::path given(path);
        if (given.is_absolute()) return given.lexically_normal();
        return project->resolveAssetPath(given).lexically_normal();
    }

    // the project's dialogues, read again when their file changed
    const std::map<fs::path, FileFacts>& projectFacts(Project* project) {
        State& s = state();
        std::set<fs::path> seen;
        for (const fs::path& owner : editor::l10n::findOwners(project->getProjectPath())) {
            if (owner.extension() != dlg::FILE_EXTENSION) continue;
            const fs::path file = owner.lexically_normal();
            seen.insert(file);
            std::error_code ec;
            const fs::file_time_type stamp = fs::last_write_time(file, ec);
            auto it = s.facts.find(file);
            if (it != s.facts.end() && it->second.stamp == stamp) continue;
            FileFacts facts;
            facts.stamp = stamp;
            std::string text;
            if (ProjectChecks::readFile(file, text)) {
                graph::LoadReport report;
                const graph::Graph g = graph::load(text, report);
                if (report.ok && g.kind == dlg::KIND_ID) {
                    if (auto guid = g.passthrough.find("guid"); guid != g.passthrough.end() && guid->is_string()) facts.guid = guid->get<std::string>();
                    for (const graph::Node& node : g.nodes) {
                        facts.nodes.insert(node.id);
                        if (node.type == "entry") facts.entries.insert(node.params.value("name", std::string()));
                        if (node.type != "jump") continue;
                        auto target = node.params.find("dialogue");
                        if (target == node.params.end() || !target->is_object() || target->value("path", std::string()).empty()) continue;
                        facts.jumps.push_back({target->value("path", std::string()), target->value("guid", std::string()), node.params.value("entry", std::string("start"))});
                    }
                }
            }
            s.facts[file] = std::move(facts);
        }
        for (auto it = s.facts.begin(); it != s.facts.end();) {
            if (!seen.count(it->first)) it = s.facts.erase(it);
            else ++it;
        }
        return s.facts;
    }

    std::map<std::string, std::string> sourceTexts(const std::string& text) {
        std::map<std::string, std::string> out;
        graph::LoadReport report;
        const graph::Graph g = graph::load(text, report);
        if (!report.ok) return out;
        for (const dlg::SourceEntry& entry : dlg::sourceEntries(g)) out[entry.key] = entry.text;
        return out;
    }

}

void dialogues::install(Project* project) {
    uninstall();
    dlg::registerDialogueKind();
    State& s = state();
    s.project = project;
    GraphEditorHooks::KindEdits edits;
    edits.companions = [](Project* owner, const fs::path& file, const std::string& before, const std::string& after) {
        std::vector<std::unique_ptr<Command>> out;
        if (std::unique_ptr<Command> step = reviewStep(owner, file, before, after)) out.push_back(std::move(step));
        return out;
    };
    edits.created = [](Project* owner, const fs::path& file, graph::Graph& created) { prepareNew(owner, file, created, false); };
    graphEditorHooks().kindEdits[dlg::KIND_ID] = edits;
    dlg::setLintContextProvider([]() { return state().project ? lintContext(state().project) : nullptr; });
    // the editor's own writes: running conversations and the editor's Localization follow them
    s.graphListener = GraphEditCmd::addChangeListener([](const fs::path& file) {
        if (file.extension() != dlg::FILE_EXTENSION) return;
        dlg::Programs::fileChanged(file.string());
        if (state().project) l10n::refreshEditorLocalization(state().project);
    });
}

void dialogues::uninstall() {
    State& s = state();
    if (s.graphListener) GraphEditCmd::removeChangeListener(s.graphListener);
    s.graphListener = 0;
    graphEditorHooks().kindEdits.erase(dlg::KIND_ID);
    dlg::setLintContextProvider(nullptr);
    s.project = nullptr;
    s.facts.clear();
}

std::vector<std::pair<std::string, dlg::Value>> dialogues::projectDefaults(const DialogueSettings& settings) {
    std::vector<std::pair<std::string, dlg::Value>> out;
    for (const DialogueSettings::Variable& variable : settings.variables) {
        if (variable.name.empty()) continue;
        if (variable.type == "string") {
            out.emplace_back(variable.name, dlg::Value(variable.defaultValue));
        } else if (variable.type == "bool") {
            out.emplace_back(variable.name, dlg::Value(variable.defaultValue == "true" || variable.defaultValue == "1"));
        } else {
            const nlohmann::json number = nlohmann::json::parse(variable.defaultValue, nullptr, false);
            out.emplace_back(variable.name, dlg::Value(number.is_number() ? number.get<double>() : 0.0));
        }
    }
    return out;
}

std::string dialogues::settingsFileText(const DialogueSettings& settings) {
    if (settings.variables.empty()) return {};
    nlohmann::ordered_json variables = nlohmann::ordered_json::object();
    for (const auto& [name, value] : projectDefaults(settings)) {
        if (value.isBool()) variables[name] = value.asBool();
        else if (value.isNumber()) variables[name] = value.asNumber();
        else variables[name] = value.toText();
    }
    const nlohmann::ordered_json file = {{"format", dlg::Variables::SETTINGS_FORMAT}, {"version", 1}, {"variables", variables}};
    return file.dump(2) + "\n";
}

void dialogues::preparePlay(Project* project) {
    dlg::Variables::reset();
    dlg::Variables::setProjectDefaults(projectDefaults(project->getDialogueSettings()));
    dlg::Programs::clear();
}

std::unique_ptr<Command> dialogues::reviewStep(Project* project, const fs::path& file, const std::string& before, const std::string& after) {
    const std::map<std::string, std::string> old = sourceTexts(before);
    const std::map<std::string, std::string> now = sourceTexts(after);
    std::set<std::string> changed;
    for (const auto& [key, text] : now) {
        auto it = old.find(key);
        if (it != old.end() && it->second != text) changed.insert(key);
    }
    if (changed.empty()) return nullptr;
    std::vector<StringTableEditCmd::FileChange> changes;
    for (const auto& [language, path] : l10n::translationSiblings(file)) {
        std::string text;
        l10n::TranslationDoc doc;
        if (!ProjectChecks::readFile(path, text) || !l10n::parseTranslation(text, doc)) continue;
        bool marked = false;
        for (const std::string& key : changed) {
            auto entry = doc.entries.find(key);
            if (entry == doc.entries.end() || entry->second.status == "needs_review") continue;
            entry->second.status = "needs_review";
            marked = true;
        }
        if (!marked) continue;
        StringTableEditCmd::FileChange change;
        change.path = path;
        change.before = text;
        change.after = l10n::saveTranslation(doc);
        changes.push_back(std::move(change));
    }
    if (changes.empty()) return nullptr;
    return std::make_unique<StringTableEditCmd>(std::move(changes), "Translations Need Review", std::string(), project);
}

void dialogues::prepareNew(Project* project, const fs::path& file, graph::Graph& g, bool starter) {
    if (!g.passthrough.contains("guid")) g.passthrough["guid"] = AssetDatabase::generateGuid();
    if (!g.metadata.contains("namespace")) {
        const std::string stem = dlg::namespaceFromStem(file.stem().string());
        std::string ns = stem;
        for (int n = 2; project && !l10n::namespaceOwner(project->getProjectPath(), ns, file).empty(); n++) ns = stem + "_" + std::to_string(n);
        g.metadata["namespace"] = ns;
    }
    if (!g.metadata.contains("source_language")) {
        g.metadata["source_language"] = project ? project->getLocalizationSettings().effectiveDefault() : std::string("en");
    }
    if (!starter || !g.nodes.empty()) return;
    graph::apply(g, graph::opFromJson({{"op", "addNode"}, {"node", {{"type", "entry"}, {"position", {0.0, 0.0}}, {"params", {{"name", "start"}}}}}}));
    graph::apply(g, graph::opFromJson({{"op", "addNode"}, {"node", {{"type", "line"}, {"position", {280.0, 0.0}}}}}));
    graph::apply(g, graph::opFromJson({{"op", "connect"}, {"from", {{"node", g.nodes[0].id}, {"port", "next"}}}, {"to", {{"node", g.nodes[1].id}, {"port", "in"}}}}));
}

std::shared_ptr<const dlg::LintContext> dialogues::lintContext(Project* project) {
    auto context = std::make_shared<dlg::LintContext>();
    const DialogueSettings& settings = project->getDialogueSettings();
    for (const DialogueSettings::Prefix& prefix : settings.actionPrefixes) context->actionPrefixes.insert(prefix.name);
    for (const DialogueSettings::Prefix& prefix : settings.conditionPrefixes) context->conditionPrefixes.insert(prefix.name);
    for (const DialogueSettings::Variable& variable : settings.variables) context->variables.insert(variable.name);
    const std::map<fs::path, FileFacts> facts = projectFacts(project);
    const fs::path root = project->getAssetsPath();
    context->jumpTarget = [facts, root](const std::string& path, const std::string& target) -> std::string {
        const fs::path file = (fs::path(path).is_absolute() ? fs::path(path) : root / path).lexically_normal();
        auto it = facts.find(file);
        if (it == facts.end()) return path + " is no dialogue of the project";
        if (target.empty() ? it->second.entries.empty() : (!it->second.entries.count(target) && !it->second.nodes.count(target))) {
            return path + " has no entry or node '" + target + "'";
        }
        return {};
    };
    context->jumpedInto = [facts, root](const std::string& guid) {
        std::set<std::string> out;
        for (const auto& [file, info] : facts) {
            for (const FileFacts::Jump& jump : info.jumps) {
                bool into = !jump.guid.empty() && jump.guid == guid;
                if (!into) {
                    auto target = facts.find((root / jump.path).lexically_normal());
                    into = target != facts.end() && target->second.guid == guid;
                }
                if (into) out.insert(jump.target);
            }
        }
        return out;
    };
    return context;
}

std::vector<std::string> dialogues::entriesOf(Project* project, const std::string& path) {
    const std::map<fs::path, FileFacts>& facts = projectFacts(project);
    auto it = facts.find(absoluteOf(project, path));
    if (it == facts.end()) return {};
    return std::vector<std::string>(it->second.entries.begin(), it->second.entries.end());
}
