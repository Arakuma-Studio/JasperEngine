// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsWatcher.h"

#include "Backend.h"
#include "App.h"
#include "Out.h"
#include "Project.h"
#include "mcp/McpContext.h"
#include "mcp/McpServer.h"
#include "mcp/tools/W4Shared.h"
#include "texsynth/SynthImage.h"
#include "texsynth/SynthesisSession.h"
#include "util/ProjectChecks.h"
#include "window/ProjectWindow.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <set>

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
        const std::string name = fs::u8path(path).filename().u8string();
        return name.empty() ? path : name;
    }

}

SpriteNormalsWatcher::SpriteNormalsWatcher(Project* project): project(project) {
}

SpriteNormalsWatcher::~SpriteNormalsWatcher() {
    if (regen) regen->cancel();
    if (scan.valid()) scan.wait();
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
    // a scan of the old project finishes on its own; its result is dropped
    if (scan.valid()) scan.wait();
    scan = std::future<ScanResult>();
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
    // off the main thread: reading recipes and hashing sources can take a while on a big project
    scan = std::async(std::launch::async, [root, previous = std::move(previous), knownHashes = std::move(knownHashes), counter]() {
        ScanResult out;
        for (const fs::path& file : ProjectChecks::findProjectFiles(root, {".png"})) {
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
                } else {
                    std::ifstream in(entry.sourceAbsolute, std::ios::binary);
                    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    h.sha1 = texsynth::sha1Hex(bytes.data(), bytes.size());
                    (*counter)++;
                }
                known = out.hashes.emplace(entry.sourceAbsolute, h).first;
            }
            entry.stale = known->second.sha1 != entry.recipeSha1;
        }
        return out;
    });
}

void SpriteNormalsWatcher::collectScan() {
    if (!scan.valid() || scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    ScanResult result = scan.get();
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
        const std::string name = output.filename().u8string();
        if (result.status == sn::Status::Cancelled) return;
        if (done->generation() != texsynth::projectGeneration()) return;
        if (result.status != sn::Status::Ok) {
            lastLog = "Could not regenerate " + name + ": " + (result.errors.empty() ? std::string("the generation failed") : result.errors.front().message);
            Out::warning("%s", lastLog.c_str());
            return;
        }
        std::vector<sn::OutputFile> files;
        std::string error, failed;
        // the plain all-or-nothing writer: not an undo step, like an FBX reimport
        if (!sn::encodeOutputs(prepared, result, files, error) || !sn::writeOutputsDirect(files, failed, error)) {
            lastLog = "Could not regenerate " + name + ": " + error;
            Out::warning("%s", lastLog.c_str());
            return;
        }
        std::vector<fs::path> changed;
        for (const sn::OutputFile& f : files) {
            changed.push_back(f.absolute);
            invalidate(f.absolute);
        }
        // the editor notices (textures reload, the Project window's thumbnails and listing)
        mcp::McpServer& server = mcp::McpServer::get();
        if (server.isStarted() && server.context().project() == project) mcp::w4::syncFiles(server.context(), changed, false);
        if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformName) {
            if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
                for (const fs::path& path : changed) projectWindow->requestThumbnailGeneration(path, true);
                projectWindow->refreshCurrentDirectory();
            }
        }
        lastLog = "Regenerated " + sn::projectRelative(root, output) + " (" + fileName(prepared.sourcePath) + " changed)";
        Out::info("Regenerated %s (%s changed)", sn::projectRelative(root, output).c_str(), fileName(prepared.sourcePath).c_str());
        return;
    }
    while (!queue.empty()) {
        const fs::path output = queue.front();
        queue.pop_front();
        auto it = outputs.find(output);
        if (it == outputs.end() || !it->second.stale || it->second.missing) continue;
        const std::string name = output.filename().u8string();
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
    if (!scan.valid() && (lastScan < 0.0 || clock() - lastScan >= scanInterval)) startScan();
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
