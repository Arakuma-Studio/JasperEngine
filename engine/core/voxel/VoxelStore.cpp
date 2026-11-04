// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelStore.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine::voxel;

namespace {

    constexpr uint32_t NO_COLD_INDEX = 0xFFFFFFFFu;

    // Fills a run of a fetch, the one place that knows a row may cross into the next block
    template<class T>
    void fillRun(T* out, int count, T value) {
        for (int i = 0; i < count; i++) out[i] = value;
    }

    void fillRowSdf(const BlockContent& content, int localX, int localY, int localZ, int run, int16_t* out) {
        if (content.sdfIsUniform()) {
            fillRun<int16_t>(out, run, content.sdfValueIfUniform());
        } else {
            std::memcpy(out, content.data->sdf() + sampleIndex(localX, localY, localZ), static_cast<size_t>(run) * sizeof(int16_t));
        }
    }

    void fillRowMaterial(const BlockContent& content, int localX, int localY, int localZ, int run, uint16_t* out) {
        if (content.materialIsUniform()) {
            fillRun<uint16_t>(out, run, content.materialValueIfUniform());
        } else {
            std::memcpy(out, content.data->material() + sampleIndex(localX, localY, localZ), static_cast<size_t>(run) * sizeof(uint16_t));
        }
    }

    bool touchesVolumeEnd(const VolumeDesc& desc, const BlockKey& key) {
        const int span = BLOCK_SIZE << key.level;
        return key.x == 0 || key.y == 0 || key.z == 0 || (key.x + 1) * span >= desc.dimensions.x
            || (key.y + 1) * span >= desc.dimensions.y || (key.z + 1) * span >= desc.dimensions.z;
    }

    BlockContent evaluateBlock(const VolumeDesc& desc, const BaseSource* source, const BlockKey& key) {
        BlockContent content;
        if (!source) {
            // A store with no base is air everywhere, which is already a valid shell
            content.sdfUniform = SDF_AIR;
            content.materialUniform = 0;
            return content;
        }
        const int spacing = 1 << key.level;
        const Int3 origin{(key.x * BLOCK_SIZE) * spacing, (key.y * BLOCK_SIZE) * spacing, (key.z * BLOCK_SIZE) * spacing};
        int16_t uniformSdf = SDF_AIR;
        uint16_t uniformMaterial = 0;
        const bool uniform = source->evaluateUniform(origin, spacing, uniformSdf, uniformMaterial);
        // Interior blocks, and air at the outer layer, stay inline. Anything else takes the array path
        // so that R8's lock applies.
        if (uniform && (!touchesVolumeEnd(desc, key) || uniformSdf >= SDF_SHELL_AIR)) {
            // An Open volume's samples past the end clone the edge, so uniform stays uniform; a Closed
            // volume's read as air whatever the block holds
            content.sdfUniform = uniformSdf;
            content.materialUniform = uniformMaterial;
            return content;
        }
        int16_t sdf[BLOCK_SAMPLES];
        uint16_t material[BLOCK_SAMPLES];
        if (uniform) {
            std::fill_n(sdf, BLOCK_SAMPLES, uniformSdf);
            std::fill_n(material, BLOCK_SAMPLES, uniformMaterial);
        } else {
            source->evaluate(origin, spacing, sdf, material);
        }
        lockBaseBlock(desc, key, sdf, material);
        return makeContent(sdf, material);
    }

}

void jasperengine::voxel::lockBaseBlock(const VolumeDesc& desc, const BlockKey& key, int16_t* sdf, uint16_t* material) {
    const int spacing = 1 << key.level;
    const Int3 origin{(key.x * BLOCK_SIZE) * spacing, (key.y * BLOCK_SIZE) * spacing, (key.z * BLOCK_SIZE) * spacing};
    const Int3 dim = desc.dimensions;
    const bool closed = desc.bounds == BoundsMode::Closed;
    if (!touchesVolumeEnd(desc, key)) return;
    for (int z = 0; z < BLOCK_SIZE; z++) {
        const int pz = origin.z + z * spacing;
        for (int y = 0; y < BLOCK_SIZE; y++) {
            const int py = origin.y + y * spacing;
            for (int x = 0; x < BLOCK_SIZE; x++) {
                const int px = origin.x + x * spacing;
                const int index = sampleIndex(x, y, z);
                if (px >= dim.x || py >= dim.y || pz >= dim.z) {
                    // Past the end: never read, since the read rule answers first, so it only has to
                    // be a value that lets the block collapse
                    const int nx = std::min(x, (dim.x - 1 - origin.x) / spacing);
                    const int ny = std::min(y, (dim.y - 1 - origin.y) / spacing);
                    const int nz = std::min(z, (dim.z - 1 - origin.z) / spacing);
                    material[index] = material[sampleIndex(nx, ny, nz)];
                    sdf[index] = closed ? SDF_AIR : sdf[sampleIndex(nx, ny, nz)];
                } else if (closed && (px == 0 || py == 0 || pz == 0 || px == dim.x - 1 || py == dim.y - 1 || pz == dim.z - 1)) {
                    if (sdf[index] < SDF_SHELL_AIR) sdf[index] = SDF_SHELL_AIR;
                }
            }
        }
    }
}

// --- Slot ---

uint64_t Slot::uniformBits(int16_t sdf, uint16_t material, bool isModified, bool isEdited) {
    return static_cast<uint64_t>(Uniform) | (isModified ? MODIFIED : 0) | (isEdited ? EDITED : 0)
        | (static_cast<uint64_t>(static_cast<uint16_t>(sdf)) << 16) | (static_cast<uint64_t>(material) << 32);
}

void Slot::retain() const {
    if (kind() == Hot && hotBlock()) {
        BlockRef ref = BlockRef::share(hotBlock());
        ref.detach();
    }
}

void Slot::release() {
    if (kind() == Hot && hotBlock()) {
        BlockRef::adopt(hotBlock());
    }
}

void Slot::setUniform(int16_t sdf, uint16_t material, bool isModified, bool isEdited) {
    if (bits != 0) release();
    bits = uniformBits(sdf, material, isModified, isEdited);
}

void Slot::setBase(bool isModified, bool isEdited) {
    release();
    bits = static_cast<uint64_t>(Base) | (isModified ? MODIFIED : 0) | (isEdited ? EDITED : 0);
}

void Slot::setCold(uint32_t index, bool isModified, bool isEdited) {
    release();
    bits = static_cast<uint64_t>(Cold) | (isModified ? MODIFIED : 0) | (isEdited ? EDITED : 0) | (static_cast<uint64_t>(index) << 32);
}

void Slot::setHot(BlockRef block, bool isModified, bool isEdited) {
    release();
    bits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(block.detach())) | (isModified ? MODIFIED : 0) | (isEdited ? EDITED : 0);
}

void Slot::setContent(const BlockContent& content, bool isModified, bool isEdited) {
    if (content.data) {
        setHot(content.data, isModified, isEdited);
    } else {
        setUniform(content.sdfUniform, content.materialUniform, isModified, isEdited);
    }
}

// --- ReadView ---

const BlockContent& ReadView::contentOf(const Cell& cell) const {
    if (cell.resolved) return cell.content;
    cell.resolved = true;
    switch (cell.slot.kind()) {
        case Slot::Hot:
            cell.content.data = BlockRef::share(cell.slot.hotBlock());
            break;
        case Slot::Uniform:
            cell.content.sdfUniform = cell.slot.uniformSdf();
            cell.content.materialUniform = cell.slot.uniformMaterial();
            break;
        case Slot::Base:
            cell.content = evaluateBlock(volume, baseSource.get(), cell.key);
            resolvedBlocks.push_back({cell.key, cell.content});
            break;
        case Slot::Cold:
            cell.content = coldSource->decode(cell.slot.coldIndex());
            resolvedBlocks.push_back({cell.key, cell.content});
            break;
    }
    return cell.content;
}

const ReadView::Cell* ReadView::cellFor(int group, const BlockKey& key) const {
    const auto& cells = group == 0 ? own : next;
    const BlockKey& middle = cells[13].key;
    const int dx = key.x - middle.x, dy = key.y - middle.y, dz = key.z - middle.z;
    if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || dz < -1 || dz > 1) return nullptr;
    const Cell& cell = cells[static_cast<size_t>((dx + 1) + 3 * (dy + 1) + 9 * (dz + 1))];
    return cell.valid ? &cell : nullptr;
}

void ReadView::fetchSdf(int level, const Int3& first, const Int3& size, int16_t* out) const {
    const int group = level == centerKey.level ? 0 : 1;
    const Int3 samples{volume.dimensions.x >> level, volume.dimensions.y >> level, volume.dimensions.z >> level};
    const bool closed = volume.bounds == BoundsMode::Closed;
    for (int z = 0; z < size.z; z++) {
        int iz = first.z + z;
        const bool pastZ = iz >= samples.z;
        iz = iz < 0 ? 0 : (pastZ ? samples.z - 1 : iz);
        for (int y = 0; y < size.y; y++) {
            int iy = first.y + y;
            const bool pastY = iy >= samples.y;
            iy = iy < 0 ? 0 : (pastY ? samples.y - 1 : iy);
            int16_t* row = out + static_cast<size_t>(size.x) * (static_cast<size_t>(y) + static_cast<size_t>(size.y) * static_cast<size_t>(z));
            if (closed && (pastZ || pastY)) {
                fillRun<int16_t>(row, size.x, SDF_AIR);
                continue;
            }
            int x = 0;
            while (x < size.x) {
                const int ix = first.x + x;
                if (ix < 0 || ix >= samples.x) {
                    // One sample at a time at the ends: only a padded window's margin gets here
                    if (ix >= samples.x && closed) {
                        row[x] = SDF_AIR;
                    } else {
                        const int clamped = ix < 0 ? 0 : samples.x - 1;
                        const BlockKey key{level, clamped >> BLOCK_SHIFT, iy >> BLOCK_SHIFT, iz >> BLOCK_SHIFT};
                        const Cell* cell = cellFor(group, key);
                        row[x] = cell ? contentOf(*cell).sdfAt(sampleIndex(clamped & 15, iy & 15, iz & 15)) : SDF_AIR;
                    }
                    x++;
                    continue;
                }
                const int localX = ix & 15;
                const int run = std::min(BLOCK_SIZE - localX, size.x - x);
                const int runInside = std::min(run, samples.x - ix);
                const BlockKey key{level, ix >> BLOCK_SHIFT, iy >> BLOCK_SHIFT, iz >> BLOCK_SHIFT};
                const Cell* cell = cellFor(group, key);
                if (cell) {
                    fillRowSdf(contentOf(*cell), localX, iy & 15, iz & 15, runInside, row + x);
                } else {
                    fillRun<int16_t>(row + x, runInside, SDF_AIR);
                }
                x += runInside;
            }
        }
    }
}

void ReadView::fetchMaterial(int level, const Int3& first, const Int3& size, uint16_t* out) const {
    const int group = level == centerKey.level ? 0 : 1;
    const Int3 samples{volume.dimensions.x >> level, volume.dimensions.y >> level, volume.dimensions.z >> level};
    for (int z = 0; z < size.z; z++) {
        const int iz = std::clamp(first.z + z, 0, samples.z - 1);
        for (int y = 0; y < size.y; y++) {
            const int iy = std::clamp(first.y + y, 0, samples.y - 1);
            uint16_t* row = out + static_cast<size_t>(size.x) * (static_cast<size_t>(y) + static_cast<size_t>(size.y) * static_cast<size_t>(z));
            int x = 0;
            while (x < size.x) {
                const int ix = first.x + x;
                if (ix < 0 || ix >= samples.x) {
                    const int clamped = ix < 0 ? 0 : samples.x - 1;
                    const BlockKey key{level, clamped >> BLOCK_SHIFT, iy >> BLOCK_SHIFT, iz >> BLOCK_SHIFT};
                    const Cell* cell = cellFor(group, key);
                    row[x] = cell ? contentOf(*cell).materialAt(sampleIndex(clamped & 15, iy & 15, iz & 15)) : 0;
                    x++;
                    continue;
                }
                const int localX = ix & 15;
                const int run = std::min(std::min(BLOCK_SIZE - localX, size.x - x), samples.x - ix);
                const BlockKey key{level, ix >> BLOCK_SHIFT, iy >> BLOCK_SHIFT, iz >> BLOCK_SHIFT};
                const Cell* cell = cellFor(group, key);
                if (cell) {
                    fillRowMaterial(contentOf(*cell), localX, iy & 15, iz & 15, run, row + x);
                } else {
                    fillRun<uint16_t>(row + x, run, 0);
                }
                x += run;
            }
        }
    }
}

bool ReadView::windowAllOneSign(bool& solid) const {
    bool first = true;
    for (const Cell& cell : own) {
        bool cellSolid = false;
        if (!cell.valid) {
            // Past the end: air when Closed. Open clones the edge, which the valid cells below decide.
            if (volume.bounds == BoundsMode::Open || cell.key.x < 0 || cell.key.y < 0 || cell.key.z < 0) continue;
            cellSolid = false;
        } else {
            const BlockContent& content = contentOf(cell);
            if (!content.sdfIsUniform()) return false;
            cellSolid = content.sdfValueIfUniform() < 0;
        }
        if (first) {
            solid = cellSolid;
            first = false;
        } else if (cellSolid != solid) {
            return false;
        }
    }
    return !first;
}

bool ReadView::windowOneMaterial(uint16_t& material) const {
    bool first = true;
    for (const Cell& cell : own) {
        if (!cell.valid) continue;
        const BlockContent& content = contentOf(cell);
        if (!content.materialIsUniform()) return false;
        if (first) {
            material = content.materialValueIfUniform();
            first = false;
        } else if (content.materialValueIfUniform() != material) {
            return false;
        }
    }
    return !first;
}

std::vector<ReadView::Resolved> ReadView::takeResolved() const {
    std::vector<Resolved> out;
    out.swap(resolvedBlocks);
    return out;
}

// --- Store ---

Store::Store(const VolumeDesc& desc): volume(desc) {
    volume.levels = levelsFor(desc.dimensions, desc.levels);
    tables.resize(static_cast<size_t>(volume.levels));
    for (int level = 0; level < volume.levels; level++) {
        const int span = BLOCK_SIZE << level;
        counts[static_cast<size_t>(level)] = Int3{(volume.dimensions.x + span - 1) / span, (volume.dimensions.y + span - 1) / span, (volume.dimensions.z + span - 1) / span};
        const Int3& count = counts[static_cast<size_t>(level)];
        tables[static_cast<size_t>(level)].resize(static_cast<size_t>(count.x) * static_cast<size_t>(count.y) * static_cast<size_t>(count.z));
        tableBytes += static_cast<int64_t>(tables[static_cast<size_t>(level)].size() * sizeof(Slot));
    }
}

Store::~Store() {
    if (memoryBudget) memoryBudget->charge(-memory().total());
}

std::unique_ptr<Store> Store::clone() const {
    auto copy = std::make_unique<Store>(volume);
    copy->tables = tables;
    copy->coldIndexOf = coldIndexOf;
    copy->baseSource = baseSource;
    copy->coldSource = coldSource;
    copy->hotBytes = hotBytes;
    copy->tableBytes = tableBytes;
    copy->dataVersion = dataVersion;
    return copy;
}

Int3 Store::levelSamples(int level) const {
    return Int3{volume.dimensions.x >> level, volume.dimensions.y >> level, volume.dimensions.z >> level};
}

Int3 Store::blockCount(int level) const {
    return counts[static_cast<size_t>(level)];
}

bool Store::validKey(const BlockKey& key) const {
    if (key.level < 0 || key.level >= volume.levels) return false;
    const Int3& count = counts[static_cast<size_t>(key.level)];
    return key.x >= 0 && key.y >= 0 && key.z >= 0 && key.x < count.x && key.y < count.y && key.z < count.z;
}

int64_t Store::slotBytes(const Slot& value) {
    return value.kind() == Slot::Hot && value.hotBlock() ? static_cast<int64_t>(value.hotBlock()->payloadBytes()) : 0;
}

void Store::addBytes(int64_t delta) {
    hotBytes += delta;
    if (memoryBudget) memoryBudget->charge(delta);
}

void Store::setSlot(const BlockKey& key, Slot value) {
    Slot& target = tables[static_cast<size_t>(key.level)][slotIndex(key)];
    addBytes(slotBytes(value) - slotBytes(target));
    target = std::move(value);
}

void Store::setBase(std::shared_ptr<const BaseSource> source) {
    baseSource = std::move(source);
    resetUnmodifiedToBase();
}

void Store::resetToBase() {
    for (int level = 0; level < volume.levels; level++) {
        auto& table = tables[static_cast<size_t>(level)];
        for (Slot& slot : table) {
            addBytes(-slotBytes(slot));
            slot.setBase(false, false);
        }
    }
    dataVersion++;
}

void Store::resetUnmodifiedToBase() {
    for (int level = 0; level < volume.levels; level++) {
        auto& table = tables[static_cast<size_t>(level)];
        for (Slot& slot : table) {
            if (slot.modified()) continue;
            addBytes(-slotBytes(slot));
            slot.setBase(false, false);
        }
    }
    dataVersion++;
}

void Store::resetBlockToBase(const BlockKey& key) {
    if (slot(key).modified()) return;
    Slot value;
    value.setBase(false, false);
    setSlot(key, std::move(value));
}

void Store::setColdSource(std::shared_ptr<const ColdSource> source) {
    const int64_t before = coldSource ? static_cast<int64_t>(coldSource->byteSize()) : 0;
    coldSource = std::move(source);
    if (memoryBudget) memoryBudget->charge((coldSource ? static_cast<int64_t>(coldSource->byteSize()) : 0) - before);
    coldIndexOf.clear();
    if (coldSource) {
        coldIndexOf.resize(tables.size());
        for (size_t level = 0; level < tables.size(); level++) coldIndexOf[level].assign(tables[level].size(), NO_COLD_INDEX);
    }
}

void Store::setColdSlot(const BlockKey& key, uint32_t index, bool isModified) {
    Slot value;
    value.setCold(index, isModified, false);
    setSlot(key, std::move(value));
    coldIndexOf[static_cast<size_t>(key.level)][slotIndex(key)] = index;
}

void Store::setLoadedSlot(const BlockKey& key, const BlockContent& content, bool isModified) {
    Slot value;
    value.setContent(content, isModified, false);
    setSlot(key, std::move(value));
}

BlockContent Store::readBlock(const BlockKey& key) const {
    const Slot& value = slot(key);
    BlockContent out;
    switch (value.kind()) {
        case Slot::Hot:
            value.hotBlock()->stampUse(useClock++);
            out.data = BlockRef::share(value.hotBlock());
            break;
        case Slot::Uniform:
            out.sdfUniform = value.uniformSdf();
            out.materialUniform = value.uniformMaterial();
            break;
        case Slot::Base:
            out = evaluateBlock(volume, baseSource.get(), key);
            break;
        case Slot::Cold:
            out = coldSource->decode(value.coldIndex());
            break;
    }
    return out;
}

BlockContent Store::content(const BlockKey& key) {
    BlockContent out = readBlock(key);
    const Slot::Kind kind = slot(key).kind();
    if (kind == Slot::Base || kind == Slot::Cold) publishOne(key, out);
    return out;
}

int16_t Store::sdfAt(int level, int x, int y, int z) {
    const Int3 samples = levelSamples(level);
    const bool closed = volume.bounds == BoundsMode::Closed;
    if (closed && (x >= samples.x || y >= samples.y || z >= samples.z)) return SDF_AIR;
    x = std::clamp(x, 0, samples.x - 1);
    y = std::clamp(y, 0, samples.y - 1);
    z = std::clamp(z, 0, samples.z - 1);
    return content(BlockKey{level, x >> BLOCK_SHIFT, y >> BLOCK_SHIFT, z >> BLOCK_SHIFT}).sdfAt(sampleIndex(x & 15, y & 15, z & 15));
}

uint16_t Store::materialAt(int level, int x, int y, int z) {
    const Int3 samples = levelSamples(level);
    x = std::clamp(x, 0, samples.x - 1);
    y = std::clamp(y, 0, samples.y - 1);
    z = std::clamp(z, 0, samples.z - 1);
    return content(BlockKey{level, x >> BLOCK_SHIFT, y >> BLOCK_SHIFT, z >> BLOCK_SHIFT}).materialAt(sampleIndex(x & 15, y & 15, z & 15));
}

std::shared_ptr<const ReadView> Store::readView(const BlockKey& key, uint8_t sideMask) const {
    auto view = std::make_shared<ReadView>();
    view->volume = volume;
    view->centerKey = key;
    view->baseSource = baseSource;
    view->coldSource = coldSource;
    const auto fill = [this](std::array<ReadView::Cell, 27>& cells, const BlockKey& middle) {
        size_t index = 0;
        for (int dz = -1; dz <= 1; dz++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    ReadView::Cell& cell = cells[index++];
                    cell.key = BlockKey{middle.level, middle.x + dx, middle.y + dy, middle.z + dz};
                    cell.valid = validKey(cell.key);
                    if (cell.valid) cell.slot = slot(cell.key);
                }
            }
        }
    };
    fill(view->own, key);
    if (sideMask != 0 && key.level + 1 < volume.levels) {
        view->hasNext = true;
        fill(view->next, BlockKey{key.level + 1, key.x >> 1, key.y >> 1, key.z >> 1});
    }
    return view;
}

void Store::publish(const std::vector<ReadView::Resolved>& resolved) {
    for (const ReadView::Resolved& item : resolved) publishOne(item.key, item.content);
}

void Store::publishOne(const BlockKey& key, const BlockContent& content) {
    const Slot& current = slot(key);
    if (current.kind() != Slot::Base && current.kind() != Slot::Cold) return;
    // A resolved block is only a cache: over the budget the slot keeps its compact form (R7)
    if (content.data && !makeRoom(static_cast<int64_t>(content.payloadBytes()))) return;
    Slot value;
    value.setContent(content, current.modified(), current.edited());
    setSlot(key, std::move(value));
}

void Store::writeBlock(const BlockKey& key, const BlockContent& content) {
    Slot value;
    value.setContent(content, true, true);
    setSlot(key, std::move(value));
}

void Store::applySlot(const BlockKey& key, const Slot& slotValue) {
    setSlot(key, slotValue);
}

void Store::writeLoadedDelta(const BlockKey& key, const BlockContent& content) {
    Slot value;
    value.setContent(content, true, true);
    setSlot(key, std::move(value));
}

BlockContent Store::evaluateBase(const BlockKey& key) const {
    return evaluateBlock(volume, baseSource.get(), key);
}

bool Store::equalsBase(const BlockKey& key, const BlockContent& content) const {
    return evaluateBase(key).sameAs(content);
}

MemoryStats Store::memory() const {
    MemoryStats stats;
    stats.hotBytes = hotBytes;
    stats.coldBytes = coldSource ? static_cast<int64_t>(coldSource->byteSize()) : 0;
    stats.tableBytes = tableBytes;
    for (const auto& indices : coldIndexOf) stats.tableBytes += static_cast<int64_t>(indices.size() * sizeof(uint32_t));
    return stats;
}

void Store::setBudget(std::shared_ptr<MemoryBudget> shared) {
    if (memoryBudget) memoryBudget->charge(-memory().total());
    memoryBudget = std::move(shared);
    if (memoryBudget) memoryBudget->charge(memory().total());
}

int64_t Store::evictUnmodified(int64_t bytes) {
    struct Candidate {
        uint32_t stamp;
        int level;
        size_t index;
    };
    std::vector<Candidate> candidates;
    for (int level = 0; level < volume.levels; level++) {
        const auto& table = tables[static_cast<size_t>(level)];
        for (size_t index = 0; index < table.size(); index++) {
            const Slot& value = table[index];
            if (value.kind() != Slot::Hot || value.edited()) continue;
            const bool hasCold = !coldIndexOf.empty() && coldIndexOf[static_cast<size_t>(level)][index] != NO_COLD_INDEX;
            if (hasCold || !value.modified()) candidates.push_back({value.hotBlock()->lastUse(), level, index});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.stamp != b.stamp) return a.stamp < b.stamp;
        if (a.level != b.level) return a.level < b.level;
        return a.index < b.index;
    });
    int64_t freed = 0;
    for (const Candidate& candidate : candidates) {
        if (freed >= bytes) break;
        Slot& value = tables[static_cast<size_t>(candidate.level)][candidate.index];
        const int64_t size = slotBytes(value);
        const bool wasModified = value.modified();
        const uint32_t cold = coldIndexOf.empty() ? NO_COLD_INDEX : coldIndexOf[static_cast<size_t>(candidate.level)][candidate.index];
        if (cold != NO_COLD_INDEX) {
            value.setCold(cold, wasModified, false);
        } else {
            value.setBase(false, false);
        }
        addBytes(-size);
        freed += size;
    }
    return freed;
}

bool Store::makeRoom(int64_t delta) {
    if (!memoryBudget || memoryBudget->fits(delta)) return true;
    evictUnmodified(memoryBudget->used() + delta - memoryBudget->capacity());
    return memoryBudget->fits(delta);
}

bool Store::wouldFit(int64_t delta) const {
    if (!memoryBudget || memoryBudget->fits(delta)) return true;
    int64_t evictable = 0;
    for (int level = 0; level < volume.levels; level++) {
        const auto& table = tables[static_cast<size_t>(level)];
        for (size_t index = 0; index < table.size(); index++) {
            const Slot& value = table[index];
            if (value.kind() != Slot::Hot || value.edited()) continue;
            const bool hasCold = !coldIndexOf.empty() && coldIndexOf[static_cast<size_t>(level)][index] != NO_COLD_INDEX;
            if (hasCold || !value.modified()) evictable += slotBytes(value);
        }
    }
    return memoryBudget->fits(delta - evictable);
}

Store::BlockCounts Store::countBlocks() const {
    BlockCounts out;
    for (const auto& table : tables) {
        for (const Slot& value : table) {
            out.total++;
            if (value.modified()) out.modified++;
            switch (value.kind()) {
                case Slot::Hot: out.nonUniform++; break;
                case Slot::Uniform: out.uniform++; break;
                case Slot::Base: out.base++; break;
                case Slot::Cold: out.cold++; break;
            }
        }
    }
    return out;
}

std::vector<BlockKey> Store::editedBlocks() const {
    std::vector<BlockKey> out;
    for (int level = 0; level < volume.levels; level++) {
        const Int3& count = counts[static_cast<size_t>(level)];
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) {
                    const BlockKey key{level, x, y, z};
                    if (slot(key).edited()) out.push_back(key);
                }
            }
        }
    }
    return out;
}

void Store::clearEditedFlags() {
    for (auto& table : tables) {
        for (Slot& value : table) {
            if (!value.edited()) continue;
            Slot copy = value;
            switch (copy.kind()) {
                case Slot::Hot: {
                    BlockRef ref = BlockRef::share(copy.hotBlock());
                    value.setHot(ref, copy.modified(), false);
                    break;
                }
                case Slot::Uniform:
                    value.setUniform(copy.uniformSdf(), copy.uniformMaterial(), copy.modified(), false);
                    break;
                case Slot::Base:
                    value.setBase(copy.modified(), false);
                    break;
                case Slot::Cold:
                    value.setCold(copy.coldIndex(), copy.modified(), false);
                    break;
            }
        }
    }
}
