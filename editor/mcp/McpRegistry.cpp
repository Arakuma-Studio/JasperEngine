// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpRegistry.h"

#include <algorithm>

using namespace jasperengine::editor::mcp;

void McpRegistry::addTool(ToolDef def) {
    if (def.inputSchema.is_null()) {
        def.inputSchema = Json{{"type", "object"}, {"properties", Json::object()}};
    }
    for (ToolDef& existing : toolList) {
        if (existing.name == def.name) {
            existing = std::move(def);
            return;
        }
    }
    toolList.push_back(std::move(def));
}

void McpRegistry::removeTool(const std::string& name) {
    toolList.erase(std::remove_if(toolList.begin(), toolList.end(),
        [&](const ToolDef& def) { return def.name == name; }), toolList.end());
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
    return tool;
}
