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
#include "mcp/McpToolGroups.h"
#include "mcp/EngineApiContext.h"

#include "Generator.h"
#include "Project.h"
#include "util/FileUtils.h"

#include "mcp_docs.h"
#include "util/LuaApiData.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;

namespace {

// A part of a big document, served as its own resource jasper://docs/<doc>/<slug>
struct DocSection {
    std::string slug;
    std::string title;   // the heading without '#'
    std::string text;
};

struct Doc {
    std::string name;
    std::string text;
    std::string title;   // first "# " heading
    std::string intro;   // text before the first "## " (big documents)
    std::vector<DocSection> sections;   // empty: the document is served whole
};

// Documents above this size are served as an index + sections, so an agent reads only what
// it needs; "## " sections above SECTION_SPLIT are split again at "### "
constexpr size_t DOC_SPLIT = 16 * 1024;
constexpr size_t SECTION_SPLIT = 6 * 1024;

std::string slugOf(const std::string& heading) {
    std::string slug;
    for (char c : heading) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) slug.push_back(static_cast<char>(std::tolower(u)));
        else if (!slug.empty() && slug.back() != '-') slug.push_back('-');
        if (slug.size() >= 40) break;
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug.empty() ? "section" : slug;
}

std::string headingText(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == '#') i++;
    while (i < line.size() && line[i] == ' ') i++;
    return line.substr(i);
}

// Splits text at lines starting with `marker` ("## " / "### "): {heading, chunk} pairs, the
// chunk before the first heading has an empty heading
std::vector<std::pair<std::string, std::string>> splitAt(const std::string& text, const std::string& marker) {
    std::vector<std::pair<std::string, std::string>> out{{"", ""}};
    std::istringstream in(text);
    std::string line;
    bool fence = false;
    while (std::getline(in, line)) {
        if (line.rfind("```", 0) == 0) fence = !fence;
        if (!fence && line.rfind(marker, 0) == 0) out.push_back({headingText(line), ""});
        out.back().second += line + "\n";
    }
    return out;
}

void splitDoc(Doc& doc) {
    if (doc.text.size() <= DOC_SPLIT) return;
    std::set<std::string> used;
    auto add = [&](const std::string& title, const std::string& text) {
        std::string slug = slugOf(title);
        for (int n = 2; used.count(slug); n++) slug = slugOf(title) + "-" + std::to_string(n);
        used.insert(slug);
        doc.sections.push_back({slug, title, text});
    };
    const auto parts = splitAt(doc.text, "## ");
    doc.intro = parts[0].second;
    for (size_t i = 1; i < parts.size(); i++) {
        const auto& [title, text] = parts[i];
        if (text.size() <= SECTION_SPLIT) {
            add(title, text);
            continue;
        }
        const auto sub = splitAt(text, "### ");
        // the section's own lead-in goes with its first subsection
        for (size_t j = 1; j < sub.size(); j++) add(sub[j].first, (j == 1 ? sub[0].second : std::string()) + sub[j].second);
        if (sub.size() == 1) add(title, text);
    }
}

// What jasper://docs/<name> serves: the whole text, or for a split document its intro and the
// section index
std::string docMain(const Doc& doc) {
    if (doc.sections.empty()) return doc.text;
    std::string text = doc.intro;
    text += "\n## Sections\n\nThis reference is split: read only the sections you need (resources/read).\n\n";
    for (const DocSection& section : doc.sections) {
        text += "- `jasper://docs/" + doc.name + "/" + section.slug + "`: " + section.title + " (" +
                std::to_string((section.text.size() + 1023) / 1024) + " KB)\n";
    }
    return text;
}

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
            splitDoc(doc);
            out.push_back(std::move(doc));
        }
        // the Lua API reference, generated from lua-api.json (S23 R46)
        {
            Doc doc;
            doc.name = "lua-api-reference";
            doc.text = LuaApiData::markdown();
            doc.title = "Lua API reference";
            splitDoc(doc);
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
    for (const Doc& doc : docs()) {
        text += "- `jasper://docs/" + doc.name + "`: " + doc.title +
                (doc.sections.empty() ? "" : " (an index of " + std::to_string(doc.sections.size()) + " sections)") + "\n";
    }
    text += "- `jasper://examples/html-ui`: HTML/CSS UI examples (menus, HUD, inventory, settings...) to copy\n";
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
    struct Piece {
        std::string uri;
        const std::string* text;
    };
    std::vector<Piece> pieces;
    for (const Doc& doc : docs()) {
        if (doc.sections.empty()) {
            pieces.push_back({"jasper://docs/" + doc.name, &doc.text});
            continue;
        }
        pieces.push_back({"jasper://docs/" + doc.name, &doc.intro});
        for (const DocSection& section : doc.sections) pieces.push_back({"jasper://docs/" + doc.name + "/" + section.slug, &section.text});
    }
    for (const Piece& piece : pieces) {
        std::istringstream in(*piece.text);
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
            scored.push_back({score, Json{{"doc", piece.uri}, {"section", heading}, {"line", number}, {"text", text}}});
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

// The words of a search query, lower-case, camelCase and snake_case split ("drawCalls" -> draw,
// calls), without the verbs and nouns every tool description shares
std::vector<std::string> queryWords(const std::string& query) {
    static const std::set<std::string> skip = {"get", "set", "list", "add", "the", "and", "for", "with", "per", "how",
                                               "gameobject", "gameobjects", "scene", "scenes", "component",
                                               "components", "entity", "entities", "object", "value", "values",
                                               "tool", "tools", "editor", "engine", "api", "lua", "script"};
    std::vector<std::string> words;
    std::string word;
    auto flush = [&]() {
        if (word.size() >= 3 && !skip.count(word)) words.push_back(word);
        word.clear();
    };
    for (size_t i = 0; i < query.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(query[i]);
        if (!std::isalnum(c)) {
            flush();
            continue;
        }
        if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(query[i - 1]))) flush();
        word += static_cast<char>(std::tolower(c));
    }
    flush();
    return words;
}

// "profiler" also matches "profile", "calls" "call"
bool hasWord(const std::string& haystack, const std::string& word) {
    if (haystack.find(word) != std::string::npos) return true;
    for (const char* suffix : {"s", "er", "ing", "ed"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (word.size() > n + 3 && word.compare(word.size() - n, n, suffix) == 0 &&
            haystack.find(word.substr(0, word.size() - n)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Editor tools this session does not list (a tool group it has not enabled) that match the query:
// agents look for frame times or the Game view size in the API search, and a client can only call
// the tools it was listed (Claude Code's tool search never sees the others)
Json unlistedToolHits(const CallPtr& call, const std::string& query) {
    Json hits = Json::array();
    const std::vector<std::string> words = queryWords(query);
    if (words.size() < 1) return hits;
    const std::set<std::string> listed = call->server().sessionToolGroups(call->session());
    struct Hit {
        size_t score;
        const ToolDef* def;
    };
    std::vector<Hit> scored;
    for (const ToolDef& def : call->server().registry().tools()) {
        const std::string& group = McpRegistry::groupOf(def);
        if (group == CORE_TOOL_GROUP || listed.count(group)) continue;
        std::string name = lower(def.name);
        std::replace(name.begin(), name.end(), '_', ' ');
        const std::string haystack = name + " " + lower(def.description);
        size_t matched = 0, inName = 0;
        for (const std::string& word : words) {
            if (!hasWord(haystack, word)) continue;
            matched++;
            if (hasWord(name, word)) inName++;
        }
        // most of the query, and at least two words of it (one when the query is one word)
        if (matched < std::min<size_t>(2, words.size()) || matched * 2 < words.size()) continue;
        scored.push_back({matched * 2 + inName, &def});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    for (const Hit& hit : scored) {
        if (hits.size() >= 3) break;
        const std::string& group = McpRegistry::groupOf(*hit.def);
        hits.push_back({{"tool", hit.def->name}, {"group", group},
                        {"enable", "tool_groups {\"enable\": [\"" + group + "\"]}"}});
    }
    return hits;
}

void searchEngineApiTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string query = args.value("query", "");
    if (query.empty()) {
        call->fail("query is required, e.g. \"Rigidbody2D applyForce\" or \"Input isKeyPressed\".");
        return;
    }
    const int maxResults = std::clamp(args.value("max_results", 16), 1, 50);
    Json api = EngineApiContext::search(query, args.value("parent", ""), maxResults);
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
    Json data = {{"results", results}, {"docs", docHits}};
    addUnlistedTools(call, query, text, data);
    call->ok(text, data);
}

// The engine's own source tree (headers and Lua bindings), as shipped with the editor
bool isEngineSourceFile(const fs::path& path) {
    static const std::set<std::string> allowed = {".h", ".hpp", ".inl", ".cpp", ".cc", ".c", ".lua"};
    return allowed.count(lower(path.extension().string())) > 0;
}

bool engineRoot(const CallPtr& call, fs::path& root) {
    root = FileUtils::getEngineDir();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        call->fail("Engine source not found at " + FileUtils::pathToGenericUtf8(root) + ".");
        return false;
    }
    return true;
}

void searchEngineSourceTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string query = lower(args.value("query", ""));
    if (query.empty()) {
        call->fail("query is required, e.g. \"Quaternion(\", \"D_KEY_\", \"applyForce\".");
        return;
    }
    const std::string pathFilter = lower(args.value("path_filter", ""));
    const int maxResults = std::clamp(args.value("max_results", 40), 1, 200);
    fs::path root;
    if (!engineRoot(call, root)) return;
    // core/ is the scriptable API surface; the third-party libs/ tree would drown the results
    fs::path searchRoot = root / "core";
    std::error_code ec;
    if (!fs::is_directory(searchRoot, ec)) searchRoot = root;

    // Sorted for stable results across platforms
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(searchRoot, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) {
            const std::string dir = it->path().filename().string();
            if (dir == "libs" || dir == "build" || dir == ".git") it.disable_recursion_pending();
            continue;
        }
        if (isEngineSourceFile(it->path())) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    Json matches = Json::array();
    bool truncated = false;
    std::string text;
    for (const fs::path& path : files) {
        const std::string rel = FileUtils::pathToGenericUtf8(fs::relative(path, root, ec));
        if (ec || (!pathFilter.empty() && lower(rel).find(pathFilter) == std::string::npos)) continue;
        std::string content;
        if (!ProjectChecks::readFile(path, content) || content.size() > 1024 * 1024) continue;
        std::istringstream in(content);
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            number++;
            if (lower(line).find(query) == std::string::npos) continue;
            if (static_cast<int>(matches.size()) >= maxResults) {
                truncated = true;
                break;
            }
            const size_t start = line.find_first_not_of(" \t");
            std::string trimmed = start == std::string::npos ? "" : line.substr(start);
            if (trimmed.size() > 240) trimmed = trimmed.substr(0, 240) + " ...";
            matches.push_back({{"path", rel}, {"line", number}, {"text", trimmed}});
            text += "\n" + rel + ":" + std::to_string(number) + ": " + trimmed;
        }
        if (truncated) break;
    }
    const std::string header = std::to_string(matches.size()) + " match(es)" + (truncated ? " (truncated)" : "") + ".";
    Json data = {{"matches", matches}, {"count", matches.size()}, {"truncated", truncated}};
    text = header + text;
    addUnlistedTools(call, args.value("query", ""), text, data);
    call->ok(text, data);
}

void readEngineSourceTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string relText = args.value("path", "");
    const fs::path rel = FileUtils::pathFromUtf8(relText).lexically_normal();
    if (relText.empty() || rel.is_absolute() || rel.empty() || *rel.begin() == "..") {
        call->fail("path must be relative to the engine source root, e.g. core/math/Quaternion.h.");
        return;
    }
    if (!isEngineSourceFile(rel)) {
        call->fail("Only engine source files (.h, .hpp, .inl, .cpp, .cc, .c, .lua) can be read.");
        return;
    }
    fs::path root;
    if (!engineRoot(call, root)) return;
    std::string content;
    if (!ProjectChecks::readFile(root / rel, content)) {
        call->fail("No engine source file " + FileUtils::pathToGenericUtf8(rel) + "; find files with search_engine_source.");
        return;
    }
    const int start = std::max(1, args.value("start_line", 1));
    const int maxLines = std::clamp(args.value("max_lines", 2000), 1, 20000);
    std::istringstream in(content);
    std::string line, out;
    int number = 0, emitted = 0;
    while (std::getline(in, line)) {
        number++;
        if (number < start || emitted >= maxLines) continue;
        out += line + "\n";
        emitted++;
    }
    if (out.size() > 256 * 1024) out = out.substr(0, 256 * 1024) + "\n... (truncated: read a smaller line range)";
    const std::string header = FileUtils::pathToGenericUtf8(rel) + " lines " + std::to_string(start) + "-" +
                               std::to_string(start + emitted - 1) + " of " + std::to_string(number) + ":\n";
    call->reply(ToolResult::success(header + out, Json{{"path", FileUtils::pathToGenericUtf8(rel)}, {"total_lines", number},
                                                       {"start_line", start}, {"lines", emitted}}));
}

} // namespace

// McpTools.h
void addUnlistedTools(const CallPtr& call, const std::string& query, std::string& text, Json& data) {
    const Json hits = unlistedToolHits(call, query);
    if (hits.empty()) return;
    std::string names, groups;
    std::set<std::string> seen;
    for (const Json& hit : hits) {
        names += (names.empty() ? "" : ", ") + hit["tool"].get<std::string>();
        if (seen.insert(hit["group"].get<std::string>()).second) {
            groups += (groups.empty() ? "\"" : ", \"") + hit["group"].get<std::string>() + "\"";
        }
    }
    const std::string next = "Editor tools for this, not in your tool list yet: " + names + ". Call tool_groups {\"enable\": [" +
                             groups + "]}, then use them.";
    text += "\n" + next;
    data["editor_tools"] = hits;
    data["next"] = next;
}

void registerDocsTools(McpRegistry& registry) {
    using namespace schema;

    // jasper://guide: the instructions and where to read more
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
        def.description = "Reference: " + doc.title + (doc.sections.empty() ? "." : " (index of its sections).");
        def.mimeType = "text/markdown";
        const std::string name = doc.name;
        def.read = [name](const std::string& uri) {
            for (const Doc& d : docs()) {
                if (d.name == name) return textResource(uri, "text/markdown", docMain(d));
            }
            return notFound("No document " + name + ".");
        };
        registry.addResource(std::move(def));
        for (const DocSection& section : doc.sections) {
            ResourceDef part;
            part.uri = "jasper://docs/" + doc.name + "/" + section.slug;
            part.name = doc.name + "/" + section.slug;
            part.title = doc.title + ": " + section.title;
            part.description = "Section of jasper://docs/" + doc.name + ".";
            part.mimeType = "text/markdown";
            const std::string slug = section.slug;
            part.read = [name, slug](const std::string& uri) {
                for (const Doc& d : docs()) {
                    if (d.name != name) continue;
                    for (const DocSection& sec : d.sections) {
                        if (sec.slug == slug) return textResource(uri, "text/markdown", sec.text);
                    }
                }
                return notFound("No section " + slug + " in " + name + ".");
            };
            registry.addResource(std::move(part));
        }
    }
    {
        // the Lua API as data (lua-api.json, S23 R46)
        ResourceDef api;
        api.uri = "jasper://lua-api";
        api.name = "lua-api";
        api.title = "Lua API (JSON)";
        api.description = "The engine's Lua API from its one parser: classes (constructors, methods with overloads, properties, "
                          "C++-only methods), enums, globals, engine events. jasper://lua-api?class=<Name>: one class.";
        api.mimeType = "application/json";
        api.read = [](const std::string& uri) { return textResource(uri, "application/json", LuaApiData::jsonText()); };
        registry.addResource(std::move(api));

        ResourceTemplateDef tmpl;
        tmpl.uriTemplate = "jasper://lua-api?class={class}";
        tmpl.name = "lua-api-class";
        tmpl.title = "Lua API class";
        tmpl.description = "One class of jasper://lua-api by its Lua name.";
        tmpl.mimeType = "application/json";
        tmpl.read = [](const std::string& uri, const std::map<std::string, std::string>& vars) {
            const std::string name = vars.count("class") ? vars.at("class") : "";
            const Json cls = LuaApiData::findClass(name);
            if (cls.is_null()) return notFound("No Lua class \"" + name + "\" (jasper://lua-api lists them).");
            return textResource(uri, "application/json", cls.dump(1));
        };
        registry.addResourceTemplate(std::move(tmpl));
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
                             "project-specific instructions its authors added (written on first read if missing).";
        agents.mimeType = "text/markdown";
        agents.read = [](const std::string& uri) {
            Project* project = McpServer::get().context().project();
            std::string text;
            if (!project) return notFound("No project is open.");
            const fs::path file = project->getProjectPath() / "AGENTS.md";
            // the editor writes it on play/build; a new project gets it on first read. Always
            // asked for, as Play does: it writes only what is missing (AGENTS.md, then CLAUDE.md),
            // so a read leaves both even when a generation from a Play wrote AGENTS.md alone
            // after its CLAUDE.md check
            Generator::writeAgentsFile(project->getProjectPath(), project->getProjectInternalPath());
            if (!ProjectChecks::readFile(file, text)) return notFound("Can't read the project's AGENTS.md.");
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
            if (!ProjectChecks::readFile(file, text)) return notFound("Can't read " + rel + ".");
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
            "Search the scripting API (Lua bindings, C++ headers) and the jasper://docs reference: confirm a class, "
            "method, property, enum or macro exists before using it. detail: Parent<sep>name(params) -> Return; sep "
            "':' Lua method (also C++), '.' static/property (both), '::' C++ only. Exact C++ signatures: "
            "search_engine_source.";
        tool.inputSchema = object({
            {"query", string("e.g. \"Sprite setTexture\", \"KEY_SPACE\".")},
            {"parent", string("Only members of this class/enum.")},
            {"max_results", withDefault(integer("", 1, 50), 16)},
            {"include_docs", withDefault(boolean(""), true)},
            {"max_doc_results", withDefault(integer("", 0, 40), 8)}
        }, {"query"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = searchEngineApiTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "search_engine_source";
        tool.title = "Search Engine Source";
        tool.description =
            "Case-insensitive substring search of the engine's C++ headers/sources and Lua bindings (engine/core): "
            "ground truth for signatures, overloads, enum values, macros, key codes (D_KEY_* in C++, Input.KEY_* in "
            "Lua); matches [{path, line, text}]; context: read_engine_source.";
        tool.inputSchema = object({
            {"query", string("")},
            {"path_filter", string("Substring of the file path, e.g. \"math/\", \"binding\".")},
            {"max_results", withDefault(integer("", 1, 200), 40)}
        }, {"query"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = searchEngineSourceTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "read_engine_source";
        tool.title = "Read Engine Source";
        tool.description =
            "Read an engine source file (path relative to the engine root, e.g. core/math/Quaternion.h, "
            "core/script/binding/CoreClassesLua.cpp).";
        tool.inputSchema = object({
            {"path", string("")},
            {"start_line", withDefault(integer("1-based.", 1, 100000000), 1)},
            {"max_lines", withDefault(integer("", 1, 20000), 2000)}
        }, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = readEngineSourceTool;
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
                "3. Scene: place every GameObject in the editor (sprites, a camera, physics bodies with colliders where things "
                "collide); make what repeats a Prefab (create_prefab, instantiate_prefab); put the score in a UI scene attached "
                "with set_child_scene; author motion with edit_animation. Read jasper://docs/authoring.\n"
                "4. Scripts: create_script (kind " + std::string(language == "Lua" ? "lua" : "cpp_script") + ") attached to the "
                "GameObjects; expose tunables as properties and set them with set_script_properties. " +
                std::string(language == "Lua" ? "Edit the files with your own tools; fix the problems the next call reports "
                                                                "(external_changes), or check_script."
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
