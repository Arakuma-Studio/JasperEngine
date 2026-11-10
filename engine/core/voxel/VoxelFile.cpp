// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelFile.h"

#include "VoxelCodec.h"

#include "zstd.h"

#include <algorithm>
#include <atomic>
#include <cstring>

using namespace jasperengine::voxel;

namespace {

    constexpr size_t CHANNEL_BYTES = BLOCK_SAMPLES * 2;
    constexpr size_t TRAILER_BYTES = 8;

    // Explicit bytes, never a struct dump (R74)
    class Writer {
    public:
        explicit Writer(std::vector<uint8_t>& out): out(out) {}
        void u8(uint8_t v) { out.push_back(v); }
        void u16(uint16_t v) {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        void u32(uint32_t v) {
            for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        void u64(uint64_t v) {
            for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        void f32(float v) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            u32(bits);
        }
        void bytes(const uint8_t* data, size_t size) { out.insert(out.end(), data, data + size); }
        std::vector<uint8_t>& out;
    };

    class Reader {
    public:
        Reader(const uint8_t* data, size_t size): data(data), size(size) {}
        bool has(size_t count) const { return at <= size && size - at >= count; }
        uint8_t u8() { return data[at++]; }
        uint16_t u16() {
            const uint16_t v = static_cast<uint16_t>(data[at] | (static_cast<uint16_t>(data[at + 1]) << 8));
            at += 2;
            return v;
        }
        uint32_t u32() {
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(data[at + static_cast<size_t>(i)]) << (8 * i);
            at += 4;
            return v;
        }
        uint64_t u64() {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(data[at + static_cast<size_t>(i)]) << (8 * i);
            at += 8;
            return v;
        }
        float f32() {
            const uint32_t bits = u32();
            float v = 0.0f;
            std::memcpy(&v, &bits, 4);
            return v;
        }
        const uint8_t* data;
        size_t size;
        size_t at = 0;
    };

    struct DirectoryEntry {
        BlockKey key;
        uint8_t sdfCodec = 0, materialCodec = 0, flags = 0;
        int16_t sdfUniform = 0;
        uint16_t materialUniform = 0;
        uint32_t offset = 0, sdfBytes = 0, materialBytes = 0;
    };

    constexpr uint8_t ENTRY_MODIFIED = 1;

    bool decodeChannel(uint8_t codec, const uint8_t* bytes, size_t size, uint8_t* out) {
        switch (static_cast<VoxelCodec>(codec)) {
            case VoxelCodec::Raw:
                if (size != CHANNEL_BYTES) return false;
                for (size_t i = 0; i < CHANNEL_BYTES; i += 2) {
                    const uint16_t value = static_cast<uint16_t>(bytes[i] | (static_cast<uint16_t>(bytes[i + 1]) << 8));
                    std::memcpy(out + i, &value, 2);
                }
                return true;
            case VoxelCodec::Zstd: {
                std::vector<uint8_t> raw(CHANNEL_BYTES);
                const size_t produced = ZSTD_decompress(raw.data(), raw.size(), bytes, size);
                if (ZSTD_isError(produced) || produced != CHANNEL_BYTES) return false;
                for (size_t i = 0; i < CHANNEL_BYTES; i += 2) {
                    const uint16_t value = static_cast<uint16_t>(raw[i] | (static_cast<uint16_t>(raw[i + 1]) << 8));
                    std::memcpy(out + i, &value, 2);
                }
                return true;
            }
            case VoxelCodec::Drle16: {
                uint16_t values[BLOCK_SAMPLES];
                if (!drle16Decode(bytes, size, values, BLOCK_SAMPLES)) return false;
                std::memcpy(out, values, CHANNEL_BYTES);
                return true;
            }
            case VoxelCodec::Uniform:
                break;
        }
        return false;
    }

    // Decodes one entry; false on a payload the file's check did not catch
    bool decodeEntry(const DirectoryEntry& entry, const uint8_t* payload, size_t payloadSize, BlockContent& out) {
        int16_t sdf[BLOCK_SAMPLES];
        uint16_t material[BLOCK_SAMPLES];
        const bool sdfCoded = static_cast<VoxelCodec>(entry.sdfCodec) != VoxelCodec::Uniform;
        const bool materialCoded = static_cast<VoxelCodec>(entry.materialCodec) != VoxelCodec::Uniform;
        const uint64_t end = static_cast<uint64_t>(entry.offset) + entry.sdfBytes + entry.materialBytes;
        if (end > payloadSize) return false;
        if (sdfCoded && !decodeChannel(entry.sdfCodec, payload + entry.offset, entry.sdfBytes, reinterpret_cast<uint8_t*>(sdf))) return false;
        if (materialCoded && !decodeChannel(entry.materialCodec, payload + entry.offset + entry.sdfBytes, entry.materialBytes, reinterpret_cast<uint8_t*>(material))) return false;
        out = BlockContent();
        out.sdfUniform = entry.sdfUniform;
        out.materialUniform = entry.materialUniform;
        // A channel the file says is non-uniform still collapses if its values are equal
        if (sdfCoded && materialCoded) {
            out = makeContent(sdf, material);
        } else if (sdfCoded || materialCoded) {
            int16_t sdfUniformArray[BLOCK_SAMPLES];
            uint16_t materialUniformArray[BLOCK_SAMPLES];
            if (!sdfCoded) std::fill_n(sdfUniformArray, BLOCK_SAMPLES, entry.sdfUniform);
            if (!materialCoded) std::fill_n(materialUniformArray, BLOCK_SAMPLES, entry.materialUniform);
            out = makeContent(sdfCoded ? sdf : sdfUniformArray, materialCoded ? material : materialUniformArray);
        }
        return true;
    }

    // Cold blocks over the file's bytes (R7). A failed decode reads as air and logs through the store's owner
    class FileColdSource final : public ColdSource {
    public:
        FileColdSource(std::span<const uint8_t> file, std::shared_ptr<const void> owner, std::vector<DirectoryEntry> entries, size_t payloadOffset, size_t payloadBytes)
            : fileBytes(file), holder(std::move(owner)), directory(std::move(entries)), offset(payloadOffset), payloadSize(payloadBytes) {}

        BlockContent decode(uint32_t index) const override {
            BlockContent out;
            if (index >= directory.size() || !decodeEntry(directory[index], fileBytes.data() + offset, payloadSize, out)) {
                failures.fetch_add(1, std::memory_order_relaxed);
                return BlockContent();
            }
            return out;
        }
        // A view keeps no copy: the web's cold bytes are not doubled (R54), and the budget still counts them
        size_t byteSize() const override { return fileBytes.size(); }

    private:
        std::span<const uint8_t> fileBytes;
        std::shared_ptr<const void> holder;
        std::vector<DirectoryEntry> directory;
        size_t offset;
        size_t payloadSize;
        mutable std::atomic<uint32_t> failures{0};
    };

    struct ParsedFile {
        VolumeFileResult result;
        std::vector<DirectoryEntry> entries;
        size_t payloadOffset = 0;
        size_t payloadBytes = 0;
    };

    void fail(VolumeFileResult& result, FileError error, std::string message) {
        result.ok = false;
        result.error = error;
        result.message = std::move(message);
    }

    bool sectionFits(uint64_t offset, uint64_t bytes, uint64_t end) {
        return offset <= end && bytes <= end - offset;
    }

    void parseTerrainCut(const uint8_t* data, size_t size, TerrainCutRuns& cut, bool& valid) {
        valid = true;
        if (size == 0) return;
        Reader reader(data, size);
        if (!reader.has(12)) {
            valid = false;
            return;
        }
        cut.width = static_cast<int>(reader.u32());
        cut.height = static_cast<int>(reader.u32());
        const uint32_t count = reader.u32();
        if (cut.width <= 0 || cut.height <= 0 || cut.width > 65536 || cut.height > 65536 || !reader.has(static_cast<size_t>(count) * 12)) {
            valid = false;
            return;
        }
        cut.runs.resize(count);
        for (TerrainCutRuns::Run& run : cut.runs) {
            run.row = static_cast<int>(reader.u32());
            run.start = static_cast<int>(reader.u32());
            run.length = static_cast<int>(reader.u32());
            if (run.row < 0 || run.row >= cut.height || run.start < 0 || run.length <= 0 || run.start >= cut.width || run.length > cut.width - run.start) {
                valid = false;
                return;
            }
        }
    }

    ParsedFile parse(std::span<const uint8_t> bytes, int64_t maxBytes) {
        ParsedFile parsed;
        VolumeFileResult& result = parsed.result;
        if (bytes.size() < 8) {
            fail(result, FileError::Truncated, "the file is too short to be a .voxelvolume");
            return parsed;
        }
        if (std::memcmp(bytes.data(), "JVOX", 4) != 0) {
            fail(result, FileError::BadMagic, "not a .voxelvolume file");
            return parsed;
        }
        Reader reader(bytes.data(), bytes.size());
        reader.at = 4;
        VoxelFileInfo& info = result.info;
        info.version = reader.u16();
        const uint16_t headerBytes = reader.u16();
        // R75: a newer major is refused by name, and the file is left alone
        if (info.version > VOXEL_FILE_VERSION) {
            fail(result, FileError::NewerVersion, "the file is version " + std::to_string(info.version) + ", this build reads up to version " + std::to_string(VOXEL_FILE_VERSION));
            return parsed;
        }
        if (info.version == 0 || headerBytes < VOXEL_FILE_HEADER_BYTES || bytes.size() < static_cast<size_t>(headerBytes) + TRAILER_BYTES) {
            fail(result, bytes.size() < VOXEL_FILE_HEADER_BYTES + TRAILER_BYTES ? FileError::Truncated : FileError::BadHeader, "the header is damaged");
            return parsed;
        }
        if (maxBytes > 0 && static_cast<int64_t>(bytes.size()) > maxBytes) {
            fail(result, FileError::TooLarge, "the file alone does not fit the memory budget");
            return parsed;
        }
        const size_t bodyBytes = bytes.size() - TRAILER_BYTES;
        Reader trailer(bytes.data() + bodyBytes, TRAILER_BYTES);
        if (trailer.u64() != fnv1a(bytes.data(), bodyBytes)) {
            fail(result, FileError::Checksum, "the file's check does not match: it is truncated or damaged");
            return parsed;
        }
        info.fileBytes = bytes.size();
        info.flags = reader.u32();
        const uint16_t bx = reader.u16(), by = reader.u16(), bz = reader.u16();
        info.blocks = Int3{bx, by, bz};
        info.levels = reader.u8();
        info.slotCount = reader.u8();
        info.voxelSize = reader.f32();
        info.baseHash = reader.u64();
        const uint32_t generatorOffset = reader.u32(), generatorBytes = reader.u32();
        const uint32_t cutOffset = reader.u32(), cutBytes = reader.u32();
        const uint32_t directoryOffset = reader.u32(), entryCount = reader.u32();
        const uint32_t payloadOffset = reader.u32(), payloadBytes = reader.u32();
        info.entryCount = entryCount;
        if (bx == 0 || by == 0 || bz == 0 || info.levels < 1 || info.levels > MAX_LEVELS || info.slotCount > MAX_SLOTS || !(info.voxelSize > 0.0f)
            || !sectionFits(generatorOffset, generatorBytes, bodyBytes) || !sectionFits(cutOffset, cutBytes, bodyBytes)
            || !sectionFits(directoryOffset, static_cast<uint64_t>(entryCount) * VOXEL_FILE_DIRECTORY_ENTRY_BYTES, bodyBytes)
            || !sectionFits(payloadOffset, payloadBytes, bodyBytes)) {
            fail(result, FileError::BadHeader, "the header's sections do not fit the file");
            return parsed;
        }
        info.generatorJson.assign(reinterpret_cast<const char*>(bytes.data()) + generatorOffset, generatorBytes);
        bool cutValid = true;
        parseTerrainCut(bytes.data() + cutOffset, cutBytes, info.terrainCut, cutValid);
        if (!cutValid) {
            fail(result, FileError::BadHeader, "the terrain-cut section is damaged");
            return parsed;
        }
        const VolumeDesc desc = info.desc();
        const int levels = levelsFor(desc.dimensions, info.levels);
        const auto validKey = [&](const BlockKey& key) {
            if (key.level >= levels) return false;
            const int span = BLOCK_SIZE << key.level;
            return key.x < (desc.dimensions.x + span - 1) / span && key.y < (desc.dimensions.y + span - 1) / span && key.z < (desc.dimensions.z + span - 1) / span;
        };
        Reader directory(bytes.data() + directoryOffset, static_cast<size_t>(entryCount) * VOXEL_FILE_DIRECTORY_ENTRY_BYTES);
        parsed.entries.resize(entryCount);
        for (uint32_t i = 0; i < entryCount; i++) {
            DirectoryEntry& entry = parsed.entries[i];
            entry.key.level = directory.u8();
            entry.sdfCodec = directory.u8();
            entry.materialCodec = directory.u8();
            entry.flags = directory.u8();
            entry.key.x = directory.u16();
            entry.key.y = directory.u16();
            entry.key.z = directory.u16();
            entry.sdfUniform = static_cast<int16_t>(directory.u16());
            entry.materialUniform = directory.u16();
            entry.offset = directory.u32();
            entry.sdfBytes = directory.u32();
            entry.materialBytes = directory.u32();
            directory.at += 2;
            const uint64_t end = static_cast<uint64_t>(entry.offset) + entry.sdfBytes + entry.materialBytes;
            if (!validKey(entry.key) || entry.sdfCodec > 3 || entry.materialCodec > 3 || end > payloadBytes
                || (i > 0 && !(parsed.entries[i - 1].key < entry.key))) {
                fail(result, FileError::BadDirectory, "the block directory is damaged");
                return parsed;
            }
        }
        parsed.payloadOffset = payloadOffset;
        parsed.payloadBytes = payloadBytes;
        result.ok = true;
        return parsed;
    }

    uint8_t bestEncoding(const uint8_t* rawLittleEndian, const uint16_t* values, const SaveOptions& options, std::vector<uint8_t>& out) {
        out.clear();
        std::vector<uint8_t> candidate;
        drle16Encode(values, BLOCK_SAMPLES, candidate);
        uint8_t codec = static_cast<uint8_t>(VoxelCodec::Drle16);
        out = candidate;
        if (options.mode == SaveOptions::Mode::Delta) return codec;
        if (options.compress) {
            candidate.clear();
            if (options.compress(rawLittleEndian, CHANNEL_BYTES, candidate) && candidate.size() < out.size()) {
                out = candidate;
                codec = static_cast<uint8_t>(VoxelCodec::Zstd);
            }
        }
        if (out.size() >= CHANNEL_BYTES) {
            out.assign(rawLittleEndian, rawLittleEndian + CHANNEL_BYTES);
            codec = static_cast<uint8_t>(VoxelCodec::Raw);
        }
        return codec;
    }

    void writeChannelBytes(const uint16_t* values, uint8_t* out) {
        for (int i = 0; i < BLOCK_SAMPLES; i++) {
            out[i * 2] = static_cast<uint8_t>(values[i]);
            out[i * 2 + 1] = static_cast<uint8_t>(values[i] >> 8);
        }
    }

}

const char* jasperengine::voxel::fileErrorCode(FileError error) {
    switch (error) {
        case FileError::None: return "";
        case FileError::BadMagic: return "bad_magic";
        case FileError::Truncated: return "truncated";
        case FileError::Checksum: return "checksum";
        case FileError::NewerVersion: return "newer_version";
        case FileError::BadHeader: return "bad_header";
        case FileError::BadDirectory: return "bad_directory";
        case FileError::TooLarge: return "too_large";
        case FileError::BaseChanged: return "base_changed";
        case FileError::DimensionMismatch: return "dimension_mismatch";
    }
    return "";
}

VolumeDesc VoxelFileInfo::desc() const {
    VolumeDesc out;
    out.dimensions = Int3{blocks.x * BLOCK_SIZE, blocks.y * BLOCK_SIZE, blocks.z * BLOCK_SIZE};
    out.voxelSize = voxelSize;
    out.levels = levels;
    out.bounds = (flags & VOXEL_FILE_OPEN_BOUNDS) ? BoundsMode::Open : BoundsMode::Closed;
    return out;
}

std::vector<uint8_t> TerrainCutRuns::toMask() const {
    std::vector<uint8_t> mask(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    for (const Run& run : runs) {
        std::fill_n(mask.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(run.row) * static_cast<size_t>(width) + static_cast<size_t>(run.start)), run.length, static_cast<uint8_t>(1));
    }
    return mask;
}

TerrainCutRuns TerrainCutRuns::fromMask(const uint8_t* mask, int width, int height) {
    TerrainCutRuns out;
    out.width = width;
    out.height = height;
    for (int row = 0; row < height; row++) {
        const uint8_t* line = mask + static_cast<size_t>(row) * static_cast<size_t>(width);
        int x = 0;
        while (x < width) {
            if (!line[x]) {
                x++;
                continue;
            }
            int end = x + 1;
            while (end < width && line[end]) end++;
            out.runs.push_back(Run{row, x, end - x});
            x = end;
        }
    }
    return out;
}

VolumeFileResult jasperengine::voxel::readVolumeFileInfo(std::span<const uint8_t> bytes, int64_t maxBytes) {
    return parse(bytes, maxBytes).result;
}

uint64_t jasperengine::voxel::volumeFileHash(std::span<const uint8_t> bytes) {
    if (bytes.size() < TRAILER_BYTES) return 0;
    Reader trailer(bytes.data() + bytes.size() - TRAILER_BYTES, TRAILER_BYTES);
    return trailer.u64();
}

VolumeFileResult jasperengine::voxel::loadVolumeFile(std::span<const uint8_t> bytes, Store& store, std::shared_ptr<const void> owner, int64_t maxBytes) {
    ParsedFile parsed = parse(bytes, maxBytes);
    if (!parsed.result.ok) return parsed.result;
    const VolumeDesc desc = parsed.result.info.desc();
    if (desc.dimensions != store.desc().dimensions || levelsFor(desc.dimensions, desc.levels) != store.levels()) {
        fail(parsed.result, FileError::DimensionMismatch, "the store was not made for this file");
        return parsed.result;
    }
    std::vector<std::pair<BlockKey, uint32_t>> coldSlots;
    for (uint32_t i = 0; i < parsed.entries.size(); i++) {
        const DirectoryEntry& entry = parsed.entries[i];
        const bool bothUniform = static_cast<VoxelCodec>(entry.sdfCodec) == VoxelCodec::Uniform && static_cast<VoxelCodec>(entry.materialCodec) == VoxelCodec::Uniform;
        if (bothUniform) {
            BlockContent content;
            content.sdfUniform = entry.sdfUniform;
            content.materialUniform = entry.materialUniform;
            store.setLoadedSlot(entry.key, content, (entry.flags & ENTRY_MODIFIED) != 0);
        } else {
            coldSlots.emplace_back(entry.key, i);
        }
    }
    const std::vector<DirectoryEntry> flags = parsed.entries;
    store.setColdSource(std::make_shared<FileColdSource>(bytes, std::move(owner), std::move(parsed.entries), parsed.payloadOffset, parsed.payloadBytes));
    for (const auto& item : coldSlots) {
        store.setColdSlot(item.first, item.second, (flags[item.second].flags & ENTRY_MODIFIED) != 0);
    }
    store.bumpVersion();
    return parsed.result;
}

VolumeFileResult jasperengine::voxel::loadDeltaFile(std::span<const uint8_t> bytes, Store& store, uint64_t expectedBaseHash) {
    ParsedFile parsed = parse(bytes, 0);
    if (!parsed.result.ok) return parsed.result;
    if (!(parsed.result.info.flags & VOXEL_FILE_DELTA)) {
        fail(parsed.result, FileError::BadHeader, "this file is not a saved delta");
        return parsed.result;
    }
    if (parsed.result.info.desc().dimensions != store.desc().dimensions) {
        fail(parsed.result, FileError::DimensionMismatch, "the saved changes are for a volume of another size");
        return parsed.result;
    }
    if (parsed.result.info.baseHash != expectedBaseHash) {
        fail(parsed.result, FileError::BaseChanged, "the volume changed since these changes were saved");
        return parsed.result;
    }
    // All or nothing: decode everything before the first write
    std::vector<std::pair<BlockKey, BlockContent>> decoded;
    decoded.reserve(parsed.entries.size());
    const uint8_t* payload = bytes.data() + parsed.payloadOffset;
    for (const DirectoryEntry& entry : parsed.entries) {
        BlockContent content;
        if (!decodeEntry(entry, payload, parsed.payloadBytes, content)) {
            fail(parsed.result, FileError::BadDirectory, "a saved block does not decode");
            return parsed.result;
        }
        decoded.emplace_back(entry.key, std::move(content));
    }
    for (const auto& item : decoded) store.writeLoadedDelta(item.first, item.second);
    if (!decoded.empty()) store.bumpVersion();
    return parsed.result;
}

std::vector<uint8_t> jasperengine::voxel::saveVolumeFile(const Store& store, const SaveOptions& options) {
    const VolumeDesc& desc = store.desc();
    const bool cooked = options.mode == SaveOptions::Mode::Cooked;
    const bool delta = options.mode == SaveOptions::Mode::Delta;
    const std::string generator = (cooked && !options.keepGenerator) ? std::string() : options.generatorJson;

    std::vector<DirectoryEntry> entries;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> encoded;

    for (int level = 0; level < store.levels(); level++) {
        const Int3 count = store.blockCount(level);
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) {
                    const BlockKey key{level, x, y, z};
                    const Slot& slot = store.slot(key);
                    if (delta && !slot.edited()) continue;
                    const bool keptBase = cooked && options.keepGenerator;
                    if (!cooked && !delta && !slot.modified()) continue;
                    if (keptBase && !slot.modified()) continue;
                    const BlockContent content = store.readBlock(key);
                    // R76: edited back to the base is not stored. A cooked file has no base left, so air is its base.
                    if (!delta && !cooked && store.equalsBase(key, content)) continue;
                    if (keptBase && store.equalsBase(key, content)) continue;
                    if (cooked && !keptBase && content.isFullyUniform() && content.sdfUniform == SDF_AIR && content.materialUniform == 0) continue;

                    DirectoryEntry entry;
                    entry.key = key;
                    entry.flags = slot.modified() ? ENTRY_MODIFIED : 0;
                    entry.offset = static_cast<uint32_t>(payload.size());
                    entry.sdfUniform = content.sdfIsUniform() ? content.sdfValueIfUniform() : 0;
                    entry.materialUniform = content.materialIsUniform() ? content.materialValueIfUniform() : 0;
                    uint8_t raw[CHANNEL_BYTES];
                    if (!content.sdfIsUniform()) {
                        const auto* values = reinterpret_cast<const uint16_t*>(content.data->sdf());
                        writeChannelBytes(values, raw);
                        entry.sdfCodec = bestEncoding(raw, values, options, encoded);
                        entry.sdfBytes = static_cast<uint32_t>(encoded.size());
                        payload.insert(payload.end(), encoded.begin(), encoded.end());
                    }
                    if (!content.materialIsUniform()) {
                        const uint16_t* values = content.data->material();
                        writeChannelBytes(values, raw);
                        entry.materialCodec = bestEncoding(raw, values, options, encoded);
                        entry.materialBytes = static_cast<uint32_t>(encoded.size());
                        payload.insert(payload.end(), encoded.begin(), encoded.end());
                    }
                    entries.push_back(entry);
                }
            }
        }
    }

    std::vector<uint8_t> cutSection;
    if (!options.terrainCut.empty() || options.terrainCut.width > 0) {
        Writer cut(cutSection);
        cut.u32(static_cast<uint32_t>(options.terrainCut.width));
        cut.u32(static_cast<uint32_t>(options.terrainCut.height));
        cut.u32(static_cast<uint32_t>(options.terrainCut.runs.size()));
        for (const TerrainCutRuns::Run& run : options.terrainCut.runs) {
            cut.u32(static_cast<uint32_t>(run.row));
            cut.u32(static_cast<uint32_t>(run.start));
            cut.u32(static_cast<uint32_t>(run.length));
        }
    }

    const uint32_t generatorOffset = VOXEL_FILE_HEADER_BYTES;
    const uint32_t cutOffset = generatorOffset + static_cast<uint32_t>(generator.size());
    const uint32_t directoryOffset = cutOffset + static_cast<uint32_t>(cutSection.size());
    const uint32_t payloadOffset = directoryOffset + static_cast<uint32_t>(entries.size()) * VOXEL_FILE_DIRECTORY_ENTRY_BYTES;

    std::vector<uint8_t> file;
    file.reserve(payloadOffset + payload.size() + TRAILER_BYTES);
    Writer out(file);
    out.bytes(reinterpret_cast<const uint8_t*>("JVOX"), 4);
    out.u16(VOXEL_FILE_VERSION);
    out.u16(VOXEL_FILE_HEADER_BYTES);
    uint32_t flags = 0;
    if (delta) flags |= VOXEL_FILE_DELTA;
    if (cooked) flags |= VOXEL_FILE_COOKED;
    if (desc.bounds == BoundsMode::Open) flags |= VOXEL_FILE_OPEN_BOUNDS;
    out.u32(flags);
    out.u16(static_cast<uint16_t>(desc.dimensions.x / BLOCK_SIZE));
    out.u16(static_cast<uint16_t>(desc.dimensions.y / BLOCK_SIZE));
    out.u16(static_cast<uint16_t>(desc.dimensions.z / BLOCK_SIZE));
    out.u8(static_cast<uint8_t>(store.levels()));
    out.u8(static_cast<uint8_t>(options.slotCount));
    out.f32(desc.voxelSize);
    out.u64(options.baseHash);
    out.u32(generatorOffset);
    out.u32(static_cast<uint32_t>(generator.size()));
    out.u32(cutOffset);
    out.u32(static_cast<uint32_t>(cutSection.size()));
    out.u32(directoryOffset);
    out.u32(static_cast<uint32_t>(entries.size()));
    out.u32(payloadOffset);
    out.u32(static_cast<uint32_t>(payload.size()));
    out.bytes(reinterpret_cast<const uint8_t*>(generator.data()), generator.size());
    out.bytes(cutSection.data(), cutSection.size());
    for (const DirectoryEntry& entry : entries) {
        out.u8(static_cast<uint8_t>(entry.key.level));
        out.u8(entry.sdfCodec);
        out.u8(entry.materialCodec);
        out.u8(entry.flags);
        out.u16(static_cast<uint16_t>(entry.key.x));
        out.u16(static_cast<uint16_t>(entry.key.y));
        out.u16(static_cast<uint16_t>(entry.key.z));
        out.u16(static_cast<uint16_t>(entry.sdfUniform));
        out.u16(entry.materialUniform);
        out.u32(entry.offset);
        out.u32(entry.sdfBytes);
        out.u32(entry.materialBytes);
        out.u16(0);
    }
    out.bytes(payload.data(), payload.size());
    out.u64(fnv1a(file.data(), file.size()));
    return file;
}
