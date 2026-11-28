// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueWalker.h"

#include "dialogue/DialogueVariables.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <deque>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace dlg = jasperengine::dialogue;

namespace {

    // the store and the language as they were, put back when the walk ends
    class Sandbox {
    public:
        Sandbox(const dialogues::WalkOptions& options): keep(options.keepStore) {
            saved = dlg::Variables::toJson();
            if (!keep) dlg::Variables::reset();
            for (const auto& [name, value] : options.variables) dlg::Variables::set(name, value);
            if (!options.language.empty() && options.language != TitleArtLanguage::global()) {
                language = TitleArtLanguage::global();
                changedLanguage = TitleArtLanguage::setGlobal(options.language);
            }
        }
        ~Sandbox() {
            if (changedLanguage) TitleArtLanguage::setGlobal(language);
            if (!keep) dlg::Variables::fromJson(saved, nullptr, false);
        }
        Sandbox(const Sandbox&) = delete;
        Sandbox& operator=(const Sandbox&) = delete;

    private:
        nlohmann::ordered_json saved;
        std::string language;
        bool changedLanguage = false;
        bool keep = false;
    };

    struct Run {
        dialogues::WalkResult result;
        std::vector<std::string> undecided;     // conditions the walk took false for want of a value
        bool atMenu = false;                    // stopped at a menu with no choice left
        std::vector<int> enabledOptions;
        std::string menuKey;                    // node and variables at that menu
    };

    std::string variablesKey() {
        nlohmann::ordered_json values = dlg::Variables::toJson()["variables"];
        return values.dump();
    }

    Run runOnce(const std::shared_ptr<const dlg::Program>& program, const dialogues::WalkOptions& options, const std::map<std::string, bool>& assumed) {
        Run run;
        dialogues::WalkResult& out = run.result;
        Sandbox sandbox(options);
        dlg::Runner runner;
        runner.setSeed(options.seed);
        runner.setRecording(true);
        auto overrides = std::make_shared<dlg::Runner::Overrides>();
        std::vector<std::string>* undecided = &run.undecided;
        overrides->condition = [&options, &assumed, undecided](std::string_view call, std::span<const dlg::Value> args) -> std::optional<dlg::Value> {
            const std::string described = dlg::describeCall(call, args);
            const std::string name(call);
            if (auto it = options.conditions.find(described); it != options.conditions.end()) return it->second;
            if (auto it = options.conditions.find(name); it != options.conditions.end()) return it->second;
            if (auto it = options.conditions.find(name.substr(0, name.find('.')) + ".*"); it != options.conditions.end()) return it->second;
            if (auto it = assumed.find(described); it != assumed.end()) return dlg::Value(it->second);
            if (std::find(undecided->begin(), undecided->end(), described) == undecided->end()) undecided->push_back(described);
            return std::nullopt;
        };
        overrides->action = [](std::string_view, std::span<const dlg::Value>) {};
        runner.setOverrides(overrides);
        runner.setDialogue(program);
        auto absorb = [&](const dlg::Runner::StepRecord& step) {
            out.nodes.insert(out.nodes.end(), step.nodes.begin(), step.nodes.end());
        };
        if (!runner.start(options.entry)) {
            out.error = runner.getError();
            out.state = dlg::stateName(runner.getState());
            return run;
        }
        size_t choice = 0;
        int lines = 0;
        int resumes = 0;
        while (true) {
            absorb(runner.lastStep());
            if (runner.getState() == dlg::RunnerState::Waiting) {
                // an action node that waits: the walk goes on as a resume() would
                if (++resumes > options.maxSteps) {
                    out.truncated = true;
                    break;
                }
                runner.resume();
                continue;
            }
            if (runner.getState() != dlg::RunnerState::Line) break;
            const dlg::Line& line = *runner.getCurrentLine();
            dialogues::WalkStep step;
            step.node = line.node;
            step.speaker = line.speakerName;
            step.text = line.text;
            step.key = line.key;
            step.variant = line.variant;
            step.options = line.options;
            step.actions = runner.lastStep().actions;
            step.assigned = runner.lastStep().assigned;
            out.transcript.push_back(std::move(step));
            if (++lines > options.maxSteps) {
                out.truncated = true;
                break;
            }
            bool anyEnabled = false;
            for (const dlg::Option& option : line.options) anyEnabled = anyEnabled || option.enabled;
            if (options.stepwise) {
                if (choice >= options.steps.size()) {
                    run.atMenu = anyEnabled;
                    break;
                }
                const int step = options.steps[choice++];
                if (step == 0) {
                    if (anyEnabled || !runner.advance()) {
                        out.error = "advance at " + line.node + ", which waits for a choice";
                        break;
                    }
                } else {
                    out.transcript.back().chosen = step;
                    if (!runner.choose(step)) {
                        out.error = "option " + std::to_string(step) + " of " + line.node + " is not enabled";
                        break;
                    }
                }
                continue;
            }
            if (!anyEnabled) {
                runner.advance();
                continue;
            }
            if (choice >= options.choices.size()) {
                run.atMenu = true;
                for (const dlg::Option& option : line.options) {
                    if (option.enabled) run.enabledOptions.push_back(option.index);
                }
                run.menuKey = line.node + "|" + variablesKey();
                break;
            }
            const int picked = options.choices[choice++];
            out.transcript.back().chosen = picked;
            if (!runner.choose(picked)) {
                out.error = "choice " + std::to_string(picked) + " (choices[" + std::to_string(choice - 1) + "]) is not an enabled option of " + line.node +
                            ": it has " + std::to_string(line.options.size()) + " presented";
                break;
            }
        }
        out.state = dlg::stateName(runner.getState());
        if (out.state == std::string("idle")) out.state = "ended";
        out.node = runner.getCurrentNode();
        out.result = runner.getResult();
        if (runner.getState() == dlg::RunnerState::Ended) out.endActions = runner.lastStep().actions;
        if (out.error.empty()) out.error = runner.getError();
        out.unknownCalls = run.undecided;
        out.variables = dlg::Variables::all();
        return run;
    }

}

dialogues::WalkResult dialogues::walk(const std::shared_ptr<const dlg::Program>& program, const WalkOptions& options) {
    return runOnce(program, options, {}).result;
}

dialogues::ExploreResult dialogues::explore(const std::shared_ptr<const dlg::Program>& program, const WalkOptions& options, int maxPaths) {
    ExploreResult out;
    struct Item {
        std::vector<int> choices;
        std::map<std::string, bool> assumed;
    };
    std::deque<Item> queue;
    queue.push_back({options.choices, {}});
    std::set<std::string> covered, shown, expanded;
    std::map<std::string, std::set<std::string>> edges;    // menu -> menus or "#end"
    std::map<std::string, std::vector<int>> menuPath;
    std::map<std::string, std::map<std::string, bool>> menuAssumed;
    std::set<std::string> seenEndings;
    std::set<std::pair<std::vector<int>, std::map<std::string, bool>>> queued;
    while (!queue.empty()) {
        if (static_cast<int>(out.paths) >= maxPaths) {
            out.truncated = true;
            break;
        }
        Item item = std::move(queue.front());
        queue.pop_front();
        WalkOptions walkOptions = options;
        walkOptions.choices = item.choices;
        walkOptions.keepStore = false;
        Run run = runOnce(program, walkOptions, item.assumed);
        out.paths++;
        covered.insert(run.result.nodes.begin(), run.result.nodes.end());
        for (const WalkStep& step : run.result.transcript) shown.insert(step.node);
        // the menu this path started from (the last menu with a choice made) leads where it stopped
        std::string from;
        {
            std::vector<int> prefix = item.choices;
            if (!prefix.empty()) {
                prefix.pop_back();
                for (const auto& [key, path] : menuPath) {
                    // the menu this path chose at: the same choices before, assumptions it still holds
                    if (path != prefix) continue;
                    bool holds = true;
                    for (const auto& [call, value] : menuAssumed[key]) {
                        auto it = item.assumed.find(call);
                        holds = holds && it != item.assumed.end() && it->second == value;
                    }
                    if (holds) from = key;
                }
            }
        }
        // each condition with no value, the other way too
        for (const std::string& call : run.undecided) {
            Item flipped = item;
            flipped.assumed[call] = true;
            if (queued.insert({flipped.choices, flipped.assumed}).second) queue.push_back(std::move(flipped));
        }
        if (!run.result.error.empty()) {
            std::string choices;
            for (int c : item.choices) choices += (choices.empty() ? "" : ",") + std::to_string(c);
            out.errors.push_back(run.result.error + " (choices [" + choices + "])");
            continue;
        }
        if (run.atMenu) {
            // a menu reached under other assumptions is another menu: its options lead elsewhere
            for (const auto& [call, value] : item.assumed) run.menuKey += "|" + call + "=" + (value ? "1" : "0");
            if (!from.empty()) edges[from].insert(run.menuKey);
            if (expanded.insert(run.menuKey).second) {
                menuPath[run.menuKey] = item.choices;
                menuAssumed[run.menuKey] = item.assumed;
                for (int index : run.enabledOptions) {
                    Item next = item;
                    next.choices.push_back(index);
                    if (queued.insert({next.choices, next.assumed}).second) queue.push_back(std::move(next));
                }
            }
            continue;
        }
        if (run.result.state == "ended") {
            if (!from.empty()) edges[from].insert("#end");
            const std::string last = run.result.nodes.empty() ? std::string() : run.result.nodes.back();
            if (seenEndings.insert(run.result.result + "|" + last).second) {
                out.endings.push_back({run.result.result, last, item.choices, item.assumed});
            }
        }
    }
    // the menus from which no ending is reachable, among those fully expanded
    if (!out.truncated) {
        std::set<std::string> reachesEnd;
        bool grew = true;
        while (grew) {
            grew = false;
            for (const auto& [menu, targets] : edges) {
                if (reachesEnd.count(menu)) continue;
                for (const std::string& target : targets) {
                    if (target == "#end" || reachesEnd.count(target)) {
                        reachesEnd.insert(menu);
                        grew = true;
                        break;
                    }
                }
            }
        }
        std::set<std::string> loopNodes;
        std::vector<int> into;
        for (const std::string& menu : expanded) {
            if (reachesEnd.count(menu)) continue;
            loopNodes.insert(menu.substr(0, menu.find('|')));
            if (into.empty() || menuPath[menu].size() < into.size()) into = menuPath[menu];
        }
        if (!loopNodes.empty()) out.loops.push_back({std::vector<std::string>(loopNodes.begin(), loopNodes.end()), into});
    }
    out.nodes = program->nodes.size();
    for (size_t i = 0; i < program->nodes.size(); i++) {
        const std::string id(program->nodeId(static_cast<int>(i)));
        if (covered.count(id)) out.covered++;
        else out.uncovered.push_back(id);
        if (program->nodes[i].kind == dlg::NodeKind::Line && !shown.count(id)) out.linesNeverShown.push_back(id);
    }
    return out;
}

nlohmann::json dialogues::toJson(const dlg::Value& value) {
    if (value.isBool()) return value.asBool();
    if (value.isNumber()) return value.asNumber();
    if (value.isString()) return value.asString();
    return nullptr;
}

namespace {

    nlohmann::json optionsJson(const std::vector<dlg::Option>& options) {
        nlohmann::json list = nlohmann::json::array();
        for (const dlg::Option& option : options) {
            nlohmann::json item = {{"index", option.index}, {"id", option.id}, {"text", option.text}, {"key", option.key}};
            if (!option.enabled) item["enabled"] = false;
            if (!option.style.empty()) item["style"] = option.style;
            list.push_back(item);
        }
        return list;
    }

}

nlohmann::json dialogues::toJson(const WalkResult& result) {
    nlohmann::json transcript = nlohmann::json::array();
    for (const WalkStep& step : result.transcript) {
        nlohmann::json item = {{"node", step.node}, {"speaker", step.speaker}, {"text", step.text}, {"key", step.key}};
        if (!step.options.empty()) item["options"] = optionsJson(step.options);
        if (!step.actions.empty()) item["actions"] = step.actions;
        if (!step.assigned.empty()) {
            nlohmann::json assigned = nlohmann::json::object();
            for (const auto& [name, value] : step.assigned) assigned[name] = toJson(value);
            item["assigned"] = assigned;
        }
        if (step.chosen > 0) item["chosen"] = step.chosen;
        transcript.push_back(item);
    }
    nlohmann::json state = {{"state", result.state}};
    if (!result.node.empty()) state["node"] = result.node;
    if (result.state == "line" && !result.transcript.empty()) state["options"] = optionsJson(result.transcript.back().options);
    if (result.state == "ended") state["result"] = result.result;
    nlohmann::json variables = nlohmann::json::object();
    for (const auto& [name, value] : result.variables) variables[name] = toJson(value);
    nlohmann::json out = {{"transcript", transcript}, {"state", state}, {"variables", variables}};
    if (!result.endActions.empty()) out["end_actions"] = result.endActions;
    if (!result.unknownCalls.empty()) out["unknown_calls"] = result.unknownCalls;
    if (!result.error.empty()) out["error"] = result.error;
    if (result.truncated) out["truncated"] = true;
    return out;
}

nlohmann::json dialogues::toJson(const ExploreResult& result) {
    nlohmann::json endings = nlohmann::json::array();
    for (const ExploreResult::Ending& ending : result.endings) {
        nlohmann::json item = {{"result", ending.result}, {"node", ending.node}, {"choices", ending.choices}};
        if (!ending.assumed.empty()) item["assumed"] = ending.assumed;
        endings.push_back(item);
    }
    nlohmann::json loops = nlohmann::json::array();
    for (const ExploreResult::Loop& loop : result.loops) loops.push_back({{"nodes", loop.nodes}, {"choices", loop.choices}});
    nlohmann::json out = {{"coverage", {{"nodes", result.nodes}, {"covered", result.covered}}}, {"endings", endings}, {"no_exit_loops", loops},
                          {"lines_never_shown", result.linesNeverShown}, {"paths", result.paths}};
    if (!result.uncovered.empty()) out["coverage"]["uncovered"] = result.uncovered;
    if (!result.errors.empty()) out["errors"] = result.errors;
    if (result.truncated) out["truncated"] = true;
    return out;
}
