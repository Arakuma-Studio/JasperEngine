// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsWatcher.h"

#include "App.h"
#include "Out.h"
#include "Project.h"
#include "texsynth/SynthImage.h"
#include "texsynth/SynthesisSession.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <thread>
#include <iterator>
#include <set>
#include <stdexcept>
#include "util/FileUtils.h"

namespace fs = std::filesystem;
namespace sn = jasperengine::editor::spritenormals;
using namespace jasperengine::editor;

namespace {

    SpriteNormalsWatcher* activeWatcher = nullptr;

    bool stamp(const fs::path& path, fs::file_time_type& time, uintmax_t& size) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return false;
        time = fs::last_write_time(path, ec);
        if (ec) return false;
        size = fs::file_size(path, ec);
        return !ec;
    }

    std::string fileName(const std::string& path) {
        const std::string name = jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(path).filename());
        return name.empty() ? path : name;
    }

}

SpriteNormalsWatcher::SpriteNormalsWatcher(Project* project): project(project) {
}

SpriteNormalsWatcher::~SpriteNormalsWatcher() {
    if (regen) regen->cancel();
    if (scan) scan->cancel = true;
    if (activeWatcher == this) activeWatcher = nullptr;
}

SpriteNormalsWatcher* SpriteNormalsWatcher::active() {
    return activeWatcher;
}

void SpriteNormalsWatcher::setActive(SpriteNormalsWatcher* watcher) {
    activeWatcher = watcher;
}

double SpriteNormalsWatcher::clock() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool SpriteNormalsWatcher::autoRegenerate() const {
    return project->getSpriteNormalsSettings().autoRegenerate;
}

void SpriteNormalsWatcher::reset() {
    if (regen) regen->cancel();
    regen.reset();
    regenOutput.clear();
    queue.clear();
    // a scan of the old project stops at its next file, never waited for; its result is dropped
    if (scan) scan->cancel = true;
    scan.reset();
    idleScans = 0;
    wakePending = false;
    noticed.clear();
    outputs.clear();
    hashes.clear();
    scannedProject.clear();
    lastScan = -1.0;
    rev++;
}

void SpriteNormalsWatcher::invalidate(const fs::path& path) {
    const fs::path key = path.lexically_normal();
    auto it = outputs.find(key);
    if (it != outputs.end()) {
        // read again by the next scan (the stamp never matches)
        it->second.size = static_cast<uintmax_t>(-1);
    }
    hashes.erase(key);
    idleScans = 0;
    // a scan in flight may have read the file before the change: its result must not make the
    // watcher dormant (collectScan)
    if (scan) wakePending = true;
    // soon, not at once: a write of several files lands first
    if (lastScan >= 0.0) lastScan = std::min(lastScan, clock() - scanInterval + 0.25);
}

void SpriteNormalsWatcher::startScan() {
    const fs::path root = project->getProjectPath();
    if (root.empty()) return;
    lastScan = clock();
    std::map<fs::path, Output> previous = outputs;
    std::map<fs::path, Hash> knownHashes = hashes;
    std::shared_ptr<std::atomic<int>> counter = hashCount;
    std::shared_ptr<ScanJob> job = std::make_shared<ScanJob>();
    scan = job;
    const bool failScan = failNextScan;
    failNextScan = false;
    // off the main thread: reading recipes and hashing sources can take a while on a big project
    auto scanOnce = [job, root, counter, failScan](const std::map<fs::path, Output>& previous, const std::map<fs::path, Hash>& knownHashes) {
        if (failScan) throw std::runtime_error("a failure injected by a test");
        ScanResult out;
        // the project's PNGs (ProjectChecks' walk: hidden and build directories skipped), stopping
        // at once when cancelled (a project switch, the editor closing)
        std::vector<fs::path> pngs;
        std::error_code walkError;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, walkError), end;
             it != end && !walkError && !job->cancel; it.increment(walkError)) {
            const fs::path& path = it->path();
            std::error_code ec;
            if (it->is_directory(ec)) {
                if (ProjectChecks::isSkippedDir(path)) it.disable_recursion_pending();
                continue;
            }
            const std::string name = jasperengine::editor::FileUtils::pathToUtf8(path.filename());
            if (name.empty() || name[0] == '.' || !it->is_regular_file(ec)) continue;
            std::string ext = jasperengine::editor::FileUtils::pathToUtf8(path.extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".png") pngs.push_back(path);
        }
        for (const fs::path& file : pngs) {
            if (job->cancel) break;
            const fs::path key = file.lexically_normal();
            Output entry;
            if (!stamp(key, entry.time, entry.size)) continue;
            auto it = previous.find(key);
            if (it != previous.end() && it->second.time == entry.time && it->second.size == entry.size) {
                out.outputs[key] = it->second;
                continue;
            }
            std::string error;
            const std::optional<nlohmann::json> recipe = sn::readRecipeHead(key, &error);
            if (recipe && sn::validateRecipe(*recipe, error)) {
                const nlohmann::json& source = (*recipe)["source"];
                entry.role = recipe->contains("role") ? (*recipe)["role"].get<std::string>() : std::string("normal");
                entry.sourcePath = source["path"].get<std::string>();
                const fs::path rel = fs::u8path(entry.sourcePath);
                entry.sourceAbsolute = rel.is_absolute() ? rel.lexically_normal() : (root / rel).lexically_normal();
                entry.recipeSha1 = source.contains("sha1") ? source["sha1"].get<std::string>() : std::string();
            }
            // a plain PNG is kept too (role empty), so it is not read again while unchanged
            out.outputs[key] = entry;
        }
        // the sources: hashed once per mtime and size
        for (auto& [key, entry] : out.outputs) {
            if (job->cancel) break;
            if (entry.role.empty()) continue;
            Hash h;
            if (!stamp(entry.sourceAbsolute, h.time, h.size)) {
                entry.missing = true;
                entry.stale = false;
                continue;
            }
            entry.missing = false;
            auto known = out.hashes.find(entry.sourceAbsolute);
            if (known == out.hashes.end()) {
                auto previousHash = knownHashes.find(entry.sourceAbsolute);
                if (previousHash != knownHashes.end() && previousHash->second.time == h.time && previousHash->second.size == h.size) {
                    h.sha1 = previousHash->second.sha1;
                } else if (h.size > static_cast<uintmax_t>(sn::kMaxInputBytes)) {
                    // over the generator's input limit: no map was made from these bytes (the
                    // source changed), and they are not read into memory
                    h.sha1 = "(over the input limit)";
                } else {
                    // read in chunks, the cancel flag checked between them
                    std::ifstream in(entry.sourceAbsolute, std::ios::binary);
                    std::vector<uint8_t> bytes;
                    std::vector<char> chunk(1 << 20);
                    while (in && !job->cancel) {
                        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + in.gcount());
                    }
                    if (job->cancel) break;
                    h.sha1 = texsynth::sha1Hex(bytes.data(), bytes.size());
                    (*counter)++;
                }
                known = out.hashes.emplace(entry.sourceAbsolute, h).first;
            }
            entry.stale = known->second.sha1 != entry.recipeSha1;
        }
        return out;
    };
    // the thread is detached: nothing may escape it (an exception would end the editor). A failed
    // scan keeps the index as it was; the next one tries again
    auto work = [job, scanOnce, previous = std::move(previous), knownHashes = std::move(knownHashes)]() {
        ScanResult out;
        try {
            out = scanOnce(previous, knownHashes);
        } catch (...) {
            out = ScanResult();
            try {
                out.outputs = previous;
                out.hashes = knownHashes;
            } catch (...) {
                out = ScanResult();
            }
        }
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->result = std::move(out);
            job->done = true;
        }
        job->finished.notify_all();
    };
    try {
        std::thread(std::move(work)).detach();
    } catch (const std::system_error&) {
        // no thread: no scan this time, another later
        scan.reset();
    }
}

void SpriteNormalsWatcher::shutdown(double timeoutSeconds) {
    if (regen) regen->cancel();
    std::shared_ptr<ScanJob> job = scan;
    reset();
    // the scan stops at its next file or chunk: waited for a moment, so no thread outlives the editor
    if (job) {
        std::unique_lock<std::mutex> lock(job->mutex);
        job->finished.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), [&job]() { return job->done.load(); });
    }
}

double SpriteNormalsWatcher::interval() const {
    // a project whose last walk found no generated map is not walked again until something says a
    // map may have appeared (invalidate: this tool's writes, the MCP file sync, a generated PNG the
    // Project window shows), except for a slow fallback (files copied in by hand, a VCS update)
    return idleScans > 0 ? dormantInterval : scanInterval;
}

void SpriteNormalsWatcher::collectScan() {
    if (!scan || !scan->done) return;
    ScanResult result;
    {
        std::lock_guard<std::mutex> lock(scan->mutex);
        result = std::move(scan->result);
    }
    scan.reset();
    // the project changed meanwhile: dropped
    if (project->getProjectPath() != scannedProject) return;
    bool changed = result.outputs.size() != outputs.size();
    for (const auto& [key, entry] : result.outputs) {
        auto it = outputs.find(key);
        if (it == outputs.end() || it->second.stale != entry.stale || it->second.missing != entry.missing || it->second.role != entry.role) {
            changed = true;
            break;
        }
    }
    outputs = std::move(result.outputs);
    hashes = std::move(result.hashes);
    bool anyGenerated = false;
    for (const auto& entry : outputs) {
        if (entry.second.role.empty()) continue;
        anyGenerated = true;
        // indexed: a later notice of this path (deleted, then restored from outside) wakes again
        noticed.erase(entry.first);
    }
    if (wakePending) {
        // a file changed while this scan ran (it may have been read before the change): not
        // dormant, and walked again soon
        wakePending = false;
        idleScans = 0;
        lastScan = std::min(lastScan, clock() - scanInterval + 0.25);
    } else {
        idleScans = anyGenerated ? 0 : idleScans + 1;
    }
    if (changed) rev++;
    if (autoRegenerate()) queueStale();
}

void SpriteNormalsWatcher::queueStale() {
    // a set is regenerated from its normal map's recipe (it rewrites the atlas and height too)
    const auto now = fs::file_time_type::clock::now();
    for (const auto& [key, entry] : outputs) {
        if (entry.role != "normal" || !entry.stale || entry.missing) continue;
        if (key == regenOutput) continue;
        if (std::find(queue.begin(), queue.end(), key) != queue.end()) continue;
        // an editor writing in several steps: wait until the source has rested a moment
        auto h = hashes.find(entry.sourceAbsolute);
        if (h != hashes.end() && now - h->second.time < std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::duration<double>(settleSeconds))) {
            if (lastScan >= 0.0) lastScan = std::min(lastScan, clock() - scanInterval + settleSeconds);
            continue;
        }
        queue.push_back(key);
    }
}

void SpriteNormalsWatcher::tickRegeneration() {
    const fs::path root = project->getProjectPath();
    if (regen) {
        if (!regen->finished()) return;
        std::shared_ptr<sn::SpriteNormalsRun> done = regen;
        const fs::path output = regenOutput;
        regen.reset();
        regenOutput.clear();
        const sn::Result& result = done->result();
        const sn::Prepared& prepared = done->prepared();
        const std::string name = jasperengine::editor::FileUtils::pathToUtf8(output.filename());
        if (result.status == sn::Status::Cancelled) return;
        if (done->generation() != texsynth::projectGeneration()) return;
        if (result.status != sn::Status::Ok) {
            lastLog = "Could not regenerate " + name + ": " + (result.errors.empty() ? std::string("the generation failed") : result.errors.front().message);
            Out::warning("%s", lastLog.c_str());
            return;
        }
        std::vector<sn::OutputFile> files;
        std::string error, failed;
        sn::Problem escape;
        // the plain all-or-nothing writer: not an undo step, like an FBX reimport; the files checked
        // again right before (a symbolic link made since the check must not lead a write out of the
        // assets directory)
        bool ok = sn::encodeOutputs(prepared, result, files, error);
        if (ok && !sn::outputsContained(project, files, escape)) {
            error = escape.message;
            ok = false;
        }
        // ... and written beneath the assets directory: a link planted on the way since is never followed
        if (ok) ok = sn::writeOutputsDirect(files, failed, error, sn::outputsRoot(project));
        if (!ok) {
            lastLog = "Could not regenerate " + name + ": " + error;
            Out::warning("%s", lastLog.c_str());
            return;
        }
        std::vector<fs::path> changed;
        for (const sn::OutputFile& f : files) {
            changed.push_back(f.absolute);
            invalidate(f.absolute);
        }
        // the editor notices (textures reload, the asset database, the Project window's thumbnails
        // and listing; with the MCP server running, its file sync)
        ProjectFileWrites::filesWritten(project, changed);
        lastLog = "Regenerated " + sn::projectRelative(root, output) + " (" + fileName(prepared.sourcePath) + " changed)";
        Out::info("Regenerated %s (%s changed)", sn::projectRelative(root, output).c_str(), fileName(prepared.sourcePath).c_str());
        return;
    }
    while (!queue.empty()) {
        const fs::path output = queue.front();
        queue.pop_front();
        auto it = outputs.find(output);
        if (it == outputs.end() || !it->second.stale || it->second.missing) continue;
        const std::string name = jasperengine::editor::FileUtils::pathToUtf8(output.filename());
        std::string error;
        const std::optional<nlohmann::json> recipe = sn::readRecipeHead(output, &error);
        sn::Spec spec;
        std::vector<std::string> notes;
        sn::Prepared prepared;
        std::vector<sn::Problem> problems;
        if (!recipe || !sn::specFromRecipe(*recipe, output, root, spec, notes, error)) {
            lastLog = "Could not regenerate " + name + ": " + (error.empty() ? std::string("no recipe") : error);
            Out::warning("%s", lastLog.c_str());
            continue;
        }
        spec.overwrite = true;
        spec.output.framesJson = false;
        if (!sn::prepare(root, spec, nullptr, prepared, problems)) {
            lastLog = "Could not regenerate " + name + ": " + (problems.empty() ? std::string("the recipe can't be used") : problems.front().message);
            Out::warning("%s", lastLog.c_str());
            // not again until it changes
            it->second.stale = false;
            continue;
        }
        regenOutput = output;
        regen = sn::SpriteNormalsRun::start(std::move(prepared));
        return;
    }
}

void SpriteNormalsWatcher::tick() {
    const fs::path root = project->getProjectPath();
    if (root != scannedProject) {
        reset();
        scannedProject = root;
    }
    if (root.empty()) return;
    // a playing scene: no scans, no writes (they would land under the play snapshot's textures)
    pausedForPlay = project->isAnyScenePlaying();
    if (pausedForPlay) return;
    collectScan();
    if (!scan && (lastScan < 0.0 || clock() - lastScan >= interval())) startScan();
    if (autoRegenerate()) {
        tickRegeneration();
    } else if (!queue.empty()) {
        queue.clear();
    }
}

bool SpriteNormalsWatcher::isStale(const fs::path& output) const {
    auto it = outputs.find(output.lexically_normal());
    return it != outputs.end() && !it->second.role.empty() && it->second.stale;
}

bool SpriteNormalsWatcher::sourceMissing(const fs::path& output) const {
    auto it = outputs.find(output.lexically_normal());
    return it != outputs.end() && !it->second.role.empty() && it->second.missing;
}

bool SpriteNormalsWatcher::wouldIndex(const fs::path& path) const {
    const fs::path root = project->getProjectPath().lexically_normal();
    if (root.empty()) return false;
    const fs::path key = path.lexically_normal();
    const fs::path relative = key.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") return false;
    const std::string name = jasperengine::editor::FileUtils::pathToUtf8(key.filename());
    std::string ext = jasperengine::editor::FileUtils::pathToUtf8(key.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.empty() || name[0] == '.' || ext != ".png") return false;
    return !ProjectChecks::isInSkippedDir(relative);
}

void SpriteNormalsWatcher::noticeGenerated(const fs::path& path) {
    if (!dormant()) return;
    const fs::path key = path.lexically_normal();
    std::string source, role;
    if (generatedInfo(key, source, role) || !wouldIndex(key)) return;
    if (!noticed.insert(key).second) return;
    invalidate(key);
}

std::vector<fs::path> SpriteNormalsWatcher::outputsOf(const fs::path& source) const {
    std::vector<fs::path> out;
    const fs::path key = source.lexically_normal();
    for (const auto& [path, entry] : outputs) {
        if (!entry.role.empty() && entry.sourceAbsolute == key) out.push_back(path);
    }
    return out;
}

bool SpriteNormalsWatcher::generatedInfo(const fs::path& output, std::string& source, std::string& role) const {
    auto it = outputs.find(output.lexically_normal());
    if (it == outputs.end() || it->second.role.empty()) return false;
    source = it->second.sourcePath;
    role = it->second.role;
    return true;
}
