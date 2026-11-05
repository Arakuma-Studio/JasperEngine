// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModuleUsage.h"

#include "ShaderNeutralApi.h"
#include "Project.h"
#include "DocumentFile.h"
#include "util/Prefab.h"
#include "util/Util.h"
#include "audio/AudioImportRegistry.h"
#include "component/AudioSourceComponent.h"
#include "util/LuaApiData.h"
#include "player/PlayerModules.h"

#include "Scene.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/ScriptComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VideoPlayerComponent.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // the evidence kept per module (the first ones: the Build Settings tooltip, the CLI)
    constexpr size_t MAX_EVIDENCE = 8;

    // the modules a dynamic access or C++ code can reach: every one the detection decides
    const std::vector<std::string>& detectedModules() {
        static const std::vector<std::string> modules = {"htmlui", "video", "runtimeshaders", "physics3d", "physics2d"};
        return modules;
    }

    // ---- a Lua lexer: names and string literals, with their lines; comments dropped ----

    struct LuaToken {
        enum Kind { Name, String, Other } kind;
        std::string text;
        int line;
    };

    // the level of a long bracket opening at s[i] ("[[", "[==["), -1 when there is none
    int longBracketLevel(const std::string& s, size_t i) {
        if (i >= s.size() || s[i] != '[') return -1;
        size_t j = i + 1;
        int level = 0;
        while (j < s.size() && s[j] == '=') { level++; j++; }
        return (j < s.size() && s[j] == '[') ? level : -1;
    }

    // the text of the long bracket at s[i] of `level`; i ends after it (or at the end)
    std::string readLongBracket(const std::string& s, size_t& i, int level, int& line) {
        i += (size_t)level + 2;
        const std::string close = "]" + std::string((size_t)level, '=') + "]";
        const size_t end = s.find(close, i);
        const size_t stop = end == std::string::npos ? s.size() : end;
        std::string text = s.substr(i, stop - i);
        line += (int)std::count(text.begin(), text.end(), '\n');
        i = end == std::string::npos ? s.size() : end + close.size();
        return text;
    }

    std::vector<LuaToken> lexLua(const std::string& s) {
        std::vector<LuaToken> tokens;
        int line = 1;
        size_t i = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\n') { line++; i++; continue; }
            if (std::isspace((unsigned char)c)) { i++; continue; }
            if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
                i += 2;
                const int level = longBracketLevel(s, i);
                if (level >= 0) {
                    readLongBracket(s, i, level, line);
                } else {
                    while (i < s.size() && s[i] != '\n') i++;
                }
                continue;
            }
            if (c == '[') {
                const int level = longBracketLevel(s, i);
                if (level >= 0) {
                    const int at = line;
                    tokens.push_back({LuaToken::String, readLongBracket(s, i, level, line), at});
                    continue;
                }
            }
            if (c == '"' || c == '\'') {
                const int at = line;
                std::string text;
                i++;
                while (i < s.size() && s[i] != c) {
                    if (s[i] == '\\' && i + 1 < s.size()) {
                        if (s[i + 1] == '\n') line++;
                        text += s[i + 1];
                        i += 2;
                        continue;
                    }
                    if (s[i] == '\n') { line++; break; }   // an unfinished string ends at its line
                    text += s[i++];
                }
                if (i < s.size() && s[i] == c) i++;
                tokens.push_back({LuaToken::String, std::move(text), at});
                continue;
            }
            if (std::isalpha((unsigned char)c) || c == '_') {
                const size_t start = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
                tokens.push_back({LuaToken::Name, s.substr(start, i - start), line});
                continue;
            }
            if (std::isdigit((unsigned char)c)) {
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '.' ||
                                        ((s[i] == '+' || s[i] == '-') && std::strchr("eEpP", s[i - 1]) != nullptr)))
                    i++;
                tokens.push_back({LuaToken::Other, "0", line});
                continue;
            }
            tokens.push_back({LuaToken::Other, std::string(1, c), line});
            i++;
        }
        return tokens;
    }

    // the identifier-like words of a string literal
    std::vector<std::string> wordsOf(const std::string& text) {
        std::vector<std::string> words;
        size_t i = 0;
        while (i < text.size()) {
            if (std::isalpha((unsigned char)text[i]) || text[i] == '_') {
                const size_t start = i;
                while (i < text.size() && (std::isalnum((unsigned char)text[i]) || text[i] == '_')) i++;
                words.push_back(text.substr(start, i - start));
            } else {
                i++;
            }
        }
        return words;
    }

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        return text;
    }

    // the class names a Lua type names ("Mesh?", "UIElement[]", "fun(arg1: AnimationEvent)")
    std::vector<std::string> typeNames(const std::string& type) {
        return wordsOf(type);
    }

    // every type a member gives a script: a property's type, a method's returns
    std::vector<std::string> memberTypes(const mcp::Json& member) {
        std::vector<std::string> out;
        if (member.contains("type") && member["type"].is_string()) {
            for (const std::string& name : typeNames(member["type"].get<std::string>())) out.push_back(name);
        }
        if (member.contains("overloads")) {
            for (const mcp::Json& overload : member["overloads"]) {
                for (const mcp::Json& ret : overload.value("returns", mcp::Json::array()))
                    if (ret.is_string())
                        for (const std::string& name : typeNames(ret.get<std::string>())) out.push_back(name);
            }
        }
        return out;
    }

    std::vector<std::string> stringList(const mcp::Json& value, const char* key) {
        std::vector<std::string> out;
        if (value.contains(key) && value[key].is_array())
            for (const mcp::Json& item : value[key])
                if (item.is_string()) out.push_back(item.get<std::string>());
        return out;
    }

    std::vector<const mcp::Json*> membersOf(const mcp::Json& cls) {
        std::vector<const mcp::Json*> out;
        for (const char* key : {"methods", "properties"})
            if (cls.contains(key))
                for (const mcp::Json& member : cls[key]) out.push_back(&member);
        return out;
    }

    // Members that make the engine create a module's objects although they are bound outside
    // it: a Spline Extrude's collision builds Rigidbodies (Mesh) or Box2D chains (Chain2D), as
    // the component's field does (SplineSystem::updateCollision). ModuleUsage.* holds each name
    // against the Lua API description.
    const std::vector<std::pair<std::string, std::vector<std::string>>>& creatingMembers() {
        static const std::vector<std::pair<std::string, std::vector<std::string>>> members = {
            {"collision", {"physics3d", "physics2d"}},
            {"SplineCollision", {"physics3d", "physics2d"}},
        };
        return members;
    }

    // the class a script property's object type names ("Mesh*", "UIDocument")
    std::string propertyClass(std::string type) {
        while (!type.empty() && (type.back() == '*' || type.back() == '&' || std::isspace((unsigned char)type.back()))) type.pop_back();
        const size_t scope = type.rfind("::");
        return scope == std::string::npos ? type : type.substr(scope + 2);
    }

    // the Lua API's tables, made once per description
    struct LuaTables {
        std::map<std::string, std::set<std::string>> nameModules;   // name -> modules it reaches
        std::map<std::string, std::set<std::string>> classModules;  // class -> its modules
        std::set<std::string> shaderNames;                          // shader-relevant entry points
        std::set<std::string> relevantClasses;
        std::set<std::string> videoNames;                           // the media API's entry points
        // the names of classes and enums that are no member of anything: in a string they reach
        // nothing (a global by its name needs _G or the like, a dynamic access)
        std::set<std::string> typeOnlyNames;
        // the names of classes and enums: the only entry points a name on its own reaches (Lua has
        // no implicit member scope: a member is reached after . or :, or by a string, as obj[k])
        std::set<std::string> globalNames;
    };

    LuaTables buildTables(const mcp::Json& api) {
        LuaTables tables;
        for (const auto& [module, names] : ModuleUsageDetector::moduleEntryPoints(api))
            for (const std::string& name : names) tables.nameModules[name].insert(module);
        if (api.contains("classes")) {
            for (const mcp::Json& cls : api["classes"]) {
                const std::string name = cls.value("name", std::string());
                for (const std::string& module : stringList(cls, "modules")) tables.classModules[name].insert(module);
                if (!ShaderNeutralApi::isNeutralClass(name) && ShaderNeutralApi::mixedClasses().count(name) == 0)
                    tables.relevantClasses.insert(name);
            }
        }
        tables.shaderNames = ModuleUsageDetector::shaderRelevantNames(api);
        std::set<std::string> members;
        if (api.contains("classes")) {
            for (const mcp::Json& cls : api["classes"]) {
                tables.typeOnlyNames.insert(cls.value("name", std::string()));
                tables.globalNames.insert(cls.value("name", std::string()));
                for (const mcp::Json* member : membersOf(cls)) members.insert(member->value("name", std::string()));
            }
        }
        for (const mcp::Json& en : api.value("enums", mcp::Json::array())) {
            tables.typeOnlyNames.insert(en.value("name", std::string()));
            tables.globalNames.insert(en.value("name", std::string()));
        }
        for (const std::string& member : members) tables.typeOnlyNames.erase(member);
        const auto& entry = ModuleUsageDetector::moduleEntryPoints(api);
        if (auto it = entry.find("video"); it != entry.end()) tables.videoNames = it->second;
        return tables;
    }

    class Analysis {
    public:
        Analysis(const ModuleScan& scan, const LuaTables& tables): scan(scan), tables(tables) {}

        ModuleUsage run() {
            if (!scan.stripEngineCode) {
                for (const std::string& module : player::allModules()) add(module, "Project Settings", "Strip Engine Code is off");
                return std::move(usage);
            }
            if (scan.debugOverlayInBuilds) add("htmlui", "Project Settings", "Debug > Overlay (an HTML UI document)");
            if (scan.navigationInBuilds) add("navigation", "Project Settings", "Navigation > Include in Builds");
            if (scan.audioImportTable) add("soundlayer", "Audio clips", "import settings that change what a clip plays");
            for (const std::string& source : scan.cppScripts)
                for (const std::string& module : detectedModules()) add(module, source, "C++ script (it can use any engine code)");
            for (const ModuleContent& content : scan.contents) scanContent(content);
            for (const ModuleSource& script : scan.luaScripts) scanLua(script.path, script.text, 0);
            for (const ModuleSource& document : scan.documents) scanDocument(document);
            // <video> needs the HTML UI that plays it
            if (usage.needs("video") && !usage.needs("htmlui")) usage.modules.erase("video");
            if (usage.needs("htmlui")) {
                for (const auto& [where, what] : videoUses) add("video", where, what);
            }
            if (!usage.needs("video")) {
                usage.evidence.erase(std::remove_if(usage.evidence.begin(), usage.evidence.end(),
                                                    [](const ModuleEvidence& e) { return e.module == "video"; }), usage.evidence.end());
            }
            return std::move(usage);
        }

    private:
        const ModuleScan& scan;
        const LuaTables& tables;
        ModuleUsage usage;
        // video evidence, kept until the HTML UI is known to be needed
        std::vector<std::pair<std::string, std::string>> videoUses;

        void add(const std::string& module, const std::string& where, const std::string& what) {
            usage.modules.insert(module);
            size_t count = 0;
            for (const ModuleEvidence& evidence : usage.evidence)
                if (evidence.module == module) count++;
            if (count < MAX_EVIDENCE) usage.evidence.push_back({module, where, what});
        }

        // A Video Player plays without a document, but the video module comes with the HTML UI
        // (engine/CMakeLists.txt): a build without it would play nothing
        void addVideoPlayer(const std::string& where, const std::string& what) {
            add("htmlui", where, what + " (video comes with the HTML UI)");
            add("video", where, what);
        }

        void addVideo(const std::string& where, const std::string& what) {
            if (videoUses.size() < MAX_EVIDENCE) videoUses.push_back({where, what});
        }

        void scanContent(const ModuleContent& content) {
            EntityRegistry* registry = content.registry;
            if (!registry) {
                for (const std::string& module : detectedModules()) add(module, content.label, "not loaded (its content is not known)");
                return;
            }
            for (Entity entity : content.entities) {
                if (!registry->isEntityCreated(entity)) continue;
                const std::string object = registry->getEntityName(entity);
                auto has = [&](auto* component) { return component != nullptr; };
                if (const AudioSourceComponent* audio = registry->findComponent<AudioSourceComponent>(entity)) {
                    if (!audio->event.empty()) add("soundlayer", content.label, "Audio Source " + object + " plays a Sound Event");
                    if (audio->playOn != 0) add("soundlayer", content.label, "Audio Source " + object + " has Play On triggers");
                    if (audio->pitchRandomSemitones != 0.0f || audio->volumeRandomDb != 0.0f) add("soundlayer", content.label, "Audio Source " + object + " has random ranges");
                }
                if (has(registry->findComponent<UIDocumentComponent>(entity))) add("htmlui", content.label, "UI Document on " + object);
                if (has(registry->findComponent<VideoPlayerComponent>(entity))) addVideoPlayer(content.label, "Video Player on " + object);
                if (has(registry->findComponent<RigidbodyComponent>(entity))) add("physics3d", content.label, "Rigidbody on " + object);
                if (has(registry->findComponent<Joint3DComponent>(entity))) add("physics3d", content.label, "Joint 3D on " + object);
                if (has(registry->findComponent<CharacterControllerComponent>(entity))) add("physics3d", content.label, "Character Controller on " + object);
                if (has(registry->findComponent<BuoyancyComponent>(entity))) add("physics3d", content.label, "Buoyancy on " + object);
                if (has(registry->findComponent<Rigidbody2DComponent>(entity))) add("physics2d", content.label, "Rigidbody 2D on " + object);
                if (has(registry->findComponent<Joint2DComponent>(entity))) add("physics2d", content.label, "Joint 2D on " + object);
                if (has(registry->findComponent<CharacterController2DComponent>(entity))) add("physics2d", content.label, "Character Controller 2D on " + object);
                if (const SplineExtrudeComponent* extrude = registry->findComponent<SplineExtrudeComponent>(entity)) {
                    if (extrude->collision == SplineCollision::Mesh) add("physics3d", content.label, "Spline Extrude collision (Mesh) on " + object);
                    if (extrude->collision == SplineCollision::Chain2D) add("physics2d", content.label, "Spline Extrude collision (Chain 2D) on " + object);
                }
                if (const ScriptComponent* scripts = registry->findComponent<ScriptComponent>(entity)) {
                    for (const ScriptEntry& script : scripts->scripts) {
                        if (script.type == ScriptType::CPP) {
                            for (const std::string& module : detectedModules())
                                add(module, content.label, "C++ script " + script.className + " on " + object);
                        }
                        for (const ScriptProperty& property : script.properties) {
                            if (property.ptrTypeName.empty()) continue;
                            const std::string cls = propertyClass(property.ptrTypeName);
                            const std::string what = "script property " + property.name + " (" + cls + ") on " + object;
                            if (auto it = tables.classModules.find(cls); it != tables.classModules.end())
                                for (const std::string& module : it->second) add(module, content.label, what);
                            if (tables.relevantClasses.count(cls)) add("runtimeshaders", content.label, what);
                        }
                    }
                }
            }
        }

        // a Lua chunk (a file, or a document's script starting at `firstLine`)
        void scanLua(const std::string& path, const std::string& text, int firstLine) {
            const std::vector<LuaToken> tokens = lexLua(text);
            auto where = [&](int line) { return path + ":" + std::to_string(firstLine + line); };
            auto afterMember = [&](size_t i) {
                return i > 0 && tokens[i - 1].kind == LuaToken::Other && (tokens[i - 1].text == "." || tokens[i - 1].text == ":");
            };
            for (size_t i = 0; i < tokens.size(); i++) {
                const LuaToken& token = tokens[i];
                if (token.kind == LuaToken::Name) {
                    // a name on its own is a local or a global: of the API, a class or an enum (a
                    // local or a table key named like a member, `local load`, `{duration = 2}`,
                    // reaches nothing of the engine)
                    const bool member = afterMember(i);
                    if (member || tables.globalNames.count(token.text)) lookUp(token.text, where(token.line));
                    if (!member) dynamicAccess(tokens, i, where(token.line));
                    if (lower(token.text).find("videoplayer") != std::string::npos) addVideoPlayer(where(token.line), token.text);
                    if (token.text == "DebugOverlay") add("htmlui", where(token.line), "DebugOverlay (an HTML UI document)");
                    if (lower(token.text).find("video") != std::string::npos) addVideo(where(token.line), token.text);
                } else if (token.kind == LuaToken::String) {
                    for (const std::string& word : wordsOf(token.text)) {
                        // a member by its name (obj["getRigidbody"]); a class or an enum is not reached so
                        if (!tables.typeOnlyNames.count(word)) lookUp(word, where(token.line));
                        if (word == "_G" || word == "_ENV") escape("\"" + word + "\" in a string", where(token.line));
                    }
                    if (lower(token.text).find("video") != std::string::npos) addVideo(where(token.line), "\"" + token.text.substr(0, 40) + "\"");
                }
            }
        }

        void lookUp(const std::string& name, const std::string& where) {
            if (auto it = tables.nameModules.find(name); it != tables.nameModules.end())
                for (const std::string& module : it->second) add(module, where, name);
            if (tables.videoNames.count(name)) addVideo(where, name);
            if (tables.shaderNames.count(name)) add("runtimeshaders", where, name);
            for (const auto& [member, modules] : creatingMembers())
                if (member == name)
                    for (const std::string& module : modules) add(module, where, name + " (creates bodies)");
        }

        void escape(const std::string& what, const std::string& where) {
            for (const std::string& module : detectedModules()) add(module, where, what + " (a dynamic global access)");
        }

        void dynamicAccess(const std::vector<LuaToken>& tokens, size_t i, const std::string& where) {
            const std::string& name = tokens[i].text;
            if (name == "require") {
                // require "x", require("x"): a project module, scanned as a file of its own
                const bool literal = (i + 1 < tokens.size() && tokens[i + 1].kind == LuaToken::String) ||
                                     (i + 3 < tokens.size() && tokens[i + 1].text == "(" && tokens[i + 2].kind == LuaToken::String &&
                                      tokens[i + 3].text == ")");
                if (!literal) escape("require with a computed name", where);
                return;
            }
            if (ModuleUsageDetector::dynamicAccessNames().count(name)) escape(name, where);
        }

        void scanDocument(const ModuleSource& document) {
            const std::string text = lower(document.text);
            if (text.find("<video") != std::string::npos) addVideo(document.path, "<video>");
            // the document's Lua: <script> bodies and on... event attributes
            size_t at = 0;
            while ((at = text.find("<script", at)) != std::string::npos) {
                const size_t open = text.find('>', at);
                if (open == std::string::npos) break;
                const size_t close = text.find("</script", open);
                const size_t end = close == std::string::npos ? text.size() : close;
                const int line = (int)std::count(document.text.begin(), document.text.begin() + (std::ptrdiff_t)open, '\n');
                scanLua(document.path, document.text.substr(open + 1, end - open - 1), line);
                at = end;
            }
            // attribute values whose name starts with "on" or "data-event"
            for (size_t i = 0; i + 3 < text.size(); i++) {
                const bool event = (text.compare(i, 2, "on") == 0 || text.compare(i, 10, "data-event") == 0) &&
                                   (i == 0 || std::isspace((unsigned char)text[i - 1]));
                if (!event) continue;
                const size_t equals = text.find('=', i);
                if (equals == std::string::npos || equals > i + 40) continue;
                size_t quote = equals + 1;
                while (quote < text.size() && std::isspace((unsigned char)text[quote])) quote++;
                if (quote >= text.size() || (text[quote] != '"' && text[quote] != '\'')) continue;
                const size_t endQuote = text.find(text[quote], quote + 1);
                if (endQuote == std::string::npos) continue;
                const int line = (int)std::count(document.text.begin(), document.text.begin() + (std::ptrdiff_t)quote, '\n');
                scanLua(document.path, document.text.substr(quote + 1, endQuote - quote - 1), line);
                i = endQuote;
            }
        }
    };

}

std::vector<std::string> ModuleUsage::list() const {
    std::vector<std::string> out;
    for (const std::string& module : player::allModules())
        if (needs(module)) out.push_back(module);
    return out;
}

std::string ModuleUsage::firstEvidence(const std::string& module) const {
    for (const ModuleEvidence& evidence : this->evidence)
        if (evidence.module == module) return evidence.where + ": " + evidence.what;
    return std::string();
}

std::vector<ModuleEvidence> ModuleUsage::evidenceOf(const std::string& module) const {
    std::vector<ModuleEvidence> out;
    for (const ModuleEvidence& evidence : this->evidence)
        if (evidence.module == module) out.push_back(evidence);
    return out;
}

const std::set<std::string>& ModuleUsageDetector::dynamicAccessNames() {
    // what reaches a global by a computed name: the global tables, the chunk loaders (a string
    // run as code), the debug library (registry, upvalues, locals) and package (package.loaded._G)
    static const std::set<std::string> names = {"_G", "_ENV", "getfenv", "setfenv", "load", "loadstring", "loadfile",
                                                "dofile", "debug", "package"};
    return names;
}

std::map<std::string, std::set<std::string>> ModuleUsageDetector::moduleEntryPoints(const mcp::Json& api) {
    std::map<std::string, std::set<std::string>> out;
    if (!api.contains("classes")) return out;
    // the classes of each module
    std::map<std::string, std::set<std::string>> classesOf;   // module -> classes
    for (const mcp::Json& cls : api["classes"])
        for (const std::string& module : stringList(cls, "modules")) classesOf[module].insert(cls.value("name", std::string()));
    for (const auto& [module, classes] : classesOf)
        for (const std::string& name : classes)
            if (name.rfind("FunctionSubscribe_", 0) != 0) out[module].insert(name);
    for (const mcp::Json& cls : api["classes"]) {
        const std::string clsName = cls.value("name", std::string());
        const std::vector<std::string> clsModules = stringList(cls, "modules");
        for (const mcp::Json* member : membersOf(cls)) {
            const std::string name = member->value("name", std::string());
            // a member under a module's condition, on a class outside the module
            for (const std::string& module : stringList(*member, "modules"))
                if (std::find(clsModules.begin(), clsModules.end(), module) == clsModules.end()) out[module].insert(name);
            // a member that gives a script one of a module's objects
            for (const std::string& type : memberTypes(*member))
                for (const auto& [module, classes] : classesOf)
                    if (classes.count(type) && std::find(clsModules.begin(), clsModules.end(), module) == clsModules.end())
                        out[module].insert(name);
        }
    }
    for (const mcp::Json& en : api.value("enums", mcp::Json::array()))
        for (const std::string& module : stringList(en, "modules")) out[module].insert(en.value("name", std::string()));
    for (const mcp::Json& event : api.value("events", mcp::Json::array())) {
        for (const mcp::Json& arg : event.value("args", mcp::Json::array()))
            for (const std::string& type : typeNames(arg.value("type", std::string())))
                for (const auto& [module, classes] : classesOf)
                    if (classes.count(type)) out[module].insert(event.value("name", std::string()));
    }
    return out;
}

std::set<std::string> ModuleUsageDetector::shaderRelevantNames(const mcp::Json& api) {
    std::set<std::string> out;
    if (!api.contains("classes")) return out;
    std::set<std::string> relevant;
    for (const mcp::Json& cls : api["classes"]) {
        const std::string name = cls.value("name", std::string());
        if (!ShaderNeutralApi::isNeutralClass(name) && ShaderNeutralApi::mixedClasses().count(name) == 0) relevant.insert(name);
    }
    out = relevant;
    for (const mcp::Json& cls : api["classes"]) {
        const std::string name = cls.value("name", std::string());
        const auto mixed = ShaderNeutralApi::mixedClasses().find(name);
        for (const mcp::Json* member : membersOf(cls)) {
            const std::string memberName = member->value("name", std::string());
            if (mixed != ShaderNeutralApi::mixedClasses().end() && mixed->second.count(memberName) == 0) out.insert(memberName);
            // a member that gives a script a shader-relevant object is an entry point to it
            for (const std::string& type : memberTypes(*member))
                if (relevant.count(type) && !relevant.count(name)) out.insert(memberName);
        }
    }
    return out;
}

ModuleUsage ModuleUsageDetector::analyze(const ModuleScan& scan, const mcp::Json& luaApi) {
    const LuaTables tables = buildTables(luaApi);
    return Analysis(scan, tables).run();
}

ModuleUsage ModuleUsageDetector::analyze(const ModuleScan& scan) {
    static const LuaTables tables = buildTables(LuaApiData::json());
    return Analysis(scan, tables).run();
}

ModuleScan ModuleUsageDetector::collect(Project* project) {
    ModuleScan scan;
    scan.stripEngineCode = project->isStripEngineCode();
    scan.navigationInBuilds = project->getNavigationSettings().includeInBuilds;
    std::vector<std::string> importProblems;
    scan.audioImportTable = !AudioImportRegistry::collect(*project, importProblems).empty();
    const DebugSettings::Mode overlay = project->getDebugSettings().overlay;
    scan.debugOverlayInBuilds = overlay == DebugSettings::Mode::AllBuilds ||
                                (overlay == DebugSettings::Mode::Development && project->isDevelopmentBuild());
    for (const SceneScriptSource& script : project->getBuildCppScripts())
        if (!script.path.empty()) scan.cppScripts.push_back(script.path.generic_string());
    for (SceneProject& scene : project->getScenes()) {
        if (scene.filepath.empty()) continue;   // not a scene of the build (an export saves it first)
        scan.contents.push_back({"scene " + scene.name, scene.scene, scene.entities});
    }
    // the prefabs the build registers: a script can spawn any of them (as collectRequiredShaderKeys
    // reads them: one not loaded is read from its file, a prefab holding instances of others is an
    // instance as the editor makes it)
    for (const PrefabSceneInfo& info : project->collectAllPrefabs()) {
        const std::string label = "prefab " + info.prefabPath.generic_string();
        const Prefab* loadedPrefab = project->getPrefab(info.prefabPath);
        bool layered = false;
        if (loadedPrefab && loadedPrefab->registry) {
            for (Entity entity : loadedPrefab->registryEntities)
                if (loadedPrefab->isInstanceRoot(entity)) layered = true;
        }
        if (layered) {
            auto preview = std::make_shared<Scene>();
            if (project->instantiatePrefabPreview(preview.get(), info.prefabPath) != NULL_ENTITY) {
                scan.contents.push_back({label, preview.get(), preview->getEntityList()});
                scan.owned.push_back(preview);
                continue;
            }
        }
        if (loadedPrefab && loadedPrefab->registry) {
            scan.contents.push_back({label, loadedPrefab->registry.get(), loadedPrefab->registryEntities});
            continue;
        }
        try {
            Prefab loaded;
            loaded.loadRegistry(DocumentFiles::read(project->getProjectPath() / info.prefabPath, DocumentKind::Prefab).root);
            std::shared_ptr<EntityRegistry> registry(std::move(loaded.registry));
            scan.contents.push_back({label, registry.get(), loaded.registryEntities});
            scan.owned.push_back(registry);
        } catch (const std::exception& e) {
            scan.contents.push_back({label + " (" + e.what() + ")", nullptr, {}});
        }
    }
    auto read = [&](const fs::path& relative) {
        std::ifstream in(project->getProjectPath() / relative, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    for (const fs::path& path : project->findProjectFiles([](const std::string& p) { return Util::isLuaFile(p); }))
        scan.luaScripts.push_back({path.generic_string(), read(path)});
    for (const fs::path& path : project->findProjectFiles([](const std::string& p) {
             const std::string ext = lower(fs::path(p).extension().string());
             return ext == ".html" || ext == ".htm" || ext == ".rml" || ext == ".xhtml";
         }))
        scan.documents.push_back({path.generic_string(), read(path)});
    return scan;
}
