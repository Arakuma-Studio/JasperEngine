// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NormalMapTilt.h"
#include "jobs/JobSystem.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    using editor::NormalMapTiltResult;

    // One measure of one version of a file, shared by the cache entry, the thread computing it
    // and whoever waits for it. The cache drops its reference when the file changes, without
    // waiting: the thread keeps the job alive until it has finished into it.
    struct Job {
        std::mutex mutex;
        std::condition_variable done;
        bool ready = false;
        NormalMapTiltResult result;

        void finish(const NormalMapTiltResult& value) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                result = value;
                ready = true;
            }
            done.notify_all();
        }

        bool poll(NormalMapTiltResult& value) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ready)
                return false;
            value = result;
            return true;
        }

        NormalMapTiltResult wait() {
            std::unique_lock<std::mutex> lock(mutex);
            done.wait(lock, [this] { return ready; });
            return result;
        }
    };

    // the version of a file a result belongs to
    struct Version {
        fs::file_time_type modified;
        std::uintmax_t size = 0;

        bool operator==(const Version& other) const { return modified == other.modified && size == other.size; }
    };

    struct Entry {
        Version version;
        std::shared_ptr<Job> job;
    };

    struct Cache {
        std::mutex mutex;
        std::map<std::string, Entry> entries;
        editor::NormalMapTilt::MeasureFunction measure;
    };

    // Never destroyed: a lookup from a static destructor still finds it (measure threads do
    // not touch it, only their Job)
    Cache& cache() {
        static Cache* const instance = new Cache();
        return *instance;
    }

    // the file's version now; false when it is not a readable file
    bool currentVersion(const fs::path& file, Version& version) {
        std::error_code ec;
        version.modified = fs::last_write_time(file, ec);
        if (ec)
            return false;
        version.size = fs::file_size(file, ec);
        return !ec;
    }

    NormalMapTiltResult measured(const editor::NormalMapTilt::MeasureFunction& function, const fs::path& file) {
        NormalMapTiltResult result;
        result.ready = true;
        try {
            result.valid = function ? function(file, result.degrees) : editor::NormalMapTilt::measureFile(file, result.degrees);
        } catch (...) {
            result.valid = false;
        }
        if (!result.valid)
            result.degrees = 0.0f;
        return result;
    }

    // The job of the file's version: the cached one, else a new one the caller runs (created),
    // with the measure function to run it with. Only the map is touched under the lock; an
    // entry of an older version lets go of its job (whose thread, if any, still holds it).
    std::shared_ptr<Job> findJob(const fs::path& file, const Version& version, bool& created,
                                 editor::NormalMapTilt::MeasureFunction& function) {
        const std::string key = file.lexically_normal().generic_string();
        std::shared_ptr<Job> previous;
        std::shared_ptr<Job> job;
        {
            Cache& c = cache();
            std::lock_guard<std::mutex> lock(c.mutex);
            Entry& entry = c.entries[key];
            created = !entry.job || !(entry.version == version);
            if (created) {
                previous = std::move(entry.job);
                entry.version = version;
                entry.job = std::make_shared<Job>();
                function = c.measure;
            }
            job = entry.job;
        }
        // (previous goes here, outside the lock)
        return job;
    }

}

bool editor::NormalMapTilt::measureFile(const fs::path& file, float& degrees, std::string* error) {
    degrees = 0.0f;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "can't open " + file.generic_string();
        return false;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int width = 0, height = 0, components = 0;
    unsigned char* pixels = bytes.empty() ? nullptr :
        stbi_load_from_memory(bytes.data(), (int)bytes.size(), &width, &height, &components, 3);
    if (!pixels) {
        if (error) *error = "not an image: " + file.generic_string();
        return false;
    }

    // a grid of at most 512 x 512 texels
    const int stepX = std::max(1, width / 512);
    const int stepY = std::max(1, height / 512);
    double sum[3] = {0.0, 0.0, 0.0};
    for (int y = 0; y < height; y += stepY) {
        for (int x = 0; x < width; x += stepX) {
            const unsigned char* texel = pixels + ((size_t)y * width + x) * 3;
            double n[3];
            for (int c = 0; c < 3; c++)
                n[c] = texel[c] / 255.0 * 2.0 - 1.0;
            const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length < 1e-6)
                continue;
            for (int c = 0; c < 3; c++)
                sum[c] += n[c] / length;
        }
    }
    stbi_image_free(pixels);

    const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (length < 1e-9) {
        // no direction at all (a blank map): as tilted as it can be
        degrees = 90.0f;
        return true;
    }
    degrees = (float)(std::acos(std::clamp(sum[2] / length, -1.0, 1.0)) * 180.0 / 3.14159265358979323846);
    return true;
}

editor::NormalMapTiltResult editor::NormalMapTilt::request(const fs::path& file) {
    Version version;
    if (!currentVersion(file, version))
        return NormalMapTiltResult{true, false, 0.0f};
    bool created = false;
    MeasureFunction function;
    std::shared_ptr<Job> job = findJob(file, version, created, function);
    if (created) {
        // a Background job of the engine's job system (S06), detached: it owns what it uses
        // (the job, the function, the path)
        jobs::enqueueDetached([job, function, file]() { job->finish(measured(function, file)); },
                              jobs::Lane::Background, "normal-map-tilt");
    }
    NormalMapTiltResult result;
    if (job->poll(result))
        return result;
    return NormalMapTiltResult();
}

editor::NormalMapTiltResult editor::NormalMapTilt::measure(const fs::path& file) {
    Version version;
    if (!currentVersion(file, version))
        return NormalMapTiltResult{true, false, 0.0f};
    bool created = false;
    MeasureFunction function;
    std::shared_ptr<Job> job = findJob(file, version, created, function);
    if (created) {
        // decoded here, without the cache's lock: other files' lookups go on meanwhile
        job->finish(measured(function, file));
    }
    // one a request (or another measure) started: its result
    return job->wait();
}

std::string editor::NormalMapTilt::describe(const fs::path& file, float degrees) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f", degrees);
    return file.filename().generic_string() + "'s average normal is " + text + "\xC2\xB0 off +Z";
}

void editor::NormalMapTilt::setMeasureFunction(MeasureFunction function) {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.measure = std::move(function);
}

void editor::NormalMapTilt::clearCache() {
    std::map<std::string, Entry> dropped;
    {
        Cache& c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        dropped.swap(c.entries);
    }
}
