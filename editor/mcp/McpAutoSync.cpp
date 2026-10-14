// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpAutoSync.h"

#include "McpContext.h"
#include "McpServer.h"
#include "tools/W4Shared.h"

#include "Project.h"
#include "util/Util.h"
#include "window/CodeEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#endif

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace fs = std::filesystem;

namespace {

// A scan (before a call, and the settle after it) stops checking assets after this much time
// (at least a quarter of it is theirs); the next one resumes where it stopped.
constexpr double SCAN_BUDGET_MS = 2.0;
constexpr double SETTLE_BUDGET_MS = 1.0;
constexpr size_t MIN_ITEMS = 16;
// Folders and code files are checked whole on every scan up to this many each
constexpr size_t WHOLE_TIER_MAX = 2000;
constexpr size_t HOT_MAX = 128;
constexpr int MAX_DEPTH = 48;
// Listed paths in a report (the rest is counted)
constexpr size_t REPORT_PATHS_MAX = 50;
// Filesystem timestamps come from a coarse clock that can lag the real time by a tick: a
// write made just after a call started may be stamped slightly before it.
constexpr int64_t SETTLE_SLACK_NS = 15'000'000;

struct FileStat {
    int64_t mtime = 0;
    uint64_t size = 0;
    bool isDir = false;
};

bool statPath(const std::string& path, FileStat& out) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(fs::path(path).c_str(), GetFileExInfoStandard, &data)) return false;
    ULARGE_INTEGER time;
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    // 100 ns ticks since 1601-01-01 -> ns since 1970-01-01
    out.mtime = (static_cast<int64_t>(time.QuadPart) - 116444736000000000LL) * 100;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
#if defined(__APPLE__)
    out.mtime = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    out.mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    out.size = static_cast<uint64_t>(st.st_size);
    out.isDir = S_ISDIR(st.st_mode);
    return true;
#endif
}

int64_t toNs(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// CPU time of the calling thread in ms: what a scan costs, whatever the host's load
double threadCpuMs() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
    auto ticks = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0.0;
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
#endif
}

std::string lowerExtension(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    const size_t slash = name.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string childOf(const std::string& dir, const std::string& name) {
    return dir.empty() ? name : dir + "/" + name;
}

std::string prefixOf(const std::string& dir) {
    return dir.empty() ? std::string() : dir + "/";
}

// Visits the direct children (no deeper) of `dir` in a map keyed by generic relative path
template <typename Map, typename Fn>
void forDirectChildren(Map& map, const std::string& dir, Fn fn) {
    const std::string prefix = prefixOf(dir);
    std::vector<std::string> keys;
    for (auto it = map.lower_bound(prefix); it != map.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        if (it->first.size() == prefix.size()) continue;   // the folder itself (dirs map)
        if (it->first.find('/', prefix.size()) != std::string::npos) continue;
        keys.push_back(it->first);
    }
    for (const std::string& key : keys) fn(key);
}

std::string shortMessage(std::string text, size_t max) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (text.size() > max) text = text.substr(0, max) + "...";
    return text;
}

} // namespace

McpAutoSync& McpAutoSync::get() {
    static McpAutoSync instance;
    return instance;
}

bool McpAutoSync::isTrackedFile(const std::string& path) {
    if (Util::isScriptFile(path) || Util::isShaderFile(path) || Util::isMaterialFile(path)) return true;
    if (isUiCodeFile(path) || isAssetFile(path)) return true;
    const std::string ext = lowerExtension(path);
    return ext == ".c" || ext == ".inl";
}

bool McpAutoSync::isUiCodeFile(const std::string& path) {
    const std::string ext = lowerExtension(path);
    return ext == ".html" || ext == ".htm" || ext == ".css";
}

bool McpAutoSync::isAssetFile(const std::string& path) {
    if (Util::isImageFile(path)) return true;
    const std::string ext = lowerExtension(path);
    return ext == ".ttf" || ext == ".otf" || ext == ".json" || ext == ".lottie";
}

bool McpAutoSync::isEditorOwnedFile(const std::string& rel) {
    // Terrain maps the editor writes from its in-memory terrain (TerrainMapFileWriter, after the
    // edit returns): reloading them from disk would only undo edits (TerrainEditWindow's rule)
    const size_t slash = rel.find_last_of('/');
    const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    if (name.rfind("terrain_edit_", 0) != 0 || slash == std::string::npos) return false;
    const std::string parent = rel.substr(0, slash);
    const size_t parentSlash = parent.find_last_of('/');
    return (parentSlash == std::string::npos ? parent : parent.substr(parentSlash + 1)) == "terrain_maps";
}

bool McpAutoSync::tracks(const std::string& rel) {
    return isTrackedFile(rel) && !isEditorOwnedFile(rel);
}

bool McpAutoSync::isSkippedDirName(const std::string& name) {
    if (name.empty() || name[0] == '.') return true;   // .jasperengine, .git, .vscode, .trash...
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "build" || lower == "node_modules" || lower == "trash";
}

bool McpAutoSync::insideSkippedDir(const std::string& rel) const {
    size_t start = 0;
    while (true) {
        const size_t slash = rel.find('/', start);
        if (slash == std::string::npos) return false;
        if (isSkippedDirName(rel.substr(start, slash - start))) return true;
        start = slash + 1;
    }
}

std::string McpAutoSync::absolute(const std::string& rel) const {
#ifdef _WIN32
    return (rel.empty() ? root : root / fs::path(rel)).string();
#else
    return rel.empty() ? root.string() : root.string() + "/" + rel;
#endif
}

McpAutoSync::FileMap& McpAutoSync::mapFor(const std::string& rel) {
    return isAssetFile(rel) ? assets : code;
}

// ------------------------------------------------------------------ baseline

bool McpAutoSync::ensureRoot(McpContext& ctx, bool* rebased) {
    if (rebased) *rebased = false;
    Project* project = ctx.project();
    if (!project) return false;
    const fs::path& path = project->getProjectPath();
    if (path.empty()) return false;
    if (path != root) {
        baseline(path);
        // The startup project: what changed since the server started (an agent editing while it
        // loaded) is external, for the first call
        std::set<std::string> recent;
        if (!startupBaselineTaken) {
            startupBaselineTaken = true;
            const fs::path& startup = mcpLaunchOptions().project;
            std::error_code ec;
            if (serverStartNs > 0 && !startup.empty() && fs::equivalent(startup, path, ec)) {
                const int64_t since = serverStartNs - SETTLE_SLACK_NS;
                for (const FileMap* map : {&code, &assets}) {
                    for (const auto& [rel, stamp] : *map) {
                        if (stamp.mtime >= since) recent.insert(rel);
                    }
                }
                for (const std::string& rel : recent) {
                    merge(pendingExternal, rel, 'm', true);
                    markHot(rel);
                }
            }
        }
        // sync_files' own snapshot starts here too: whichever of the two sees an edit first
        // reports it, and tells the other it is handled
        w4::takeSyncSnapshot(project, recent);
        if (rebased) *rebased = true;
    }
    return true;
}

void McpAutoSync::baseline(const fs::path& projectRoot) {
    root = projectRoot;
    code.clear();
    assets.clear();
    dirs.clear();
    hot.clear();
    pendingExternal.clear();
    deferred.clear();
    dirCursor.clear();
    codeCursor.clear();
    assetCursor.clear();
    walkDir("", nullptr, 0);
}

// Records a folder and everything under it; with `changes`, files not known yet are added
// (and known ones with another stamp modified)
void McpAutoSync::walkDir(const std::string& rel, std::vector<Change>* changes, int depth) {
    FileStat st;
    if (!statPath(absolute(rel), st) || !st.isDir) return;
    dirs[rel] = st.mtime;
    if (depth >= MAX_DEPTH) return;

    std::error_code ec;
    fs::directory_iterator it(fs::path(absolute(rel)), fs::directory_options::skip_permission_denied, ec);
    for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code typeEc;
        if (it->is_symlink(typeEc) && it->is_directory(typeEc)) continue;   // no cycles
        const std::string child = childOf(rel, name);
        if (it->is_directory(typeEc)) {
            if (!isSkippedDirName(name)) walkDir(child, changes, depth + 1);
            continue;
        }
        if (!tracks(child)) continue;
        FileStat fileStat;
        if (!statPath(absolute(child), fileStat) || fileStat.isDir) continue;
        FileMap& map = mapFor(child);
        auto found = map.find(child);
        const Stamp stamp{fileStat.mtime, fileStat.size};
        if (found == map.end()) {
            map[child] = stamp;
            if (changes) {
                changes->push_back({child, 'a', fileStat.mtime});
                markHot(child);
            }
        } else if (found->second.mtime != stamp.mtime || found->second.size != stamp.size) {
            found->second = stamp;
            if (changes) {
                changes->push_back({child, 'm', fileStat.mtime});
                markHot(child);
            }
        }
    }
}

void McpAutoSync::prepare(McpContext& ctx) {
    ensureRoot(ctx);
}

void McpAutoSync::serverStarted() {
    if (serverStartNs == 0) serverStartNs = toNs(std::chrono::system_clock::now());
}

// ------------------------------------------------------------------ scanning

void McpAutoSync::markHot(const std::string& rel) {
    auto it = std::find(hot.begin(), hot.end(), rel);
    if (it != hot.end()) hot.erase(it);
    hot.push_back(rel);
    if (hot.size() > HOT_MAX) hot.erase(hot.begin());
}

int64_t McpAutoSync::nearestDirTime(const std::string& rel) const {
    std::string dir = rel;
    while (true) {
        const size_t slash = dir.find_last_of('/');
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash);
        FileStat st;
        if (statPath(absolute(dir), st)) return st.mtime;
        if (dir.empty()) return 0;
    }
}

void McpAutoSync::removeTree(const std::string& relDir, std::vector<Change>* changes) {
    const std::string prefix = prefixOf(relDir);
    const int64_t when = changes ? nearestDirTime(relDir) : 0;
    for (FileMap* map : {&code, &assets}) {
        for (auto it = map->lower_bound(prefix); it != map->end() && it->first.compare(0, prefix.size(), prefix) == 0;) {
            if (changes) changes->push_back({it->first, 'r', when});
            it = map->erase(it);
        }
    }
    dirs.erase(relDir);
    for (auto it = dirs.lower_bound(prefix); it != dirs.end() && it->first.compare(0, prefix.size(), prefix) == 0;) {
        it = dirs.erase(it);
    }
}

void McpAutoSync::checkFile(const std::string& rel, std::vector<Change>& changes) {
    scanChecked++;
    FileMap& map = mapFor(rel);
    auto it = map.find(rel);
    if (it == map.end()) return;
    FileStat st;
    if (!statPath(absolute(rel), st) || st.isDir) {
        map.erase(it);
        changes.push_back({rel, 'r', nearestDirTime(rel)});
        markHot(rel);
        return;
    }
    if (st.mtime != it->second.mtime || st.size != it->second.size) {
        it->second = {st.mtime, st.size};
        changes.push_back({rel, 'm', st.mtime});
        markHot(rel);
    }
}

// A folder's mtime changes when an entry is added, removed or renamed in it (not when a
// file's content changes): then its children are listed again.
void McpAutoSync::checkDir(const std::string& rel, std::vector<Change>& changes) {
    scanChecked++;
    auto known = dirs.find(rel);
    if (known == dirs.end()) return;
    FileStat st;
    if (!statPath(absolute(rel), st) || !st.isDir) {
        if (rel.empty()) return;   // the project folder itself is gone: nothing sensible to do
        removeTree(rel, &changes);
        return;
    }
    if (st.mtime == known->second) return;
    known->second = st.mtime;

    std::set<std::string> presentFiles, presentDirs;
    std::error_code ec;
    fs::directory_iterator it(fs::path(absolute(rel)), fs::directory_options::skip_permission_denied, ec);
    for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code typeEc;
        if (it->is_symlink(typeEc) && it->is_directory(typeEc)) continue;
        const std::string child = childOf(rel, name);
        if (it->is_directory(typeEc)) {
            if (isSkippedDirName(name)) continue;
            presentDirs.insert(child);
            if (!dirs.count(child)) walkDir(child, &changes, 1);
            continue;
        }
        if (!tracks(child)) continue;
        presentFiles.insert(child);
        FileMap& map = mapFor(child);
        if (map.count(child)) continue;   // modifications: checkFile
        FileStat fileStat;
        if (!statPath(absolute(child), fileStat) || fileStat.isDir) continue;
        map[child] = {fileStat.mtime, fileStat.size};
        changes.push_back({child, 'a', fileStat.mtime});
        markHot(child);
    }
    if (ec) return;   // a listing that failed half-way must not remove anything

    for (FileMap* map : {&code, &assets}) {
        forDirectChildren(*map, rel, [&](const std::string& child) {
            if (presentFiles.count(child)) return;
            map->erase(child);
            changes.push_back({child, 'r', st.mtime});
            markHot(child);
        });
    }
    forDirectChildren(dirs, rel, [&](const std::string& child) {
        if (!presentDirs.count(child)) removeTree(child, &changes);
    });
}

void McpAutoSync::scan(std::vector<Change>& changes, double budgetMs, size_t minAssets, bool& complete) {
    const auto start = std::chrono::steady_clock::now();
    const auto budget = std::chrono::microseconds(static_cast<int64_t>(budgetMs * 1000.0));
    complete = true;
    scanChecked = 0;

    // Files changed this session first: the ones being worked on
    const std::vector<std::string> hotNow = hot;
    for (const std::string& rel : hotNow) {
        if (mapFor(rel).count(rel)) checkFile(rel, changes);
        else hot.erase(std::remove(hot.begin(), hot.end(), rel), hot.end());
    }

    // Round-robin over one tier from its cursor until `deadline` (at least minItems);
    // false when it stopped before a full pass
    auto sweep = [&](auto& map, std::string& cursor, auto check, std::chrono::steady_clock::time_point deadline,
                     size_t minItems) {
        const size_t total = map.size();
        size_t done = 0;
        std::string key;
        auto it = map.lower_bound(cursor);
        while (done < total && !map.empty()) {
            if (done >= minItems && std::chrono::steady_clock::now() >= deadline) {
                cursor = it == map.end() ? std::string() : it->first;
                return false;
            }
            if (it == map.end()) it = map.begin();
            key = it->first;
            check(key);
            done++;
            it = map.upper_bound(key);   // the check may erase entries
        }
        cursor = it == map.end() ? std::string() : it->first;
        return true;
    };

    // Folders (new and removed files) and code files are what the agent edits: checked whole
    // on every scan unless there are very many; assets (images, fonts, json) share what is left
    // of the budget
    const auto never = std::chrono::steady_clock::time_point::max();
    const bool dirsWhole = dirs.size() <= WHOLE_TIER_MAX;
    if (!sweep(dirs, dirCursor, [&](const std::string& rel) { checkDir(rel, changes); },
               dirsWhole ? never : start + budget * 3 / 10, MIN_ITEMS)) complete = false;
    const bool codeWhole = code.size() <= WHOLE_TIER_MAX;
    if (!sweep(code, codeCursor, [&](const std::string& rel) { checkFile(rel, changes); },
               codeWhole ? never : start + budget * 65 / 100, MIN_ITEMS)) complete = false;
    // Assets: the rest of the budget, but at least a quarter of it whatever the rest took
    const auto assetsDeadline = std::max(start + budget, std::chrono::steady_clock::now() + budget / 4);
    if (!sweep(assets, assetCursor, [&](const std::string& rel) { checkFile(rel, changes); },
               assetsDeadline, minAssets)) complete = false;
}

void McpAutoSync::restamp(const std::string& rel) {
    if (rel.empty() || rel.rfind("../", 0) == 0 || rel == ".." || insideSkippedDir(rel)) return;
    FileStat st;
    const bool exists = statPath(absolute(rel), st);
    if (!exists) {
        // a file, or a folder with what it held
        code.erase(rel);
        assets.erase(rel);
        removeTree(rel, nullptr);
        return;
    }
    if (st.isDir) {
        if (isSkippedDirName(fs::path(rel).filename().string())) return;
        walkDir(rel, nullptr, 1);
        return;
    }
    if (!tracks(rel)) return;
    mapFor(rel)[rel] = {st.mtime, st.size};
}

void McpAutoSync::merge(std::map<std::string, Pending>& into, const std::string& rel, char kind, bool report) {
    auto it = into.find(rel);
    if (it == into.end()) {
        into[rel] = {kind, report};
        return;
    }
    Pending& p = it->second;
    p.report = p.report || report;
    if (p.kind == 'a' && kind == 'r') {
        into.erase(it);   // came and went
    } else if (p.kind == 'a') {
        // still added
    } else if (p.kind == 'r' && kind == 'a') {
        p.kind = 'm';
    } else {
        p.kind = kind;
    }
}

// ------------------------------------------------------------------ pick-up and report

Json McpAutoSync::compactProblems(const Json& problems) {
    Json out = Json::array();
    for (const Json& problem : problems) {
        Json item = {{"path", problem.value("path", std::string())}};
        if (problem.contains("line")) item["line"] = problem["line"];
        // HTML UI problems (w4::syncHtmlUi) also carry col, severity, code and suggestion
        for (const char* key : {"col", "severity", "code"}) {
            if (problem.contains(key)) item[key] = problem[key];
        }
        item["message"] = problem.value("message", std::string());
        if (problem.contains("suggestion")) item["suggestion"] = problem["suggestion"];
        out.push_back(item);
    }
    return out;
}

void McpAutoSync::addUiReport(Json& report, const Json& reloaded, int current) {
    if (!reloaded.empty()) report["ui_reloaded"] = reloaded;
    if (current > 0) report["ui_current"] = current;
}

Json McpAutoSync::process(McpContext& ctx, const std::vector<Change>& changes, std::vector<std::string>& shaderFiles) {
    std::vector<fs::path> paths;
    std::set<std::string> reportable;
    for (const Change& change : changes) {
        paths.push_back((root / fs::path(change.rel)).lexically_normal());
        if (change.report) reportable.insert(change.rel);
    }
    Json result = w4::syncFiles(ctx, paths, false);
    if (reportable.empty()) return nullptr;

    Json changed = Json::array(), added = Json::array(), removed = Json::array();
    for (const Change& change : changes) {
        if (!change.report) continue;
        if (changed.size() < REPORT_PATHS_MAX) changed.push_back(change.rel);
        if (change.kind == 'a' && added.size() < REPORT_PATHS_MAX) added.push_back(change.rel);
        if (change.kind == 'r' && removed.size() < REPORT_PATHS_MAX) removed.push_back(change.rel);
        if (change.kind != 'r' && Util::isShaderFile(change.rel)) shaderFiles.push_back(change.rel);
    }
    Json report = {{"changed", changed}};
    if (reportable.size() > changed.size()) report["changed_count"] = reportable.size();
    if (!added.empty()) report["added"] = added;
    if (!removed.empty()) report["removed"] = removed;

    report["problems"] = compactProblems(result.value("problems", Json::array()));
    addUiReport(report, result.value("ui_reloaded", Json::array()), result.value("ui_current", 0));

    Json scripts = Json::array();
    for (const Json& item : result.value("script_properties", Json::array())) {
        if (!item.contains("added") && !item.contains("removed")) continue;
        Json compact = {{"entity", item.contains("entity") ? item["entity"].value("path", std::string()) : std::string()},
                        {"script", item.value("script", std::string())}};
        if (item.contains("added")) compact["added"] = item["added"];
        if (item.contains("removed")) compact["removed"] = item["removed"];
        scripts.push_back(compact);
    }
    if (!scripts.empty()) report["script_properties"] = scripts;
    const Json reloaded = result.contains("code_editor") ? result["code_editor"].value("reloaded", Json::array()) : Json::array();
    if (!reloaded.empty()) report["reloaded_buffers"] = reloaded;
    return report;
}

void McpAutoSync::beforeCall(McpContext& ctx, const CallPtr& call, const std::string& tool, std::function<void(Json)> run) {
    // sync_files does the same work and reports it itself. A baseline just taken (the call came
    // before the frame that would have) has nothing new, but may hold the startup changes.
    if (tool == "sync_files" || !ensureRoot(ctx)) {
        run(nullptr);
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    const double cpuStart = threadCpuMs();
    std::vector<Change> found;
    bool complete = true;
    scan(found, SCAN_BUDGET_MS, MIN_ITEMS, complete);
    lastScanMs = msSince(started);
    lastScanCpuMs = threadCpuMs() - cpuStart;
    lastScanChecked = scanChecked;
    lastComplete = complete;

    // One entry per path: this scan, what the last settle left, and (once Play stopped) what
    // Play deferred
    std::map<std::string, Pending> merged;
    for (auto& [rel, pending] : pendingExternal) merge(merged, rel, pending.kind, pending.report);
    pendingExternal.clear();
    for (const Change& change : found) merge(merged, change.rel, change.kind, true);

    const bool playing = ctx.isPlaying();
    if (playing) {
        if (merged.empty()) {
            run(nullptr);
            return;
        }
        // HTML UI files are not deferred: the documents (the running ones and the edit-time
        // ones) reload now. Images go to the documents now and to the scene textures after Play.
        std::vector<fs::path> uiPaths, imagePaths;
        bool otherDeferred = false;
        for (const auto& [rel, pending] : merged) {
            const fs::path path = (root / fs::u8path(rel)).lexically_normal();
            // a project UI effect's .frag (or one that is gone) reloads its documents now too (phase 5)
            if (isUiCodeFile(rel) || (isAssetFile(rel) && !Util::isImageFile(rel)) ||
                (lowerExtension(rel) == ".frag" && (pending.kind == 'r' || ProjectChecks::isUiEffectFile(path)))) {
                uiPaths.push_back(path);
                continue;
            }
            if (Util::isImageFile(rel)) imagePaths.push_back(path);
            merge(deferred, rel, pending.kind, pending.report);
            if (pending.report) otherDeferred = true;
        }
        Json uiProblems = Json::array(), uiReloaded = Json::array();
        int uiCurrent = 0;
        if (!uiPaths.empty()) {
            const Json synced = w4::syncFiles(ctx, uiPaths, false);
            uiProblems = synced.value("problems", Json::array());
            uiReloaded = synced.value("ui_reloaded", Json::array());
            uiCurrent = synced.value("ui_current", 0);
        }
        if (!imagePaths.empty()) {
            const Json synced = w4::syncHtmlUi(ctx, imagePaths);
            for (const Json& problem : synced["problems"]) uiProblems.push_back(problem);
            for (const Json& item : synced["reloaded"]) uiReloaded.push_back(item);
            uiCurrent += synced.value("current", 0);
        }

        Json changed = Json::array(), added = Json::array(), removed = Json::array(), problems = compactProblems(uiProblems);
        for (const auto& [rel, pending] : merged) {
            if (!pending.report) continue;
            if (changed.size() < REPORT_PATHS_MAX) changed.push_back(rel);
            if (pending.kind == 'a' && added.size() < REPORT_PATHS_MAX) added.push_back(rel);
            if (pending.kind == 'r' && removed.size() < REPORT_PATHS_MAX) removed.push_back(rel);
            // A syntax check runs nothing: safe during Play
            if (pending.kind != 'r' && Util::isLuaFile(rel)) {
                std::string text;
                if (ProjectChecks::readFile(root / fs::path(rel), text)) {
                    const ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(text, rel);
                    if (!syntax.ok) problems.push_back({{"path", rel}, {"line", syntax.line}, {"message", "Lua syntax error: " + syntax.message}});
                }
            }
        }
        if (changed.empty()) {
            run(nullptr);
            return;
        }
        Json report = {{"changed", changed}};
        if (!added.empty()) report["added"] = added;
        if (!removed.empty()) report["removed"] = removed;
        report["problems"] = problems;
        addUiReport(report, uiReloaded, uiCurrent);
        if (otherDeferred) {
            report["deferred"] = "A scene is playing: script properties, shaders, materials and textures are picked up when play stops"
                                 " (HTML UI documents reloaded now).";
        }
        run(report);
        return;
    }

    for (auto& [rel, pending] : deferred) merge(merged, rel, pending.kind, pending.report);
    deferred.clear();
    if (merged.empty()) {
        run(nullptr);
        return;
    }

    std::vector<Change> changes;
    for (const auto& [rel, pending] : merged) changes.push_back({rel, pending.kind, 0, pending.report});
    std::vector<std::string> shaderFiles;
    Json report = process(ctx, changes, shaderFiles);
    if (report.is_null()) {
        run(nullptr);
        return;
    }

    std::vector<ProjectChecks::ShaderUse> forks = shaderFiles.empty() ? std::vector<ProjectChecks::ShaderUse>()
                                                                     : ProjectChecks::shaderForksUsingFiles(ctx.project(), shaderFiles);
    if (forks.empty()) {
        run(report);
        return;
    }
    // Compile the forks those files belong to, so their errors come with this call
    w4::checkShadersAsync(call, forks, [run, report, shaderFiles](Json results) mutable {
        Json& problems = report["problems"];
        // shared by the results: a UI effect's forms (phase 5) report the same error once
        std::set<std::string> seen;
        for (const Json& result : results) {
            if (result.value("ok", false)) continue;
            const std::string base = result.value("shader", std::string());
            const Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(base);
            // Name the changed file of the fork (else its fragment shader)
            std::string path = paths.frag.empty() ? base : paths.frag;
            for (const std::string& file : shaderFiles) {
                if (file == paths.vert || file == paths.frag) {
                    path = file;
                    break;
                }
            }
            const Json errors = result.value("errors", Json::array());
            size_t added = 0;
            for (const Json& error : errors) {
                if (error.value("severity", std::string("error")) != "error") continue;
                const std::string message = shortMessage(error.value("message", std::string()), 300);
                if (message.find("compilation terminated") != std::string::npos) continue;
                // glslang names the file (an include, or the stage file) when it can
                const std::string source = error.value("source", std::string());
                const bool namesFile = source.find('.') != std::string::npos || source.find('/') != std::string::npos;
                const std::string where = namesFile ? source : path;
                const int line = error.value("line", 0);
                if (!seen.insert(where + ":" + std::to_string(line) + ":" + message).second) continue;
                Json problem = {{"path", where}, {"line", line},
                                {"message", "Shader " + base + " (" + result.value("type", std::string()) + ") does not compile: " + message}};
                // a UI effect (phase 5): effect-shader-compile, or effect-manifest
                if (result.contains("code")) problem["code"] = result["code"];
                problems.push_back(problem);
                if (++added >= 10) break;
            }
            if (added == 0) {
                problems.push_back({{"path", path}, {"message", "Shader " + base + " does not compile: " +
                                                                    shortMessage(result.value("log", std::string("unknown error")), 400)}});
            }
        }
        run(report);
    });
}

void McpAutoSync::afterCall(McpContext& ctx, const std::string& tool, std::chrono::system_clock::time_point start,
                            std::chrono::system_clock::time_point end) {
    bool rebased = false;
    if (tool == "sync_files" || !ensureRoot(ctx, &rebased) || rebased) return;

    const auto started = std::chrono::steady_clock::now();
    const double cpuStart = threadCpuMs();
    std::vector<Change> found;
    bool complete = true;
    scan(found, SETTLE_BUDGET_MS, 0, complete);
    lastSettleMs = msSince(started);
    lastSettleCpuMs = threadCpuMs() - cpuStart;
    if (found.empty()) return;

    // Stamped inside the call: the tool's own writes, which the tool handled (the scan has
    // taken their new stamps). Older: external, for the next call.
    const int64_t from = toNs(start) - SETTLE_SLACK_NS;
    const int64_t to = toNs(end) + SETTLE_SLACK_NS / 5;
    for (const Change& change : found) {
        if (change.when < from || change.when > to) merge(pendingExternal, change.rel, change.kind, true);
    }
}

bool McpAutoSync::mayWriteFiles(bool readOnly, const Json& args) {
    return !readOnly || (args.is_object() && args.contains("save_path"));
}

void McpAutoSync::filesSynced(McpContext& ctx, const std::vector<fs::path>& paths, bool full) {
    bool rebased = false;
    if (!ensureRoot(ctx, &rebased) || rebased) return;
    const bool playing = ctx.isPlaying();
    if (full) {
        // Everything was rescanned and picked up (script properties wait for Play to stop)
        std::map<std::string, Pending> keep;
        if (playing) keep = deferred;
        baseline(root);
        deferred = keep;
        return;
    }
    std::vector<fs::path> scripts;
    for (const fs::path& path : paths) {
        const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || rel.is_absolute()) continue;
        const std::string key = rel.generic_string();
        if (key == "." || key.rfind("..", 0) == 0) continue;
        restamp(key);
        pendingExternal.erase(key);
        if (playing) {
            // syncFiles leaves script properties for after Play: make sure they get re-parsed then
            if (Util::isScriptFile(key)) merge(deferred, key, 'm', false);
        } else {
            deferred.erase(key);
            if (Util::isScriptFile(key)) scripts.push_back(path);
        }
    }
    // The code editor's own 1 s watch must not re-parse what was just handled
    if (!scripts.empty()) {
        if (CodeEditor* editor = ctx.codeEditor()) editor->markScriptFilesCurrent(scripts);
    }
}

// ------------------------------------------------------------------ results

void McpAutoSync::attach(Json& result, const Json& report) {
    if (report.is_null() || !result.is_object()) return;
    if (!result.contains("structuredContent") || !result["structuredContent"].is_object()) {
        result["structuredContent"] = Json::object();
    }
    result["structuredContent"]["external_changes"] = report;

    std::string text = "External changes picked up (files changed on disk since your last call):";
    const Json& changed = report["changed"];
    auto has = [&](const char* key, const std::string& rel) {
        if (!report.contains(key)) return false;
        for (const Json& item : report[key]) {
            if (item == rel) return true;
        }
        return false;
    };
    size_t listed = 0;
    for (const Json& item : changed) {
        if (listed == 6) break;
        const std::string rel = item.get<std::string>();
        text += (listed ? ", " : " ") + rel + (has("added", rel) ? " (added)" : has("removed", rel) ? " (removed)" : "");
        listed++;
    }
    const size_t total = report.value("changed_count", changed.size());
    if (total > listed) text += " and " + std::to_string(total - listed) + " more";
    text += ".";
    const Json problems = report.value("problems", Json::array());
    if (problems.empty()) {
        text += " No problems.";
    } else {
        text += " Problems:";
        size_t shown = 0;
        for (const Json& problem : problems) {
            if (shown == 5) {
                text += " (+" + std::to_string(problems.size() - shown) + " more)";
                break;
            }
            text += " " + problem.value("path", std::string());
            if (problem.contains("line") && problem["line"].get<int>() > 0) text += ":" + std::to_string(problem["line"].get<int>());
            if (problem.value("severity", std::string()) == "warning") {
                text += ": warning: " + shortMessage(problem.value("message", std::string()), 200);
                if (!problem.value("code", std::string()).empty()) text += " [" + problem["code"].get<std::string>() + "]";
                text += ";";
            } else {
                text += ": " + shortMessage(problem.value("message", std::string()), 200) + ";";
            }
            shown++;
        }
    }
    const Json uiReloaded = report.value("ui_reloaded", Json::array());
    const int uiCurrent = report.value("ui_current", 0);
    if (!uiReloaded.empty() || uiCurrent > 0) {
        text += " Reloaded UI:";
        size_t listed = 0;
        for (const Json& item : uiReloaded) {
            if (listed == 4) {
                text += " and " + std::to_string(uiReloaded.size() - listed) + " more";
                break;
            }
            const std::string name = item.contains("entity") ? item["entity"].value("name", std::string()) : std::string();
            text += std::string(listed ? ", " : " ") + item.value("html", std::string()) + " in \"" + name + "\"" +
                    (item.value("stylesheet_only", false) ? " (styles only)" : "");
            listed++;
        }
        if (uiReloaded.empty()) text += " none";
        if (uiCurrent > 0) text += "; " + std::to_string(uiCurrent) + " document(s) were already current";
        text += ".";
    }
    if (report.contains("deferred")) text += " " + report["deferred"].get<std::string>();
    if (!result.contains("content") || !result["content"].is_array()) result["content"] = Json::array();
    result["content"].push_back({{"type", "text"}, {"text", text}});
}

Json McpAutoSync::stats() const {
    auto round3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    return {{"files", code.size() + assets.size()}, {"dirs", dirs.size()}, {"checked", lastScanChecked},
            {"complete", lastComplete}, {"scan_ms", round3(lastScanMs)}, {"settle_ms", round3(lastSettleMs)},
            {"scan_cpu_ms", round3(lastScanCpuMs)}, {"settle_cpu_ms", round3(lastSettleCpuMs)}};
}
