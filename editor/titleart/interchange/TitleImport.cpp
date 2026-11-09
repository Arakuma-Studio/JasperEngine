// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// titleSafeFileName is ported from Pachislop src/io/project.ts (safeFileName) by SafEight, used
// with the author's permission.

#include "TitleImport.h"

#include "util/FileUtils.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool readAll(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        out = buffer.str();
        return true;
    }

    std::string generic(const fs::path& p) {
        return FileUtils::pathToUtf8(fs::path(p.lexically_normal().generic_string()));
    }

}

TitleJson TitleImportNote::toJson() const {
    TitleJson j = TitleJson::object();
    j["layer"] = layer;
    j["field"] = field;
    j["what"] = what;
    j["action"] = action;
    if (!code.empty()) j["code"] = code;
    return j;
}

std::string TitleImportNote::toLine() const {
    std::string line;
    if (!layer.empty()) line += layer + ": ";
    if (!field.empty()) line += field + ": ";
    line += what;
    if (!action.empty() && action != "note") line += " (" + action + ")";
    return line;
}

std::string editor::titleImportKindLabel(const std::string& kind) {
    if (kind == "pachislop") return "Pachislop project";
    if (kind == "psd") return "PSD";
    if (kind == "svg") return "SVG";
    if (kind == "ps_styles") return "Photoshop style library";
    if (kind == "strings") return "strings file";
    return kind;
}

std::string editor::titleImportUndoName(const std::string& kind, const std::string& sourceFileName) {
    const std::string label = kind == "pachislop" ? "Pachislop Project" : kind == "psd" ? "PSD" : kind == "svg" ? "SVG"
                                                   : titleImportKindLabel(kind);
    return "Import " + label + " (" + sourceFileName + ")";
}

TitleJson editor::titleImportJson(const TitleCreateResult& result, const fs::path& projectPath) {
    TitleJson files = TitleJson::array();
    for (const TitleOpFile& f : result.files) {
        std::error_code ec;
        const fs::path relative = fs::relative(fs::path(f.path), projectPath, ec);
        files.push_back(generic(ec ? f.path : relative));
    }
    TitleJson warnings = TitleJson::array();
    for (const std::string& w : result.warnings) warnings.push_back(w);
    return TitleJson{{"kind", result.kind}, {"files", files}, {"notes", result.notes}, {"warnings", warnings}};
}

std::string editor::titleSafeFileName(const std::string& name) {
    std::string trimmed = name;
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back())) trimmed.pop_back();
    size_t start = 0;
    while (start < trimmed.size() && std::isspace((unsigned char)trimmed[start])) start++;
    trimmed = trimmed.substr(start);
    // runs of \ / : * ? " < > | and whitespace become one '_'
    std::string out;
    bool run = false;
    for (char c : trimmed) {
        const bool bad = std::string("\\/:*?\"<>|").find(c) != std::string::npos || std::isspace((unsigned char)c) || (unsigned char)c < 32;
        if (bad) {
            if (!run) out += '_';
            run = true;
        } else {
            out += c;
            run = false;
        }
    }
    size_t a = 0, b = out.size();
    while (a < b && out[a] == '_') a++;
    while (b > a && out[b - 1] == '_') b--;
    out = out.substr(a, b - a);
    // "." and ".." are no file names
    if (out.empty() || out == "." || out == "..") return "untitled";
    return out;
}

fs::path editor::titleImportFolder(const fs::path& document) {
    std::string stem = document.filename().string();
    const std::string ext = ".titleart";
    if (stem.size() > ext.size() && lower(stem.substr(stem.size() - ext.size())) == ext) stem = stem.substr(0, stem.size() - ext.size());
    return document.parent_path() / (stem + "_assets");
}

TitleImportAssets::TitleImportAssets(const fs::path& document, const fs::path& assetsPath)
    : directory(titleImportFolder(document)), assets(assetsPath) {
}

void TitleImportAssets::scan(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path path = it->path();
        if (path.extension() == ".meta") continue;
        taken.insert(lower(generic(path)));
        std::string bytes;
        if (!readAll(path, bytes)) continue;
        std::error_code rel;
        const fs::path relative = fs::relative(path, assets, rel);
        if (!rel) byHash.emplace(sha256Hex(bytes), generic(relative));
    }
}

std::string TitleImportAssets::add(const std::string& name, const std::string& bytes, const std::string& subfolder) {
    if (!scanned) {
        scan(directory);
        scanned = true;
    }
    const std::string hash = sha256Hex(bytes);
    if (auto it = byHash.find(hash); it != byHash.end()) return it->second;
    const fs::path dir = subfolder.empty() ? directory : directory / subfolder;
    const std::string safe = titleSafeFileName(name);
    const fs::path base(safe);
    const std::string stem = base.stem().string().empty() ? safe : base.stem().string();
    const std::string extension = base.extension().string();
    fs::path target = dir / safe;
    for (int n = 2; taken.count(lower(generic(target))); n++) target = dir / (stem + "_" + std::to_string(n) + extension);
    taken.insert(lower(generic(target)));
    staged.push_back({target.string(), bytes});
    std::error_code ec;
    const std::string relative = generic(fs::relative(target, assets, ec));
    byHash.emplace(hash, relative);
    return relative;
}
