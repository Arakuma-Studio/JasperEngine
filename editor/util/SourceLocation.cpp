// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The path-start scan and the ":L:C" / "(L,C)" readers follow OpenEngine's SourceLocation
// (Apps/Editor/Source/ScriptEditor/SourceLocation.cpp): Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT.

#include "SourceLocation.h"

#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace fs = std::filesystem;

namespace jasperengine::editor {

namespace {

    bool isPathChar(char c){
        const unsigned char u = static_cast<unsigned char>(c);
        return std::isalnum(u) || u >= 0x80 || std::strchr("_-./\\+~@%", c) != nullptr;
    }

    bool isDigit(char c){
        return c >= '0' && c <= '9';
    }

    // Digits at `at` (at most 9): the number and where they end; false when there are none
    bool readNumber(std::string_view text, size_t at, int& value, size_t& end){
        value = 0;
        end = at;
        while (end < text.size() && isDigit(text[end]) && end - at < 9) {
            value = value * 10 + (text[end] - '0');
            end++;
        }
        return end > at && (end >= text.size() || !isDigit(text[end]));
    }

    std::string lowerExtension(std::string_view path){
        const size_t slash = path.find_last_of("/\\");
        const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot == std::string_view::npos || dot + 1 >= name.size()) return {};
        std::string extension(name.substr(dot + 1));
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension;
    }

    // A file name with an extension that starts with a letter: "player.lua", "CMakeLists.txt"
    bool looksLikeFile(std::string_view path){
        if (path.empty() || path.back() == '/' || path.back() == '\\' || path.back() == '.') return false;
        const std::string extension = lowerExtension(path);
        return !extension.empty() && std::isalpha(static_cast<unsigned char>(extension[0])) && extension.size() <= 12;
    }

    std::string kindOf(std::string_view path){
        const std::string extension = lowerExtension(path);
        const size_t slash = path.find_last_of("/\\");
        const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        if (extension == "lua") return "lua";
        if (name == "CMakeLists.txt" || extension == "cmake") return "cmake";
        static const char* const cpp[] = {"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp", "m", "mm"};
        for (const char* e : cpp) {
            if (extension == e) return "cpp";
        }
        static const char* const shader[] = {"vert", "frag", "glsl", "comp", "geom", "hlsl", "metal", "wgsl", "vs", "fs"};
        for (const char* e : shader) {
            if (extension == e) return "shader";
        }
        static const char* const html[] = {"html", "htm", "css", "rml", "rcss"};
        for (const char* e : html) {
            if (extension == e) return "html";
        }
        static const char* const asset[] = {"scene", "prefab", "material", "yaml", "yml", "json", "meta", "anim", "tilemap"};
        for (const char* e : asset) {
            if (extension == e) return "asset";
        }
        return "file";
    }

    // Where the path that ends at `end` (exclusive) begins; `end` itself when there is none
    size_t pathStart(std::string_view text, size_t end){
        size_t start = end;
        while (start > 0 && isPathChar(text[start - 1])) start--;
        // a Windows drive: C:\... or C:/...
        if (start >= 2 && start < end && text[start - 1] == ':' && std::isalpha(static_cast<unsigned char>(text[start - 2])) &&
            (text[start] == '\\' || text[start] == '/') && (start == 2 || !isPathChar(text[start - 3]))) {
            start -= 2;
        }
        // an absolute path with spaces starting its line ("C:\Users\Jo Doe\x.cpp(3)", "/home/jo doe/x.lua:3:")
        if (start > 0 && text[start - 1] == ' ') {
            size_t lineStart = text.rfind('\n', start - 1);
            lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
            while (lineStart < start && (text[lineStart] == ' ' || text[lineStart] == '\t')) lineStart++;
            const std::string_view candidate = text.substr(lineStart, end - lineStart);
            const bool absolute = !candidate.empty() && (candidate[0] == '/' ||
                (candidate.size() > 2 && std::isalpha(static_cast<unsigned char>(candidate[0])) && candidate[1] == ':' &&
                 (candidate[2] == '\\' || candidate[2] == '/')));
            bool plain = absolute;
            for (size_t i = 0; plain && i < candidate.size(); i++) {
                const char c = candidate[i];
                plain = isPathChar(c) || c == ' ' || (c == ':' && i == 1);
            }
            if (plain) start = lineStart;
        }
        return start;
    }

    void add(std::vector<SourceLocation>& out, std::string path, int line, int column, size_t begin, size_t end){
        if (line <= 0 || !looksLikeFile(path)) return;
        for (const SourceLocation& existing : out) {
            if (existing.path == path && existing.line == line && existing.column == column) return;
        }
        SourceLocation location;
        location.kind = kindOf(path);
        location.path = std::move(path);
        location.line = line;
        location.column = column;
        location.begin = begin;
        location.end = end;
        out.push_back(std::move(location));
    }

    bool startsWith(std::string_view text, std::string_view prefix){
        return text.substr(0, prefix.size()) == prefix;
    }

}

std::vector<SourceLocation> findSourceLocations(std::string_view text){
    std::vector<SourceLocation> out;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];

        // [string "scripts/p.lua"]:12:
        if (c == '[' && startsWith(text.substr(i), "[string \"")) {
            const size_t pathBegin = i + 9;
            const size_t quote = text.find("\"]", pathBegin);
            if (quote != std::string_view::npos && quote + 2 < text.size() && text[quote + 2] == ':') {
                int line = 0;
                size_t lineEnd = 0;
                if (readNumber(text, quote + 3, line, lineEnd)) {
                    add(out, std::string(text.substr(pathBegin, quote - pathBegin)), line, 0, i, lineEnd);
                    i = lineEnd;
                    continue;
                }
            }
            i++;
            continue;
        }

        // path:line[:column]
        if (c == ':' && i + 1 < text.size() && isDigit(text[i + 1])) {
            int line = 0;
            size_t lineEnd = 0;
            if (readNumber(text, i + 1, line, lineEnd)) {
                int column = 0;
                size_t end = lineEnd;
                size_t columnEnd = 0;
                if (lineEnd + 1 < text.size() && text[lineEnd] == ':' && isDigit(text[lineEnd + 1]) &&
                    readNumber(text, lineEnd + 1, column, columnEnd)) {
                    end = columnEnd;
                } else {
                    column = 0;
                }
                size_t start = pathStart(text, i);
                std::string path(text.substr(start, i - start));
                // a URL-like prefix: lua://p.lua (the engine's virtual paths), file://...
                if (startsWith(path, "//") && start >= 2 && text[start - 1] == ':') {
                    size_t scheme = start - 1;
                    while (scheme > 0 && std::isalpha(static_cast<unsigned char>(text[scheme - 1]))) scheme--;
                    const std::string_view name = text.substr(scheme, start - 1 - scheme);
                    if (name == "http" || name == "https") {
                        i = end;
                        continue;
                    }
                    path = path.substr(2);
                    if (name == "file" && path.size() > 1 && path[0] == '/' && path.size() > 2 && path[2] == ':') path = path.substr(1);
                    start = scheme;
                }
                add(out, std::move(path), line, column, start, end);
                i = end;
                continue;
            }
        }

        // path(line[,column]): (MSVC)
        if (c == '(' && i + 1 < text.size() && isDigit(text[i + 1])) {
            int line = 0;
            size_t lineEnd = 0;
            if (readNumber(text, i + 1, line, lineEnd)) {
                int column = 0;
                size_t close = lineEnd;
                size_t columnEnd = 0;
                if (close < text.size() && text[close] == ',' && readNumber(text, close + 1, column, columnEnd)) close = columnEnd;
                if (close < text.size() && text[close] == ')') {
                    size_t after = close + 1;
                    while (after < text.size() && text[after] == ' ') after++;
                    if (after < text.size() && text[after] == ':') {
                        const size_t start = pathStart(text, i);
                        add(out, std::string(text.substr(start, i - start)), line, column, start, close + 1);
                        i = close + 1;
                        continue;
                    }
                }
            }
        }
        i++;
    }
    return out;
}

std::string projectFileFor(const fs::path& projectPath, const std::string& file){
    if (file.empty() || projectPath.empty()) return {};
    const fs::path path = FileUtils::pathFromUtf8(file).lexically_normal();
    std::vector<fs::path> parts(path.begin(), path.end());
    std::error_code ec;
    for (size_t start = 0; start < parts.size(); start++) {
        fs::path suffix;
        for (size_t i = start; i < parts.size(); i++) suffix /= parts[i];
        if (suffix.is_absolute() || suffix.has_root_name() || suffix.has_root_directory()) continue;
        // the build cache (.jasperengine/export/...) holds copies: the source is elsewhere
        if (*suffix.begin() == ".jasperengine") continue;
        if (parts.size() - start >= 1 && fs::is_regular_file(projectPath / suffix, ec)) {
            if (parts.size() - start == 1 && start != parts.size() - 1) continue;
            return FileUtils::pathToGenericUtf8(suffix);
        }
    }
    return {};
}

ResolvedSource resolveSourceLocation(const std::string& pathText, const SourceRoots& roots){
    ResolvedSource out;
    if (pathText.empty()) return out;
    const fs::path path = FileUtils::pathFromUtf8(pathText);
    std::error_code ec;
    auto finish = [&](const fs::path& absolute) {
        out.absolute = absolute.lexically_normal();
        out.exists = fs::is_regular_file(out.absolute, ec);
        if (!roots.project.empty()) {
            const fs::path relative = out.absolute.lexically_relative(roots.project.lexically_normal());
            if (!relative.empty() && *relative.begin() != ".." && !relative.is_absolute()) {
                out.inProject = true;
                out.projectPath = relative.generic_string();
            }
        }
        return out;
    };

    if (path.is_absolute()) {
        if (fs::is_regular_file(path, ec)) return finish(path);
        // a build tree elsewhere (another machine, the export cache): its project file
        const std::string projectFile = projectFileFor(roots.project, pathText);
        if (!projectFile.empty()) return finish(roots.project / FileUtils::pathFromUtf8(projectFile));
        return finish(path);
    }
    for (const fs::path* root : {&roots.project, &roots.lua, &roots.assets}) {
        if (!root->empty() && fs::is_regular_file(*root / path, ec)) return finish(*root / path);
    }
    const std::string projectFile = projectFileFor(roots.project, pathText);
    if (!projectFile.empty()) return finish(roots.project / FileUtils::pathFromUtf8(projectFile));
    if (!roots.engine.empty()) {
        for (const fs::path& base : {roots.engine, roots.engine.parent_path()}) {
            if (fs::is_regular_file(base / path, ec)) {
                ResolvedSource engine = finish(base / path);
                engine.inProject = false;
                engine.projectPath.clear();
                return engine;
            }
        }
    }
    // not found: where it would be in the Lua directory (Lua) or the project
    const fs::path fallback = (lowerExtension(pathText) == "lua" && !roots.lua.empty() ? roots.lua : roots.project) / path;
    ResolvedSource missing = finish(fallback);
    missing.exists = false;
    return missing;
}

std::string formatSourceLocation(const std::string& path, int line, int column){
    std::string text = path;
    if (line > 0) text += ":" + std::to_string(line);
    if (line > 0 && column > 0) text += ":" + std::to_string(column);
    return text;
}

}
