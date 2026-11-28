// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueRunner.h"

#include "Log.h"
#include "dialogue/DialogueVariables.h"
#include "localization/Localization.h"
#include "localization/MessageFormat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

using namespace jasperengine;

namespace jasperengine::dialogue {

namespace {

    // a node may run this many nodes that present nothing in one step (R7)
    constexpr int MAX_SILENT_NODES = 256;

    struct HandlerTables {
        std::map<std::string, ActionFn, std::less<>> actions;
        std::map<std::string, ConditionFn, std::less<>> conditions;
    };

    // allocated on the first registration (R18)
    std::unique_ptr<HandlerTables>& handlerTables() {
        static std::unique_ptr<HandlerTables> tables;
        return tables;
    }

    HandlerTables& ensureTables() {
        std::unique_ptr<HandlerTables>& tables = handlerTables();
        if (!tables) tables = std::make_unique<HandlerTables>();
        return *tables;
    }

    LocalizationValue toArgument(const Value& value) {
        if (value.isNumber()) {
            const double number = value.asNumber();
            if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 9.0e15) return static_cast<int64_t>(number);
            return number;
        }
        return value.toText();
    }

    Value fromArgument(const LocalizationValue& value) {
        if (const std::string* text = std::get_if<std::string>(&value)) return Value(*text);
        if (const int64_t* integer = std::get_if<int64_t>(&value)) return Value(static_cast<double>(*integer));
        return Value(std::get<double>(value));
    }

    void assignArgument(LocalizationArgs& list, size_t& count, std::string_view name, LocalizationValue value) {
        if (count < list.size()) {
            list[count].first.assign(name.data(), name.size());
            list[count].second = std::move(value);
        } else {
            list.emplace_back(std::string(name), std::move(value));
        }
        count++;
    }

    bool sameOptions(const std::vector<Option>& a, const std::vector<Option>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].id != b[i].id || a[i].text != b[i].text || a[i].enabled != b[i].enabled || a[i].style != b[i].style) return false;
        }
        return true;
    }

}

const char* stateName(RunnerState state) {
    switch (state) {
        case RunnerState::Line: return "line";
        case RunnerState::Waiting: return "waiting";
        case RunnerState::Ended: return "ended";
        default: return "idle";
    }
}

std::string describeCall(std::string_view name, std::span<const Value> args) {
    std::string out(name);
    out += "(";
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) out += ", ";
        out += args[i].describe();
    }
    out += ")";
    return out;
}

// ------------------------------------------------------------------ handlers

void Handlers::onAction(const std::string& prefix, ActionFn fn) {
    HandlerTables& tables = ensureTables();
    if (tables.actions.count(prefix)) Log::warn("Dialogue.onAction: the handler of '%s' was replaced", prefix.c_str());
    tables.actions[prefix] = std::move(fn);
}

void Handlers::onCondition(const std::string& prefix, ConditionFn fn) {
    HandlerTables& tables = ensureTables();
    if (tables.conditions.count(prefix)) Log::warn("Dialogue.onCondition: the handler of '%s' was replaced", prefix.c_str());
    tables.conditions[prefix] = std::move(fn);
}

void Handlers::remove(const std::string& prefix) {
    if (std::unique_ptr<HandlerTables>& tables = handlerTables()) {
        tables->actions.erase(prefix);
        tables->conditions.erase(prefix);
    }
}

void Handlers::clear() {
    handlerTables().reset();
}

const ActionFn* Handlers::action(std::string_view prefix) {
    const std::unique_ptr<HandlerTables>& tables = handlerTables();
    if (!tables) return nullptr;
    auto it = tables->actions.find(prefix);
    return it == tables->actions.end() ? nullptr : &it->second;
}

const ConditionFn* Handlers::condition(std::string_view prefix) {
    const std::unique_ptr<HandlerTables>& tables = handlerTables();
    if (!tables) return nullptr;
    auto it = tables->conditions.find(prefix);
    return it == tables->conditions.end() ? nullptr : &it->second;
}

bool Handlers::registered() {
    return handlerTables() != nullptr;
}

// ------------------------------------------------------------------ the expression environment

class Runner::Env final : public ExprEnv {
public:
    explicit Env(Runner& runner): runner(runner) {}

    const std::string& nameOf(uint32_t index) const { return runner.program->exprs.names[index]; }

    Value variable(uint32_t index) override {
        return Variables::read(nameOf(index), runner.program.get());
    }

    Value argument(uint32_t index) override {
        const std::string& name = nameOf(index);
        for (const auto& [argName, value] : runner.args) {
            if (argName == name) return fromArgument(value);
        }
        return Variables::read(name, runner.program.get());
    }

    void assign(uint32_t index, const Value& value) override {
        if (runner.recording) runner.record.assigned.emplace_back(nameOf(index), value);
        Variables::set(nameOf(index), value);
    }

    uint32_t visits(uint32_t index) override {
        return Variables::visits(runner.program->identity(), nameOf(index));
    }

    double random01() override {
        return runner.random01();
    }

    Value callCondition(const CallSite& call, std::span<const Value> args) override {
        if (runner.overrides) {
            std::optional<Value> value;
            if (runner.overrides->condition) value = runner.overrides->condition(call.name, args);
            if (value) return *value;
            noteUnknown(describeCall(call.name, args));
            return Value(false);
        }
        if (const ConditionFn* fn = Handlers::condition(call.prefix)) return (*fn)(runner, call.verb, args);
        unknownPrefix(call, args, "condition");
        return Value(false);
    }

    void callAction(const CallSite& call, std::span<const Value> args) override {
        if (runner.recording) runner.record.actions.push_back(describeCall(call.name, args));
        if (runner.overrides) {
            if (runner.overrides->action) runner.overrides->action(call.name, args);
            return;
        }
        if (const ActionFn* fn = Handlers::action(call.prefix)) {
            if ((*fn)(runner, call.verb, args)) runner.waitRequested = true;
            return;
        }
        unknownPrefix(call, args, "action");
    }

private:
    Runner& runner;

    void noteUnknown(std::string call) {
        if (std::find(runner.unknown.begin(), runner.unknown.end(), call) == runner.unknown.end()) runner.unknown.push_back(std::move(call));
    }

    void unknownPrefix(const CallSite& call, std::span<const Value> args, const char* what) {
        noteUnknown(describeCall(call.name, args));
        const std::string key = std::string(what) + ":" + call.prefix;
        if (std::find(runner.warnedPrefixes.begin(), runner.warnedPrefixes.end(), key) != runner.warnedPrefixes.end()) return;
        runner.warnedPrefixes.push_back(key);
        Log::warn("Dialogue: no %s handler for '%s' (%s in %s): Dialogue.on%s(\"%s\", ...) registers one", what, call.prefix.c_str(),
                  call.name.c_str(), runner.program->path.c_str(), std::string(what) == "action" ? "Action" : "Condition", call.prefix.c_str());
    }
};

// ------------------------------------------------------------------ runner

Runner::Runner() {
    stack.reserve(16);
    programListener = Programs::addListener([this](const std::shared_ptr<const Program>& reloaded) { programReloaded(reloaded); });
    variablesListener = Variables::addListener([this](const std::string& name, const Value& value) {
        if (onVariableChanged && program && program->usesVariable(name)) onVariableChanged(*this, name, value);
    });
    localizationListener = Localization::addListener([this](const std::string&) { refreshLine(); });
}

Runner::~Runner() {
    Programs::removeListener(programListener);
    Variables::removeListener(variablesListener);
    Localization::removeListener(localizationListener);
}

uint64_t Runner::nextRandom() {
    // splitmix64: integer-only, the same numbers on every platform
    uint64_t z = (stream += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double Runner::random01() {
    return static_cast<double>(nextRandom() >> 11) * (1.0 / 9007199254740992.0);
}

void Runner::setSeed(uint64_t value) {
    seed = value;
    stream = value;
}

bool Runner::setDialogue(std::shared_ptr<const Program> next) {
    if (isRunning()) finish(std::string());
    program = std::move(next);
    lastVariant.assign(program ? program->nodes.size() : 0, -1);
    hasLine = false;
    state = RunnerState::Idle;
    current = -1;
    return program != nullptr;
}

std::string Runner::getCurrentNode() const {
    if (!program || current < 0 || !isRunning()) return {};
    return std::string(program->nodeId(current));
}

void Runner::setArg(std::string name, LocalizationValue value) {
    for (auto& [argName, argValue] : args) {
        if (argName == name) {
            argValue = std::move(value);
            return;
        }
    }
    args.emplace_back(std::move(name), std::move(value));
}

bool Runner::start(std::string_view entry, const LocalizationArgs& startArgs) {
    if (!program) {
        error = "the runner has no dialogue";
        Log::warn("Dialogue: start() without a dialogue");
        return false;
    }
    const int at = entry.empty() ? program->defaultEntry() : program->findEntry(entry);
    if (at < 0) {
        error = entry.empty() ? program->path + " has no entry node" : program->path + " has no entry '" + std::string(entry) + "'";
        Log::warn("Dialogue: %s", error.c_str());
        return false;
    }
    if (isRunning()) finish(std::string());
    generation++;
    args = startArgs;
    error.clear();
    result.clear();
    unknown.clear();
    warnedPrefixes.clear();
    warnedNodes.clear();
    record = StepRecord();
    hasLine = false;
    state = RunnerState::Waiting;
    current = at;
    const uint64_t started = generation;
    if (onStart) onStart(*this);
    if (generation != started) return true;
    return run(at);
}

bool Runner::advance() {
    if (state != RunnerState::Line || current < 0) return false;
    // a line with an enabled option waits for a choice; with none (all hidden or disabled) it goes on
    for (const Option& option : line.options) {
        if (option.enabled) return false;
    }
    record = StepRecord();
    state = RunnerState::Waiting;
    return run(program->nodes[static_cast<size_t>(current)].next);
}

bool Runner::choose(int index) {
    if (state != RunnerState::Line || current < 0 || index < 1 || index > static_cast<int>(presented.size())) return false;
    if (!line.options[static_cast<size_t>(index - 1)].enabled) return false;
    const ProgramNode& node = program->nodes[static_cast<size_t>(current)];
    const ProgramOption& option = program->options[node.optionsFirst + static_cast<uint32_t>(presented[static_cast<size_t>(index - 1)])];
    record = StepRecord();
    scratch.assign(program->nodeId(current)).append(".").append(program->str(option.id));
    Variables::addVisit(program->identity(), scratch);
    state = RunnerState::Waiting;
    const int target = option.target;
    const int from = current;
    const uint64_t chosen = generation;
    waitRequested = false;
    runExpr(from, option.action, nullptr);
    if (generation != chosen) return true;
    if (waitRequested) {
        continuation = target;
        return true;
    }
    return run(target);
}

bool Runner::resume() {
    if (state != RunnerState::Waiting) return false;
    record = StepRecord();
    return run(continuation);
}

void Runner::stop() {
    if (isRunning()) finish(std::string());
}

void Runner::finish(const std::string& endResult) {
    generation++;
    state = RunnerState::Ended;
    current = -1;
    continuation = -1;
    result = endResult;
    if (onEnd) onEnd(*this, result);
}

void Runner::warnOnce(int node, const std::string& message) {
    if (std::find(warnedNodes.begin(), warnedNodes.end(), node) != warnedNodes.end()) return;
    warnedNodes.push_back(node);
    Log::warn("Dialogue: %s, node %s: %s", program->path.c_str(), std::string(program->nodeId(node)).c_str(), message.c_str());
}

void Runner::enter(int node) {
    const std::string_view id = program->nodeId(node);
    Variables::addVisit(program->identity(), id);
    if (recording) record.nodes.emplace_back(id);
}

bool Runner::runExpr(int node, const Expr& expr, Value* value) {
    if (expr.empty()) {
        if (value) *value = Value(true);
        return true;
    }
    Env env(*this);
    Value out;
    std::string failure;
    evaluatingNode = node;
    // keep the program alive while a handler may reload or replace it
    const std::shared_ptr<const Program> keep = program;
    const bool ok = evaluate(keep->exprs, expr, env, stack, out, &failure);
    evaluatingNode = -1;
    if (!ok) {
        warnOnce(node, failure);
        if (value) *value = Value(false);
        return false;
    }
    if (value) *value = std::move(out);
    return true;
}

bool Runner::run(int node) {
    const uint64_t running = generation;
    int silent = 0;
    int at = node;
    while (true) {
        if (at < 0) {
            finish(std::string());
            return true;
        }
        const ProgramNode& n = program->nodes[static_cast<size_t>(at)];
        if (n.kind != NodeKind::Line && ++silent > MAX_SILENT_NODES) {
            error = "dialogue-loop: more than " + std::to_string(MAX_SILENT_NODES) + " nodes without a line, at " + std::string(program->nodeId(at));
            Log::error("Dialogue: %s: %s; the conversation stops", program->path.c_str(), error.c_str());
            finish(std::string());
            return false;
        }
        enter(at);
        switch (n.kind) {
            case NodeKind::Entry:
                at = n.next;
                break;
            case NodeKind::Set:
                runExpr(at, n.expr, nullptr);
                if (generation != running) return true;
                at = n.next;
                break;
            case NodeKind::Branch: {
                Value value;
                const bool ok = runExpr(at, n.expr, &value);
                if (generation != running) return true;
                at = ok && value.truthy() ? n.next : n.alt;
                break;
            }
            case NodeKind::Action: {
                waitRequested = false;
                const int next = n.next;
                const bool waits = n.wait;
                runExpr(at, n.expr, nullptr);
                if (generation != running) return true;
                if (waitRequested || waits) {
                    state = RunnerState::Waiting;
                    current = at;
                    continuation = next;
                    return true;
                }
                at = next;
                break;
            }
            case NodeKind::Jump: {
                const std::string path(program->str(n.b));
                const std::string targetName(program->str(n.a));
                std::shared_ptr<const Program> target = program;
                std::string loadError;
                if (!path.empty() && path != program->path) target = Programs::load(path, &loadError);
                const int index = target ? target->findTarget(targetName) : -1;
                if (index < 0) {
                    error = "jump-target: " + std::string(program->nodeId(at)) + " jumps to " + (path.empty() ? program->path : path) + " '" +
                            targetName + "', which " + (target ? "has no such entry or node" : "does not load (" + loadError + ")");
                    Log::error("Dialogue: %s; the conversation stops", error.c_str());
                    finish(std::string());
                    return false;
                }
                if (target != program) {
                    program = target;
                    lastVariant.assign(program->nodes.size(), -1);
                }
                at = index;
                break;
            }
            case NodeKind::End:
                finish(std::string(program->str(n.a)));
                return true;
            case NodeKind::Line:
                present(at);
                return true;
            case NodeKind::Unknown:
                warnOnce(at, "a node of an unknown type ends the conversation");
                finish(std::string());
                return true;
        }
    }
}

void Runner::present(int node) {
    const uint64_t presenting = generation;
    const ProgramNode& n = program->nodes[static_cast<size_t>(node)];
    // the line's action runs as it is shown (a "wait" from it is ignored: the line waits anyway)
    runExpr(node, n.expr, nullptr);
    if (generation != presenting) return;
    buildLine(node, line);
    hasLine = true;
    state = RunnerState::Line;
    current = node;
    if (onLine) onLine(*this, line);
}

void Runner::formatText(std::string_view key, std::string_view source, std::string& out) {
    size_t count = 0;
    // the arguments the text names: start args and setArg first, then variables
    for (size_t open = source.find('{'); open != std::string_view::npos; open = source.find('{', open + 1)) {
        size_t begin = open + 1;
        while (begin < source.size() && source[begin] == ' ') begin++;
        size_t end = begin;
        while (end < source.size() && (std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_')) end++;
        if (end == begin) continue;
        const std::string_view name = source.substr(begin, end - begin);
        bool known = false;
        for (size_t i = 0; i < count; i++) known = known || messageArgs[i].first == name;
        if (known) continue;
        const LocalizationValue* given = nullptr;
        for (const auto& [argName, value] : args) {
            if (argName == name) given = &value;
        }
        if (given) {
            assignArgument(messageArgs, count, name, *given);
            continue;
        }
        const Value variable = Variables::read(name, program.get());
        if (!variable.isNil()) assignArgument(messageArgs, count, name, toArgument(variable));
    }
    messageArgs.resize(count);
    out.clear();
    if (Localization::isActive() && Localization::translate(key, messageArgs, out)) return;
    out.clear();
    // into `out` itself: a warm step reuses its capacity
    if (l10n::MessageFormat::isPlain(source)) {
        out.assign(source.data(), source.size());
        return;
    }
    l10n::FormatOptions options;
    const std::string& language = Localization::getLanguage();
    // (each branch a view of a string that outlives the call: "en" : sourceLanguage would be a temporary std::string)
    options.language = !language.empty() ? std::string_view(language)
                     : program->sourceLanguage.empty() ? std::string_view("en") : std::string_view(program->sourceLanguage);
    options.args = &messageArgs;
    options.showMissing = true;
    l10n::MessageFormat::format(source, options, out);
}

bool Runner::buildLine(int node, Line& out, int forcedVariant) {
    const ProgramNode& n = program->nodes[static_cast<size_t>(node)];
    const std::string_view id = program->nodeId(node);
    out.node.assign(id);
    out.speaker.assign(program->str(n.a));
    out.mood.assign(program->str(n.b));
    out.tags.resize(n.tagsCount);
    for (uint16_t i = 0; i < n.tagsCount; i++) out.tags[i].assign(program->str(program->tags[n.tagsFirst + i]));
    out.voice = AssetRef(std::string(program->str(n.voice)));
    scratch.assign("speakers.").append(out.speaker);
    out.speakerName.clear();
    if (out.speaker.empty() || !Localization::has(scratch) || !Localization::translate(scratch, {}, out.speakerName)) out.speakerName = out.speaker;

    // the variant: 0 is the line's own text, k the k-th of `variants`
    const int pool = 1 + n.variantsCount;
    int chosen = 0;
    if (forcedVariant >= 0) {
        chosen = std::min(forcedVariant, pool - 1);
    } else if (pool > 1) {
        const uint32_t visits = std::max<uint32_t>(1, Variables::visits(program->identity(), id));
        switch (n.variantMode) {
            case VariantMode::Sequence: chosen = static_cast<int>((visits - 1) % static_cast<uint32_t>(pool)); break;
            case VariantMode::Once: chosen = static_cast<int>(std::min<uint32_t>(visits - 1, static_cast<uint32_t>(pool - 1))); break;
            case VariantMode::Random: {
                const int last = lastVariant[static_cast<size_t>(node)];
                if (last < 0) {
                    chosen = static_cast<int>(((nextRandom() >> 32) * static_cast<uint64_t>(pool)) >> 32);
                } else {
                    chosen = static_cast<int>(((nextRandom() >> 32) * static_cast<uint64_t>(pool - 1)) >> 32);
                    if (chosen >= last) chosen++;
                }
                break;
            }
        }
        lastVariant[static_cast<size_t>(node)] = static_cast<int16_t>(chosen);
    }
    std::string_view source = program->str(n.text);
    out.key.assign(program->ns).append(".").append(id);
    out.variant.clear();
    if (chosen > 0) {
        const ProgramVariant& variant = program->variants[n.variantsFirst + static_cast<uint32_t>(chosen - 1)];
        source = program->str(variant.text);
        out.variant.assign(program->str(variant.id));
        out.key.append(".").append(out.variant);
    }
    formatText(out.key, source, out.text);

    // the options after their conditions: hidden ones and once options chosen before left out
    presented.clear();
    size_t count = 0;
    const uint64_t building = generation;
    for (uint16_t k = 0; k < n.optionsCount; k++) {
        const ProgramOption& option = program->options[n.optionsFirst + k];
        if (option.once) {
            scratch.assign(id).append(".").append(program->str(option.id));
            if (Variables::visits(program->identity(), scratch) > 0) continue;
        }
        Value value;
        const bool ok = runExpr(node, option.condition, &value);
        if (generation != building) return false;
        const bool enabled = ok && value.truthy();
        if (!enabled && !option.disable) continue;
        if (count >= out.options.size()) out.options.emplace_back();
        Option& o = out.options[count++];
        o.index = static_cast<int>(count);
        o.id.assign(program->str(option.id));
        o.key.assign(program->ns).append(".").append(id).append(".").append(o.id);
        formatText(o.key, program->str(option.text), o.text);
        o.style.assign(program->str(option.style));
        o.tags.resize(option.tagsCount);
        for (uint16_t i = 0; i < option.tagsCount; i++) o.tags[i].assign(program->str(program->tags[option.tagsFirst + i]));
        o.enabled = enabled;
        presented.push_back(k);
    }
    out.options.resize(count);
    return true;
}

void Runner::refreshLine() {
    if (state != RunnerState::Line || current < 0 || !program) return;
    const ProgramNode& n = program->nodes[static_cast<size_t>(current)];
    // the same variant again: a refresh draws nothing from the stream and counts no visit
    int keep = 0;
    for (uint16_t i = 0; i < n.variantsCount; i++) {
        if (program->str(program->variants[n.variantsFirst + i].id) == line.variant) keep = i + 1;
    }
    Line rebuilt;
    const uint64_t refreshing = generation;
    buildLine(current, rebuilt, keep);
    if (generation != refreshing) return;
    if (rebuilt.text == line.text && rebuilt.speakerName == line.speakerName && sameOptions(rebuilt.options, line.options)) return;
    line = std::move(rebuilt);
    if (onLine) onLine(*this, line);
}

void Runner::programReloaded(const std::shared_ptr<const Program>& reloaded) {
    if (!program || reloaded == program || reloaded->path != program->path) return;
    if (!isRunning()) {
        program = reloaded;
        lastVariant.assign(program->nodes.size(), -1);
        return;
    }
    const std::string node(program->nodeId(current));
    const std::string after = continuation >= 0 ? std::string(program->nodeId(continuation)) : std::string();
    const int index = reloaded->findNode(node);
    std::vector<int16_t> variants(reloaded->nodes.size(), -1);
    for (size_t i = 0; i < program->nodes.size(); i++) {
        const int moved = reloaded->findNode(program->nodeId(static_cast<int>(i)));
        if (moved >= 0) variants[static_cast<size_t>(moved)] = lastVariant[i];
    }
    program = reloaded;
    lastVariant = std::move(variants);
    if (index < 0) {
        Log::warn("Dialogue: %s was edited while playing and its node %s is gone: the conversation stops", program->path.c_str(), node.c_str());
        finish(std::string());
        return;
    }
    current = index;
    continuation = after.empty() ? -1 : program->findNode(after);
    if (state == RunnerState::Line) refreshLine();
}

}  // namespace jasperengine::dialogue
