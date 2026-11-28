// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "ConsoleCommands.h"

#include "DebugOverlay.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/Transform.h"
#include "manager/SceneManager.h"
#include "script/LuaBinding.h"
#include "util/FunctionSubscribe.h"

#include "lua.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

using namespace jasperengine;

namespace jasperengine::console {

FunctionSubscribe<void(const std::string&)> Events::onCheatUsed;

}  // namespace jasperengine::console

namespace jasperengine::console {

    // runs one line: parsing, the capture of what the command logs, the result
    struct Executor {
        static Result run(const std::string& line, Source source);
    };

}

namespace jasperengine::console {

namespace {

    constexpr size_t maxNameLength = 48;
    constexpr size_t maxAliases = 8;
    constexpr int maxDepth = 4;
    constexpr size_t maxCandidates = 50;
    constexpr size_t maxGameObjectCandidates = 20;

    struct Registry {
        std::vector<CommandSpec> game;
        std::vector<CommandSpec> builtIns;
        std::set<std::string> warnedShadows;
        int depth = 0;
    };

    // allocated by the first registration or execution (spec R22)
    Registry* registryStorage = nullptr;
    std::atomic<bool> enabledFlag{false};
    bool cheatFlag = false;

    void addBuiltIns(Registry& r);

    Registry& registry(){
        if (!registryStorage){
            registryStorage = new Registry();
            addBuiltIns(*registryStorage);
        }
        return *registryStorage;
    }

    std::string consoleLower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return text;
    }

    bool startsWithNoCase(const std::string& text, const std::string& prefix){
        if (prefix.size() > text.size())
            return false;
        for (size_t i = 0; i < prefix.size(); i++){
            if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
    }

    bool validName(const std::string& name){
        if (name.empty() || name.size() > maxNameLength)
            return false;
        for (char c : name){
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
                return false;
        }
        return true;
    }

    // %g without a trailing ".0" (1, 0.5, 100)
    std::string formatNumber(double value){
        char text[64];
        std::snprintf(text, sizeof(text), "%g", value);
        return text;
    }

    size_t editDistance(const std::string& a, const std::string& b){
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++)
            row[j] = j;
        for (size_t i = 1; i <= a.size(); i++){
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); j++){
                const size_t above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                diagonal = above;
            }
        }
        return row[b.size()];
    }

    // the closest of `names` within edit distance 2 of `typed` ("" when none)
    std::string closest(const std::string& typed, const std::vector<std::string>& names){
        std::string best;
        size_t bestDistance = 3;
        for (const std::string& name : names){
            const size_t distance = editDistance(typed, name);
            if (distance < bestDistance || (distance == bestDistance && !best.empty() && name < best)){
                bestDistance = distance;
                best = name;
            }
        }
        return best;
    }

    // ---- the line

    struct Token {
        std::string text;
        size_t start = 0;           // byte offset of its first character (a quote included)
        bool openQuote = false;     // the line ended inside its quotes
    };

    // Words split on spaces and tabs; "double" or 'single' quotes group words, and inside them a
    // backslash escapes the quote and itself (\" \\)
    std::vector<Token> tokenize(const std::string& line, bool* endsInSpace = nullptr){
        std::vector<Token> tokens;
        Token token;
        bool inWord = false;
        char quote = 0;
        for (size_t i = 0; i < line.size(); i++){
            const char c = line[i];
            if (quote){
                if (c == '\\' && i + 1 < line.size() && (line[i + 1] == quote || line[i + 1] == '\\')){
                    token.text += line[++i];
                }else if (c == quote){
                    quote = 0;
                }else{
                    token.text += c;
                }
                continue;
            }
            if (c == ' ' || c == '\t'){
                if (inWord){
                    tokens.push_back(token);
                    token = Token();
                    inWord = false;
                }
                continue;
            }
            if (!inWord){
                token.start = i;
                inWord = true;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else
                token.text += c;
        }
        if (inWord){
            token.openQuote = quote != 0;
            tokens.push_back(token);
        }
        if (endsInSpace)
            *endsInSpace = !line.empty() && !inWord;
        return tokens;
    }

    // a value as a word the splitter gives back: quoted when it has a space or a quote
    std::string quoteWord(const std::string& text, bool close){
        if (text.find_first_of(" \t\"'\\") == std::string::npos)
            return text;
        std::string out = "\"";
        for (char c : text){
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        if (close)
            out += '"';
        return out;
    }

    // ---- lookup

    bool holds(const CommandSpec& spec, const std::string& name){
        return spec.name == name || std::find(spec.aliases.begin(), spec.aliases.end(), name) != spec.aliases.end();
    }

    CommandSpec* findGame(Registry& r, const std::string& name){
        for (CommandSpec& spec : r.game){
            if (holds(spec, name))
                return &spec;
        }
        return nullptr;
    }

    bool shadowed(const Registry& r, const CommandSpec& builtIn){
        for (const CommandSpec& spec : r.game){
            if (spec.name == builtIn.name)
                return true;
        }
        return false;
    }

    CommandSpec* findAny(Registry& r, const std::string& name){
        if (CommandSpec* spec = findGame(r, name))
            return spec;
        for (CommandSpec& spec : r.builtIns){
            if (holds(spec, name) && !shadowed(r, spec))
                return &spec;
        }
        return nullptr;
    }

    // `lua` is listed while Lua commands are allowed
    bool listed(const CommandSpec& spec){
        return !(spec.builtIn && spec.name == "lua" && !DebugOverlay::getSettings().allowLua);
    }

    std::vector<std::string> visibleNames(bool withAliases){
        std::vector<std::string> names;
        for (const CommandSpec* spec : console::list()){
            names.push_back(spec->name);
            if (withAliases)
                names.insert(names.end(), spec->aliases.begin(), spec->aliases.end());
        }
        return names;
    }

    // ---- GameObjects of the running scenes (GameObject.find's rules)

    Entity parentOf(Scene* scene, Entity entity){
        Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    bool matchesPath(Scene* scene, Entity entity, const std::vector<std::string>& path, bool fromRoot){
        Entity current = entity;
        for (size_t i = path.size(); i-- > 0;){
            if (current == NULL_ENTITY || !scene->isEntityCreated(current) || scene->getEntityNameRef(current) != path[i])
                return false;
            current = parentOf(scene, current);
        }
        return !fromRoot || current == NULL_ENTITY;
    }

    std::string pathOf(Scene* scene, Entity entity){
        std::string path;
        for (Entity at = entity; at != NULL_ENTITY && scene->isEntityCreated(at); at = parentOf(scene, at))
            path = "/" + scene->getEntityNameRef(at) + path;
        return path;
    }

    bool findGameObject(const std::string& text, Scene*& sceneOut, Entity& entityOut){
        if (text.size() > 1 && text[0] == '#'){
            char* end = nullptr;
            errno = 0;
            const unsigned long long id = std::strtoull(text.c_str() + 1, &end, 10);
            if (errno != 0 || !end || *end != '\0' || !std::isdigit(static_cast<unsigned char>(text[1])) || id > UINT32_MAX)
                return false;
            for (Scene* scene : Engine::getSceneLayers()){
                if (scene && scene->isEntityCreated(static_cast<Entity>(id))){
                    sceneOut = scene;
                    entityOut = static_cast<Entity>(id);
                    return true;
                }
            }
            return false;
        }
        const bool fromRoot = !text.empty() && text[0] == '/';
        std::vector<std::string> path;
        size_t start = fromRoot ? 1 : 0;
        while (start <= text.size()){
            const size_t slash = text.find('/', start);
            path.push_back(text.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        if (path.empty() || path.back().empty())
            return false;
        for (Scene* scene : Engine::getSceneLayers()){
            if (!scene)
                continue;
            for (Entity entity : scene->getEntityList()){
                if (scene->isEntityCreated(entity) && scene->getEntityNameRef(entity) == path.back() &&
                        scene->isEntityActiveInHierarchy(entity) && matchesPath(scene, entity, path, fromRoot)){
                    sceneOut = scene;
                    entityOut = entity;
                    return true;
                }
            }
        }
        return false;
    }

    // the names (or, for a name several GameObjects have, their paths) of the active GameObjects
    // of the running scenes that start with `prefix`
    std::vector<std::string> gameObjectCandidates(const std::string& prefix, size_t limit){
        std::map<std::string, std::vector<std::pair<Scene*, Entity>>> byName;
        for (Scene* scene : Engine::getSceneLayers()){
            if (!scene)
                continue;
            for (Entity entity : scene->getEntityList()){
                if (!scene->isEntityCreated(entity))
                    continue;
                const std::string& name = scene->getEntityNameRef(entity);
                if (name.empty() || !startsWithNoCase(name, prefix) || !scene->isEntityActiveInHierarchy(entity))
                    continue;
                byName[name].push_back({scene, entity});
            }
        }
        std::set<std::string> out;
        for (const auto& [name, objects] : byName){
            if (objects.size() == 1){
                out.insert(name);
            }else{
                for (const auto& [scene, entity] : objects)
                    out.insert(pathOf(scene, entity));
            }
        }
        std::vector<std::string> list(out.begin(), out.end());
        if (list.size() > limit)
            list.resize(limit);
        return list;
    }

    std::vector<std::string> gameObjectNames(){
        std::set<std::string> names;
        for (Scene* scene : Engine::getSceneLayers()){
            if (!scene)
                continue;
            for (Entity entity : scene->getEntityList()){
                if (scene->isEntityCreated(entity) && !scene->getEntityNameRef(entity).empty() && scene->isEntityActiveInHierarchy(entity))
                    names.insert(scene->getEntityNameRef(entity));
            }
        }
        return std::vector<std::string>(names.begin(), names.end());
    }

    // ---- typed arguments

    const std::vector<std::string>& boolWords(){
        static const std::vector<std::string> words = {"false", "no", "off", "on", "true", "yes"};
        return words;
    }

    bool parseInt(const std::string& text, int64_t& out){
        size_t at = 0;
        bool negative = false;
        if (at < text.size() && (text[at] == '+' || text[at] == '-')){
            negative = text[at] == '-';
            at++;
        }
        int base = 10;
        if (text.size() > at + 1 && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'X')){
            base = 16;
            at += 2;
        }
        if (at >= text.size())
            return false;
        for (size_t i = at; i < text.size(); i++){
            if (!(base == 16 ? std::isxdigit(static_cast<unsigned char>(text[i])) : std::isdigit(static_cast<unsigned char>(text[i]))))
                return false;
        }
        errno = 0;
        const unsigned long long magnitude = std::strtoull(text.c_str() + at, nullptr, base);
        if (errno == ERANGE)
            return false;
        if (negative){
            if (magnitude > static_cast<unsigned long long>(INT64_MAX) + 1ull)
                return false;
            out = magnitude == static_cast<unsigned long long>(INT64_MAX) + 1ull ? INT64_MIN : -static_cast<int64_t>(magnitude);
        }else{
            if (magnitude > static_cast<unsigned long long>(INT64_MAX))
                return false;
            out = static_cast<int64_t>(magnitude);
        }
        return true;
    }

    // a finite decimal (no hex floats, inf or nan)
    bool parseFloat(const std::string& text, double& out){
        if (text.empty() || text.find_first_not_of("0123456789+-.eE") != std::string::npos)
            return false;
        char* end = nullptr;
        out = std::strtod(text.c_str(), &end);
        return end && *end == '\0' && std::isfinite(out);
    }

    bool parseBool(const std::string& text, bool& out){
        const std::string word = consoleLower(text);
        if (word == "true" || word == "on" || word == "yes" || word == "1"){
            out = true;
            return true;
        }
        if (word == "false" || word == "off" || word == "no" || word == "0"){
            out = false;
            return true;
        }
        return false;
    }

    // a choice, case-insensitive, or a prefix of exactly one
    bool parseEnum(const std::string& text, const std::vector<std::string>& choices, std::string& out){
        const std::string word = consoleLower(text);
        const std::string* match = nullptr;
        for (const std::string& choice : choices){
            if (consoleLower(choice) == word){
                out = choice;
                return true;
            }
            if (!word.empty() && startsWithNoCase(choice, word)){
                if (match)
                    return false;
                match = &choice;
            }
        }
        if (!match)
            return false;
        out = *match;
        return true;
    }

    bool parseVector3(const std::string& text, Vector3& out){
        double parts[3];
        size_t start = 0;
        for (int i = 0; i < 3; i++){
            const size_t comma = text.find(',', start);
            if ((i < 2) == (comma == std::string::npos))
                return false;
            const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!parseFloat(part, parts[i]))
                return false;
            start = comma + 1;
        }
        out = Vector3(static_cast<float>(parts[0]), static_cast<float>(parts[1]), static_cast<float>(parts[2]));
        return true;
    }

    std::string rangeText(const ArgSpec& arg, const char* noun){
        const bool hasMin = std::isfinite(arg.min);
        const bool hasMax = std::isfinite(arg.max);
        if (hasMin && hasMax)
            return std::string(noun) + " from " + formatNumber(arg.min) + " to " + formatNumber(arg.max);
        if (hasMin)
            return std::string(noun) + " of at least " + formatNumber(arg.min);
        if (hasMax)
            return std::string(noun) + " of at most " + formatNumber(arg.max);
        return noun;
    }

    std::string expectedText(const ArgSpec& arg){
        switch (arg.type){
            case ArgType::Int: return rangeText(arg, "a whole number");
            case ArgType::Float: return rangeText(arg, "a number");
            case ArgType::Bool: return "true or false (on/off, yes/no, 1/0)";
            case ArgType::Enum: {
                std::string text = "one of ";
                for (size_t i = 0; i < arg.choices.size(); i++)
                    text += (i ? ", " : "") + arg.choices[i];
                return text;
            }
            case ArgType::Vector3: return "x,y,z (three numbers, as 1,0,-2.5)";
            case ArgType::GameObject: return "a GameObject (a name, a /path or #id)";
            default: return "a word";
        }
    }

    // parses `text` as `arg`; false with `problem` (the part after "<command>: ")
    bool parseArg(const ArgSpec& arg, const std::string& text, ArgValue& value, std::string& problem){
        value.type = arg.type;
        value.given = true;
        value.text = text;
        auto mismatch = [&](){
            problem = arg.name + " must be " + expectedText(arg) + " (got \"" + text + "\").";
            return false;
        };
        switch (arg.type){
            case ArgType::Int:
                if (!parseInt(text, value.i) || static_cast<double>(value.i) < arg.min || static_cast<double>(value.i) > arg.max)
                    return mismatch();
                value.f = static_cast<double>(value.i);
                return true;
            case ArgType::Float:
                if (!parseFloat(text, value.f) || value.f < arg.min || value.f > arg.max)
                    return mismatch();
                return true;
            case ArgType::Bool:
                return parseBool(text, value.b) || mismatch();
            case ArgType::Enum:
                return parseEnum(text, arg.choices, value.text) || mismatch();
            case ArgType::Vector3:
                return parseVector3(text, value.v) || mismatch();
            case ArgType::GameObject: {
                if (findGameObject(text, value.scene, value.entity))
                    return true;
                if (!text.empty() && text[0] == '#'){
                    problem = "no GameObject " + text + " in the running scenes.";
                    return false;
                }
                const size_t slash = text.rfind('/');
                const std::string suggestion = closest(slash == std::string::npos ? text : text.substr(slash + 1), gameObjectNames());
                problem = "no GameObject \"" + text + "\"" + (suggestion.empty() ? std::string() : " (did you mean \"" + suggestion + "\"?)") + ".";
                return false;
            }
            default:
                return true;
        }
    }

    std::string wordCount(int count){
        return std::to_string(count) + (count == 1 ? " word" : " words");
    }

    // a spec a registration refuses: the reason
    std::string specProblem(const CommandSpec& spec){
        if (!validName(spec.name))
            return "'" + spec.name + "' is not a command name (lower-case letters, digits, '_', '-' and '.', 1-48 characters)";
        if (!spec.run)
            return "'" + spec.name + "' has no function";
        if (spec.aliases.size() > maxAliases)
            return "'" + spec.name + "' has more than 8 aliases";
        for (size_t i = 0; i < spec.aliases.size(); i++){
            const std::string& alias = spec.aliases[i];
            if (!validName(alias))
                return "alias '" + alias + "' is not a command name (lower-case letters, digits, '_', '-' and '.', 1-48 characters)";
            if (alias == spec.name || std::find(spec.aliases.begin(), spec.aliases.begin() + static_cast<std::ptrdiff_t>(i), alias) != spec.aliases.begin() + static_cast<std::ptrdiff_t>(i))
                return "alias '" + alias + "' is given twice";
        }
        if (spec.maxArgs >= 0 && spec.maxArgs < spec.minArgs)
            return "'" + spec.name + "': maxArgs is below minArgs";
        bool optionalSeen = false;
        for (size_t i = 0; i < spec.args.size(); i++){
            const ArgSpec& arg = spec.args[i];
            if (arg.name.empty())
                return "'" + spec.name + "': argument " + std::to_string(i + 1) + " has no name";
            for (size_t j = 0; j < i; j++){
                if (spec.args[j].name == arg.name)
                    return "'" + spec.name + "': two arguments are named '" + arg.name + "'";
            }
            if (arg.type == ArgType::Rest && i + 1 != spec.args.size())
                return "'" + spec.name + "': the rest argument '" + arg.name + "' must be the last";
            if (arg.type == ArgType::Enum && arg.choices.empty())
                return "'" + spec.name + "': the enum argument '" + arg.name + "' has no choices";
            if (arg.min > arg.max)
                return "'" + spec.name + "': '" + arg.name + "' has min above max";
            if (optionalSeen && !arg.optional)
                return "'" + spec.name + "': the required argument '" + arg.name + "' comes after an optional one";
            optionalSeen = optionalSeen || arg.optional;
            if (!arg.defaultValue.empty() && arg.type != ArgType::GameObject && arg.type != ArgType::Rest){
                ArgValue value;
                std::string problem;
                if (!parseArg(arg, arg.defaultValue, value, problem))
                    return "'" + spec.name + "': the default of " + problem;
            }
        }
        return std::string();
    }

    // ---- built-in commands

    ArgSpec argOf(const char* name, ArgType type, bool optional){
        ArgSpec arg;
        arg.name = name;
        arg.type = type;
        arg.optional = optional;
        return arg;
    }

    const char* statsName(DebugOverlaySettings::Stats mode){
        switch (mode){
            case DebugOverlaySettings::Stats::Fps: return "fps";
            case DebugOverlaySettings::Stats::Full: return "full";
            default: return "off";
        }
    }

    std::string helpLine(const CommandSpec& spec){
        std::string line = spec.name;
        if (!spec.description.empty())
            line += " - " + spec.description;
        if (!spec.aliases.empty()){
            line += " [";
            for (size_t i = 0; i < spec.aliases.size(); i++)
                line += (i ? ", " : "") + spec.aliases[i];
            line += "]";
        }
        if (spec.cheat)
            line += " (cheat)";
        return line;
    }

    std::string argLine(const ArgSpec& arg){
        std::string line = "  " + arg.name + ": " + typeName(arg.type);
        if (arg.type == ArgType::Enum){
            line += " (";
            for (size_t i = 0; i < arg.choices.size(); i++)
                line += (i ? ", " : "") + arg.choices[i];
            line += ")";
        }
        if (std::isfinite(arg.min) || std::isfinite(arg.max))
            line += " " + (std::isfinite(arg.min) ? formatNumber(arg.min) : std::string()) + ".." +
                    (std::isfinite(arg.max) ? formatNumber(arg.max) : std::string());
        if (arg.optional)
            line += ", optional";
        if (!arg.defaultValue.empty())
            line += ", default " + arg.defaultValue;
        return line;
    }

    void runHelp(Call& call){
        const ArgValue& topic = call.args()[0];
        if (!topic.given){
            nlohmann::json all = nlohmann::json::array();
            for (const CommandSpec* spec : console::list()){
                call.print(helpLine(*spec));
                all.push_back(describe(*spec));
            }
            call.setValue(all);
            return;
        }
        const std::string word = consoleLower(topic.text);
        if (!word.empty() && word.back() == '*'){
            const std::string prefix = word.substr(0, word.size() - 1);
            nlohmann::json all = nlohmann::json::array();
            for (const CommandSpec* spec : console::list()){
                if (spec->name.compare(0, prefix.size(), prefix) != 0)
                    continue;
                call.print(helpLine(*spec));
                all.push_back(describe(*spec));
            }
            call.setValue(all);
            return;
        }
        const CommandSpec* spec = console::find(word);
        if (!spec || !listed(*spec)){
            const std::string suggestion = closest(word, visibleNames(true));
            call.fail("no command '" + topic.text + "'" + (suggestion.empty() ? std::string() : " (did you mean '" + suggestion + "'?)"));
            return;
        }
        call.print(helpLine(*spec));
        call.print("Usage: " + usageOf(*spec));
        for (const ArgSpec& arg : spec->args)
            call.print(argLine(arg));
        call.setValue(describe(*spec));
    }

    void runLua(Call& call){
        if (!DebugOverlay::getSettings().allowLua){
            call.fail("Lua commands are off (Project Settings > Debug > Allow Lua Commands)");
            return;
        }
        lua_State* L = LuaBinding::getLuaState();
        if (!L){
            call.fail("no Lua state");
            return;
        }
        const std::string& code = call.args()[0].text;
        const int top = lua_gettop(L);
        // an expression prints its value, as Lua's own prompt does
        const std::string expression = "return " + code;
        if (luaL_loadbuffer(L, expression.c_str(), expression.size(), "=console") != LUA_OK){
            lua_pop(L, 1);
            if (luaL_loadbuffer(L, code.c_str(), code.size(), "=console") != LUA_OK){
                const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
                lua_settop(L, top);
                call.fail(error);
                return;
            }
        }
        if (LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET) != LUA_OK){
            const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
            lua_settop(L, top);
            Log::error("Lua Error: %s", error.c_str());
            const size_t end = error.find('\n');
            call.raise(end == std::string::npos ? error : error.substr(0, end));
            return;
        }
        std::string results;
        for (int i = top + 1; i <= lua_gettop(L); i++){
            size_t length = 0;
            const char* text = luaL_tolstring(L, i, &length);
            if (i > top + 1) results += "\t";
            results.append(text, length);
            lua_pop(L, 1);
        }
        lua_settop(L, top);
        if (!results.empty())
            Log::print("%s", results.c_str());
    }

    void addBuiltIns(Registry& r){
        auto add = [&r](CommandSpec spec){
            spec.builtIn = true;
            r.builtIns.push_back(std::move(spec));
        };
        {
            CommandSpec spec;
            spec.name = "help";
            spec.description = "lists the commands; help <name> describes one, help <prefix>* filters";
            spec.args = {argOf("name", ArgType::String, true)};
            spec.run = &runHelp;
            spec.complete = [](int argIndex, const std::string& prefix, const std::vector<std::string>&){
                std::vector<std::string> out;
                if (argIndex != 0)
                    return out;
                for (const std::string& name : visibleNames(true)){
                    if (startsWithNoCase(name, prefix))
                        out.push_back(name);
                }
                return out;
            };
            add(std::move(spec));
        }
        {
            CommandSpec spec;
            spec.name = "clear";
            spec.description = "clears the console (the log buffer)";
            spec.maxArgs = 0;
            spec.run = [](Call&){ Log::clearEntries(); };
            add(std::move(spec));
        }
        {
            CommandSpec spec;
            spec.name = "stats";
            spec.description = "the stats panel: off, fps or full (none cycles)";
            ArgSpec mode = argOf("mode", ArgType::Enum, true);
            mode.choices = {"off", "fps", "full"};
            spec.args = {mode};
            spec.run = [](Call& call){
                const ArgValue& mode = call.args()[0];
                if (!mode.given) DebugOverlay::cycleStatsMode();
                else if (mode.text == "off") DebugOverlay::setStatsMode(DebugOverlaySettings::Stats::Off);
                else if (mode.text == "fps") DebugOverlay::setStatsMode(DebugOverlaySettings::Stats::Fps);
                else DebugOverlay::setStatsMode(DebugOverlaySettings::Stats::Full);
                call.setValue(statsName(DebugOverlay::getStatsMode()));
            };
            add(std::move(spec));
        }
        {
            CommandSpec spec;
            spec.name = "timescale";
            spec.description = "Engine.timeScale (0 freezes, 1 normal); none prints it";
            ArgSpec x = argOf("x", ArgType::Float, true);
            x.min = 0.0;
            x.max = 100.0;
            spec.args = {x};
            spec.cheat = true;
            spec.run = [](Call& call){
                const ArgValue& x = call.args()[0];
                if (x.given)
                    Engine::setTimeScale(static_cast<float>(x.f));
                Log::print("timescale %g", Engine::getTimeScale());
                call.setValue(static_cast<double>(Engine::getTimeScale()));
            };
            add(std::move(spec));
        }
        {
            CommandSpec spec;
            spec.name = "scene";
            spec.description = "loads that scene (SceneManager.loadScene)";
            spec.args = {argOf("name", ArgType::Rest, false)};
            spec.cheat = true;
            spec.run = [](Call& call){
                const std::string& name = call.args()[0].text;
                if (!SceneManager::loadScene(name))
                    call.fail("no scene named '" + name + "'");
            };
            add(std::move(spec));
        }
        {
            CommandSpec spec;
            spec.name = "lua";
            spec.description = "runs Lua (Project Settings > Debug > Allow Lua Commands)";
            spec.args = {argOf("code", ArgType::Rest, false)};
            spec.cheat = true;
            spec.run = &runLua;
            add(std::move(spec));
        }
    }

    // ---- completion

    std::string commonPrefix(const std::vector<std::string>& words){
        if (words.empty())
            return std::string();
        std::string prefix = words[0];
        for (const std::string& word : words){
            size_t n = 0;
            while (n < prefix.size() && n < word.size() && prefix[n] == word[n])
                n++;
            prefix.resize(n);
        }
        return prefix;
    }

    void finishCandidates(std::vector<std::string>& candidates){
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        if (candidates.size() > maxCandidates)
            candidates.resize(maxCandidates);
    }

}

// ---------------------------------------------------------------- Call

void Call::print(const std::string& line){
    if (!result)
        return;
    result->lines.push_back(line);
    printing = true;
    Log::print("[%s] %s", commandName.c_str(), line.c_str());
    printing = false;
}

void Call::fail(const std::string& message){
    if (!result)
        return;
    result->status = Status::Failed;
    result->error = message;
}

void Call::raise(const std::string& message){
    if (!result)
        return;
    result->status = Status::Error;
    result->error = message;
}

void Call::setValue(nlohmann::json value){
    if (result)
        result->value = std::move(value);
}

void Call::setScriptValue(std::shared_ptr<void> value){
    if (result)
        result->scriptValue = std::move(value);
}

// ---------------------------------------------------------------- execution

Result Executor::run(const std::string& line, Source source){
    Result result;
    if (!Log::isMainThread()){
        result.status = Status::Error;
        result.error = "main thread only";
        return result;
    }
    if (source != Source::Game && !commandsEnabled()){
        result.status = Status::Disabled;
        result.error = disabledMessage();
        return result;
    }
    Registry& r = registry();
    const std::vector<Token> tokens = tokenize(line);
    if (tokens.empty()){
        result.status = Status::Unknown;
        result.error = "no command (type help)";
        return result;
    }
    if (r.depth >= maxDepth){
        result.status = Status::Failed;
        result.command = consoleLower(tokens[0].text);
        result.error = "nested too deep (commands run commands at most " + std::to_string(maxDepth) + " levels deep)";
        Log::error("%s: %s", result.command.c_str(), result.error.c_str());
        return result;
    }
    Log::print("> %s", line.c_str());

    const std::string typed = consoleLower(tokens[0].text);
    const CommandSpec* spec = findAny(r, typed);
    if (!spec){
        result.status = Status::Unknown;
        result.command = typed;
        const std::string suggestion = closest(typed, visibleNames(true));
        result.error = "unknown command '" + tokens[0].text + "' (" + (suggestion.empty() ? std::string() : "did you mean '" + suggestion + "'? ") + "type help)";
        Log::warn("%s", result.error.c_str());
        return result;
    }
    result.command = spec->name;
    result.cheat = spec->cheat;

    Call call;
    call.commandName = spec->name;
    std::string problem;
    const size_t wordCountGiven = tokens.size() - 1;
    if (spec->args.empty()){
        for (size_t i = 1; i < tokens.size(); i++)
            call.wordValues.push_back(tokens[i].text);
        const int count = static_cast<int>(wordCountGiven);
        if (count < spec->minArgs)
            problem = "takes at least " + wordCount(spec->minArgs) + " (got " + std::to_string(count) + ").";
        else if (spec->maxArgs >= 0 && count > spec->maxArgs)
            problem = spec->maxArgs == 0 ? "takes no words (got " + std::to_string(count) + ")." :
                                           "takes at most " + wordCount(spec->maxArgs) + " (got " + std::to_string(count) + ").";
    }else{
        size_t next = 1;
        for (const ArgSpec& arg : spec->args){
            ArgValue value;
            value.type = arg.type;
            if (arg.type == ArgType::Rest){
                if (next < tokens.size()){
                    std::string rest = line.substr(tokens[next].start);
                    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t'))
                        rest.pop_back();
                    value.given = true;
                    value.text = rest;
                    for (; next < tokens.size(); next++)
                        call.wordValues.push_back(tokens[next].text);
                }else if (!arg.defaultValue.empty()){
                    value.given = true;
                    value.text = arg.defaultValue;
                }else if (!arg.optional){
                    problem = arg.name + " is missing.";
                }
            }else if (next < tokens.size()){
                call.wordValues.push_back(tokens[next].text);
                parseArg(arg, tokens[next].text, value, problem);
                next++;
            }else if (!arg.defaultValue.empty()){
                parseArg(arg, arg.defaultValue, value, problem);
            }else if (!arg.optional){
                problem = arg.name + " is missing.";
            }
            call.argValues.push_back(value);
            if (!problem.empty())
                break;
        }
        if (problem.empty() && next < tokens.size())
            problem = "takes at most " + wordCount(static_cast<int>(spec->args.size())) + " (got " + std::to_string(wordCountGiven) + ").";
    }
    if (!problem.empty()){
        result.status = Status::Usage;
        result.error = spec->name + ": " + problem;
        result.usage = usageOf(*spec);
        Log::warn("%s Usage: %s", result.error.c_str(), result.usage.c_str());
        return result;
    }

    // a copy: the command may remove itself
    const std::function<void(Call&)> function = spec->run;
    const bool cheat = spec->cheat;
    const std::string name = spec->name;

    call.result = &result;
    bool errorLogged = false;
    std::vector<std::string> captured;
    // R21: a listener for this call only
    const LogListenerId listener = Log::addListener([&call, &result, &captured, &errorLogged](const LogEntry& entry){
        if (!entry.mainThread || call.printing)
            return;
        if (entry.level <= LogLevel::Info){
            captured.push_back(entry.message);
        }else{
            result.log.push_back({entry.level, entry.stackTrace.empty() ? entry.message : entry.message + "\n" + entry.stackTrace});
            errorLogged = errorLogged || entry.level >= LogLevel::Error;
        }
    });
    r.depth++;
    function(call);
    // (a command may have called reset(): test support)
    if (registryStorage)
        registryStorage->depth--;
    Log::removeListener(listener);
    call.result = nullptr;
    result.lines.insert(result.lines.end(), captured.begin(), captured.end());

    if (result.status == Status::Failed){
        Log::error("%s: %s", name.c_str(), result.error.c_str());
    }else if (result.status == Status::Error && !errorLogged){
        Log::error("%s: %s", name.c_str(), result.error.c_str());
    }else if (result.ok() && cheat && !cheatFlag){
        cheatFlag = true;
        if (Events::onCheatUsed.hasSubscribers())
            Events::onCheatUsed.call(name);
    }
    return result;
}

// ---------------------------------------------------------------- registry

bool add(CommandSpec spec, std::string* error){
    Registry& r = registry();
    std::string problem = specProblem(spec);
    if (problem.empty() && spec.name == "help")
        problem = "'help' is a built-in command that cannot be replaced";
    if (problem.empty()){
        for (const std::string& alias : spec.aliases){
            if (alias == "help"){
                problem = "alias 'help' is taken by 'help'";
                break;
            }
        }
    }
    // names and aliases of the others (the game command of this name is being replaced)
    auto holderOf = [&r, &spec](const std::string& word, bool asAlias) -> std::string {
        for (const CommandSpec& other : r.game){
            if (other.name == spec.name)
                continue;
            if ((asAlias && other.name == word) || std::find(other.aliases.begin(), other.aliases.end(), word) != other.aliases.end())
                return other.name;
        }
        if (asAlias){
            for (const CommandSpec& other : r.builtIns){
                if (other.name == word && !shadowed(r, other))
                    return other.name;
            }
        }
        return std::string();
    };
    if (problem.empty()){
        const std::string holder = holderOf(spec.name, false);
        if (!holder.empty())
            problem = "'" + spec.name + "' is taken by '" + holder + "' (an alias)";
    }
    if (problem.empty()){
        for (const std::string& alias : spec.aliases){
            const std::string holder = holderOf(alias, true);
            if (!holder.empty()){
                problem = "alias '" + alias + "' is taken by '" + holder + "'";
                break;
            }
        }
    }
    if (!problem.empty()){
        Log::error("DebugOverlay.addCommand: %s.", problem.c_str());
        if (error)
            *error = problem;
        return false;
    }
    spec.builtIn = false;
    for (const CommandSpec& builtIn : r.builtIns){
        if (builtIn.name == spec.name && r.warnedShadows.insert(spec.name).second)
            Log::warn("console: '%s' now runs the game's command; the built-in returns when it is removed", spec.name.c_str());
    }
    for (CommandSpec& existing : r.game){
        if (existing.name == spec.name){
            existing = std::move(spec);
            return true;
        }
    }
    r.game.push_back(std::move(spec));
    return true;
}

void remove(const std::string& name){
    if (!registryStorage)
        return;
    std::vector<CommandSpec>& game = registryStorage->game;
    game.erase(std::remove_if(game.begin(), game.end(), [&name](const CommandSpec& c){ return c.name == name; }), game.end());
}

void removeByTag(const std::string& substring){
    if (!registryStorage || substring.empty())
        return;
    std::vector<CommandSpec>& game = registryStorage->game;
    game.erase(std::remove_if(game.begin(), game.end(), [&substring](const CommandSpec& c){
        return !c.ownerTag.empty() && c.ownerTag.find(substring) != std::string::npos;
    }), game.end());
}

void removeOwned(){
    if (!registryStorage)
        return;
    std::vector<CommandSpec>& game = registryStorage->game;
    game.erase(std::remove_if(game.begin(), game.end(), [](const CommandSpec& c){ return !c.ownerTag.empty(); }), game.end());
}

const CommandSpec* find(const std::string& nameOrAlias){
    return findAny(registry(), consoleLower(nameOrAlias));
}

std::vector<const CommandSpec*> list(){
    Registry& r = registry();
    std::vector<const CommandSpec*> out;
    for (const CommandSpec& spec : r.game)
        out.push_back(&spec);
    for (const CommandSpec& spec : r.builtIns){
        if (!shadowed(r, spec) && listed(spec))
            out.push_back(&spec);
    }
    std::sort(out.begin(), out.end(), [](const CommandSpec* a, const CommandSpec* b){ return a->name < b->name; });
    return out;
}

std::string usageOf(const CommandSpec& spec){
    if (!spec.usage.empty())
        return spec.usage;
    std::string usage = spec.name;
    for (const ArgSpec& arg : spec.args){
        const std::string name = arg.type == ArgType::Rest ? arg.name + "..." : arg.name;
        if (arg.optional || !arg.defaultValue.empty())
            usage += " [" + name + (arg.defaultValue.empty() ? std::string() : "=" + arg.defaultValue) + "]";
        else
            usage += " <" + name + ">";
    }
    return usage;
}

nlohmann::json describe(const CommandSpec& spec){
    nlohmann::json args = nlohmann::json::array();
    for (const ArgSpec& arg : spec.args){
        nlohmann::json item = {{"name", arg.name}, {"type", typeName(arg.type)}, {"optional", arg.optional || !arg.defaultValue.empty()}};
        if (!arg.defaultValue.empty())
            item["default"] = arg.defaultValue;
        if (std::isfinite(arg.min))
            item["min"] = arg.min;
        if (std::isfinite(arg.max))
            item["max"] = arg.max;
        if (arg.type == ArgType::Enum)
            item["choices"] = arg.choices;
        args.push_back(item);
    }
    return {{"name", spec.name}, {"description", spec.description}, {"usage", usageOf(spec)}, {"aliases", spec.aliases},
            {"cheat", spec.cheat}, {"builtIn", spec.builtIn}, {"args", args}};
}

Result execute(const std::string& line, Source source){
    return Executor::run(line, source);
}

Completion complete(const std::string& line, size_t cursor){
    Completion out;
    const std::string before = line.substr(0, std::min(cursor, line.size()));
    bool endsInSpace = false;
    std::vector<Token> tokens = tokenize(before, &endsInSpace);
    std::string prefix;
    size_t index = tokens.size();
    out.start = before.size();
    if (!tokens.empty() && !endsInSpace){
        index = tokens.size() - 1;
        prefix = tokens.back().text;
        out.start = tokens.back().start;
    }
    const std::string typed = before.substr(out.start);

    std::vector<std::string>& candidates = out.candidates;
    if (index == 0){
        for (const std::string& name : visibleNames(true)){
            if (startsWithNoCase(name, prefix))
                candidates.push_back(name);
        }
    }else{
        const CommandSpec* spec = find(tokens[0].text);
        const int argIndex = static_cast<int>(index) - 1;
        if (spec && listed(*spec)){
            if (spec->complete){
                std::vector<std::string> words;
                for (size_t i = 1; i < index && i < tokens.size(); i++)
                    words.push_back(tokens[i].text);
                for (std::string& candidate : spec->complete(argIndex, prefix, words)){
                    if (startsWithNoCase(candidate, prefix))
                        candidates.push_back(std::move(candidate));
                }
            }else if (static_cast<size_t>(argIndex) < spec->args.size()){
                const ArgSpec& arg = spec->args[static_cast<size_t>(argIndex)];
                if (arg.type == ArgType::Enum){
                    for (const std::string& choice : arg.choices){
                        if (startsWithNoCase(choice, prefix))
                            candidates.push_back(choice);
                    }
                }else if (arg.type == ArgType::Bool){
                    for (const std::string& word : boolWords()){
                        if (startsWithNoCase(word, prefix))
                            candidates.push_back(word);
                    }
                }else if (arg.type == ArgType::GameObject){
                    candidates = gameObjectCandidates(prefix, maxGameObjectCandidates);
                }
            }
        }
    }
    finishCandidates(candidates);

    if (candidates.empty()){
        out.replacement = typed;
    }else if (candidates.size() == 1){
        out.replacement = quoteWord(candidates[0], true) + " ";
    }else{
        const std::string common = commonPrefix(candidates);
        // what was typed in another case than the candidates stays as typed when they share no more
        out.replacement = common.size() >= prefix.size() ? quoteWord(common, false) : typed;
    }
    return out;
}

// ---------------------------------------------------------------- the gate, cheats, session

void setCommandsEnabled(bool enabled){
    enabledFlag.store(enabled, std::memory_order_relaxed);
}

bool commandsEnabled(){
    return enabledFlag.load(std::memory_order_relaxed);
}

const char* disabledMessage(){
    return "Console commands are off in this build (Project Settings > Debug > Console Commands).";
}

bool cheatsUsed(){
    return cheatFlag;
}

void resetCheatsUsed(){
    cheatFlag = false;
}

void endSession(){
    cheatFlag = false;
    if (!registryStorage)
        return;
    removeOwned();
    registryStorage->warnedShadows.clear();
}

void reset(){
    delete registryStorage;
    registryStorage = nullptr;
    cheatFlag = false;
    enabledFlag.store(false, std::memory_order_relaxed);
}

bool isAllocated(){
    return registryStorage != nullptr;
}

const char* statusName(Status status){
    switch (status){
        case Status::Ok: return "ok";
        case Status::Unknown: return "unknown";
        case Status::Usage: return "usage";
        case Status::Failed: return "failed";
        case Status::Error: return "error";
        case Status::Disabled: return "disabled";
    }
    return "error";
}

const char* typeName(ArgType type){
    switch (type){
        case ArgType::String: return "string";
        case ArgType::Int: return "int";
        case ArgType::Float: return "float";
        case ArgType::Bool: return "bool";
        case ArgType::Enum: return "enum";
        case ArgType::Vector3: return "vector3";
        case ArgType::GameObject: return "gameobject";
        case ArgType::Rest: return "rest";
    }
    return "string";
}

bool parseType(const std::string& name, ArgType& out){
    static const ArgType types[] = {ArgType::String, ArgType::Int, ArgType::Float, ArgType::Bool, ArgType::Enum,
                                    ArgType::Vector3, ArgType::GameObject, ArgType::Rest};
    const std::string word = consoleLower(name);
    for (ArgType type : types){
        if (word == typeName(type)){
            out = type;
            return true;
        }
    }
    return false;
}

}  // namespace jasperengine::console
