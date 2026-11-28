// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_RUNNER_H
#define DIALOGUE_RUNNER_H

// Contract (docs/specs/dialogue-graphs.md R6-R10, R12, R17, D3, D7)
// - The state machine that plays a dialogue program. start(entry) runs entry, set, branch, action
//   and jump nodes without presenting until a line (onLine, state Line) or the end; advance()
//   follows a line's next (also the fallback when every option is hidden), choose(i) (1-based among
//   the presented options) runs that option's action and follows it; an action handler returning
//   "wait" (or an action node with `wait`) pauses (Waiting) until resume(); stop() ends without a
//   result. An end node, an unconnected output or stop fires onEnd and leaves the runner Ended.
//   More than 256 non-presenting nodes in one step is a `dialogue-loop` error: logged, stopped.
// - Handlers are game-wide, by prefix (Handlers); an unknown prefix warns once per prefix per run,
//   skips the action and makes the condition false. Overrides (the editor's walker, run_dialogue)
//   take every handler call instead: nothing registered runs.
// - Variants: `random` from the runner's splitmix64 stream (never the same variant twice in a row
//   with 2+), `sequence` by visit count, `once` each once then the last. The stream is consumed only
//   by random variants, random() and chance(): the same seed and choices give the same transcript
//   on every platform, so a walk predicts Play.
// - Texts: Localization's text of the key in the game language with the message arguments (start
//   args and setArg win over variables: `{gold}` reads $gold), else the source text formatted the
//   same way. Speakers show `speakers.<speaker>` when a table has it.
// - Hot reload: a running runner moves to the new program of its file (Programs listener) while
//   its current node exists (a presented line whose text or options changed is sent again), else
//   it stops with a warning naming the node. A table or language change sends a changed line again.
// - Steps reuse their buffers: a warm advance allocates nothing in C++ but the payload's strings.
// - Thread-safety: main thread.

#include "Export.h"
#include "asset/AssetRef.h"
#include "dialogue/DialogueProgram.h"
#include "localization/LocalizationArgs.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::dialogue {

    struct Option {
        int index = 0;              // 1-based among the presented options
        std::string id, key, text, style;
        std::vector<std::string> tags;
        bool enabled = true;
    };

    struct Line {
        std::string node, key, text, speaker, speakerName, mood, variant;
        std::vector<std::string> tags;
        AssetRef voice;
        std::vector<Option> options;
    };

    enum class RunnerState { Idle, Line, Waiting, Ended };
    JASPERENGINE_API const char* stateName(RunnerState state);

    class Runner;

    using ActionFn = std::function<bool(Runner& runner, std::string_view verb, std::span<const Value> args)>;   // true: wait
    using ConditionFn = std::function<Value(Runner& runner, std::string_view verb, std::span<const Value> args)>;

    // Game-wide handlers by prefix; no allocation until the first registration. Main thread.
    class JASPERENGINE_API Handlers {
    public:
        // a second registration of a prefix replaces the first and warns
        static void onAction(const std::string& prefix, ActionFn fn);
        static void onCondition(const std::string& prefix, ConditionFn fn);
        static void remove(const std::string& prefix);
        // at Stop (Engine::clearAllSubscriptions)
        static void clear();
        static const ActionFn* action(std::string_view prefix);
        static const ConditionFn* condition(std::string_view prefix);
        static bool registered();
    };

    // One handler call as the walker records it: "quest.start(\"intro\")"
    JASPERENGINE_API std::string describeCall(std::string_view name, std::span<const Value> args);

    class JASPERENGINE_API Runner {
    public:
        // What replaces the handlers (the editor's preview and walker): a condition's value (none:
        // false, listed as unknown) and an action seen (never run)
        struct Overrides {
            std::function<std::optional<Value>(std::string_view call, std::span<const Value> args)> condition;
            std::function<void(std::string_view call, std::span<const Value> args)> action;
        };
        // What one step did, for transcripts (recorded only with setRecording)
        struct StepRecord {
            std::vector<std::string> actions;                       // "quest.start(\"intro\")"
            std::vector<std::pair<std::string, Value>> assigned;    // $name = value
            std::vector<std::string> nodes;                         // every node entered
        };

        Runner();
        ~Runner();
        Runner(const Runner&) = delete;
        Runner& operator=(const Runner&) = delete;

        bool setDialogue(std::shared_ptr<const Program> program);
        const std::shared_ptr<const Program>& getDialogue() const { return program; }
        // `entry`: an entry's name ("" = `start`, else the lowest id); false (nothing runs) when
        // there is no dialogue or no such entry
        bool start(std::string_view entry = {}, const LocalizationArgs& args = {});
        bool advance();
        bool choose(int index);
        bool resume();
        void stop();
        void setArg(std::string name, LocalizationValue value);

        RunnerState getState() const { return state; }
        bool isRunning() const { return state == RunnerState::Line || state == RunnerState::Waiting; }
        // the presented line (state Line), or the last one shown; null before any
        const Line* getCurrentLine() const { return hasLine ? &line : nullptr; }
        // the line or waiting node's id; "" when idle or ended
        std::string getCurrentNode() const;
        // the result of the last end ("" for stop or an unconnected output)
        const std::string& getResult() const { return result; }

        // the stream's seed (0 = 0); the stream restarts from it
        void setSeed(uint64_t seed);
        uint64_t getSeed() const { return seed; }

        void setOverrides(std::shared_ptr<Overrides> overrides) { this->overrides = std::move(overrides); }
        void setRecording(bool recording) { this->recording = recording; }
        const StepRecord& lastStep() const { return record; }
        // handler calls of an unknown prefix (and every override that gave no value) since start
        const std::vector<std::string>& unknownCalls() const { return unknown; }
        // an error stopped the last step (dialogue-loop, a missing jump target)
        const std::string& getError() const { return error; }

        // the GameObject a component runner belongs to (Lua handlers receive its DialogueRunner)
        void setOwner(void* scene, uint32_t entity) { ownerScene = scene; ownerEntity = entity; }
        void* getOwnerScene() const { return ownerScene; }
        uint32_t getOwnerEntity() const { return ownerEntity; }

        // the events (the component forwards them to its FunctionSubscribe members)
        std::function<void(Runner&)> onStart;
        std::function<void(Runner&, const Line&)> onLine;
        std::function<void(Runner&, const std::string& result)> onEnd;
        std::function<void(Runner&, const std::string& name, const Value& value)> onVariableChanged;

    private:
        class Env;
        friend class Env;

        std::shared_ptr<const Program> program;
        RunnerState state = RunnerState::Idle;
        int current = -1;               // the presented line, or what a resume continues at
        int continuation = -1;
        uint64_t generation = 0;      // every start and end: a handler that restarted or stopped the runner ends the step it ran in
        Line line;
        bool hasLine = false;
        std::string result;
        std::string error;
        LocalizationArgs args;
        uint64_t seed = 0;
        uint64_t stream = 0;
        std::shared_ptr<Overrides> overrides;
        bool recording = false;
        StepRecord record;
        std::vector<std::string> unknown;
        std::vector<std::string> warnedPrefixes;
        std::vector<int> warnedNodes;
        std::vector<int16_t> lastVariant;   // per node: the random variant shown last (-1 none)
        std::vector<int> presented;         // the option index of each presented option
        std::vector<Value> stack;
        bool waitRequested = false;
        int evaluatingNode = -1;
        void* ownerScene = nullptr;
        uint32_t ownerEntity = 0;
        int programListener = 0;
        int variablesListener = 0;
        int localizationListener = 0;
        std::string scratch;            // a key or argument being built
        LocalizationArgs messageArgs;

        uint64_t nextRandom();
        double random01();
        void enter(int node);
        bool run(int node);
        bool runExpr(int node, const Expr& expr, Value* value);
        void present(int node);
        // `forcedVariant` >= 0: that variant (a refresh), else the node's mode picks one
        bool buildLine(int node, Line& out, int forcedVariant = -1);
        void formatText(std::string_view key, std::string_view source, std::string& out);
        void finish(const std::string& endResult);
        void warnOnce(int node, const std::string& message);
        void programReloaded(const std::shared_ptr<const Program>& reloaded);
        void refreshLine();
    };

}

#endif //DIALOGUE_RUNNER_H
