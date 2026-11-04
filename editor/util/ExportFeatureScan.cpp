// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ExportFeatureScan.h"

#include "io/watch/LinkedFolders.h"

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string lowerText(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    bool readFile(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

}

ExportFeatureScan::Feature ExportFeatureScan::http() {
    Feature feature;
    feature.name = "Http";
    // any reference to the name in Lua: Http.get, Http:..., Http["get"], local H = Http
    feature.lua = std::regex(R"(\bHttp\b)");
    feature.cpp = std::regex(R"(\bjasperengine::Http\b|\bHttp\s*::|\bHttpRequestDesc\b|"net/Http\.h")");
    return feature;
}

std::string ExportFeatureScan::luaOfHtml(const std::string& html) {
    std::string lua;
    const std::string lower = lowerText(html);
    size_t pos = 0;
    while ((pos = lower.find("<script", pos)) != std::string::npos) {
        const size_t tagEnd = lower.find('>', pos);
        if (tagEnd == std::string::npos) break;
        const std::string tag = lower.substr(pos, tagEnd - pos);
        const size_t close = lower.find("</script", tagEnd);
        const size_t bodyEnd = close == std::string::npos ? lower.size() : close;
        if (tag.find("text/lua") != std::string::npos) {
            lua += html.substr(tagEnd + 1, bodyEnd - tagEnd - 1);
            lua += "\n";
        }
        pos = bodyEnd;
    }
    return lua;
}

void ExportFeatureScan::collect(const fs::path& root, const fs::path& dir, std::vector<fs::path>& files) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    // the files of a linked folder ship too, so what they use counts
    for (watch::LinkedDirectoryIterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (!name.empty() && (name[0] == '.' || name == "build")) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) files.push_back(it->path().lexically_relative(root));
    }
}

std::vector<ExportFeatureScan::Use> ExportFeatureScan::scan(const fs::path& root, const std::vector<Feature>& features) {
    std::vector<fs::path> files;
    collect(root, root, files);
    return scanFiles(root, std::move(files), features);
}

std::vector<ExportFeatureScan::Use> ExportFeatureScan::scanFiles(const fs::path& root, std::vector<fs::path> files,
                                                                 const std::vector<Feature>& features) {
    std::vector<Use> uses(features.size());
    for (size_t i = 0; i < features.size(); i++) uses[i].feature = features[i].name;
    // the same "first file" on every machine
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    for (const fs::path& relative : files) {
        const fs::path file = root / relative;
        const std::string ext = lowerText(file.extension().string());
        const bool isLua = ext == ".lua";
        const bool isHtml = ext == ".html" || ext == ".htm" || ext == ".rml";
        const bool isCpp = ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".hxx" ||
                           ext == ".mm" || ext == ".m";
        if (!isLua && !isHtml && !isCpp) continue;
        std::string text;
        if (!readFile(file, text)) continue;
        if (isHtml) text = luaOfHtml(text);
        for (size_t i = 0; i < features.size(); i++) {
            const std::regex& pattern = isCpp ? features[i].cpp : features[i].lua;
            if (!std::regex_search(text, pattern)) continue;
            if (uses[i].files == 0) uses[i].file = relative.generic_string();
            uses[i].files++;
        }
    }

    std::vector<Use> found;
    for (Use& use : uses) {
        if (use.files > 0) found.push_back(std::move(use));
    }
    return found;
}
