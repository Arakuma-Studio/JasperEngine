// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    class McpCall;
    using CallPtr = std::shared_ptr<McpCall>;

    // MCP tool annotations (hints for the client, not enforced).
    struct ToolAnnotations {
        bool readOnly = false;
        bool destructive = false;
        bool idempotent = false;
        bool openWorld = false;
    };

    // What a tool does, for the server's busy policy (McpServer::busyRefusal): a running job
    // that holds part of the editor exclusively (an export reading the project from its own
    // thread, a C++ script build) refuses the activities that would race it.
    enum class ToolActivity {
        Auto,           // Read when annotations.readOnly, else Edit
        Read,           // reads only: always allowed
        Control,        // jobs, dialogs, views, clipboard: always allowed (checks its own actions)
        Edit,           // changes the project: refused while an export runs
        PlayStart,      // refused while an export or a C++ script build runs
        Build,          // export / build player / shaders: refused while another export or a script build runs
        ScriptCompile,  // C++ script build: refused while an export runs
        ProjectSwitch   // open / create a project: refused while any exclusive job runs
    };

    struct ToolDef {
        std::string name;          // snake_case verb_noun, e.g. "create_entity"
        std::string title;         // human label
        std::string description;   // what it does, when to use it, gotchas. Agents read this.
        Json inputSchema;          // JSON Schema object; put real constraints here
        Json outputSchema;         // optional (null = none)
        ToolAnnotations annotations;
        // A read-only tool that writes files for some arguments (screenshot's save_path): true for
        // such a call. The server then treats it as a change (McpServer::mayChange): refused to HTTP
        // clients while "Allow changes" is off, and settled by auto-sync afterwards.
        std::function<bool(const Json& args)> writesFiles;
        bool allowedWhilePlaying = true;       // the server refuses the call during play if false
        ToolActivity activity = ToolActivity::Auto;  // busy policy (see ToolActivity)
        // Tool group (McpToolGroups.h): empty = core, always listed; any other group is listed
        // once a session enables it, and callable either way. New tools of a spec go into
        // that spec's group (docs/mcp/conventions.md).
        std::string group;
        // Section of docs/mcp/TOOLS.md; buildToolRegistry fills it from the registering module
        std::string module;
        // The call's undo steps are the agent's (S24 R24): edit tools always; a control tool
        // whose steps are edits of the user's state too (select_entities) sets this
        bool attributesUndo = false;
        // Answered on the transport's thread, never queued for the main thread, so it replies
        // while the editor is hung (get_editor_health only): the handler may read only state that
        // is safe from any thread, and no busy, play or dialog checks apply
        bool offMainThread = false;
        std::function<void(CallPtr)> handler;  // runs on the MAIN thread (but offMainThread)
    };

    // One item of resources/read
    struct ResourceContent {
        std::string uri;
        std::string mimeType;
        std::string text;          // text content, or
        std::string blobBase64;    // binary content (base64), when text is empty
    };

    struct ResourceResult {
        bool ok = true;
        std::string error;         // when !ok: JSON-RPC error message (resource not found...)
        std::vector<ResourceContent> contents;
    };

    // A static resource, listed by resources/list. read runs on the main thread.
    struct ResourceDef {
        std::string uri;           // e.g. jasper://guide
        std::string name;
        std::string title;
        std::string description;
        std::string mimeType;
        std::function<ResourceResult(const std::string& uri)> read;
    };

    // A parameterized resource (RFC 6570 level 1-2: {var} matches one path segment,
    // {+var} matches the rest of the uri), listed by resources/templates/list.
    struct ResourceTemplateDef {
        std::string uriTemplate;   // e.g. jasper://scene/{id}
        std::string name;
        std::string title;
        std::string description;
        std::string mimeType;
        std::function<ResourceResult(const std::string& uri, const std::map<std::string, std::string>& vars)> read;
    };

    struct PromptArgument {
        std::string name;
        std::string description;
        bool required = false;
    };

    struct PromptResult {
        bool ok = true;
        std::string error;
        std::string description;
        Json messages = Json::array();   // [{role, content:{type:"text", text}}]
    };

    struct PromptDef {
        std::string name;
        std::string title;
        std::string description;
        std::vector<PromptArgument> arguments;
        std::function<PromptResult(const std::map<std::string, std::string>& args)> get;  // main thread
    };

    // Tools, resources and prompts. Filled once at server start on the main thread
    // (McpServer calls every register*Tools function), read-only afterwards, so the
    // transport threads list it without locking.
    class McpRegistry {
    private:
        std::vector<ToolDef> toolList;
        std::vector<ResourceDef> resourceList;
        std::vector<ResourceTemplateDef> templateList;
        std::vector<PromptDef> promptList;

    public:
        // The same name replaces the definition, keeping its list position.
        void addTool(ToolDef def);
        void addResource(ResourceDef def);
        void addResourceTemplate(ResourceTemplateDef def);
        void addPrompt(PromptDef def);

        const ToolDef* findTool(const std::string& name) const;
        const std::vector<ToolDef>& tools() const { return toolList; }
        // While the registry is built only (buildToolRegistry stamps module and group)
        std::vector<ToolDef>& mutableTools() { return toolList; }
        const std::vector<ResourceDef>& resources() const { return resourceList; }
        const std::vector<ResourceTemplateDef>& resourceTemplates() const { return templateList; }
        const std::vector<PromptDef>& prompts() const { return promptList; }
        const PromptDef* findPrompt(const std::string& name) const;

        // A static resource with this uri, or else the first template matching it (vars filled).
        const ResourceDef* findResource(const std::string& uri) const;
        const ResourceTemplateDef* matchResourceTemplate(const std::string& uri, std::map<std::string, std::string>& vars) const;
        // Reads a resource or template by uri (resources/read and read_file). found is false when nothing
        // matches the uri; a reader's exception becomes a failed result.
        ResourceResult readResource(const std::string& uri, bool& found) const;

        static bool matchUriTemplate(const std::string& uriTemplate, const std::string& uri, std::map<std::string, std::string>& vars);

        // MCP wire shapes. A tool carries _meta {"jasper/group", "jasper/module", "jasper/while_playing"}.
        static Json toolToJson(const ToolDef& def);
        // The group a tool is listed in ("core" for an empty group)
        static const std::string& groupOf(const ToolDef& def);
        // What a tool costs an agent per session, as tools/mcp/tool_budget.py measures it:
        // characters (code points) of name + description + compact JSON of the listed inputSchema
        static size_t toolChars(const ToolDef& def);
    };

}
