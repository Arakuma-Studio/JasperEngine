// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef CONSOLECOMMANDS_H
#define CONSOLECOMMANDS_H

// The console's commands (docs/specs/dev-console-for-agents.md): typed arguments, usage, aliases,
// a structured result, completion and a cheat flag. Plain C++ in every player, HTML UI or not: the
// debug overlay's command field, the game's scripts (DebugOverlay.executeCommand), MCP's
// eval_lua {command} and the player's --command all run lines through execute().
// Nothing is allocated until the first registration or execution, and nothing runs per frame.
// Main thread only.

#include "Export.h"
#include "ecs/Entity.h"
#include "log/LogCategory.h"
#include "math/Vector3.h"

#include "json.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;
    // the event type is defined in util/FunctionSubscribe.h
    template<typename T> class FunctionSubscribe;

}

namespace jasperengine::console {

    enum class ArgType { String, Int, Float, Bool, Enum, Vector3, GameObject, Rest };

    struct ArgSpec {
        std::string name;
        ArgType type = ArgType::String;
        bool optional = false;
        std::string defaultValue;                  // parsed like typed text ("" = none)
        double min = -std::numeric_limits<double>::infinity();   // Int, Float
        double max = std::numeric_limits<double>::infinity();
        std::vector<std::string> choices;          // Enum
    };

    enum class Status { Ok, Unknown, Usage, Failed, Error, Disabled };

    // who typed the line: the gate (setCommandsEnabled) applies to every source but the game's code
    enum class Source { Game, Overlay, Agent, LaunchOption };

    // one parsed argument (given: typed or defaulted; an optional one left out is not given)
    struct ArgValue {
        ArgType type = ArgType::String;
        bool given = false;
        std::string text;                          // String, Rest, Enum (the choice as declared)
        int64_t i = 0;
        double f = 0.0;
        bool b = false;
        Vector3 v;
        Scene* scene = nullptr;                    // GameObject
        Entity entity = NULL_ENTITY;
    };

    struct LogLine {
        LogLevel level = LogLevel::Info;
        std::string message;
    };

    struct Result {
        Status status = Status::Ok;
        std::string command;                       // the resolved name, after aliases
        std::string error;                         // one line
        std::string usage;                         // on Usage
        std::vector<std::string> lines;            // print() first, then Info lines logged during the call
        std::vector<LogLine> log;                  // warnings and errors logged during the call
        nlohmann::json value;                      // null when none
        // what a script's run returned, in its own language (the Lua binding's reference), for
        // callers in that language; null when none
        std::shared_ptr<void> scriptValue;
        bool cheat = false;
        bool ok() const { return status == Status::Ok; }
    };

    // a running command's view of its call
    class JASPERENGINE_API Call {
    public:
        const std::vector<ArgValue>& args() const { return argValues; }
        // untyped commands: the words after the name (`lua`-style rest commands: the rest)
        const std::vector<std::string>& words() const { return wordValues; }
        const std::string& name() const { return commandName; }
        // a line of the result, logged at Info as "[name] line"
        void print(const std::string& line);
        // ends with status Failed
        void fail(const std::string& message);
        // ends with status Error: the command's own code broke (a Lua error)
        void raise(const std::string& message);
        void setValue(nlohmann::json value);
        void setScriptValue(std::shared_ptr<void> value);

    private:
        friend struct Executor;
        std::string commandName;
        std::vector<ArgValue> argValues;
        std::vector<std::string> wordValues;
        Result* result = nullptr;
        bool printing = false;
    };

    struct CommandSpec {
        std::string name;                          // [a-z0-9_.-], 1-48 characters
        std::string description;                   // one line
        std::string usage;                         // "" = made from args: "give <item> [count=1]"
        std::vector<std::string> aliases;          // the name's rule, at most 8
        std::vector<ArgSpec> args;
        int minArgs = 0, maxArgs = -1;             // untyped only (-1 = unbounded)
        bool cheat = false;
        // the subscription tag of what added it (a Lua script's "_<address>_"): removeByTag;
        // "" = until remove()
        std::string ownerTag;
        bool builtIn = false;                      // set by the registry
        std::function<void(Call&)> run;
        // the candidates for the word being typed at argIndex (0 = the first after the name)
        std::function<std::vector<std::string>(int argIndex, const std::string& prefix,
                                               const std::vector<std::string>& words)> complete;
    };

    struct Completion {
        std::string replacement;                   // replaces line[start, cursor)
        std::vector<std::string> candidates;       // sorted, at most 50
        size_t start = 0;
    };

    // false (and the reason, also logged as an error) for a bad spec, `help`, or a name or alias
    // another command holds as an alias (or an alias that is another command's name). A game
    // command of the same name is replaced; a built-in of that name is shadowed until it goes.
    JASPERENGINE_API bool add(CommandSpec spec, std::string* error = nullptr);
    JASPERENGINE_API void remove(const std::string& name);
    // commands whose owner tag contains `substring` (Engine::removeSubscriptionsByTag)
    JASPERENGINE_API void removeByTag(const std::string& substring);
    // every command added with an owner tag (Play stop, the Lua state going)
    JASPERENGINE_API void removeOwned();
    // by name or alias: a game command first, then a built-in
    JASPERENGINE_API const CommandSpec* find(const std::string& nameOrAlias);
    // sorted by name; shadowed built-ins and `lua` while Lua commands are off are left out
    JASPERENGINE_API std::vector<const CommandSpec*> list();
    // the usage line (spec.usage, else made from its args)
    JASPERENGINE_API std::string usageOf(const CommandSpec& spec);
    // {name, description, usage, aliases, cheat, builtIn, args: [{name, type, optional, default,
    // min, max, choices}]}: help's value, getCommands in Lua
    JASPERENGINE_API nlohmann::json describe(const CommandSpec& spec);

    JASPERENGINE_API Result execute(const std::string& line, Source source = Source::Game);
    // cursor: the bytes of `line` before the cursor
    JASPERENGINE_API Completion complete(const std::string& line, size_t cursor);

    // Project Settings > Debug > Console Commands (DebugOverlay::configure): whether the overlay,
    // agents and --command may run commands; the game's own calls always run
    JASPERENGINE_API void setCommandsEnabled(bool enabled);
    JASPERENGINE_API bool commandsEnabled();
    // the text a gated source gets
    JASPERENGINE_API const char* disabledMessage();

    JASPERENGINE_API bool cheatsUsed();
    JASPERENGINE_API void resetCheatsUsed();

    // the end of a game session (Play stop, Engine::systemShutdown): owned commands go, the cheat
    // flag and the shadow warnings reset
    JASPERENGINE_API void endSession();
    // test support: everything goes, the storage too (the next use builds it again)
    JASPERENGINE_API void reset();
    // test support: the registry's storage exists
    JASPERENGINE_API bool isAllocated();

    JASPERENGINE_API const char* statusName(Status status);    // "usage"
    JASPERENGINE_API const char* typeName(ArgType type);       // "gameobject"
    // "gameobject" -> GameObject; false for another word
    JASPERENGINE_API bool parseType(const std::string& name, ArgType& out);

    // the events (a class, so Engine::clearAllSubscriptions walks them: tools/check_event_clears.py)
    class JASPERENGINE_API Events {
    public:
        // the first command flagged cheat that ends ok in a session: (its name)
        static FunctionSubscribe<void(const std::string&)> onCheatUsed;
    };

}

#endif // CONSOLECOMMANDS_H
