// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Notices.h"

#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

}

bool publish::loadNoticeLibraries(const fs::path& noticesJson, std::vector<NoticeLibrary>& out, std::string& error) {
    std::string text;
    if (!readText(noticesJson, text)) {
        error = "cannot read " + noticesJson.string();
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.contains("libraries")) {
        error = noticesJson.string() + " is not engine/notices.json";
        return false;
    }
    out.clear();
    for (const auto& [name, entry] : json["libraries"].items()) {
        NoticeLibrary library{name, entry.value("dir", ""), {}};
        const auto when = entry.find("when");
        if (when == entry.end()) {
            library.when = {"always"};
        } else if (when->is_string()) {
            library.when = {when->get<std::string>()};
        } else if (when->is_array()) {
            for (const auto& condition : *when) {
                if (condition.is_string()) library.when.push_back(condition.get<std::string>());
            }
        }
        out.push_back(std::move(library));
    }
    std::sort(out.begin(), out.end(), [](const NoticeLibrary& a, const NoticeLibrary& b) { return a.name < b.name; });
    return true;
}

std::vector<publish::NoticeLibrary> publish::shippedLibraries(const std::vector<NoticeLibrary>& all, const std::vector<std::string>& features) {
    std::vector<NoticeLibrary> out;
    for (const NoticeLibrary& library : all) {
        const bool ships = std::any_of(library.when.begin(), library.when.end(), [&](const std::string& condition) {
            return condition == "always" || std::find(features.begin(), features.end(), condition) != features.end();
        });
        if (ships) out.push_back(library);
    }
    return out;
}

std::vector<fs::path> publish::licenceFiles(const fs::path& libraryDir) {
    // (as tools/ci/check-notices.py's LICENCE_FILE; the extension may name the licence and its version, as
    // FFmpeg's COPYING.LGPLv2.1 does)
    static const std::regex licence(R"(^(licen[cs]e|copying|unlicense)(-[\w-]+)?(\.(txt|md|rst|[al]?gplv\d(\.\d)?))?$)", std::regex::icase);
    static const std::regex primary(R"(^(licen[cs]e|copying)(\.(txt|md))?$)", std::regex::icase);
    static const std::regex notice(R"(^notice(\.(txt|md))?$)", std::regex::icase);
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(libraryDir, ec))
        if (entry.is_regular_file(ec)) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::vector<fs::path> licences, notices;
    for (const fs::path& file : files) {
        const std::string name = file.filename().string();
        if (std::regex_match(name, licence)) licences.push_back(file);
        else if (std::regex_match(name, notice)) notices.push_back(file);
    }
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::stable_sort(licences.begin(), licences.end(), [&](const fs::path& a, const fs::path& b) {
        const bool aPrimary = std::regex_match(a.filename().string(), primary);
        const bool bPrimary = std::regex_match(b.filename().string(), primary);
        if (aPrimary != bPrimary) return aPrimary;
        return lower(a.filename().string()) < lower(b.filename().string());
    });
    licences.insert(licences.end(), notices.begin(), notices.end());
    return licences;
}

std::string publish::stagedNoticeName(const std::string& library, size_t index, const fs::path& file) {
    return index == 0 ? library + "-LICENSE" : library + "-" + file.filename().string();
}

std::string publish::thirdPartyNotices(const std::string& noticeMd, const std::vector<NoticeLibrary>& libraries) {
    std::vector<std::string> rows;
    size_t start = 0;
    while (start < noticeMd.size()) {
        size_t end = noticeMd.find('\n', start);
        if (end == std::string::npos) end = noticeMd.size();
        std::string line = noticeMd.substr(start, end - start);
        start = end + 1;
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '|') rows.push_back(line);
    }
    static const std::regex link(R"(\[([^\]]*)\]\([^)]*\))");
    std::string out = "Third-party software in this game\n\nEach library below keeps its own licence; its text is in licenses/<library>-LICENSE.\n\n";
    for (const NoticeLibrary& library : libraries) {
        const std::string location = "`" + library.dir + "/`";
        auto row = std::find_if(rows.begin(), rows.end(), [&](const std::string& r) { return r.find(location) != std::string::npos; });
        if (row == rows.end()) continue;
        std::vector<std::string> cells;
        std::string trimmed = *row;
        trimmed.erase(0, trimmed.find('|') + 1);
        if (!trimmed.empty() && trimmed.back() == '|') trimmed.pop_back();
        size_t at = 0;
        while (at <= trimmed.size()) {
            const size_t bar = trimmed.find('|', at);
            std::string cell = trimmed.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
            cell = std::regex_replace(cell, link, "$1");
            cell.erase(std::remove(cell.begin(), cell.end(), '`'), cell.end());
            const size_t b = cell.find_first_not_of(' '), e = cell.find_last_not_of(' ');
            cells.push_back(b == std::string::npos ? "" : cell.substr(b, e - b + 1));
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
        cells.resize(5);
        out += library.name + ": " + cells[0] + ", version " + cells[1] + ", " + cells[3] + " (licenses/" + library.name +
               "-LICENSE). Used for: " + cells[4] + "\n";
    }
    return out;
}

bool publish::findNoticeSources(NoticeSources& out, std::string& error) {
    // the engine sources the editor exports from (a repository's engine/, or the install's copy)
    const fs::path engine = FileUtils::getEngineDir();
    const fs::path exe = FileUtils::getExecutableDir();
    std::error_code ec;
    for (const fs::path& root : {engine.parent_path(), exe, exe.parent_path()}) {
        if (fs::is_regular_file(root / "engine" / "notices.json", ec) && fs::is_regular_file(root / "NOTICE.md", ec)) {
            out = {root / "engine" / "notices.json", root / "NOTICE.md", root};
            return true;
        }
    }
    if (fs::is_regular_file(engine / "notices.json", ec)) {
        for (const fs::path& md : {exe / "NOTICE.md", exe.parent_path() / "NOTICE.md", engine.parent_path() / "NOTICE.md"}) {
            if (fs::is_regular_file(md, ec)) {
                out = {engine / "notices.json", md, engine.parent_path()};
                return true;
            }
        }
    }
    error = "the editor's engine/notices.json and NOTICE.md were not found (next to " + engine.string() + ")";
    return false;
}

bool publish::stageCompileNotices(const fs::path& target, const std::vector<std::string>& features, std::vector<std::string>& staged,
                                  std::string& error) {
    NoticeSources sources;
    if (!findNoticeSources(sources, error)) return false;
    std::vector<NoticeLibrary> all;
    if (!loadNoticeLibraries(sources.noticesJson, all, error)) return false;
    const std::vector<NoticeLibrary> shipped = shippedLibraries(all, features);
    std::error_code ec;
    fs::create_directories(target / "licenses", ec);
    for (const NoticeLibrary& library : shipped) {
        // a library dir is the repository's (engine/libs/x), or engine-relative in an install
        fs::path dir = sources.repositoryRoot / library.dir;
        if (!fs::is_directory(dir, ec)) dir = FileUtils::getEngineDir() / fs::path(library.dir).lexically_relative("engine");
        const std::vector<fs::path> files = licenceFiles(dir);
        if (files.empty()) {
            error = "no licence file of " + library.name + " in " + dir.string();
            return false;
        }
        for (size_t i = 0; i < files.size(); i++) {
            const std::string name = stagedNoticeName(library.name, i, files[i]);
            // a Compile build's folder itself, where the last build's notices are: not a staging folder
            io::AtomicWriteOptions cache;
            cache.durability = io::Durability::Cache;
            if (!FileUtils::copyFileAtomic(files[i], target / "licenses" / name, ec, cache)) {
                error = "cannot stage " + name + ": " + ec.message();
                return false;
            }
            staged.push_back(name);
        }
    }
    std::string md;
    if (!readText(sources.noticeMd, md)) {
        error = "cannot read " + sources.noticeMd.string();
        return false;
    }
    const io::WriteResult written = io::writeFileAtomic(target / "THIRD-PARTY-NOTICES.txt", thirdPartyNotices(md, shipped));
    if (!written.ok()) {
        error = io::describeWriteError(written);
        return false;
    }
    return true;
}
