// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/SceneDiffTracker.h"

#include "Project.h"
#include "command/CommandHistory.h"

#include <fstream>
#include <sstream>

namespace jasperengine::editor::vcs {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream content;
        content << in.rdbuf();
        out = content.str();
        return true;
    }

    std::shared_ptr<scene::SceneYamlModel> emptyModel(const fs::path& file) {
        auto model = std::make_shared<scene::SceneYamlModel>();
        model->kind = file.extension() == ".prefab" ? DocumentKind::Prefab : DocumentKind::Scene;
        scene::SceneObject settings;
        settings.id = scene::kSceneSettingsId;
        settings.name = "Scene Settings";
        settings.settings = true;
        model->objects.emplace(scene::kSceneSettingsId, std::move(settings));
        return model;
    }

    SceneProject* documentOf(Project& project, uint32_t sceneId) {
        for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
            if (sceneProject->id == sceneId) return sceneProject;
        }
        return nullptr;
    }

    fs::path fileOf(Project& project, const SceneProject& sceneProject) {
        if (sceneProject.filepath.empty()) return {};
        if (const PrefabStage* stage = project.getPrefabStage(sceneProject.id)) return project.getProjectPath() / stage->assetPath;
        return sceneProject.filepath.is_relative() ? project.getProjectPath() / sceneProject.filepath : sceneProject.filepath;
    }

    // the job: the current side (the editor's encode, or the file) parsed and diffed
    std::shared_ptr<scene::SceneDiffResult> diffJob(YAML::Node current, fs::path file, std::shared_ptr<const scene::SceneYamlModel> baseline,
                                                    std::string* error, bool changedOnly = true) {
        scene::SceneYamlModel model;
        std::string why;
        // (no encode is a null node, which is "defined": only a map is a document)
        if (current.IsMap()) {
            if (!scene::SceneYamlModel::fromDocument(current, model, &why)) {
                if (error) *error = why;
                return nullptr;
            }
        } else {
            std::string text;
            if (!readText(file, text)) {
                // deleted: everything of the baseline is removed
                model = *emptyModel(file);
            } else if (!scene::SceneYamlModel::parse(text, model, &why)) {
                if (error) *error = file.filename().string() + ": " + why;
                return nullptr;
            }
        }
        scene::SceneDiffOptions options;
        options.changedOnly = changedOnly;
        return std::make_shared<scene::SceneDiffResult>(scene::diffScenes(*baseline, model, options));
    }

}

const scene::ObjectDiff* SceneDiffView::object(uint64_t id) const {
    auto it = byId.find(id);
    return it != byId.end() ? it->second : nullptr;
}

SceneDiffTracker::~SceneDiffTracker() {
    for (auto& [key, entry] : entries) {
        if (entry.job) entry.job->wait();
    }
    for (const std::shared_ptr<OneOff>& op : oneOffs) {
        if (op->job) op->job->wait();
    }
}

void SceneDiffTracker::watch(uint32_t sceneId, const std::string& rev) {
    Entry& entry = entries[{sceneId, rev}];
    entry.view.sceneId = sceneId;
    entry.view.rev = rev;
    entry.watched = true;
}

const SceneDiffView* SceneDiffTracker::view(uint32_t sceneId, const std::string& rev) const {
    auto it = entries.find({sceneId, rev});
    return it != entries.end() ? &it->second.view : nullptr;
}

void SceneDiffTracker::clear() {
    baselines.clear();
}

std::string SceneDiffTracker::baselineKeyOf(const VcsService& service, const RepoPath& key, const std::string& rev) const {
    return service.contentMemoKey(key, rev);
}

std::shared_ptr<SceneDiffTracker::Baseline> SceneDiffTracker::baselineFor(VcsService& service, const RepoPath& key, const std::string& rev,
                                                                           const std::string& memo) {
    auto it = baselines.find(memo);
    if (it != baselines.end()) return it->second;
    auto baseline = std::make_shared<Baseline>();
    baselines[memo] = baseline;
    const fs::path file = service.toAbsolute(key);
    // a file the baseline has not got: every GameObject of it is added
    const VcsChange* change = service.snapshot() ? service.snapshot()->find(key) : nullptr;
    const std::string readRev = service.effectiveRevision(rev);
    if (readRev == service.baselineRevision() && change && !change->hasBaseline()) {
        baseline->model = emptyModel(file);
        baseline->ready = true;
        return baseline;
    }
    std::shared_ptr<IVcsProvider> provider = service.providerForReads();
    if (!provider || !provider->validRevision(readRev)) {
        baseline->error = provider ? "not a revision: " + rev : std::string("Version control is not active");
        baseline->ready = true;
        return baseline;
    }
    // a rename, move or copy compares with the file it came from
    const RepoPath source = change ? change->diffSource() : key;
    // a process: on a blocking channel, never the main thread (R24)
    jobs::system().processChannel().submit([provider, key, source, readRev, baseline]() {
        std::string text;
        const VcsResult result = provider->baseContent(source, readRev, text);
        auto model = std::make_shared<scene::SceneYamlModel>();
        std::string error;
        if (!result.ok) {
            error = result.message;
        } else if (!scene::SceneYamlModel::parse(text, *model, &error)) {
            error = "the baseline of " + key + " does not read: " + error;
        }
        std::lock_guard<std::mutex> lock(baseline->mutex);
        if (error.empty()) baseline->model = model;
        baseline->error = error;
        baseline->ready = true;
    }, jobs::SubmitOptions{.name = "vcs-baseline"});
    return baseline;
}

bool SceneDiffTracker::startDiff(Project& project, Entry& entry) {
    VcsService& service = project.getVcs();
    SceneDiffView& view = entry.view;
    std::shared_ptr<Baseline> baseline = baselineFor(service, view.key, view.rev, entry.baselineKey);
    std::shared_ptr<const scene::SceneYamlModel> baselineModel;
    {
        std::lock_guard<std::mutex> lock(baseline->mutex);
        if (!baseline->ready) return false;
        if (!baseline->error.empty()) {
            view.error = baseline->error;
            view.pending = false;
            entry.dirty = false;
            return true;
        }
        baselineModel = baseline->model;
    }
    SceneProject* sceneProject = documentOf(project, view.sceneId);
    if (!sceneProject) return false;
    YAML::Node current;
    bool fromDisk = entry.slowEncode || !sceneProject->isModified;
    if (!fromDisk) {
        const Clock::time_point start = Clock::now();
        current = project.encodeDocument(view.sceneId);
        view.encodeMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (view.encodeMs > kEncodeBudgetMs) {
            // R26: too big to encode every edit; from now on its saved file is diffed
            entry.slowEncode = true;
            slowEncodeCount++;
        }
    }
    entry.dirty = false;
    entry.jobBaseline = baselineModel;
    entry.jobFromDisk = entry.slowEncode;
    const fs::path file = view.file;
    entry.job = jobs::submit([current, file, baselineModel]() {
        std::string error;
        std::shared_ptr<scene::SceneDiffResult> result = diffJob(current, file, baselineModel, &error);
        if (!result) throw std::runtime_error(error);
        return result;
    }, jobs::SubmitOptions{.lane = jobs::Lane::Background, .name = "vcs-scene-diff"});
    view.pending = true;
    return true;
}

bool SceneDiffTracker::update(Project& project) {
    VcsService& service = project.getVcs();
    bool changed = false;
    if (watchAllOpen) {
        for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
            if (!sceneProject->filepath.empty()) watch(sceneProject->id);
        }
    }
    const Clock::time_point now = Clock::now();
    const uint64_t historyChanges = CommandHistory::changeCount();
    for (auto it = entries.begin(); it != entries.end();) {
        Entry& entry = it->second;
        SceneDiffView& view = entry.view;
        SceneProject* sceneProject = documentOf(project, view.sceneId);
        // not watched since the last update, or its document closed: dropped
        if (!entry.watched || !sceneProject) {
            if (entry.job) entry.job->wait();
            it = entries.erase(it);
            continue;
        }
        entry.watched = false;
        // a finished diff
        if (entry.job && entry.job->isReady()) {
            try {
                std::shared_ptr<scene::SceneDiffResult> result = entry.job->get();
                view.baseline = entry.jobBaseline;
                view.diff = result;
                view.byId.clear();
                for (const scene::ObjectDiff& object : result->objects) view.byId[object.id] = &object;
                view.error.clear();
            } catch (const std::exception& e) {
                view.error = e.what();
            }
            view.fromDisk = entry.jobFromDisk;
            view.version++;
            view.pending = entry.dirty;
            entry.job.reset();
            changed = true;
        }
        if (!service.active()) {
            view.error = service.stateMessage().empty() ? std::string("Version control is not active") : service.stateMessage();
            view.pending = false;
            ++it;
            continue;
        }
        const fs::path file = fileOf(project, *sceneProject);
        RepoPath key;
        if (file.empty() || !service.toKey(file, key)) {
            view.error = file.empty() ? "The document has not been saved yet" : "The document is not in the repository";
            view.pending = false;
            ++it;
            continue;
        }
        view.file = file;
        view.key = key;
        const std::string memo = baselineKeyOf(service, key, view.rev);
        if (memo != entry.baselineKey) {
            entry.baselineKey = memo;
            entry.dirty = true;
        }
        // R15: 500 ms after the last edit (the first diff at once)
        if (historyChanges != entry.historyChanges) {
            const bool first = entry.historyChanges == ~uint64_t(0);
            entry.historyChanges = historyChanges;
            entry.dirty = true;
            if (!first) entry.changedAt = now;
        }
        if (entry.dirty && !entry.job && now - entry.changedAt >= kDebounce) startDiff(project, entry);
        ++it;
    }

    // the one-off diffs
    for (auto it = oneOffs.begin(); it != oneOffs.end();) {
        OneOff& op = **it;
        if (op.job) {
            if (!op.job->isReady()) {
                ++it;
                continue;
            }
            std::shared_ptr<const scene::SceneYamlModel> baselineModel;
            {
                std::lock_guard<std::mutex> lock(op.baseline->mutex);
                baselineModel = op.baseline->model;
            }
            try {
                std::shared_ptr<scene::SceneDiffResult> result = op.job->get();
                op.done(std::string(), baselineModel, result);
            } catch (const std::exception& e) {
                op.done(e.what(), baselineModel, nullptr);
            }
            it = oneOffs.erase(it);
            continue;
        }
        std::shared_ptr<const scene::SceneYamlModel> baselineModel;
        std::string error;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(op.baseline->mutex);
            ready = op.baseline->ready;
            baselineModel = op.baseline->model;
            error = op.baseline->error;
        }
        if (!ready) {
            ++it;
            continue;
        }
        if (!error.empty()) {
            op.done(error, nullptr, nullptr);
            it = oneOffs.erase(it);
            continue;
        }
        const YAML::Node current = op.current;
        const fs::path file = op.request.file;
        const bool changedOnly = op.request.changedOnly;
        op.job = jobs::submit([current, file, baselineModel, changedOnly]() {
            std::string why;
            std::shared_ptr<scene::SceneDiffResult> result = diffJob(current, file, baselineModel, &why, changedOnly);
            if (!result) throw std::runtime_error(why);
            return result;
        }, jobs::SubmitOptions{.lane = jobs::Lane::Background, .name = "vcs-scene-diff"});
        ++it;
    }
    return changed;
}

void SceneDiffTracker::diffNow(Project& project, const Request& request, DiffDone done) {
    VcsService& service = project.getVcs();
    if (!service.active()) {
        done("Version control is not active: " + service.stateMessage(), nullptr, nullptr);
        return;
    }
    auto op = std::make_shared<OneOff>();
    op->request = request;
    op->done = std::move(done);
    if (request.sceneId != 0) {
        SceneProject* sceneProject = documentOf(project, request.sceneId);
        if (!sceneProject) {
            op->done("no open scene or prefab stage " + std::to_string(request.sceneId), nullptr, nullptr);
            return;
        }
        op->request.file = fileOf(project, *sceneProject);
        if (op->request.file.empty()) {
            op->done(sceneProject->name + " has not been saved yet: there is no baseline", nullptr, nullptr);
            return;
        }
        // the editor's state: the document's encode, its unsaved edits included
        if (request.fromEditor && sceneProject->isModified) op->current = project.encodeDocument(request.sceneId);
    }
    RepoPath key;
    if (!service.toKey(op->request.file, key)) {
        op->done(op->request.file.string() + " is not in the repository", nullptr, nullptr);
        return;
    }
    op->baseline = baselineFor(service, key, request.rev, baselineKeyOf(service, key, request.rev));
    oneOffs.push_back(op);
}

}
