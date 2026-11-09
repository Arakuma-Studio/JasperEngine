// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteSwap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

using namespace jasperengine;
using namespace jasperengine::pixelart;

// file-local, inside the namespace: a unity build's other sources never see these names
namespace jasperengine::pixelart {
namespace {

    constexpr double kSwapIdleSeconds = 2.0;

    size_t cellTexel(Rgba8 color) {
        const int r = color.r >> 3, g = color.g >> 3, b = color.b >> 3;
        const int x = r + 32 * (b % 8);
        const int y = g + 32 * (b / 8);
        return ((size_t)y * kSwapCellsWidth + (size_t)x) * 4;
    }

    size_t rowTexel(int x, int row) {
        return ((size_t)row * kSwapRowsWidth + (size_t)x) * 4;
    }

    void putColor(std::vector<uint8_t>& bytes, size_t at, Rgba8 color) {
        bytes[at] = color.r;
        bytes[at + 1] = color.g;
        bytes[at + 2] = color.b;
        bytes[at + 3] = color.a;
    }

    uint64_t hashBytes(uint64_t hash, const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; i++) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // a cycling ramp's speed in the rows texture: steps per second as signed 8.8
    void putSpeed(std::vector<uint8_t>& bytes, size_t at, float cycle) {
        const double encoded = std::floor(((double)std::clamp(cycle, -127.99f, 127.99f) + 128.0) * 256.0 + 0.5);
        const uint32_t value = (uint32_t)std::clamp(encoded, 0.0, 65535.0);
        bytes[at] = (uint8_t)(value >> 8);
        bytes[at + 1] = (uint8_t)(value & 255u);
        bytes[at + 2] = 0;
        bytes[at + 3] = 0;
    }

    float readSpeed(const std::vector<uint8_t>& bytes, size_t at) {
        return (float)(bytes[at] * 256 + bytes[at + 1]) / 256.0f - 128.0f;
    }

    std::string rowName(const PaletteTarget& target, const Palette* palette) {
        if (!target.variant.empty()) return target.variant;
        if (palette && !palette->name.empty()) return palette->name;
        const std::string& path = target.palette.path;
        const size_t slash = path.find_last_of('/');
        std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = stem.rfind('.');
        return dot == std::string::npos ? stem : stem.substr(0, dot);
    }

    struct CacheEntry {
        std::shared_ptr<PaletteSwapTextures> textures;
        double idleSince = -1.0;
    };

    struct SwapCache {
        std::map<uint64_t, CacheEntry> entries;
        // a binding and the versions of its palettes -> the content hash its rows built: a
        // binding whose palettes did not change rebinds without building (R29)
        std::map<std::string, uint64_t> byBinding;
        std::shared_ptr<PaletteSwapTextures> identity;
        size_t builds = 0;
    };

    std::string bindingKey(const PaletteBinding& binding) {
        std::string key = binding.source.path + "@" + std::to_string(PaletteLibrary::version(binding.source.path)) +
                          (binding.useVariants ? "|v" : "|-");
        for (const PaletteTarget& target : binding.targets)
            key += "|" + target.text() + "@" + std::to_string(PaletteLibrary::version(target.palette.path));
        return key;
    }

    void holdPalettes(PaletteSwapTextures& textures, const std::vector<std::shared_ptr<const Palette>>& keepAlive) {
        for (const std::shared_ptr<const Palette>& palette : keepAlive) {
            if (std::find(textures.palettes.begin(), textures.palettes.end(), palette) == textures.palettes.end())
                textures.palettes.push_back(palette);
        }
    }

    SwapCache& swapCache() {
        static SwapCache* instance = new SwapCache();
        return *instance;
    }

    bool createSwapTextures(PaletteSwapTextures& textures, PaletteSwapData& data) {
        void* cellsData[1] = {data.cells.data()};
        size_t cellsSize[1] = {data.cells.size()};
        void* rowsData[1] = {data.rows.data()};
        size_t rowsSize[1] = {data.rows.size()};
        const bool cells = textures.cells.createTexture("palette swap cells", kSwapCellsWidth, kSwapCellsHeight, ColorFormat::RGBA,
                                                        TextureType::TEXTURE_2D, 1, cellsData, cellsSize, TextureFilter::NEAREST,
                                                        TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
        const bool rows = textures.rows.createTexture("palette swap rows", kSwapRowsWidth, data.rowsHeight, ColorFormat::RGBA,
                                                      TextureType::TEXTURE_2D, 1, rowsData, rowsSize, TextureFilter::NEAREST,
                                                      TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
        textures.rowCount = data.rowCount;
        textures.rowNames = data.rowNames;
        textures.hash = data.hash;
        return cells && rows;
    }

}
}

PaletteSwapTextures::~PaletteSwapTextures() {
    cells.destroyTexture();
    rows.destroyTexture();
}

int PaletteSwapData::sourceIndex(Rgba8 texel) const {
    if (cells.size() < (size_t)kSwapCellsWidth * kSwapCellsHeight * 4 || rowsHeight <= 0) return -1;
    const size_t at = cellTexel(texel);
    for (int k = 0; k < 4; k++) {
        const int index = (int)cells[at + k] - 1;
        if (index < 0) continue;
        const size_t source = rowTexel(index, 0);
        if (rows[source] == texel.r && rows[source + 1] == texel.g && rows[source + 2] == texel.b) return index;
    }
    return -1;
}

Rgba8 PaletteSwapData::swap(Rgba8 texel, int row, float time, float cycleSpeed, bool cycling) const {
    int index = sourceIndex(texel);
    if (index < 0) return texel;
    if (cycling) {
        const size_t meta = rowTexel(index, kSwapCycleRow);
        if (rows[meta] != 0) {
            const int length = rows[meta + 1];
            const int orderRow = kSwapCycleRow + 1 + rows[meta + 2];
            const float speed = readSpeed(rows, rowTexel(255, orderRow));
            // the shader's float maths: floor(time * speed * cycleSpeed), mod the length
            const float steps = std::floor(time * speed * cycleSpeed);
            const float shifted = (float)(rows[meta] - 1) + steps;
            int position = (int)(shifted - (float)length * std::floor(shifted / (float)length));
            position = std::clamp(position, 0, length - 1);
            index = rows[rowTexel(position, orderRow)];
        }
    }
    const size_t target = rowTexel(index, std::clamp(row, 0, kSwapTargetRows - 1));
    const unsigned alpha = (unsigned)texel.a * rows[target + 3];
    // texel.a * target.a in [0, 1], written to an 8-bit target: rounded
    return Rgba8{rows[target], rows[target + 1], rows[target + 2], (uint8_t)((alpha + 127u) / 255u)};
}

void pixelart::identityPaletteSwapData(PaletteSwapData& out) {
    out.cells.assign((size_t)kSwapCellsWidth * kSwapCellsHeight * 4, 0);
    out.rowsHeight = kSwapTargetRows + 1;
    out.rows.assign((size_t)kSwapRowsWidth * out.rowsHeight * 4, 0);
    out.rowCount = 1;
    out.rowNames = {""};
    out.hash = 0;
}

bool pixelart::buildPaletteSwapData(const Palette& source, const std::vector<PaletteSwapRow>& targets, PaletteSwapData& out,
                                    std::vector<Diagnostic>& diagnostics) {
    identityPaletteSwapData(out);
    const int count = source.count();
    if (count > kSwapMaxSourceColors) {
        diagnostics.push_back({Diagnostic::Severity::Error, "palette_too_large", "source",
                               "a Palette Swap source holds at most 255 colours; '" + source.name + "' has " + std::to_string(count),
                               "use a smaller source palette (Reduce Colours…)"});
        return false;
    }

    // the cycling ramps (validated not to overlap) and their order rows
    std::vector<const PaletteRamp*> cycling;
    for (const PaletteRamp& ramp : source.ramps) {
        if (ramp.cycle != 0.0f && !ramp.colors.empty() && (int)cycling.size() < Palette::kMaxCyclingRamps) cycling.push_back(&ramp);
    }
    out.rowsHeight = kSwapTargetRows + 1 + (int)cycling.size();
    out.rows.assign((size_t)kSwapRowsWidth * out.rowsHeight * 4, 0);

    // the cells: every colour of the source, in order; a fully transparent slot is not a
    // colour to match (an opaque black texel must not take a "#00000000" slot's place)
    std::map<size_t, std::vector<int>> overflow;
    for (int i = 0; i < count; i++) {
        const Rgba8 color = source.colors[i];
        if (color.a == 0) continue;
        int first = -1;
        for (int j = 0; j < i && first < 0; j++) {
            const Rgba8 other = source.colors[j];
            if (other.a != 0 && other.r == color.r && other.g == color.g && other.b == color.b) first = j;
        }
        if (first >= 0) {
            diagnostics.push_back({Diagnostic::Severity::Warning, "duplicate_color", "colors[" + std::to_string(i) + "]",
                                   "colour " + std::to_string(i) + " (" + toHex(color) + ") repeats colour " + std::to_string(first) +
                                   ": Palette Swap maps it to " + std::to_string(first),
                                   "change one of them, or remove the duplicate"});
            continue;
        }
        const size_t at = cellTexel(color);
        int slot = -1;
        for (int k = 0; k < 4 && slot < 0; k++) {
            if (out.cells[at + k] == 0) slot = k;
        }
        if (slot < 0) {
            std::vector<int>& members = overflow[at];
            if (members.empty()) {
                for (int k = 0; k < 4; k++) members.push_back(out.cells[at + k] - 1);
            }
            members.push_back(i);
            continue;
        }
        out.cells[at + slot] = (uint8_t)(i + 1);
    }
    if (!overflow.empty()) {
        for (const auto& [at, members] : overflow) {
            std::string list;
            for (size_t m = 0; m < members.size(); m++) {
                list += (m ? ", " : "") + std::to_string(members[m]) + " " + toHex(source.colors[members[m]]);
            }
            diagnostics.push_back({Diagnostic::Severity::Error, "palette_cell_overflow", "colors",
                                   "more than 4 colours of '" + source.name + "' are within 8/255 of each other on every channel: " + list,
                                   "merge these colours or use a smaller source palette"});
        }
        identityPaletteSwapData(out);
        return false;
    }

    // the rows: the source, then the targets; past the last, the last again
    std::vector<PaletteSwapRow> rows;
    rows.push_back({"", source.colors});
    for (const PaletteSwapRow& target : targets) {
        if ((int)rows.size() == kSwapTargetRows) {
            diagnostics.push_back({Diagnostic::Severity::Warning, "palette_rows_dropped", "targets",
                                   "a palette binding holds 15 targets; '" + target.name + "' and the ones after it are left out",
                                   "use fewer variants or targets (Use Variants off keeps only the extra targets)"});
            break;
        }
        rows.push_back(target);
    }
    for (int row = 0; row < kSwapTargetRows; row++) {
        const PaletteSwapRow& from = rows[std::min(row, (int)rows.size() - 1)];
        for (int i = 0; i < count; i++) {
            const Rgba8 color = i < (int)from.colors.size() ? from.colors[i] : source.colors[i];
            putColor(out.rows, rowTexel(i, row), color);
        }
    }
    out.rowCount = (int)rows.size();
    out.rowNames.clear();
    for (const PaletteSwapRow& row : rows) out.rowNames.push_back(row.name);

    // the cycle table and each cycling ramp's order
    for (size_t k = 0; k < cycling.size(); k++) {
        const PaletteRamp& ramp = *cycling[k];
        const int orderRow = kSwapCycleRow + 1 + (int)k;
        const int length = std::min((int)ramp.colors.size(), kSwapRowsWidth - 1);
        for (int p = 0; p < length; p++) {
            const int index = ramp.colors[p];
            if (index >= count) continue;
            putColor(out.rows, rowTexel(index, kSwapCycleRow), Rgba8{(uint8_t)(p + 1), (uint8_t)length, (uint8_t)k, 0});
            putColor(out.rows, rowTexel(p, orderRow), Rgba8{(uint8_t)index, 0, 0, 0});
        }
        putSpeed(out.rows, rowTexel(255, orderRow), ramp.cycle);
    }

    uint64_t hash = 1469598103934665603ull;
    hash = hashBytes(hash, out.cells.data(), out.cells.size());
    hash = hashBytes(hash, out.rows.data(), out.rows.size());
    for (const std::string& name : out.rowNames) hash = hashBytes(hash, (const uint8_t*)name.data(), name.size() + 1);
    out.hash = hash == 0 ? 1 : hash;
    return true;
}

std::vector<PaletteSwapRow> pixelart::resolvePaletteRows(const Palette& source, const PaletteBinding& binding, std::vector<Diagnostic>& diagnostics,
                                                         std::vector<std::shared_ptr<const Palette>>* keepAlive) {
    std::vector<PaletteSwapRow> rows;
    if (binding.useVariants) {
        for (size_t v = 0; v < source.variants.size(); v++) rows.push_back({source.variants[v].name, source.variantColors((int)v)});
    }
    for (size_t t = 0; t < binding.targets.size(); t++) {
        const PaletteTarget& target = binding.targets[t];
        const std::string& path = target.palette.path;
        std::shared_ptr<const Palette> palette = PaletteLibrary::get(path);
        PaletteSwapRow row;
        row.name = rowName(target, palette.get());
        row.colors = source.colors;
        if (!palette) {
            diagnostics.push_back({Diagnostic::Severity::Warning, "palette_target_missing", "targets[" + std::to_string(t) + "]",
                                   "the target palette '" + path + "' cannot be read: its row shows the source's colours",
                                   "fix the path, or remove the target"});
        } else if (!target.variant.empty()) {
            const int variant = palette->findVariant(target.variant);
            if (variant < 0) {
                diagnostics.push_back({Diagnostic::Severity::Warning, "palette_target_missing", "targets[" + std::to_string(t) + "]",
                                       "'" + path + "' has no variant '" + target.variant + "': its row shows the source's colours",
                                       "use one of the palette's variant names"});
            } else {
                const std::vector<Rgba8> colors = palette->variantColors(variant);
                for (size_t i = 0; i < row.colors.size() && i < colors.size(); i++) row.colors[i] = colors[i];
            }
        } else {
            for (size_t i = 0; i < row.colors.size() && i < palette->colors.size(); i++) row.colors[i] = palette->colors[i];
        }
        if (palette && keepAlive && path.rfind("runtime:", 0) == 0) keepAlive->push_back(palette);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::shared_ptr<PaletteSwapTextures> PaletteSwapCache::identity() {
    SwapCache& cache = swapCache();
    if (!cache.identity) {
        auto textures = std::make_shared<PaletteSwapTextures>();
        PaletteSwapData data;
        identityPaletteSwapData(data);
        createSwapTextures(*textures, data);
        textures->identity = true;
        cache.identity = textures;
    }
    return cache.identity;
}

std::shared_ptr<PaletteSwapTextures> PaletteSwapCache::acquire(const PaletteBinding& binding, std::vector<Diagnostic>& diagnostics) {
    if (binding.source.empty()) {
        diagnostics.push_back({Diagnostic::Severity::Warning, "palette_source_missing", "source",
                               "Palette Swap has no Source Palette: the sprite draws unchanged", "set the material's Source Palette"});
        return identity();
    }
    std::vector<Diagnostic> sourceDiagnostics;
    std::shared_ptr<const Palette> source = PaletteLibrary::get(binding.source.path, &sourceDiagnostics);
    if (!source) {
        diagnostics.insert(diagnostics.end(), sourceDiagnostics.begin(), sourceDiagnostics.end());
        diagnostics.push_back({Diagnostic::Severity::Error, "palette_source_missing", "source",
                               "the Source Palette '" + binding.source.path + "' cannot be read: the sprite draws unchanged",
                               "fix the path, or the palette's errors"});
        return identity();
    }
    std::vector<std::shared_ptr<const Palette>> keepAlive;
    if (binding.source.rfind("runtime:", 0) == 0) keepAlive.push_back(source);
    const std::vector<PaletteSwapRow> rows = resolvePaletteRows(*source, binding, diagnostics, &keepAlive);
    SwapCache& cache = swapCache();
    const std::string key = bindingKey(binding);
    auto known = cache.byBinding.find(key);
    if (known != cache.byBinding.end()) {
        auto it = cache.entries.find(known->second);
        if (it != cache.entries.end()) {
            it->second.idleSince = -1.0;
            holdPalettes(*it->second.textures, keepAlive);
            return it->second.textures;
        }
        cache.byBinding.erase(known);
    }
    PaletteSwapData data;
    cache.builds++;
    if (!buildPaletteSwapData(*source, rows, data, diagnostics)) return identity();
    cache.byBinding[key] = data.hash;

    auto it = cache.entries.find(data.hash);
    if (it != cache.entries.end()) {
        it->second.idleSince = -1.0;
        holdPalettes(*it->second.textures, keepAlive);
        return it->second.textures;
    }
    auto textures = std::make_shared<PaletteSwapTextures>();
    if (!createSwapTextures(*textures, data)) {
        diagnostics.push_back({Diagnostic::Severity::Error, "palette_texture_failed", "", "the Palette Swap textures could not be made",
                               "see the log for the graphics error"});
        return identity();
    }
    textures->palettes = std::move(keepAlive);
    cache.entries[data.hash] = CacheEntry{textures, -1.0};
    return textures;
}

void PaletteSwapCache::sweep(double nowSeconds) {
    SwapCache& cache = swapCache();
    for (auto it = cache.entries.begin(); it != cache.entries.end();) {
        CacheEntry& entry = it->second;
        if (entry.textures.use_count() > 1) {
            entry.idleSince = -1.0;
        } else if (entry.idleSince < 0.0) {
            entry.idleSince = nowSeconds;
        } else if (nowSeconds - entry.idleSince >= kSwapIdleSeconds) {
            it = cache.entries.erase(it);
            continue;
        }
        ++it;
    }
    for (auto it = cache.byBinding.begin(); it != cache.byBinding.end();) {
        if (cache.entries.count(it->second) == 0) it = cache.byBinding.erase(it);
        else ++it;
    }
}

void PaletteSwapCache::clear() {
    SwapCache& cache = swapCache();
    cache.entries.clear();
    cache.byBinding.clear();
    cache.identity.reset();
}

size_t PaletteSwapCache::builds() {
    return swapCache().builds;
}

size_t PaletteSwapCache::size() {
    SwapCache& cache = swapCache();
    return cache.entries.size() + (cache.identity ? 1 : 0);
}
