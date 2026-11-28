// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .vcolors files from the editor's side (docs/specs/V1-vertex-colors.md R8, R9, R131): the live
// values of each file, the new file's place and GUID, the background writer (latest bytes win per
// file, as the terrain maps' writer), and the save with the scene, which refreshes the editor-only
// snapshot and writes every file that is not on disk as it is.

#include "VertexColorEditorInternal.h"
#include "VertexColorProject.h"

#include "Out.h"
#include "Project.h"
#include "component/VertexColorsComponent.h"
#include "io/AtomicFile.h"
#include "io/watch/FileWatchService.h"
#include "jobs/JobSystem.h"
#include "subsystem/VertexColorScene.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "vertexcolor/VertexColorFile.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
namespace vcd = jasperengine::editor::vertexcolor::detail;

namespace {

    // one file's live values (main thread)
    struct VcLiveFile {
        std::shared_ptr<jasperengine::editor::vertexcolor::vc::Data> data;
        uint64_t version = 1;          // grows with every write to `data`
        bool hold = false;             // R10: written by the scene's save only
    };

    // by absolute file (generic form): two projects never share an entry
    std::map<std::string, VcLiveFile>& vcLiveFiles() {
        static std::map<std::string, VcLiveFile> files;
        return files;
    }

    std::string vcFileKey(Project& project, const std::string& assetPath) {
        return vcd::absoluteAssetPath(project, assetPath).generic_string();
    }

    // The background writer: the latest values win per file, written on the io-write channel; a
    // failed write is kept and tried again at the next flush (the terrain maps' writer, for these)
    class VcFileWriter {
    public:
        struct Job {
            std::shared_ptr<const jasperengine::vertexcolor::Data> data;
            uint64_t version = 0;
            fs::path root;
        };

        // never destroyed: its drain task belongs to the job system, gone at exit before statics are.
        // What is queued is written when the project closes (releaseVertexColorFiles flushes)
        static VcFileWriter& get() {
            static VcFileWriter* writer = new VcFileWriter();
            return *writer;
        }

        void enqueue(const std::string& file, std::shared_ptr<const jasperengine::vertexcolor::Data> data, uint64_t version, fs::path root) {
            std::unique_lock<std::mutex> lock(mutex);
            failed.erase(file);
            Job& job = jobs[file];
            job.data = std::move(data);
            job.version = version;
            job.root = std::move(root);
            if (!running) {
                running = true;
                jobs::SubmitOptions options;
                options.name = "vertex-colors-write";
                drainTask = jobs::system().ioWriteChannel().submit([this]() {
                    std::unique_lock<std::mutex> inner(mutex);
                    drainLocked(inner);
                    running = false;
                    condition.notify_all();
                }, options);
            }
        }

        // writes what is queued (and what failed before) on this thread, after a drain that writes already
        bool flushAll() {
            std::unique_lock<std::mutex> lock(mutex);
            for (auto& [file, job] : failed) {
                if (jobs.find(file) == jobs.end()) jobs[file] = std::move(job);
            }
            failed.clear();
            while (!drainLocked(lock)) condition.wait(lock, [this]() { return !draining; });
            return failed.empty();
        }

        // a write made outside the queue (a new file, written at once)
        void noteWritten(const std::string& file, uint64_t version) {
            std::unique_lock<std::mutex> lock(mutex);
            written[file] = std::max(written[file], version);
        }

        uint64_t writtenVersion(const std::string& file) {
            std::unique_lock<std::mutex> lock(mutex);
            const auto it = written.find(file);
            return it == written.end() ? 0 : it->second;
        }

        void forget() {
            std::unique_lock<std::mutex> lock(mutex);
            while (draining) condition.wait(lock, [this]() { return !draining; });
            jobs.clear();
            failed.clear();
            written.clear();
        }

        static bool write(const std::string& file, const Job& job, std::string& error);

    private:
        VcFileWriter() = default;

        bool drainLocked(std::unique_lock<std::mutex>& lock) {
            if (draining) return false;
            draining = true;
            while (!jobs.empty()) {
                auto it = jobs.begin();
                const std::string file = it->first;
                Job job = std::move(it->second);
                jobs.erase(it);
                lock.unlock();
                std::string error;
                const bool ok = write(file, job, error);
                if (!ok) Out::error("Vertex colors not written to %s: %s. The paint is kept in memory and written at the next save.", file.c_str(), error.c_str());
                lock.lock();
                if (ok) {
                    written[file] = std::max(written[file], job.version);
                } else if (jobs.find(file) == jobs.end()) {
                    failed[file] = std::move(job);
                }
            }
            draining = false;
            condition.notify_all();
            return true;
        }

        std::mutex mutex;
        std::condition_variable condition;
        std::map<std::string, Job> jobs;
        std::map<std::string, Job> failed;
        std::map<std::string, uint64_t> written;
        bool draining = false;
        bool running = false;
        jobs::Task<void> drainTask;
    };

    bool VcFileWriter::write(const std::string& file, const Job& job, std::string& error) {
        const std::vector<uint8_t> bytes = jasperengine::vertexcolor::encode(*job.data, true);
        const fs::path path = FileUtils::pathFromUtf8(file);
        if (!job.root.empty()) {
            return safewrite::writeFileResolvedBeneath(job.root, path, std::string(bytes.begin(), bytes.end()), error);
        }
        // the watcher's report of this write is the write arriving, not somebody's change (S05 R10)
        ScopedExpectedWrite expected(path);
        io::AtomicWriteOptions options;
        options.skipIfUnchanged = true;
        const io::WriteResult result = io::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
        if (!result.ok()) {
            error = io::describeWriteError(result);
            return false;
        }
        return true;
    }

    fs::path vcWriteRoot() {
        const std::vector<fs::path> roots = safewrite::Scope::currentRoots();
        return roots.empty() ? fs::path() : roots.front();
    }

    void vcQueue(const std::string& key, VcLiveFile& file) {
        if (file.hold || !file.data) return;
        VcFileWriter::get().enqueue(key, std::make_shared<const jasperengine::vertexcolor::Data>(*file.data), file.version, vcWriteRoot());
    }

    std::string vcSafeStem(const std::string& name, const char* fallback) {
        std::string stem;
        for (const char c : name) stem += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
        while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
        return stem.empty() ? std::string(fallback) : stem;
    }

}

// ---- detail

SceneProject* vcd::sceneProjectOf(Project* project, uint32_t sceneId) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    return sceneProject && sceneProject->scene ? sceneProject : nullptr;
}

Scene* vcd::sceneOf(Project* project, uint32_t sceneId) {
    SceneProject* sceneProject = sceneProjectOf(project, sceneId);
    return sceneProject ? sceneProject->scene : nullptr;
}

VertexColorsComponent* vcd::componentOf(Project* project, uint32_t sceneId, Entity entity) {
    Scene* scene = sceneOf(project, sceneId);
    return scene && entity != NULL_ENTITY ? scene->findComponent<VertexColorsComponent>(entity) : nullptr;
}

fs::path vcd::absoluteAssetPath(Project& project, const std::string& assetPath) {
    return (project.getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal();
}

std::shared_ptr<jasperengine::editor::vertexcolor::vc::Data> vcd::resolve(Project* project, uint32_t sceneId, Entity entity, std::string* error) {
    Scene* scene = sceneOf(project, sceneId);
    VertexColorsComponent* component = componentOf(project, sceneId, entity);
    if (!component || component->data.path.empty()) return nullptr;
    const std::string key = vcFileKey(*project, component->data.path);
    auto& files = vcLiveFiles();
    auto found = files.find(key);
    if (component->loaded && component->loadedPath == component->data.path) {
        if (found == files.end()) {
            files[key].data = component->loaded;
        } else if (found->second.data != component->loaded) {
            // the table's are the unsaved values of the file
            component->loaded = found->second.data;
            component->revision++;
            jasperengine::vertexcolor::markVertexColorsChanged(scene, entity);
        }
        return component->loaded;
    }
    std::shared_ptr<jasperengine::vertexcolor::Data> data;
    if (found != files.end() && found->second.data) {
        data = found->second.data;
    } else {
        auto read = std::make_shared<jasperengine::vertexcolor::Data>();
        std::string why;
        if (!jasperengine::vertexcolor::readFile(absoluteAssetPath(*project, component->data.path), *read, why)) {
            if (error) *error = "'" + component->data.path + "' did not load (" + why + "), so it stays as it is. Fix or replace it, or clear the Data field.";
            return nullptr;
        }
        data = std::move(read);
        files[key].data = data;
    }
    component->loaded = data;
    component->loadedPath = component->data.path;
    component->revision++;
    jasperengine::vertexcolor::markVertexColorsChanged(scene, entity);
    return data;
}

void vcd::touched(Project* project, uint32_t sceneId, Entity entity, bool queueWrite) {
    VertexColorsComponent* component = componentOf(project, sceneId, entity);
    if (!component || !component->loaded) return;
    const std::shared_ptr<jasperengine::vertexcolor::Data> data = component->loaded;
    // every GameObject showing these values draws them again (R14)
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        auto array = sceneProject->scene->getComponentArray<VertexColorsComponent>();
        for (size_t i = 0; i < array->size(); i++) {
            VertexColorsComponent& other = array->getComponentFromIndex(i);
            if (other.loaded != data) continue;
            other.revision++;
            jasperengine::vertexcolor::markVertexColorsChanged(sceneProject->scene, array->getEntity(i));
            sceneProject->needUpdateRender = true;
        }
    }
    if (component->data.path.empty()) return;
    const std::string key = vcFileKey(*project, component->data.path);
    VcLiveFile& file = vcLiveFiles()[key];
    file.data = data;
    file.version++;
    if (queueWrite) vcQueue(key, file);
}

void vcd::registerData(Project& project, const std::string& assetPath, std::shared_ptr<jasperengine::vertexcolor::Data> data, bool dirty) {
    const std::string key = vcFileKey(project, assetPath);
    VcLiveFile& file = vcLiveFiles()[key];
    file.data = std::move(data);
    file.version++;
    if (!dirty) {
        VcFileWriter::get().noteWritten(key, file.version);
    }
}

void vcd::holdUntilSave(Project& project, const std::string& assetPath) {
    VcLiveFile& file = vcLiveFiles()[vcFileKey(project, assetPath)];
    file.hold = true;
    file.version++;
}

bool vcd::isDirty(Project& project, const std::string& assetPath) {
    const std::string key = vcFileKey(project, assetPath);
    const auto it = vcLiveFiles().find(key);
    if (it == vcLiveFiles().end()) return false;
    return it->second.hold || VcFileWriter::get().writtenVersion(key) < it->second.version;
}

bool vcd::writeNewFile(Project& project, const std::string& assetPath, const jasperengine::vertexcolor::Data& data, AssetRef& ref, std::string* error) {
    const fs::path file = absoluteAssetPath(project, assetPath);
    std::error_code ignored;
    fs::create_directories(file.parent_path(), ignored);
    VcFileWriter::Job job;
    job.data = std::make_shared<const jasperengine::vertexcolor::Data>(data);
    job.root = vcWriteRoot();
    std::string why;
    if (!VcFileWriter::write(file.generic_string(), job, why)) {
        if (error) *error = "Could not write '" + assetPath + "': " + why;
        return false;
    }
    AssetDatabase& database = project.getAssetDatabase();
    Guid guid = database.getAssetGuid(file);
    if (guid.isNull()) {
        project.onAssetFilesChanged(database.importAssets(file));
        guid = database.getAssetGuid(file);
    }
    ref.set(assetPath, guid);
    return true;
}

std::string vcd::newFilePath(Project& project, const SceneProject& sceneProject, const std::string& entityName) {
    fs::path folder = "VertexColors";
    if (!sceneProject.filepath.empty()) {
        const fs::path scene = sceneProject.filepath.is_absolute() ? sceneProject.filepath : project.getProjectPath() / sceneProject.filepath;
        const fs::path relative = scene.parent_path().lexically_normal().lexically_relative(project.getAssetsPath().lexically_normal());
        if (!relative.empty() && *relative.begin() != "..") {
            folder = (relative / FileUtils::pathFromUtf8(vcSafeStem(FileUtils::pathToUtf8(scene.stem()), "Scene") + "_VertexColors")).lexically_normal();
        }
    }
    const std::string stem = vcSafeStem(entityName, "GameObject");
    for (int attempt = 0;; attempt++) {
        const std::string name = attempt == 0 ? stem + jasperengine::vertexcolor::VCOLORS_EXTENSION : stem + " " + std::to_string(attempt) + jasperengine::vertexcolor::VCOLORS_EXTENSION;
        const fs::path candidate = (folder / FileUtils::pathFromUtf8(name)).lexically_normal();
        std::error_code ignored;
        if (fs::exists(project.getAssetsPath() / candidate, ignored)) continue;
        // a file made this session and not written yet is taken too
        if (vcLiveFiles().count(absoluteAssetPath(project, candidate.generic_string()).generic_string())) continue;
        return candidate.generic_string();
    }
}

bool vcd::flushWrites() {
    return VcFileWriter::get().flushAll();
}

void vcd::fillSnapshot(jasperengine::vertexcolor::TargetData& target, const jasperengine::vertexcolor::MeshView& mesh) {
    // a stale target keeps the snapshot of the mesh it was painted on: its transfer needs it (R10)
    if (target.fingerprint != mesh.fingerprint()) return;
    target.snapshotPositions = mesh.positions;
    target.snapshotIndices = mesh.indices;
    if (mesh.normals.size() == mesh.positions.size()) {
        target.snapshotNormals = mesh.normals;
        return;
    }
    target.snapshotNormals.resize(mesh.positions.size());
    for (size_t v = 0; v < mesh.positions.size(); v++) {
        const uint32_t logical = v < mesh.logical.size() ? mesh.logical[v] : 0;
        target.snapshotNormals[v] = logical < mesh.logicalNormals.size() ? mesh.logicalNormals[logical] : Vector3(0.0f, 1.0f, 0.0f);
    }
}

void vcd::refreshSnapshots(Project* project, uint32_t sceneId, Entity entity, jasperengine::vertexcolor::Data& data) {
    Scene* scene = sceneOf(project, sceneId);
    if (!scene) return;
    for (jasperengine::vertexcolor::TargetData& target : data.targets) {
        const std::shared_ptr<const jasperengine::vertexcolor::MeshView> mesh = jasperengine::vertexcolor::sceneMeshView(scene, entity, target.key);
        if (mesh) fillSnapshot(target, *mesh);
    }
}

void vcd::resetFiles() {
    VcFileWriter::get().flushAll();
    VcFileWriter::get().forget();
    vcLiveFiles().clear();
}

// ---- the scene's save (VertexColorProject.h)

bool editor::saveSceneVertexColors(Project& project, SceneProject& sceneProject) {
    Scene* scene = sceneProject.scene;
    if (!scene) return true;
    auto array = scene->getComponentArray<VertexColorsComponent>();
    std::vector<std::pair<std::string, std::string>> written;   // key, GameObject name
    for (size_t i = 0; i < array->size(); i++) {
        VertexColorsComponent& component = array->getComponentFromIndex(i);
        if (!component.loaded || component.data.path.empty() || component.loadedPath != component.data.path) continue;
        const std::string key = vcFileKey(project, component.data.path);
        auto it = vcLiveFiles().find(key);
        if (it == vcLiveFiles().end() || it->second.data != component.loaded) continue;
        VcLiveFile& file = it->second;
        const bool dirty = file.hold || VcFileWriter::get().writtenVersion(key) < file.version;
        if (!dirty) continue;
        // the snapshot is the mesh as it is at this save (R9): what a reimport transfers from
        vcd::refreshSnapshots(&project, sceneProject.id, array->getEntity(i), *file.data);
        file.hold = false;
        file.version++;
        vcQueue(key, file);
        written.push_back({key, scene->getEntityName(array->getEntity(i))});
    }
    const bool flushed = VcFileWriter::get().flushAll();
    bool ok = true;
    for (const auto& [key, name] : written) {
        const auto it = vcLiveFiles().find(key);
        if (it != vcLiveFiles().end() && VcFileWriter::get().writtenVersion(key) < it->second.version) {
            ok = false;
            Out::error("Vertex Colors of '%s' not saved to %s: they stay in memory, the next save tries again.", name.c_str(), key.c_str());
        }
    }
    // the others' writes (other scenes' files) are on disk now too, or reported
    return ok && (flushed || written.empty());
}

bool editor::vertexColorFileDirty(Project& project, const std::string& assetPath) {
    return vcd::isDirty(project, assetPath);
}

bool editor::flushVertexColorFiles() {
    return vcd::flushWrites();
}

void editor::releaseVertexColorFiles() {
    vcd::resetFiles();
    vcd::resetRemap();
    vcd::resetSelections();
}

void editor::forgetUnusedVertexColorFiles(Project& project) {
    (void)project;
    auto& files = vcLiveFiles();
    for (auto it = files.begin(); it != files.end();) {
        const bool clean = !it->second.hold && VcFileWriter::get().writtenVersion(it->first) >= it->second.version;
        // only the table holds them, and the disk has them: read again when wanted
        if (clean && it->second.data.use_count() == 1) it = files.erase(it);
        else ++it;
    }
}
