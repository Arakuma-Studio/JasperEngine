// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NormalMapTilt.h"

#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    struct Entry {
        fs::file_time_type modified;
        bool ready = false;
        jasperengine::editor::NormalMapTiltResult result;
        std::shared_future<jasperengine::editor::NormalMapTiltResult> pending;
    };

    std::mutex& cacheMutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::map<std::string, Entry>& cache() {
        static std::map<std::string, Entry> entries;
        return entries;
    }

    jasperengine::editor::NormalMapTiltResult measured(const fs::path& file) {
        jasperengine::editor::NormalMapTiltResult result;
        result.ready = true;
        result.valid = jasperengine::editor::NormalMapTilt::measureFile(file, result.degrees);
        return result;
    }

    // The entry of the file as it is now (a changed file is measured again); null when it is
    // not there
    Entry* currentEntry(const fs::path& file, std::string& key) {
        std::error_code ec;
        const fs::file_time_type modified = fs::last_write_time(file, ec);
        if (ec)
            return nullptr;
        key = file.lexically_normal().generic_string();
        auto found = cache().find(key);
        if (found != cache().end() && found->second.modified != modified)
            cache().erase(found);
        Entry& entry = cache()[key];
        entry.modified = modified;
        return &entry;
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
    std::lock_guard<std::mutex> lock(cacheMutex());
    std::string key;
    Entry* entry = currentEntry(file, key);
    if (!entry)
        return NormalMapTiltResult{true, false, 0.0f};
    if (entry->ready)
        return entry->result;
    if (!entry->pending.valid()) {
        entry->pending = std::async(std::launch::async, measured, file).share();
    }
    if (entry->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        entry->result = entry->pending.get();
        entry->ready = true;
        entry->pending = {};
        return entry->result;
    }
    return NormalMapTiltResult();
}

editor::NormalMapTiltResult editor::NormalMapTilt::measure(const fs::path& file) {
    std::shared_future<NormalMapTiltResult> pending;
    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        std::string key;
        Entry* entry = currentEntry(file, key);
        if (!entry)
            return NormalMapTiltResult{true, false, 0.0f};
        if (entry->ready)
            return entry->result;
        if (!entry->pending.valid()) {
            entry->result = measured(file);
            entry->ready = true;
            return entry->result;
        }
        pending = entry->pending;
    }
    // one a request started: its result
    return pending.get();
}

std::string editor::NormalMapTilt::describe(const fs::path& file, float degrees) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f", degrees);
    return file.filename().generic_string() + "'s average normal is " + text + "\xC2\xB0 off +Z";
}
