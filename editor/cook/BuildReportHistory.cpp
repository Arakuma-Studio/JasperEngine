// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildReportHistory.h"

#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    bool utcParts(std::time_t when, std::tm& out){
#ifdef _WIN32
        return gmtime_s(&out, &when) == 0;
#else
        return gmtime_r(&when, &out) != nullptr;
#endif
    }

    // ids sort by their time, then by the collision number ("-2" after none, "-10" after "-9")
    struct HistoryIdKey {
        std::string time, method;
        int number = 1;
    };

    HistoryIdKey historyIdKey(const std::string& id){
        HistoryIdKey key;
        key.time = id.substr(0, std::min<size_t>(15, id.size()));
        std::string rest = id.size() > 16 ? id.substr(16) : std::string();
        const size_t dash = rest.rfind('-');
        if (dash != std::string::npos && dash + 1 < rest.size() &&
            std::all_of(rest.begin() + (long)dash + 1, rest.end(), [](char c){ return c >= '0' && c <= '9'; })){
            key.number = std::atoi(rest.c_str() + dash + 1);
            rest = rest.substr(0, dash);
        }
        key.method = rest;
        return key;
    }

    bool newerHistoryId(const std::string& a, const std::string& b){
        const HistoryIdKey x = historyIdKey(a), y = historyIdKey(b);
        if (x.time != y.time) return x.time > y.time;
        if (x.number != y.number) return x.number > y.number;
        return x.method > y.method;
    }

    std::vector<std::string> historyPlatforms(const fs::path& root){
        std::vector<std::string> out;
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) out.push_back(FileUtils::pathToUtf8(it->path().filename()));
        std::sort(out.begin(), out.end());
        return out;
    }

}

fs::path BuildReportHistory::directory(const fs::path& projectPath){
    return projectPath / ".jasperengine" / "build-reports";
}

std::string BuildReportHistory::isoTime(std::time_t when){
    std::tm parts{};
    if (!utcParts(when, parts)) return std::string();
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return text;
}

std::string BuildReportHistory::newId(const fs::path& projectPath, const std::string& platform, const std::string& method, std::time_t when){
    std::tm parts{};
    utcParts(when, parts);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &parts);
    const std::string base = std::string(stamp) + "-" + method;
    const fs::path folder = directory(projectPath) / platform;
    std::error_code ec;
    std::string id = base;
    for (int n = 2; fs::exists(folder / (id + ".json"), ec); n++) id = base + "-" + std::to_string(n);
    return id;
}

bool BuildReportHistory::record(const fs::path& projectPath, const BuildReport& report, const std::string& bytes, std::string& warning){
    warning.clear();
    const fs::path folder = directory(projectPath) / report.platform;
    std::error_code ec;
    fs::create_directories(folder, ec);
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult result = ec ? io::WriteResult() : io::writeFileAtomic(folder / (report.id + ".json"), bytes, options);
    if (ec || !result.ok()){
        warning = "the Build Report history could not be written (" + FileUtils::pathToUtf8(folder) + ": " +
                  (ec ? ec.message() : result.message) + "): Compare With... will not list this build";
        return true;
    }
    const std::vector<Entry> entries = list(projectPath, report.platform);
    for (size_t i = kKeep; i < entries.size(); i++) fs::remove(entries[i].file, ec);
    return true;
}

std::vector<BuildReportHistory::Entry> BuildReportHistory::list(const fs::path& projectPath, const std::string& platform){
    std::vector<Entry> out;
    const fs::path root = directory(projectPath);
    for (const std::string& name : platform.empty() ? historyPlatforms(root) : std::vector<std::string>{platform}){
        std::error_code ec;
        for (fs::directory_iterator it(root / FileUtils::pathFromUtf8(name), ec), end; !ec && it != end; it.increment(ec)){
            if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
            out.push_back({name, FileUtils::pathToUtf8(it->path().stem()), it->path()});
        }
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b){
        return a.id != b.id ? newerHistoryId(a.id, b.id) : a.platform < b.platform;
    });
    return out;
}

fs::path BuildReportHistory::resolve(const fs::path& projectPath, const std::string& idOrPath, std::string& error){
    error.clear();
    if (idOrPath.empty() || idOrPath == "last"){
        const fs::path file = last(projectPath);
        if (file.empty()) error = "no Build Report yet: export first (every export writes one)";
        return file;
    }
    for (const Entry& entry : list(projectPath))
        if (entry.id == idOrPath) return entry.file;
    fs::path path = FileUtils::pathFromUtf8(idOrPath);
    if (path.is_relative()) path = projectPath / path;
    std::error_code ec;
    if (fs::is_directory(path, ec)) path /= BuildReport::kFileName;
    if (fs::is_regular_file(path, ec)) return path;
    std::string ids;
    for (const Entry& entry : list(projectPath)) ids += (ids.empty() ? "" : ", ") + entry.id;
    error = "no Build Report \"" + idOrPath + "\": give a history id (" + (ids.empty() ? std::string("the history is empty") : ids) +
            "), a build-report.json or the output folder holding one";
    return fs::path();
}

std::string BuildReportHistory::previous(const fs::path& projectPath, const BuildReport& report){
    const std::vector<Entry> entries = list(projectPath, report.platform);
    for (size_t i = 0; i < entries.size(); i++)
        if (entries[i].id == report.id) return i + 1 < entries.size() ? entries[i + 1].id : std::string();
    // a report outside the history (an older export's file): the newest entry before it, if it has a time
    for (const Entry& entry : entries)
        if (report.id.empty() || newerHistoryId(report.id, entry.id)) return entry.id;
    return std::string();
}

fs::path BuildReportHistory::last(const fs::path& projectPath){
    std::error_code ec;
    const fs::path session = BuildReport::last();
    if (!session.empty() && fs::is_regular_file(session, ec)) return session;
    const std::vector<Entry> entries = list(projectPath);
    return entries.empty() ? fs::path() : entries.front().file;
}
