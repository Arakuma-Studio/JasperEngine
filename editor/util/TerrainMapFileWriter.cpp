// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/TerrainMapFileWriter.h"

#include "Out.h"
#include "util/FileUtils.h"
#include "util/PngWriter.h"
#include "util/SafeWrite.h"
#include "stb_image_write.h"

#include <cstdio>
#include <utility>

using namespace jasperengine::editor;

namespace fs = std::filesystem;

TerrainMapFileWriter& TerrainMapFileWriter::get(){
    static TerrainMapFileWriter instance;
    return instance;
}

void TerrainMapFileWriter::enqueue(const fs::path& path, int width, int height, int channels, int bytesPerChannel, std::vector<unsigned char> pixels,
                                   const fs::path& root){
    std::unique_lock<std::mutex> lock(mutex);
    failedJobs.erase(path.string());  // superseded by newer pixels
    Job& job = jobs[path.string()];
    job.width = width;
    job.height = height;
    job.channels = channels;
    job.bytesPerChannel = bytesPerChannel;
    job.pixels = std::move(pixels);
    job.root = root;
    startWorkerLocked();
    condition.notify_all();
}

void TerrainMapFileWriter::flushPath(const std::string& path){
    std::unique_lock<std::mutex> lock(mutex);
    retryFailedLocked(path);
    drainInlineIfAlone(lock);
    condition.wait(lock, [&]{ return jobs.find(path) == jobs.end() && writingPath != path; });
}

void TerrainMapFileWriter::drainInlineIfAlone(std::unique_lock<std::mutex>& lock){
    // single-thread mode runs channel jobs from the frame's pump: write here instead
    if (!jobs::system().isSingleThreaded() || jobs.empty())
        return;
    lock.unlock();
    run();
    lock.lock();
}

bool TerrainMapFileWriter::flushAll(){
    std::unique_lock<std::mutex> lock(mutex);
    for (auto& [path, job] : failedJobs){
        if (jobs.find(path) == jobs.end()){
            jobs[path] = std::move(job);
        }
    }
    failedJobs.clear();
    if (!jobs.empty()){
        startWorkerLocked();
        condition.notify_all();
    }
    drainInlineIfAlone(lock);
    condition.wait(lock, [&]{ return jobs.empty() && writingPath.empty(); });
    return failedJobs.empty();
}

size_t TerrainMapFileWriter::failedCount(){
    std::unique_lock<std::mutex> lock(mutex);
    return failedJobs.size();
}

std::vector<std::string> TerrainMapFileWriter::failedPaths(){
    std::unique_lock<std::mutex> lock(mutex);
    std::vector<std::string> paths;
    paths.reserve(failedJobs.size());
    for (const auto& [path, job] : failedJobs){
        paths.push_back(path);
    }
    return paths;
}

TerrainMapFileWriter::~TerrainMapFileWriter(){
    {
        std::unique_lock<std::mutex> lock(mutex);
        stopRequested = true;
    }
    condition.notify_all();
    drainTask.wait();

    // Writes still queued (a drain the job system's shutdown cancelled) and those that kept
    // failing: written synchronously now — a since-resolved disk problem (freed space, fixed
    // permissions) still persists the sculpt. Out may already be torn down this late in
    // shutdown, so report irrecoverable losses to stderr.
    for (auto& [path, job] : jobs){
        if (failedJobs.find(path) == failedJobs.end())
            failedJobs[path] = std::move(job);
    }
    jobs.clear();
    for (const auto& [path, job] : failedJobs){
        if (!writeJob(path, job)){
            std::fprintf(stderr, "[ERROR] Terrain map lost, could not be written: %s\n", path.c_str());
        }
    }
}

bool TerrainMapFileWriter::writeJob(const std::string& path, const Job& job){
    // encoded in memory, then written: beneath the job's root when it has one (the queuing
    // thread's write scope), else as a plain replace-by-rename
    std::string bytes;
    if (job.bytesPerChannel >= 2){
        std::vector<unsigned char> png;
        if (!encodePng16(job.width, job.height, 1, job.pixels.data(), job.pixels.size(), png)) return false;
        bytes.assign(png.begin(), png.end());
    } else {
        auto append = [](void* context, void* data, int size) {
            static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
        };
        if (!stbi_write_png_to_func(append, &bytes, job.width, job.height, job.channels, job.pixels.data(), job.width * job.channels)) return false;
    }
    if (!job.root.empty()){
        std::string error;
        if (safewrite::writeFileResolvedBeneath(job.root, fs::path(path), bytes, error)) return true;
        std::fprintf(stderr, "[ERROR] %s\n", error.c_str());
        return false;
    }
    return FileUtils::writeFileReplacing(fs::path(path), bytes);
}

void TerrainMapFileWriter::startWorkerLocked(){
    if (!workerRunning){
        workerRunning = true;
        stopRequested = false;
        // a long disk write: the io-write channel; the drain writes until nothing is queued
        jobs::SubmitOptions options;
        options.name = "terrain-map-write";
        drainTask = jobs::system().ioWriteChannel().submit([this]{ run(); }, options);
    }
}

void TerrainMapFileWriter::retryFailedLocked(const std::string& path){
    auto failed = failedJobs.find(path);
    if (failed == failedJobs.end()){
        return;
    }
    if (jobs.find(path) == jobs.end()){
        jobs[path] = std::move(failed->second);
    }
    failedJobs.erase(failed);
    startWorkerLocked();
    condition.notify_all();
}

void TerrainMapFileWriter::run(){
    std::unique_lock<std::mutex> lock(mutex);
    while (!jobs.empty()){
        auto it = jobs.begin();
        const std::string path = it->first;
        Job job = std::move(it->second);
        jobs.erase(it);
        writingPath = path;
        lock.unlock();

        const bool written = writeJob(path, job);
        if (!written){
            // Out marshals to the main thread when called from a worker.
            Out::error("Failed to write terrain map %s — the edit is kept in memory and the write will be retried on the next save", path.c_str());
        }

        lock.lock();
        if (!written && jobs.find(path) == jobs.end()){
            failedJobs[path] = std::move(job);
        }
        writingPath.clear();
        condition.notify_all();
    }
    workerRunning = false;
    condition.notify_all();
}
