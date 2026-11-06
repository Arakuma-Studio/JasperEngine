// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Palette.h"

#include "Log.h"
#include "io/Data.h"

#include <algorithm>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::pixelart;

// file-local, inside the namespace: a unity build's other sources never see these names
namespace jasperengine::pixelart {
namespace {

    struct LibraryEntry {
        std::shared_ptr<const Palette> palette;
        std::weak_ptr<const Palette> runtime;   // a runtime palette, held by its users
        std::vector<Diagnostic> diagnostics;
        uint32_t version = 0;
    };

    struct Library {
        std::mutex mutex;
        std::map<std::string, LibraryEntry> entries;
        uint32_t nextVersion = 1;
        uint32_t generation = 1;
        uint64_t nextRuntime = 1;
    };

    bool isRuntimeKey(const std::string& path) {
        return path.rfind("runtime:", 0) == 0;
    }

    Library& library() {
        static Library* instance = new Library();
        return *instance;
    }

    // with the library locked
    // changed: a reload of what a binding may have read (a first read changes nothing anyone saw)
    void readInto(Library& lib, const std::string& path, LibraryEntry& entry, bool changed) {
        entry.palette.reset();
        entry.diagnostics.clear();
        entry.version = lib.nextVersion++;
        if (changed) lib.generation++;
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
            entry.diagnostics.push_back({Diagnostic::Severity::Error, "not_found", "", "cannot read the palette " + path,
                                         "check the path (relative to the assets directory)"});
            Log::error("[Palette] Cannot read %s", path.c_str());
            return;
        }
        Palette palette;
        if (!PaletteCodec::read(data.readString(), palette, entry.diagnostics)) {
            for (const Diagnostic& diagnostic : entry.diagnostics) {
                if (diagnostic.severity == Diagnostic::Severity::Error) {
                    Log::error("[Palette] %s: %s: %s", path.c_str(), diagnostic.field.c_str(), diagnostic.message.c_str());
                }
            }
            return;
        }
        entry.palette = std::make_shared<const Palette>(std::move(palette));
    }

}
}

bool pixelart::hasErrors(const std::vector<Diagnostic>& diagnostics) {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == Diagnostic::Severity::Error) return true;
    }
    return false;
}

bool Palette::isLocked(int index) const {
    return std::find(locked.begin(), locked.end(), (uint16_t)index) != locked.end();
}

std::string Palette::colorName(int index) const {
    auto it = names.find(index);
    return it != names.end() ? it->second : std::string();
}

const PaletteRamp* Palette::findRamp(std::string_view rampName) const {
    const int index = findRampIndex(rampName);
    return index >= 0 ? &ramps[index] : nullptr;
}

int Palette::findRampIndex(std::string_view rampName) const {
    for (size_t i = 0; i < ramps.size(); i++) {
        if (ramps[i].name == rampName) return (int)i;
    }
    return -1;
}

int Palette::findVariant(std::string_view variantName) const {
    for (size_t i = 0; i < variants.size(); i++) {
        if (variants[i].name == variantName) return (int)i;
    }
    return -1;
}

std::vector<Rgba8> Palette::rampColors(std::string_view rampName, int steps) const {
    const PaletteRamp* ramp = findRamp(rampName);
    if (!ramp) return {};
    std::vector<Rgba8> resolved;
    resolved.reserve(ramp->colors.size());
    for (uint16_t index : ramp->colors) {
        if (validIndex(index)) resolved.push_back(colors[index]);
    }
    if (steps > 0) return fitRamp(resolved, steps);
    return resolved;
}

Rgba8 Palette::variantColor(int variant, int index) const {
    if (!validIndex(index)) return Rgba8{0, 0, 0, 0};
    if (variant >= 0 && variant < (int)variants.size()) {
        const PaletteVariant& row = variants[variant];
        if (index < (int)row.colors.size() && row.colors[index].has_value()) return *row.colors[index];
    }
    return colors[index];
}

std::vector<Rgba8> Palette::variantColors(int variant) const {
    std::vector<Rgba8> row(colors.size());
    for (size_t i = 0; i < colors.size(); i++) row[i] = variantColor(variant, (int)i);
    return row;
}

int Palette::nearest(Rgba8 color, ColorMetric metric) const {
    return PaletteMatcher(colors, metric).nearestUncached(color).first;
}

int Palette::indexOf(Rgba8 color) const {
    for (size_t i = 0; i < colors.size(); i++) {
        if (colors[i] == color) return (int)i;
    }
    return -1;
}

void Palette::remap(const std::vector<int>& oldOfNew, Rgba8 fill) {
    std::vector<int> newOfOld(colors.size(), -1);
    for (size_t i = 0; i < oldOfNew.size(); i++) {
        const int old = oldOfNew[i];
        if (old >= 0 && old < (int)colors.size() && newOfOld[old] < 0) newOfOld[old] = (int)i;
    }
    auto mapped = [&](int old) { return old >= 0 && old < (int)newOfOld.size() ? newOfOld[old] : -1; };

    std::vector<Rgba8> newColors(oldOfNew.size(), fill);
    for (size_t i = 0; i < oldOfNew.size(); i++) {
        if (oldOfNew[i] >= 0 && oldOfNew[i] < (int)colors.size()) newColors[i] = colors[oldOfNew[i]];
    }

    std::map<int, std::string> newNames;
    for (const auto& [index, text] : names) {
        const int to = mapped(index);
        if (to >= 0) newNames[to] = text;
    }

    std::vector<uint16_t> newLocked;
    for (uint16_t index : locked) {
        const int to = mapped(index);
        if (to >= 0) newLocked.push_back((uint16_t)to);
    }
    std::sort(newLocked.begin(), newLocked.end());
    newLocked.erase(std::unique(newLocked.begin(), newLocked.end()), newLocked.end());

    for (PaletteRamp& ramp : ramps) {
        std::vector<uint16_t> indices;
        for (uint16_t index : ramp.colors) {
            const int to = mapped(index);
            if (to >= 0) indices.push_back((uint16_t)to);
        }
        ramp.colors = std::move(indices);
    }

    for (PaletteVariant& variant : variants) {
        std::vector<std::optional<Rgba8>> row(oldOfNew.size());
        for (size_t i = 0; i < oldOfNew.size(); i++) {
            const int old = oldOfNew[i];
            if (old >= 0 && old < (int)variant.colors.size()) row[i] = variant.colors[old];
        }
        while (!row.empty() && !row.back().has_value()) row.pop_back();
        variant.colors = std::move(row);
    }

    transparent = mapped(transparent);
    colors = std::move(newColors);
    names = std::move(newNames);
    locked = std::move(newLocked);
}

std::shared_ptr<const Palette> PaletteLibrary::get(const std::string& path, std::vector<Diagnostic>* diagnostics) {
    Library& lib = library();
    std::lock_guard<std::mutex> lock(lib.mutex);
    auto it = lib.entries.find(path);
    if (isRuntimeKey(path)) {
        std::shared_ptr<const Palette> palette = it != lib.entries.end() ? it->second.runtime.lock() : nullptr;
        if (!palette) {
            if (it != lib.entries.end()) lib.entries.erase(it);
            if (diagnostics) {
                *diagnostics = {{Diagnostic::Severity::Error, "not_found", "", "the runtime palette " + path + " is gone",
                                 "keep a reference to the Palette made with Palette.new while it is used"}};
            }
        } else if (diagnostics) {
            diagnostics->clear();
        }
        return palette;
    }
    if (it == lib.entries.end()) {
        it = lib.entries.emplace(path, LibraryEntry{}).first;
        readInto(lib, path, it->second, false);
    }
    if (diagnostics) *diagnostics = it->second.diagnostics;
    return it->second.palette;
}

std::string PaletteLibrary::addRuntime(std::shared_ptr<const Palette> palette) {
    Library& lib = library();
    std::lock_guard<std::mutex> lock(lib.mutex);
    // the entries of runtime palettes nobody holds any more
    for (auto it = lib.entries.begin(); it != lib.entries.end();) {
        if (isRuntimeKey(it->first) && it->second.runtime.expired()) {
            it = lib.entries.erase(it);
        } else {
            ++it;
        }
    }
    const std::string key = "runtime:" + std::to_string(lib.nextRuntime++);
    LibraryEntry& entry = lib.entries[key];
    entry.runtime = palette;
    entry.version = lib.nextVersion++;
    // a new key no binding names yet: nothing to redraw (Palette.new every frame stays cheap)
    return key;
}

void PaletteLibrary::reload(const std::string& path) {
    if (isRuntimeKey(path)) return;
    Library& lib = library();
    std::lock_guard<std::mutex> lock(lib.mutex);
    readInto(lib, path, lib.entries[path], true);
}

void PaletteLibrary::set(const std::string& path, const Palette& palette) {
    Library& lib = library();
    std::lock_guard<std::mutex> lock(lib.mutex);
    LibraryEntry& entry = lib.entries[path];
    entry.palette = std::make_shared<const Palette>(palette);
    entry.diagnostics.clear();
    entry.version = lib.nextVersion++;
    lib.generation++;
}

void PaletteLibrary::clear() {
    Library& lib = library();
    std::lock_guard<std::mutex> lock(lib.mutex);
    lib.entries.clear();
    lib.generation++;
}

uint32_t PaletteLibrary::version(const std::string& path) {
    Library& lib = library();
    std::lock_guard<std::mutex> lock(lib.mutex);
    auto it = lib.entries.find(path);
    return it != lib.entries.end() ? it->second.version : 0;
}

uint32_t PaletteLibrary::generation() {
    Library& lib = library();
    std::lock_guard<std::mutex> lock(lib.mutex);
    return lib.generation;
}
