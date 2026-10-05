// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W4: agent guide + reference docs as MCP resources, engine API search, prompts
#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/legacy/EngineApiContext.h"

#include "Project.h"

#include "mcp_docs.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;

namespace {

struct Doc {
    std::string name;
    std::string text;
    std::string title;   // first "# " heading
};

const std::vector<Doc>& docs() {
    static const std::vector<Doc> list = []() {
        std::vector<Doc> out;
        for (const McpEmbeddedDoc& embedded : mcp_embedded_docs) {
            const std::string name = embedded.name;
            if (name == "README") continue;   // the maintainers' index; jasper://docs replaces it
            Doc doc;
            doc.name = name;
            doc.text.assign(reinterpret_cast<const char*>(embedded.data), embedded.size);
            std::istringstream in(doc.text);
            std::string line;
            while (std::getline(in, line)) {
                if (line.rfind("# ", 0) == 0) {
                    doc.title = line.substr(2);
                    break;
                }
            }
            if (doc.title.empty()) doc.title = name;
            out.push_back(std::move(doc));
        }
        std::sort(out.begin(), out.end(), [](const Doc& a, const Doc& b) { return a.name < b.name; });
        return out;
    }();
    return list;
}

std::string docsIndex() {
    std::string text = "# JasperEngine agent reference\n\nRead with resources/read:\n\n";
    text += "- `jasper://guide`: the agent guide (same as the initialize instructions)\n";
    for (const Doc& doc : docs()) text += "- `jasper://docs/" + doc.name + "`: " + doc.title + "\n";
    text += "- `jasper://project/AGENTS.md`: the open project's AGENTS.md (layout, build, generated files)\n";
    text += "- `jasper://project/{path}`: any text file of the open project (scripts, materials, shaders...)\n";
    text += "\nTools: `search_engine_api` (API symbols + these docs), `search_engine_source` / `read_engine_source` "
            "(the engine's real headers and Lua bindings).\n";
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Lines of the reference docs matching every word of the query, with their section heading
Json searchDocs(const std::string& query, size_t maxResults) {
    std::vector<std::string> words;
    {
        std::istringstream in(lower(query));
        std::string word;
        while (in >> word) words.push_back(word);
    }
    Json hits = Json::array();
    if (words.empty()) return hits;
    struct Hit {
        int score;
        Json data;
    };
    std::vector<Hit> scored;
    for (const Doc& doc : docs()) {
        std::istringstream in(doc.text);
        std::string line, heading;
        int number = 0;
        while (std::getline(in, line)) {
            number++;
            if (line.rfind("#", 0) == 0) {
                heading = line;
                while (!heading.empty() && heading[0] == '#') heading.erase(0, 1);
                if (!heading.empty() && heading[0] == ' ') heading.erase(0, 1);
            }
            const std::string l = lower(line);
            int score = 0;
            bool all = true;
            for (const std::string& word : words) {
                if (l.find(word) != std::string::npos) score += 10;
                else if (lower(heading).find(word) != std::string::npos) score += 4;
                else all = false;
            }
            if (!all || score == 0) continue;
            std::string text = line;
            if (text.size() > 300) text = text.substr(0, 300) + " ...";
            scored.push_back({score, Json{{"doc", "jasper://docs/" + doc.name}, {"section", heading}, {"line", number}, {"text", text}}});
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    for (const Hit& hit : scored) {
        if (hits.size() >= maxResults) break;
        hits.push_back(hit.data);
    }
    return hits;
}

ResourceResult textResource(const std::string& uri, const std::string& mime, const std::string& text) {
    ResourceResult result;
    result.contents.push_back({uri, mime, text, {}});
    return result;
}

ResourceResult notFound(const std::string& message) {
    ResourceResult result;
    result.ok = false;
    result.error = message;
    return result;
}

void searchEngineApiTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string query = args.value("query", "");
    if (query.empty()) {
        call->fail("query is required, e.g. \"Rigidbody2D applyForce\" or \"Input isKeyPressed\".");
        return;
    }
    const int maxResults = std::clamp(args.value("max_results", 16), 1, 50);
    Json api = legacy::EngineApiContext::search(query, args.value("parent", ""), maxResults);
    Json results = Json::array();
    std::string text = "API symbols for \"" + query + "\":";
    for (const Json& item : api.value("results", Json::array())) {
        Json compact = {{"detail", item.value("detail", "")}, {"kind", item.value("kind", "")}};
        if (item.contains("lua_call_hint")) compact["lua"] = item["lua_call_hint"];
        if (item.contains("lua_note")) compact["note"] = item["lua_note"];
        if (item.contains("lua_callable")) compact["lua_callable"] = item["lua_callable"];
        results.push_back(compact);
        text += "\n- " + compact["detail"].get<std::string>() + " [" + compact["kind"].get<std::string>() + "]";
        if (compact.contains("note")) text += " (" + compact["note"].get<std::string>() + ")";
    }
    if (results.empty()) text += " none.";
    Json docHits = Json::array();
    if (args.value("include_docs", true)) {
        docHits = searchDocs(query, static_cast<size_t>(std::clamp(args.value("max_doc_results", 8), 0, 40)));
        if (!docHits.empty()) {
            text += "\nIn the reference docs:";
            for (const Json& hit : docHits) {
                text += "\n- " + hit["doc"].get<std::string>() + " (" + hit["section"].get<std::string>() + ", line " +
                        std::to_string(hit["line"].get<int>()) + "): " + hit["text"].get<std::string>();
            }
        }
    }
    call->ok(text, Json{{"results", results}, {"docs", docHits}});
}

} // namespace

void registerDocsTools(McpRegistry& registry) {
    using namespace schema;

    // jasper://guide (replaces the core placeholder): the instructions + where to read more
    {
        ResourceDef guide;
        guide.uri = "jasper://guide";
        guide.name = "guide";
        guide.title = "JasperEngine agent guide";
        guide.description = "How to drive the JasperEngine editor through MCP: the initialize instructions, plus the list of "
                            "reference documents.";
        guide.mimeType = "text/markdown";
        guide.read = [](const std::string& uri) {
            return textResource(uri, "text/markdown", std::string(mcpServerInstructions()) + "\n\n" + docsIndex());
        };
        registry.addResource(std::move(guide));
    }
    {
        ResourceDef index;
        index.uri = "jasper://docs";
        index.name = "docs";
        index.title = "Reference documents";
        index.description = "Index of the jasper://docs/... reference documents.";
        index.mimeType = "text/markdown";
        index.read = [](const std::string& uri) { return textResource(uri, "text/markdown", docsIndex()); };
        registry.addResource(std::move(index));
    }
    for (const Doc& doc : docs()) {
        ResourceDef def;
        def.uri = "jasper://docs/" + doc.name;
        def.name = doc.name;
        def.title = doc.title;
        def.description = "Reference: " + doc.title + ".";
        def.mimeType = "text/markdown";
        const std::string name = doc.name;
        def.read = [name](const std::string& uri) {
            for (const Doc& d : docs()) {
                if (d.name == name) return textResource(uri, "text/markdown", d.text);
            }
            return notFound("No document " + name + ".");
        };
        registry.addResource(std::move(def));
    }
    {
        // Any other jasper://docs/<name> gets a helpful error listing the documents
        ResourceTemplateDef tmpl;
        tmpl.uriTemplate = "jasper://docs/{name}";
        tmpl.name = "doc";
        tmpl.title = "Reference document";
        tmpl.description = "A reference document by name (see jasper://docs).";
        tmpl.mimeType = "text/markdown";
        tmpl.read = [](const std::string& uri, const std::map<std::string, std::string>& vars) {
            const std::string name = vars.count("name") ? vars.at("name") : "";
            for (const Doc& d : docs()) {
                if (d.name == name || d.name + ".md" == name) return textResource(uri, "text/markdown", d.text);
            }
            std::string known;
            for (const Doc& d : docs()) known += " " + d.name;
            return notFound("No document \"" + name + "\". Documents:" + known + ".");
        };
        registry.addResourceTemplate(std::move(tmpl));
    }
    {
        ResourceDef agents;
        agents.uri = "jasper://project/AGENTS.md";
        agents.name = "project-agents";
        agents.title = "Project AGENTS.md";
        agents.description = "The open project's AGENTS.md: project layout, generated files, C++ build rules, plus any "
                             "project-specific instructions its authors added.";
        agents.mimeType = "text/markdown";
        agents.read = [](const std::string& uri) {
            Project* project = McpServer::get().context().project();
            std::string text;
            if (!project || !w4::readFile(project->getProjectPath() / "AGENTS.md", text)) {
                return notFound("The project has no AGENTS.md yet (the editor writes it when the project is saved or built).");
            }
            return textResource(uri, "text/markdown", text);
        };
        registry.addResource(std::move(agents));
    }
    {
        ResourceTemplateDef tmpl;
        tmpl.uriTemplate = "jasper://project/{+path}";
        tmpl.name = "project-file";
        tmpl.title = "Project file";
        tmpl.description = "A text file of the open project by project-relative path, e.g. jasper://project/scripts/Player.lua.";
        tmpl.mimeType = "text/plain";
        tmpl.read = [](const std::string& uri, const std::map<std::string, std::string>& vars) {
            Project* project = McpServer::get().context().project();
            const std::string rel = vars.count("path") ? vars.at("path") : "";
            fs::path file;
            std::string error;
            if (!McpRefs::resolveProjectPath(project, rel, file, error, true)) return notFound(error);
            if (fs::is_directory(file) || !w4::isTextFile(file)) return notFound(rel + " is not a text file.");
            std::string text;
            if (!w4::readFile(file, text)) return notFound("Can't read " + rel + ".");
            if (text.size() > 1024 * 1024) text = text.substr(0, 1024 * 1024) + "\n... (truncated)";
            const std::string ext = w4::extensionOf(file);
            const std::string mime = ext == ".md" ? "text/markdown" : ext == ".json" ? "application/json" : "text/plain";
            return textResource(uri, mime, text);
        };
        registry.addResourceTemplate(std::move(tmpl));
    }

    {
        ToolDef tool;
        tool.name = "search_engine_api";
        tool.title = "Search Engine API";
        tool.description =
            "Search the engine's scripting API (generated from the Lua bindings and C++ headers) and the reference docs "
            "(jasper://docs/...). Use it before writing script code to confirm a class, method, property, enum or macro "
            "exists. Each result's detail reads Parent<sep>name(params) -> Return: ':' a Lua method (also C++), '.' a "
            "static function or property (both languages), '::' C++ only (not callable from Lua; note names the Lua "
            "property to use). Verify exact C++ signatures and overloads with search_engine_source.";
        tool.inputSchema = object({
            {"query", string("Symbol, class, method or behavior, e.g. \"Sprite setTexture\", \"KEY_SPACE\", \"applyForce\".")},
            {"parent", string("Only members of this class/enum, e.g. \"Rigidbody2D\".")},
            {"max_results", withDefault(integer("Max API symbols.", 1, 50), 16)},
            {"include_docs", withDefault(boolean("Also search the reference docs."), true)},
            {"max_doc_results", withDefault(integer("Max doc lines.", 0, 40), 8)}
        }, {"query"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = searchEngineApiTool;
        registry.addTool(std::move(tool));
    }

    {
        PromptDef prompt;
        prompt.name = "new_2d_game";
        prompt.title = "New 2D game";
        prompt.description = "Plan and build a small playable 2D game in the open project with the editor tools.";
        prompt.arguments = {{"idea", "What the game is (e.g. \"a top-down asteroid shooter\").", true},
                            {"language", "Script language: lua (default) or cpp.", false}};
        prompt.get = [](const std::map<std::string, std::string>& args) {
            PromptResult result;
            const std::string idea = args.count("idea") ? args.at("idea") : "a small arcade game";
            const std::string language = args.count("language") && args.at("language") == "cpp" ? "C++" : "Lua";
            result.description = "Build " + idea + " in JasperEngine";
            std::string text =
                "Build " + idea + " as a playable 2D game in the open JasperEngine project, scripted in " + language + ".\n\n"
                "Work in small verified steps:\n"
                "1. editor_status, then read jasper://project/AGENTS.md and the parts of jasper://docs/" +
                std::string(language == "Lua" ? "lua-api" : "cpp-scripting") + " you need. Confirm API names with search_engine_api.\n"
                "2. Art: list_assets type texture; if nothing fits, make sprites with generate_placeholder_art under the assets "
                "directory and look at them with view_asset.\n"
                "3. Scene: create the GameObjects (sprites, a camera, UI text for the score) with the scene/entity tools, physics "
                "bodies with colliders where things collide.\n"
                "4. Scripts: create_script (kind " + std::string(language == "Lua" ? "lua" : "cpp_script") + ") attached to the "
                "GameObjects; expose tunables as properties and set them with set_script_properties. " +
                std::string(language == "Lua" ? "check_script after each edit (or sync_files after editing on disk)."
                                              : "compile_scripts after each edit and fix every error it lists.") + "\n"
                "5. Save the scene, play it, take Game view screenshots, drive it with the input tools and read the Console "
                "since your last cursor. Stop, fix, repeat until it plays correctly and the Console is clean.\n"
                "6. Summarize what was built, how to play, and anything left unfinished.";
            result.messages.push_back({{"role", "user"}, {"content", {{"type", "text"}, {"text", text}}}});
            return result;
        };
        registry.addPrompt(std::move(prompt));
    }

    {
        PromptDef prompt;
        prompt.name = "fix_script_errors";
        prompt.title = "Fix script errors";
        prompt.description = "Find and fix the errors of the project's scripts and shaders (compile, syntax and runtime).";
        prompt.get = [](const std::map<std::string, std::string>&) {
            PromptResult result;
            result.description = "Fix the project's script errors";
            const std::string text =
                "Find and fix the errors in this JasperEngine project's scripts and shaders:\n"
                "1. sync_files (full) to pick up every file and get Lua syntax problems.\n"
                "2. compile_scripts if the project has C++ scripts; check_shader for the shader forks.\n"
                "3. Play the start scene for a few seconds, read the Console for \"Script crash\" and Lua errors, stop.\n"
                "4. For each error read the file around the reported line, fix it minimally (confirm API names with "
                "search_engine_api), and re-run the check that reported it.\n"
                "5. Report what was wrong and what you changed.";
            result.messages.push_back({{"role", "user"}, {"content", {{"type", "text"}, {"text", text}}}});
            return result;
        };
        registry.addPrompt(std::move(prompt));
    }
}

}
