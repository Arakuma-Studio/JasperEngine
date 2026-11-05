// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpAutoSync.h"

#include "McpContext.h"
#include "McpServer.h"
#include "tools/W4Shared.h"

#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "vat/VatDocument.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "util/ScriptReloadService.h"
#include "reload/HotReloadService.h"
#include "util/FileUtils.h"

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
// the writing calls whose run the journal pick-up remembers
constexpr size_t TOOL_WINDOWS_MAX = 32;

struct FileStat {
    int64_t mtime = 0;
    uint64_t size = 0;
    bool isDir = false;
};

bool statPath(const std::string& path, FileStat& out) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(FileUtils::pathFromUtf8(path).c_str(), GetFileExInfoStandard, &data)) return false;
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

// .gltf/.glb/.obj/.fbx and glTF .bin buffers: AnimationClipPool's model sub-clips come from them
bool isModelSourceFile(const std::string& path) {
    return Util::isModelFile(path) || lowerExtension(path) == ".bin";
}

} // namespace

McpAutoSync& McpAutoSync::get() {
    static McpAutoSync instance;
    return instance;
}

bool McpAutoSync::isTrackedFile(const std::string& path) {
    if (Util::isScriptFile(path) || Util::isShaderFile(path) || Util::isMaterialFile(path) || Util::isColorLutFile(path)) return true;
    if (isUiCodeFile(path) || isAssetFile(path)) return true;
    const std::string ext = lowerExtension(path);
    // pose animations and humanoid sidecars: their open documents reload (editor/pose); clips:
    // their documents reload, AnimationClipPool drops them and their players reload
    if (ext == ".c" || ext == ".inl" || ext == ".poseanim" || ext == ".humanoid" || Util::isAnimationClipFile(path)) return true;
    // Avatar Masks and Animator Controllers: their documents reload, their pools drop them and
    // their players reload (AnimationClipDocument::filesChanged reaches them)
    if (Util::isAvatarMaskFile(path) || Util::isAnimatorControllerFile(path)) return true;
    // Vertex Animations: their documents reload, VertexAnimationPool drops them, animators reload
    if (Util::isVertexAnimationFile(path)) return true;
    // models and glTF buffers: the clips made from their animations reload (picked up, not reported)
    return isModelSourceFile(path);
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
    // UTF-8 like the keys (the ANSI code page can't hold every file name)
    return FileUtils::pathToUtf8(rel.empty() ? root : root / FileUtils::pathFromUtf8(rel));
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
    journalCursor = FileWatchService::get().journalHead();
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
    fs::directory_iterator it(FileUtils::pathFromUtf8(absolute(rel)), fs::directory_options::skip_permission_denied, ec);
    for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string name = FileUtils::pathToUtf8(it->path().filename());
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
    // clip documents' own writes (edits, their undo and redo) go through the same sync as the
    // tools' writes, so they are not reported as outside changes (installPoseSyncHook does this
    // for pose documents)
    static McpContext* clipHookContext = nullptr;
    if (clipHookContext != &ctx) {
        clipHookContext = &ctx;
        McpContext* target = &ctx;
        AnimationClipDocument::setFilesWrittenHook([target](const std::vector<fs::path>& changed) {
            w4::syncFiles(*target, changed, false);
        });
        // and .vat documents' (Inspector edits of a Vertex Animation)
        VatDocument::setFilesWrittenHook([target](const std::vector<fs::path>& changed) {
            w4::syncFiles(*target, changed, false);
        });
    }
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
    fs::directory_iterator it(FileUtils::pathFromUtf8(absolute(rel)), fs::directory_options::skip_permission_denied, ec);
    for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string name = FileUtils::pathToUtf8(it->path().filename());
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
        if (isSkippedDirName(FileUtils::pathToUtf8(FileUtils::pathFromUtf8(rel).filename()))) return;
        walkDir(rel, nullptr, 1);
        return;
    }
    if (!tracks(rel)) return;
    mapFor(rel)[rel] = {st.mtime, st.size};
}

void McpAutoSync::merge(std::map<std::string, Pending>& into, const std::string& rel, char kind, bool report, uint64_t sequence) {
    auto it = into.find(rel);
    if (it == into.end()) {
        into[rel] = {kind, report, sequence};
        return;
    }
    Pending& p = it->second;
    p.report = p.report || report;
    p.sequence = std::max(p.sequence, sequence);
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

namespace {
// one per path, line and message (the watcher and the call may both have checked a file)
Json uniqueProblems(const Json& problems) {
    Json out = Json::array();
    std::set<std::string> seen;
    for (const Json& problem : problems) {
        if (seen.insert(problem.value("path", std::string()) + ":" + std::to_string(problem.value("line", 0)) + ":" +
                        problem.value("message", std::string())).second) {
            out.push_back(problem);
        }
    }
    return out;
}
} // namespace

void McpAutoSync::addUiReport(Json& report, const Json& reloaded, int current) {
    if (!reloaded.empty()) report["ui_reloaded"] = reloaded;
    if (current > 0) report["ui_current"] = current;
}

Json McpAutoSync::process(McpContext& ctx, const std::vector<Change>& changes) {
    std::vector<fs::path> paths, models;
    std::set<std::string> reportable;
    w4::SyncOptions sync;
    for (const Change& change : changes) {
        const fs::path path = (root / FileUtils::pathFromUtf8(change.rel)).lexically_normal();
        // a model picked up (not reported): only what is already loaded from it is dropped (its
        // sub-clips, their documents and players). The full sync would also queue its thumbnail,
        // which converts an FBX nothing loaded yet.
        (!change.report && isModelSourceFile(change.rel) ? models : paths).push_back(path);
        if (change.report) reportable.insert(change.rel);
        // the journal change: what the watcher reloaded for it already is not done again
        if (change.sequence != 0) sync.sequences[path] = change.sequence;
    }
    if (!models.empty()) {
        AnimationClipDocument::filesChanged(models, ctx.project());
        // a regenerated .vat.glb stands for its .vat: the pool drops it, its animators reload
        VatDocument::filesChanged(models, ctx.project());
        filesSynced(ctx, models, false);
    }
    // quiet: the call's result reports it (the Console lines are the watcher's)
    Json result = paths.empty() ? Json{{"problems", Json::array()}} : w4::syncFiles(ctx, paths, false, sync);
    if (reportable.empty()) return nullptr;

    Json changed = Json::array(), added = Json::array(), removed = Json::array();
    for (const Change& change : changes) {
        if (!change.report) continue;
        if (changed.size() < REPORT_PATHS_MAX) changed.push_back(change.rel);
        if (change.kind == 'a' && added.size() < REPORT_PATHS_MAX) added.push_back(change.rel);
        if (change.kind == 'r' && removed.size() < REPORT_PATHS_MAX) removed.push_back(change.rel);
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
    // what the hot reload did (S05 R29): done, and still on a worker (beforeCall waits for those)
    report["reloaded"] = result.value("reloaded", Json::array());
    report["pending"] = result.value("pending", Json::array());
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
    // the file watcher's journal when it runs; the stat scan when it is off, degraded, behind
    // (an overflow) or its flush timed out (R29)
    heldAtCall = 0;
    const std::string source = takeJournal(found) ? "watcher" : "scan";
    if (source == "scan") {
        bool complete = true;
        scan(found, SCAN_BUDGET_MS, MIN_ITEMS, complete);
        lastScanChecked = scanChecked;
        lastComplete = complete;
    }
    lastSource = source;
    lastScanMs = msSince(started);
    lastScanCpuMs = threadCpuMs() - cpuStart;

    // One entry per path: this scan, what the last settle left, and (once Play stopped) what
    // Play deferred
    std::map<std::string, Pending> merged;
    for (auto& [rel, pending] : pendingExternal) merge(merged, rel, pending.kind, pending.report);
    pendingExternal.clear();
    // a changed model is picked up (its sub-clips reload) but is not news to report
    for (const Change& change : found) merge(merged, change.rel, change.kind, !isModelSourceFile(change.rel), change.sequence);

    const bool playing = ctx.isPlaying();
    if (playing) {
        if (merged.empty()) {
            run(nullptr);
            return;
        }
        // HTML UI files are not deferred: the documents (the running ones and the edit-time
        // ones) reload now. Images go to the documents now and to the scene textures after Play.
        std::vector<fs::path> uiPaths, imagePaths, scriptPaths;
        std::vector<std::string> scriptRels;
        const uint64_t scriptSerial = ScriptReloadService::get().lastSerial();
        w4::SyncOptions uiSync;
        bool otherDeferred = false;
        for (const auto& [rel, pending] : merged) {
            const fs::path path = (root / FileUtils::pathFromUtf8(rel)).lexically_normal();
            // a project UI effect's .frag (or one that is gone) and the includes effects may read
            // reload their documents now too (phase 5); so do clip and pose animation documents
            // (players see a clip's new pool generation; their problems are reported now)
            if (isUiCodeFile(rel) || (isAssetFile(rel) && !Util::isImageFile(rel)) || lowerExtension(rel) == ".glsl" ||
                Util::isGuidJsonAssetFile(rel) || Util::isPoseAnimationFile(rel) ||
                (lowerExtension(rel) == ".frag" && (pending.kind == 'r' || ProjectChecks::isUiEffectFile(path)))) {
                uiPaths.push_back(path);
                if (pending.sequence != 0) uiSync.sequences[path] = pending.sequence;
                continue;
            }
            if (Util::isImageFile(rel)) imagePaths.push_back(path);
            // a script reloaded during Play (S23): not deferred (Reload After Stopping Play,
            // C++ with its reload off: deferred as before)
            if (pending.kind != 'r' && Util::isScriptFile(rel) && ScriptReloadService::get().notifyChanged(path)) {
                scriptPaths.push_back(path);
                scriptRels.push_back(rel);
                continue;
            }
            merge(deferred, rel, pending.kind, pending.report);
            if (pending.report) otherDeferred = true;
        }
        Json uiProblems = Json::array(), uiReloaded = Json::array();
        int uiCurrent = 0;
        if (!uiPaths.empty()) {
            const Json synced = w4::syncFiles(ctx, uiPaths, false, uiSync);
            uiProblems = synced.value("problems", Json::array());
            uiReloaded = synced.value("ui_reloaded", Json::array());
            uiCurrent = synced.value("ui_current", 0);
        }
        // (the watcher's own reload of an image reloads the documents showing it)
        if (!imagePaths.empty() && source != "watcher") {
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
                if (ProjectChecks::readFile(root / FileUtils::pathFromUtf8(rel), text)) {
                    const ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(text, rel);
                    if (!syntax.ok) problems.push_back({{"path", rel}, {"line", syntax.line}, {"message", "Lua syntax error: " + syntax.message}});
                }
            }
        }
        if (changed.empty()) {
            run(nullptr);
            return;
        }
        // what the watcher's reloads found (before this call, or in its flush)
        const HotReloadService::WatcherUi watcherUi = HotReloadService::get().takeWatcherUi(started);
        for (const Json& item : watcherUi.reloaded) uiReloaded.push_back(item);
        uiCurrent += watcherUi.current;
        for (const Json& problem : HotReloadService::get().takeLateProblems()) problems.push_back(compactProblems(Json::array({problem}))[0]);
        Json report = {{"changed", changed}};
        if (!added.empty()) report["added"] = added;
        if (!removed.empty()) report["removed"] = removed;
        report["problems"] = uniqueProblems(problems);
        addUiReport(report, uiReloaded, uiCurrent);
        report["source"] = source;
        if (otherDeferred) {
            report["deferred"] = source == "watcher"
                ? "A scene is playing: script properties are re-parsed when play stops (textures, materials, shaders, models, audio "
                  "and HTML UI reload now)."
                : "A scene is playing: script properties, shaders, materials and textures are picked up when play stops"
                  " (HTML UI documents reloaded now).";
        }
        if (scriptPaths.empty()) {
            run(report);
            return;
        }
        // the scripts reload into the running game (S23 R45): the call waits for them (at most 5 s)
        call->waitUntil([scriptPaths]() { return ScriptReloadService::get().idleFor(scriptPaths); }, 5000,
                        [run, report, scriptSerial, scriptRels](bool) mutable {
            report["script_reload"] = ScriptReloadService::reportJson(ScriptReloadService::get().recordsSince(scriptSerial), scriptRels);
            run(report);
        });
        return;
    }

    for (auto& [rel, pending] : deferred) merge(merged, rel, pending.kind, pending.report);
    deferred.clear();
    if (merged.empty()) {
        // what a reload found after the call that reported its change had returned (an import
        // the watcher's own reload ran, a shader fork's compile): the next call says it
        Json problems = Json::array();
        for (const Json& problem : HotReloadService::get().takeLateProblems()) problems.push_back(compactProblems(Json::array({problem}))[0]);
        if (problems.empty()) {
            run(nullptr);
            return;
        }
        run(Json{{"changed", Json::array()}, {"problems", uniqueProblems(problems)}, {"source", source}});
        return;
    }

    std::vector<Change> changes;
    for (const auto& [rel, pending] : merged) changes.push_back({rel, pending.kind, 0, pending.report, pending.sequence});
    HotReloadService& reloads = HotReloadService::get();
    const uint64_t serial = reloads.lastSerial();
    Json report = process(ctx, changes);
    if (report.is_null()) {
        run(nullptr);
        return;
    }
    report["source"] = source;
    // the HTML UI documents the watcher reloaded: in this call's flush (reloaded for it), or
    // before (already current)
    const HotReloadService::WatcherUi watcherUi = reloads.takeWatcherUi(started);
    if (!watcherUi.reloaded.empty() || watcherUi.current > 0) {
        Json uiReloaded = report.value("ui_reloaded", Json::array());
        for (const Json& item : watcherUi.reloaded) uiReloaded.push_back(item);
        addUiReport(report, uiReloaded, report.value("ui_current", 0) + watcherUi.current);
    }

    // the reloads it started run on workers: the call waits for them (at most 10 s), as it
    // waits for the shader forks they compile
    auto finish = [run, report, serial](bool) mutable {
        HotReloadService& reloads = HotReloadService::get();
        Json stillPending = Json::array();
        for (const Json& path : report["pending"]) {
            HotReloadService::Record record;
            const bool done = reloads.lastRecordOf(path.get<std::string>(), record) && record.serial > serial;
            if (!done) stillPending.push_back(path);
            else if (record.ok) report["reloaded"].push_back({{"path", record.path}, {"kind", record.kind}, {"ms", std::round(record.ms)}});
        }
        report["pending"] = stillPending;
        // the failures (the old version kept) and the shader forks' compile errors
        for (const Json& problem : reloads.takeLateProblems()) report["problems"].push_back(compactProblems(Json::array({problem}))[0]);
        report["problems"] = uniqueProblems(report["problems"]);
        run(report);
    };
    std::vector<fs::path> pending;
    for (const Json& path : report["pending"]) pending.push_back((root / FileUtils::pathFromUtf8(path.get<std::string>())).lexically_normal());
    if (pending.empty()) {
        finish(false);
        return;
    }
    call->waitUntil([pending]() { return HotReloadService::get().idleFor(pending); }, 10000, finish);
}

bool McpAutoSync::takeJournal(std::vector<Change>& found) {
    FileWatchService& watcher = FileWatchService::get();
    if (!watcher.isAttached()) return false;
    const FileWatchService::State state = watcher.stats().state;
    if (state == FileWatchService::State::Off || state == FileWatchService::State::Degraded) return false;
    if (!watcher.flush(std::chrono::milliseconds(250))) return false;
    const std::vector<FileChange> journal = watcher.journalSince(journalCursor);
    std::vector<Change> taken;
    // Handled already: the baseline holds what the disk holds (sync_files picked it up before
    // the watcher delivered it, a writing call's settle stamped it, or it came and went)
    std::map<std::string, bool> handled;
    auto isHandled = [&](const std::string& key) {
        auto known = handled.find(key);
        if (known != handled.end()) return known->second;
        const FileMap& map = mapFor(key);
        auto it = map.find(key);
        FileStat st;
        bool same = false;
        if (!statPath(absolute(key), st) || st.isDir) same = it == map.end();
        else same = it != map.end() && it->second.mtime == st.mtime && it->second.size == st.size;
        handled[key] = same;
        return same;
    };
    for (const FileChange& change : journal) {
        // fell behind the ring: everything may have changed, the scan looks
        if (change.kind == FileChangeKind::Rescan && change.path == watcher.getRoot()) return false;
        // the editor's own writes are not external (the tools handled theirs), nor what a
        // writing call stamped while it ran (a tool writing past the atomic writer)
        if (change.kind == FileChangeKind::Rescan || change.directory || change.origin == ChangeOrigin::Editor) continue;
        if (change.kind != FileChangeKind::Removed && std::any_of(toolWindows.begin(), toolWindows.end(), [&](const auto& window) {
                return change.mtimeNs >= window.first && change.mtimeNs <= window.second;
            })) {
            continue;
        }
        auto add = [&](const fs::path& path, char kind) {
            const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
            if (rel.empty() || rel.is_absolute()) return;
            const std::string key = FileUtils::pathToGenericUtf8(rel);
            if (key == "." || key.rfind("..", 0) == 0 || insideSkippedDir(key) || !tracks(key) || isHandled(key)) return;
            taken.push_back({key, kind, change.mtimeNs, true, change.sequence});
        };
        switch (change.kind) {
            case FileChangeKind::Added: add(change.path, 'a'); break;
            case FileChangeKind::Modified: add(change.path, 'm'); break;
            case FileChangeKind::Removed: add(change.path, 'r'); break;
            case FileChangeKind::Moved:
                add(change.oldPath, 'r');
                add(change.path, 'a');
                break;
            case FileChangeKind::Rescan: break;
        }
    }
    // the scan's baseline follows, so its settle (and a later fallback) does not find them again
    for (const Change& change : taken) {
        restamp(change.rel);
        markHot(change.rel);
    }
    found.insert(found.end(), taken.begin(), taken.end());
    // Auto Refresh off: what waited in the journal for this call (editor_status pending)
    heldAtCall = HotReloadService::get().getAutoRefresh() ? 0 : taken.size();
    return true;
}

void McpAutoSync::afterCall(McpContext& ctx, const std::string& tool, std::chrono::system_clock::time_point start,
                            std::chrono::system_clock::time_point end) {
    if (writingCalls > 0) writingCalls--;
    // the journal's safety net: what a writing call stamped inside its run is its own
    toolWindows.emplace_back(toNs(start) - SETTLE_SLACK_NS, toNs(end) + SETTLE_SLACK_NS / 5);
    while (toolWindows.size() > TOOL_WINDOWS_MAX) toolWindows.pop_front();
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
    for (const fs::path& path : paths) {
        const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || rel.is_absolute()) continue;
        const std::string key = FileUtils::pathToGenericUtf8(rel);
        if (key == "." || key.rfind("..", 0) == 0) continue;
        restamp(key);
        pendingExternal.erase(key);
        if (playing) {
            // syncFiles leaves script properties for after Play: make sure they get re-parsed then
            if (Util::isScriptFile(key)) merge(deferred, key, 'm', false);
        } else {
            deferred.erase(key);
        }
    }
}

// ------------------------------------------------------------------ results

void McpAutoSync::attach(Json& result, const Json& report) {
    if (report.is_null() || !result.is_object()) return;
    if (!result.contains("structuredContent") || !result["structuredContent"].is_object()) {
        result["structuredContent"] = Json::object();
    }
    result["structuredContent"]["external_changes"] = report;

    const Json& changed = report["changed"];
    // (no change, only what a reload found after the call that reported it)
    std::string text = changed.empty() ? "Found since your last call by the reload of earlier external changes" :
                                         "External changes picked up (files changed on disk since your last call):";
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
    return {{"source", lastSource}, {"files", code.size() + assets.size()}, {"dirs", dirs.size()}, {"checked", lastScanChecked},
            {"complete", lastComplete}, {"scan_ms", round3(lastScanMs)}, {"settle_ms", round3(lastSettleMs)},
            {"scan_cpu_ms", round3(lastScanCpuMs)}, {"settle_cpu_ms", round3(lastSettleCpuMs)}};
}
