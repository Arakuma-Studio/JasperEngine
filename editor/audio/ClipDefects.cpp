// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipDefects.h"

#include "App.h"
#include "Backend.h"
#include "AudioAnalysis.h"
#include "AudioImportSettings.h"
#include "EditorHost.h"
#include "Project.h"
#include "SoundTypes.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <optional>
#include <sstream>
#include <mutex>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

// The results, the queue and the worker, behind one mutex. A function-local static, made at the
// first request (after the editor host, so destroyed before it): its destructor stops the worker
// and joins a running job. Statics are destroyed in the reverse order they were made, so the ones
// a job reads (the type table, the analysis'
// window) are made before
// this object by its constructor: they are still there while the destructor waits for the job.
struct ClipDefects::State {
    // results kept: the clips the Inspector shows at once, and a few before them
    static constexpr size_t KEPT = 64;

    struct Entry {
        Key key;
        bool done = false;
        Result result;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stopping = false;
    // the project the results belong to
    fs::path projectDirectory;
    // newest first
    std::vector<Entry> entries;
    // newest first: the next job is the front one
    std::deque<Job> queue;

    State() {
        SoundTypes::all();
        AudioAnalysis::Signal probe;
        probe.channels = 1;
        probe.sampleRate = 48000;
        probe.frames = (size_t)AudioAnalysis::FFT_SIZE * 2;
        probe.samples.assign(probe.frames, 0.0f);
        probe.samples[probe.frames / 2] = 0.5f;
        AudioAnalysis::measure(probe, AudioMeterMode::File);
        AudioAnalysis::defects(probe, AudioAnalysis::Region());
    }

    ~State() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            queue.clear();
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }
};

ClipDefects::State& ClipDefects::state() {
    static State instance;
    return instance;
}

ClipDefects::Stamp ClipDefects::stampOf(const fs::path& path) {
    Stamp stamp;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) return stamp;
    const auto modified = fs::last_write_time(path, ec);
    if (ec) return stamp;
    stamp.known = true;
    stamp.size = size;
    stamp.modified = (int64_t)modified.time_since_epoch().count();
    return stamp;
}

ClipDefects::Result ClipDefects::run(const Job& job) {
    Result out;
    out.pending = false;
    AudioAnalysis::Json cached;
    std::string error;
    if (!AudioAnalysis::analyzeClip(job.file, job.cacheDirectory, cached, error)) {
        out.error = error;
        return out;
    }
    // the full item, less its loudness curves: the waveform draws the onset ticks
    AudioAnalysis::Json item = AudioAnalysis::fileItem(cached, 1, job.key.id, AudioAnalysis::labelsOf(job.file), true, SoundTypes::HEAD_SILENCE_MAX_MS);
    item.erase("momentary");
    item.erase("short_term");
    out.notes = item.value("notes", Json::array());
    out.item = std::move(item);
    return out;
}

void ClipDefects::work() {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    while (true) {
        s.wake.wait(lock, [&]() { return s.stopping || !s.queue.empty(); });
        if (s.stopping) return;
        const Job job = std::move(s.queue.front());
        s.queue.pop_front();
        lock.unlock();
        const Result result = run(job);
        lock.lock();
        if (s.stopping) return;
        auto found = std::find_if(s.entries.begin(), s.entries.end(), [&](const State::Entry& e) { return e.key == job.key; });
        if (found == s.entries.end()) continue;  // superseded, or another project since
        found->done = true;
        found->result = result;
        // the editor loop draws a frame for it (the task itself has nothing to do)
        getEditorHost().enqueueMainThreadTask([]() {});
    }
}

ClipDefects::Result ClipDefects::get(const Project* project, const std::string& id) {
    Result none;
    none.pending = false;
    if (!project || id.empty()) return none;

    Job job;
    job.key.id = id;
    job.file = project->resolveAssetPath(id);
    job.key.file = stampOf(job.file);
    job.key.meta = stampOf(SetAssetMetaCmd::metaPathOf(job.file));
    job.cacheDirectory = AudioAnalysis::cacheDirectory(project->getProjectPath());
    if (!job.key.file.known) {
        none.error = "the file can't be read";
        return none;
    }
    return request(project, job);
}

ClipDefects::Result ClipDefects::request(const Project* project, const Job& job) {
    const std::string& id = job.key.id;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.projectDirectory != project->getProjectPath()) {
        // another project: nothing of the last one is kept
        s.entries.clear();
        s.queue.clear();
        s.projectDirectory = project->getProjectPath();
    }
    auto found = std::find_if(s.entries.begin(), s.entries.end(), [&](const State::Entry& e) { return e.key == job.key; });
    if (found != s.entries.end()) {
        if (found->done) return found->result;
        return Result();
    }

    // a new measurement: the clip's older result goes, and the request goes first in the queue
    s.entries.erase(std::remove_if(s.entries.begin(), s.entries.end(), [&](const State::Entry& e) { return e.key.id == id; }),
                    s.entries.end());
    State::Entry entry;
    entry.key = job.key;
    s.entries.insert(s.entries.begin(), entry);
    if (s.entries.size() > State::KEPT) s.entries.resize(State::KEPT);
    s.queue.erase(std::remove_if(s.queue.begin(), s.queue.end(), [&](const Job& queued) { return queued.key.id == id; }), s.queue.end());
    s.queue.push_front(job);
    if (!s.thread.joinable()) s.thread = std::thread(&ClipDefects::work);
    s.wake.notify_one();
    return Result();
}

void ClipDefects::clear() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.clear();
    s.queue.clear();
    s.projectDirectory.clear();
}

bool ClipDefects::addClickSafeFades(Project* project, const fs::path& asset, const Json& fix) {
    const std::string title = "Add Click-Safe Fades";
    if (!project) return false;
    AudioImportSettings settings;
    std::string error;
    if (!AudioImportSettings::load(asset, settings, error)) {
        Backend::getApp().registerAlert(title, error + ": fix it in Import Settings first.");
        return false;
    }
    const auto fadeIn = fix.find("fade_in_ms");
    if (fadeIn != fix.end() && fadeIn->is_number()) settings.fadeInMs = fadeIn->get<double>();
    const auto fadeOut = fix.find("fade_out_ms");
    if (fadeOut != fix.end() && fadeOut->is_number()) settings.fadeOutMs = fadeOut->get<double>();
    if (!settings.validate(error)) {
        Backend::getApp().registerAlert(title, error);
        return false;
    }
    auto* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(asset), AudioImportSettings::metaKey, settings.toYaml());
    command->setNoMerge();
    // addCommand runs execute() and deletes the command when it fails
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        Backend::getApp().registerAlert(title, "Writing the .meta of " + asset.filename().string() + " failed (permissions or a full disk).");
        return false;
    }
    return true;
}
