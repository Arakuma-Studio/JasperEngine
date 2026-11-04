// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiExamples.h"

#include "util/FileUtils.h"

// The only translation unit that includes the generated header (CMakeLists.txt)
#include "html_ui_examples.h"

#include <algorithm>
#include <cctype>
#include <set>
#include "util/Prefab.h"
#include "io/File.h"

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    constexpr const char* kManifestPath = "examples.json";
    constexpr const char* kBlank = "blank";

    bool isAsciiAlnum(unsigned char c){
        return c < 0x80 && std::isalnum(c);
    }

    bool isNameChar(unsigned char c){
        return isAsciiAlnum(c) || c == '_';
    }

    void examplesReplaceAll(std::string& text, const std::string& from, const std::string& to){
        if (from.empty())
            return;
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos){
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // Every occurrence of `word` not preceded or followed by a character of a Lua name
    void replaceWord(std::string& text, const std::string& word, const std::string& to){
        if (word.empty() || word == to)
            return;
        size_t pos = 0;
        while ((pos = text.find(word, pos)) != std::string::npos){
            const bool before = pos > 0 && isNameChar(static_cast<unsigned char>(text[pos - 1]));
            const size_t end = pos + word.size();
            const bool after = end < text.size() && isNameChar(static_cast<unsigned char>(text[end]));
            if (before || after){
                pos = end;
                continue;
            }
            text.replace(pos, word.size(), to);
            pos += to.size();
        }
    }

    bool examplesEndsWith(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string lower(std::string text){
        for (char& c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // Project-relative paths are generic ('/'), never absolute and never leave the project
    std::string normalizedRelative(const std::string& path){
        std::string out = path;
        std::replace(out.begin(), out.end(), '\\', '/');
        while (out.size() > 1 && out.compare(0, 2, "./") == 0)
            out.erase(0, 2);
        while (!out.empty() && out.back() == '/')
            out.pop_back();
        return out;
    }

    bool leavesRoot(const std::string& path){
        if (path.empty())
            return false;
        if (path[0] == '/' || (path.size() > 1 && path[1] == ':'))
            return true;
        size_t start = 0;
        while (start <= path.size()){
            size_t end = path.find('/', start);
            if (end == std::string::npos)
                end = path.size();
            if (path.compare(start, end - start, "..") == 0 && end - start == 2)
                return true;
            start = end + 1;
        }
        return false;
    }

    std::string joinRelative(const std::string& dir, const std::string& name){
        return dir.empty() ? name : dir + "/" + name;
    }

}

const std::vector<editor::HtmlUiExamples::File>& editor::HtmlUiExamples::files(){
    static const std::vector<File> all = [](){
        std::vector<File> list;
        for (const HtmlUiEmbeddedFile& embedded : html_ui_embedded_files){
            list.push_back({embedded.path, std::string_view(reinterpret_cast<const char*>(embedded.data), embedded.size)});
        }
        std::sort(list.begin(), list.end(), [](const File& a, const File& b){ return a.path < b.path; });
        return list;
    }();
    return all;
}

const editor::HtmlUiExamples::File* editor::HtmlUiExamples::find(const std::string& path){
    const std::vector<File>& all = files();
    auto it = std::lower_bound(all.begin(), all.end(), path, [](const File& file, const std::string& key){ return file.path < key; });
    return it != all.end() && it->path == path ? &*it : nullptr;
}

const nlohmann::json& editor::HtmlUiExamples::manifest(){
    static const nlohmann::json parsed = [](){
        const File* file = find(kManifestPath);
        if (!file)
            return nlohmann::json::object();
        nlohmann::json json = nlohmann::json::parse(file->data.begin(), file->data.end(), nullptr, false);
        return json.is_discarded() || !json.is_object() ? nlohmann::json::object() : json;
    }();
    return parsed;
}

std::vector<std::string> editor::HtmlUiExamples::names(){
    std::vector<std::string> examples;
    std::vector<std::string> templates;
    const nlohmann::json& json = manifest();
    if (json.contains("examples") && json["examples"].is_array()){
        for (const nlohmann::json& entry : json["examples"]){
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
                continue;
            const bool example = entry.value("example", true);
            (example ? examples : templates).push_back(entry["name"].get<std::string>());
        }
    }
    examples.insert(examples.end(), templates.begin(), templates.end());
    return examples;
}

bool editor::HtmlUiExamples::has(const std::string& name){
    const std::vector<std::string> all = names();
    return std::find(all.begin(), all.end(), name) != all.end();
}

bool editor::HtmlUiExamples::writeTo(const fs::path& dir, std::string& error){
    std::error_code ec;
    FileUtils::createDirectories(dir, ec);
    if (ec){
        error = "cannot create " + FileUtils::pathToUtf8(dir) + ": " + ec.message();
        return false;
    }

    std::set<fs::path> kept;
    int failed = 0;
    std::string firstFailure;
    for (const File& file : files()){
        const fs::path target = dir / FileUtils::pathFromUtf8(file.path);
        kept.insert(target.lexically_normal());
        // a folder where the file has to be (or an unreadable file): writeFile's comparison throws
        bool written = false;
        try {
            written = FileUtils::writeFile(target, std::string(file.data)) != FileUtils::WriteResult::Failed;
        } catch (const std::exception&) {
            written = false;
        }
        if (!written){
            if (failed++ == 0)
                firstFailure = FileUtils::pathToUtf8(target);
        }
    }

    // Files a previous editor wrote that are no longer embedded, then the folders left empty
    std::vector<fs::path> stale;
    std::vector<fs::path> folders;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)){
        std::error_code entryEc;
        if (it->is_directory(entryEc))
            folders.push_back(it->path());
        else if (kept.count(it->path().lexically_normal()) == 0)
            stale.push_back(it->path());
    }
    for (const fs::path& path : stale){
        std::error_code removeEc;
        FileUtils::removePath(path, removeEc);
    }
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b){ return a.native().size() > b.native().size(); });
    for (const fs::path& folder : folders){
        std::error_code emptyEc;
        if (fs::is_empty(folder, emptyEc) && !emptyEc)
            FileUtils::removePath(folder, emptyEc);
    }

    if (failed > 0){
        error = "could not write " + std::to_string(failed) + " example file(s), the first " + firstFailure;
        return false;
    }
    return true;
}

std::string editor::HtmlUiExamples::pascalName(const std::string& stem){
    std::string out;
    bool wordStart = true;
    for (unsigned char c : stem){
        if (!isAsciiAlnum(c)){
            wordStart = true;
            continue;
        }
        out += wordStart ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
        wordStart = false;
    }
    if (out.empty())
        return "Document";
    if (std::isdigit(static_cast<unsigned char>(out[0])))
        out = "Ui" + out;
    return out;
}

std::string editor::HtmlUiExamples::titleName(const std::string& stem){
    std::string out;
    bool wordStart = true;
    for (unsigned char c : stem){
        if (c == '_' || c == '-' || c == ' '){
            if (!out.empty() && out.back() != ' ')
                out += ' ';
            wordStart = true;
            continue;
        }
        out += wordStart && c < 0x80 ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
        wordStart = false;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

bool editor::HtmlUiExamples::instantiate(const std::string& name, const std::string& htmlRel, const std::string& luaDirRel,
                                         Instance& out, std::string& error){
    out = Instance();
    if (!has(name)){
        std::string list;
        for (const std::string& known : names())
            list += (list.empty() ? "" : ", ") + known;
        error = "unknown template '" + name + "' (one of: " + list + ")";
        return false;
    }

    const std::string html = normalizedRelative(htmlRel);
    const std::string luaDir = normalizedRelative(luaDirRel);
    const std::string extension = lower(FileUtils::pathToUtf8(FileUtils::pathFromUtf8(html).extension()));
    if (extension != ".html" && extension != ".htm"){
        error = "the document must be an .html or .htm file: '" + htmlRel + "'";
        return false;
    }
    if (leavesRoot(html) || leavesRoot(luaDir)){
        error = "paths must be project-relative and stay inside the project: '" + (leavesRoot(html) ? htmlRel : luaDirRel) + "'";
        return false;
    }
    const size_t slash = html.rfind('/');
    const std::string dir = slash == std::string::npos ? "" : html.substr(0, slash);
    const std::string fileName = slash == std::string::npos ? html : html.substr(slash + 1);
    const std::string stem = fileName.substr(0, fileName.size() - extension.size());
    if (stem.empty() || stem.find_first_not_of(". ") == std::string::npos){
        error = "the document needs a file name: '" + htmlRel + "'";
        return false;
    }

    const nlohmann::json* entry = nullptr;
    const nlohmann::json& json = manifest();
    if (json.contains("examples") && json["examples"].is_array()){
        for (const nlohmann::json& candidate : json["examples"]){
            if (candidate.is_object() && candidate.value("name", "") == name)
                entry = &candidate;
        }
    }
    const std::string htmlName = entry ? entry->value("html", name + ".html") : name + ".html";
    const std::string cssName = entry ? entry->value("css", name + ".css") : name + ".css";
    const std::string luaName = entry ? entry->value("lua", "") : "";

    const File* htmlFile = find(name + "/" + htmlName);
    const File* cssFile = find(name + "/" + cssName);
    if (!htmlFile || !cssFile){
        error = "the template '" + name + "' is incomplete in this editor (no " + htmlName + " or " + cssName + ")";
        return false;
    }

    const std::string assetsDir = stem + "_assets/";

    std::string htmlText(htmlFile->data);
    examplesReplaceAll(htmlText, "href=\"" + cssName + "\"", "href=\"" + stem + ".css\"");
    examplesReplaceAll(htmlText, "\"assets/", "\"" + assetsDir);
    examplesReplaceAll(htmlText, "'assets/", "'" + assetsDir);
    if (name == kBlank){
        const std::string title = titleName(stem);
        examplesReplaceAll(htmlText, "<title>Blank</title>", "<title>" + title + "</title>");
        examplesReplaceAll(htmlText, ">Blank</h1>", ">" + title + "</h1>");
    }
    out.files.emplace_back(html, htmlText);

    std::string cssText(cssFile->data);
    examplesReplaceAll(cssText, "url(assets/", "url(" + assetsDir);
    examplesReplaceAll(cssText, "url(\"assets/", "url(\"" + assetsDir);
    examplesReplaceAll(cssText, "url('assets/", "url('" + assetsDir);
    out.files.emplace_back(joinRelative(dir, stem + ".css"), cssText);

    const std::string assetsPrefix = name + "/assets/";
    for (const File& file : files()){
        if (file.path.compare(0, assetsPrefix.size(), assetsPrefix) == 0)
            out.files.emplace_back(joinRelative(dir, assetsDir + file.path.substr(assetsPrefix.size())), std::string(file.data));
    }

    if (!luaName.empty()){
        const File* luaFile = find(name + "/" + luaName);
        if (!luaFile){
            error = "the template '" + name + "' names " + luaName + ", which this editor does not have";
            out = Instance();
            return false;
        }
        const std::string templateClass = luaName.size() > 4 && examplesEndsWith(luaName, ".lua") ? luaName.substr(0, luaName.size() - 4) : luaName;
        out.luaClass = pascalName(stem) + "UI";
        out.luaPath = joinRelative(luaDir, out.luaClass + ".lua");
        std::string luaText(luaFile->data);
        replaceWord(luaText, templateClass, out.luaClass);
        examplesReplaceAll(luaText, "\"assets/", "\"" + assetsDir);
        examplesReplaceAll(luaText, "'assets/", "'" + assetsDir);
        out.files.emplace_back(out.luaPath, luaText);
    }
    return true;
}
