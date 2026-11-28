// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpRegistry.h"

#include "McpCall.h"
#include "McpToolGroups.h"
#include "util/NearestName.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;

namespace jasperengine::editor::mcp {

namespace {

std::string argumentNamesText(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) text += (text.empty() ? "" : ", ") + name;
    return text.empty() ? std::string("none") : text;
}

std::vector<std::string> schemaKeys(const Json& object) {
    std::vector<std::string> keys;
    for (auto it = object.begin(); it != object.end(); ++it) keys.push_back(it.key());
    return keys;
}

bool closedObject(const Json& schema) {
    return schema.contains("additionalProperties") && schema["additionalProperties"] == false;
}

struct NameMiss {
    std::string given;
    std::string nearest;
};

// The names in a call's arguments that its schema does not know but has a close match for (S99 R1):
// a key of a closed object below the top level, and a string outside its enum (not even in another
// case or with other underscores). anyOf branches are not entered: which one was meant is unknown.
void collectNameMisses(const Json& schema, const Json& value, std::vector<NameMiss>& misses) {
    if (!schema.is_object()) return;
    if (value.is_string() && schema.contains("enum") && schema["enum"].is_array()) {
        std::vector<std::string> options;
        for (const Json& option : schema["enum"]) {
            if (option.is_string()) options.push_back(option.get<std::string>());
        }
        const std::string& text = value.get_ref<const std::string&>();
        const std::string folded = FoldedName(text);
        const bool known = std::any_of(options.begin(), options.end(), [&](const std::string& o) { return FoldedName(o) == folded; });
        if (!known) {
            std::string nearest = NearestIdentifier(text, options);
            if (!nearest.empty()) misses.push_back({text, std::move(nearest)});
        }
    }
    if (value.is_object() && schema.contains("properties") && schema["properties"].is_object()) {
        const Json& props = schema["properties"];
        const std::vector<std::string> keys = schemaKeys(props);
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (props.contains(it.key())) {
                collectNameMisses(props[it.key()], it.value(), misses);
            } else if (closedObject(schema)) {
                std::string nearest = NearestIdentifier(it.key(), keys);
                if (!nearest.empty()) misses.push_back({it.key(), std::move(nearest)});
            }
        }
    }
    if (value.is_array() && schema.contains("items") && schema["items"].is_object()) {
        for (const Json& item : value) collectNameMisses(schema["items"], item, misses);
    }
}

// Where the error mentions `name`, quoted or as a whole word: right after it, else npos
size_t afterMention(const std::string& text, const std::string& name) {
    for (const char* quote : {"\"", "'", "`"}) {
        const size_t found = text.find(quote + name + quote);
        if (found != std::string::npos) return found + name.size() + 2;
    }
    auto wordChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t found = text.find(name); found != std::string::npos; found = text.find(name, found + 1)) {
        const size_t end = found + name.size();
        if ((found == 0 || !wordChar(text[found - 1])) && (end == text.size() || !wordChar(text[end]))) return end;
    }
    return std::string::npos;
}

} // namespace

std::string McpRegistry::argumentProblem(const std::string& tool, const Json& schema, const Json& args) {
    if (!args.is_object() || !closedObject(schema) || !schema.contains("properties") || !schema["properties"].is_object()) return {};
    const Json& props = schema["properties"];
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (props.contains(it.key())) continue;
        const std::vector<std::string> known = schemaKeys(props);
        return "Unknown argument \"" + it.key() + "\" for " + tool + NearestIdentifierSuffix(it.key(), known) +
               ". Arguments: " + argumentNamesText(known) + ".";
    }
    return {};
}

void McpRegistry::suggestNearestNames(const Json& schema, const Json& args, std::string& errorText) {
    if (errorText.find("did you mean") != std::string::npos) return;
    std::vector<NameMiss> misses;
    collectNameMisses(schema, args, misses);
    // after the handler's own mention of the name; an error that does not mention it is about
    // something else (a handler may take aliases its enum does not list, or keys a lean schema does not)
    for (const NameMiss& miss : misses) {
        const size_t at = afterMention(errorText, miss.given);
        if (at == std::string::npos) continue;
        errorText.insert(at, " (did you mean '" + miss.nearest + "'?)");
        return;
    }
}

std::string McpRegistry::nearestToolName(const std::string& name) const {
    std::vector<std::string> names;
    for (const ToolDef& def : toolList) names.push_back(def.name);
    return NearestIdentifier(name, names);
}

std::string McpRegistry::unknownToolMessage(const std::string& name) const {
    const std::string nearest = nearestToolName(name);
    return "Unknown tool: " + name + (nearest.empty() ? std::string() : " (did you mean '" + nearest + "'?)") +
           ". tool_groups lists every tool by group.";
}

void McpRegistry::addTool(ToolDef def) {
#ifndef NDEBUG
    // A typo in a group name would hide the tool from every listing for good
    if (!findToolGroup(def.group)) {
        std::fprintf(stderr, "MCP tool %s is registered into the unknown group \"%s\" (editor/mcp/McpToolGroups.cpp)\n",
                     def.name.c_str(), def.group.c_str());
        std::abort();
    }
#endif
    if (def.inputSchema.is_null()) {
        def.inputSchema = Json{{"type", "object"}, {"properties", Json::object()}};
    }
    // additionalProperties: false is enforced here (a misspelled argument fails, naming the nearest
    // valid one and listing them) instead of being sent in tools/list (see toolToJson)
    const Json& props = def.inputSchema.contains("properties") ? def.inputSchema["properties"] : Json();
    if (def.handler && props.is_object() && def.inputSchema.value("additionalProperties", true) == false) {
        def.handler = [inner = def.handler, name = def.name, schema = def.inputSchema](CallPtr call) {
            const std::string problem = argumentProblem(name, schema, call->args());
            if (!problem.empty()) {
                call->fail(problem);
                return;
            }
            inner(call);
        };
    }
    for (ToolDef& existing : toolList) {
        if (existing.name == def.name) {
            existing = std::move(def);
            return;
        }
    }
    toolList.push_back(std::move(def));
}

void McpRegistry::addResource(ResourceDef def) {
    for (ResourceDef& existing : resourceList) {
        if (existing.uri == def.uri) {
            existing = std::move(def);
            return;
        }
    }
    resourceList.push_back(std::move(def));
}

void McpRegistry::addResourceTemplate(ResourceTemplateDef def) {
    for (ResourceTemplateDef& existing : templateList) {
        if (existing.uriTemplate == def.uriTemplate) {
            existing = std::move(def);
            return;
        }
    }
    templateList.push_back(std::move(def));
}

void McpRegistry::addPrompt(PromptDef def) {
    for (PromptDef& existing : promptList) {
        if (existing.name == def.name) {
            existing = std::move(def);
            return;
        }
    }
    promptList.push_back(std::move(def));
}

const ToolDef* McpRegistry::findTool(const std::string& name) const {
    for (const ToolDef& def : toolList) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

const PromptDef* McpRegistry::findPrompt(const std::string& name) const {
    for (const PromptDef& def : promptList) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

const ResourceDef* McpRegistry::findResource(const std::string& uri) const {
    for (const ResourceDef& def : resourceList) {
        if (def.uri == uri) return &def;
    }
    return nullptr;
}

const ResourceTemplateDef* McpRegistry::matchResourceTemplate(const std::string& uri, std::map<std::string, std::string>& vars) const {
    for (const ResourceTemplateDef& def : templateList) {
        vars.clear();
        if (matchUriTemplate(def.uriTemplate, uri, vars)) return &def;
    }
    vars.clear();
    return nullptr;
}

ResourceResult McpRegistry::readResource(const std::string& uri, bool& found) const {
    found = true;
    try {
        if (const ResourceDef* def = findResource(uri)) {
            return def->read ? def->read(uri) : ResourceResult{false, "Resource has no reader.", {}};
        }
        std::map<std::string, std::string> vars;
        if (const ResourceTemplateDef* tmpl = matchResourceTemplate(uri, vars)) {
            return tmpl->read ? tmpl->read(uri, vars) : ResourceResult{false, "Resource has no reader.", {}};
        }
    } catch (const std::exception& e) {
        return ResourceResult{false, e.what(), {}};
    }
    found = false;
    std::vector<std::string> uris;
    for (const ResourceDef& def : resourceList) uris.push_back(def.uri);
    return ResourceResult{false, "Resource not found: " + uri + NearestIdentifierSuffix(uri, uris) + ".", {}};
}

bool McpRegistry::matchUriTemplate(const std::string& uriTemplate, const std::string& uri, std::map<std::string, std::string>& vars) {
    size_t t = 0;
    size_t u = 0;
    while (t < uriTemplate.size()) {
        if (uriTemplate[t] == '{') {
            const size_t close = uriTemplate.find('}', t);
            if (close == std::string::npos) return false;
            std::string name = uriTemplate.substr(t + 1, close - t - 1);
            const bool reserved = !name.empty() && name[0] == '+';
            if (reserved) name.erase(0, 1);
            t = close + 1;
            // The literal that follows bounds the value
            const char stop = t < uriTemplate.size() ? uriTemplate[t] : '\0';
            size_t end = u;
            while (end < uri.size() && uri[end] != stop && (reserved || uri[end] != '/')) end++;
            if (end == u) return false;
            vars[name] = uri.substr(u, end - u);
            u = end;
        } else {
            if (u >= uri.size() || uri[u] != uriTemplate[t]) return false;
            t++;
            u++;
        }
    }
    return u == uri.size();
}

Json McpRegistry::toolToJson(const ToolDef& def) {
    Json tool = {
        {"name", def.name},
        {"description", def.description},
        {"inputSchema", def.inputSchema}
    };
    // Enforced by addTool's wrapper; tool definitions are paid for in every agent session
    if (tool["inputSchema"].value("additionalProperties", true) == false) tool["inputSchema"].erase("additionalProperties");
    if (!def.title.empty()) tool["title"] = def.title;
    if (!def.outputSchema.is_null()) tool["outputSchema"] = def.outputSchema;
    Json annotations = {
        {"readOnlyHint", def.annotations.readOnly},
        {"destructiveHint", def.annotations.destructive},
        {"idempotentHint", def.annotations.idempotent},
        {"openWorldHint", def.annotations.openWorld}
    };
    if (!def.title.empty()) annotations["title"] = def.title;
    tool["annotations"] = annotations;
    // for clients and the CLI's help; _meta is not part of what the budget measures
    tool["_meta"] = Json{{"jasper/group", groupOf(def)}, {"jasper/module", def.module},
                         {"jasper/while_playing", def.allowedWhilePlaying}};
    return tool;
}

const std::string& McpRegistry::groupOf(const ToolDef& def) {
    static const std::string core = CORE_TOOL_GROUP;
    return def.group.empty() ? core : def.group;
}

size_t McpRegistry::toolChars(const ToolDef& def) {
    const Json listed = toolToJson(def);
    const std::string text = def.name + def.description + listed["inputSchema"].dump(-1, ' ', false, Json::error_handler_t::replace);
    // code points, as Python's len() counts them
    size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) count++;
    }
    return count;
}

}  // namespace jasperengine::editor::mcp
