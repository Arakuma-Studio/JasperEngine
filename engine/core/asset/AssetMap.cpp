// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetMap.h"

#include "Data.h"
#include "Log.h"

#include <algorithm>
#include <cstring>
#include <mutex>

using namespace jasperengine;

// assetmap.bin, little-endian:
//   "JEAM", u32 version (1), u32 entry count, u32 blob size
//   entries sorted by GUID: 16 GUID bytes (Guid::toBytes order), u32 blob offset
//   blob: per record one byte (low 7 bits AssetType, bit 7 = former GUID), the path, NUL

namespace {

    constexpr uint32_t mapVersion = 1;
    constexpr size_t headerSize = 16;
    constexpr size_t entrySize = 20;
    constexpr uint8_t formerFlag = 0x80;

    void putU32(std::vector<uint8_t>& out, uint32_t value) {
        for (int i = 0; i < 4; i++) {
            out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    uint32_t getU32(const uint8_t* bytes) {
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
               (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    }

    struct LoadedMap {
        std::vector<uint8_t> bytes;    // the whole file
        uint32_t count = 0;
        const uint8_t* entries = nullptr;
        const char* blob = nullptr;
        uint32_t blobSize = 0;
        std::vector<uint32_t> byPath;  // live entry indices sorted by path, built on the first path query

        Guid guidAt(uint32_t index) const { return Guid::fromBytes(entries + index * entrySize); }
        uint32_t offsetAt(uint32_t index) const { return getU32(entries + index * entrySize + 16); }
        uint8_t flagsAt(uint32_t index) const { return static_cast<uint8_t>(blob[offsetAt(index)]); }
        const char* pathAt(uint32_t index) const { return blob + offsetAt(index) + 1; }
    };

    std::mutex mapMutex;
    LoadedMap loaded;
    bool attempted = false;
    const AssetMap::Resolver* resolver = nullptr;

    bool parse(std::vector<uint8_t> bytes, LoadedMap& map) {
        if (bytes.size() < headerSize || std::memcmp(bytes.data(), "JEAM", 4) != 0) {
            return false;
        }
        const uint32_t version = getU32(bytes.data() + 4);
        const uint32_t count = getU32(bytes.data() + 8);
        const uint32_t blobSize = getU32(bytes.data() + 12);
        if (version != mapVersion || bytes.size() != headerSize + static_cast<size_t>(count) * entrySize + blobSize) {
            return false;
        }
        map.bytes = std::move(bytes);
        map.count = count;
        map.entries = map.bytes.data() + headerSize;
        map.blob = reinterpret_cast<const char*>(map.entries + static_cast<size_t>(count) * entrySize);
        map.blobSize = blobSize;
        map.byPath.clear();
        // Every offset must start a record that ends inside the blob
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t offset = map.offsetAt(i);
            if (offset + 1 >= blobSize || std::memchr(map.blob + offset + 1, '\0', blobSize - offset - 1) == nullptr) {
                map = LoadedMap();
                return false;
            }
        }
        return true;
    }

    // Under mapMutex
    void ensureLoaded() {
        if (attempted) {
            return;
        }
        attempted = true;
        Data data;
        const std::string path = std::string("asset://") + AssetMap::fileName;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
            Log::warn("No %s in this build: Asset GUID queries find nothing", AssetMap::fileName);
            return;
        }
        std::vector<uint8_t> bytes(data.getMemPtr(), data.getMemPtr() + data.length());
        if (!parse(std::move(bytes), loaded)) {
            Log::error("%s is malformed: Asset GUID queries find nothing", AssetMap::fileName);
        }
    }

    void ensurePathIndex() {
        if (!loaded.byPath.empty() || loaded.count == 0) {
            return;
        }
        loaded.byPath.reserve(loaded.count);
        for (uint32_t i = 0; i < loaded.count; i++) {
            if ((loaded.flagsAt(i) & formerFlag) == 0) {
                loaded.byPath.push_back(i);
            }
        }
        std::sort(loaded.byPath.begin(), loaded.byPath.end(), [](uint32_t a, uint32_t b) {
            return std::strcmp(loaded.pathAt(a), loaded.pathAt(b)) < 0;
        });
    }

    bool findLoadedPath(const std::string& path, uint32_t& index) {
        ensurePathIndex();
        auto it = std::lower_bound(loaded.byPath.begin(), loaded.byPath.end(), path, [](uint32_t entry, const std::string& value) {
            return std::strcmp(loaded.pathAt(entry), value.c_str()) < 0;
        });
        if (it == loaded.byPath.end() || path != loaded.pathAt(*it)) {
            return false;
        }
        index = *it;
        return true;
    }

}

void AssetMap::setResolver(const Resolver* value) {
    std::lock_guard<std::mutex> lock(mapMutex);
    resolver = value;
}

bool AssetMap::findPath(const Guid& guid, std::string& path, AssetType* type) {
    std::lock_guard<std::mutex> lock(mapMutex);
    if (guid.isNull()) {
        return false;
    }
    if (resolver) {
        AssetType found = AssetType::Unknown;
        if (!resolver->findPath(guid, path, found)) {
            return false;
        }
        if (type) *type = found;
        return true;
    }
    ensureLoaded();
    uint32_t low = 0;
    uint32_t high = loaded.count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        const Guid current = loaded.guidAt(middle);
        if (current < guid) {
            low = middle + 1;
        } else if (guid < current) {
            high = middle;
        } else {
            path = loaded.pathAt(middle);
            if (type) *type = static_cast<AssetType>(loaded.flagsAt(middle) & ~formerFlag);
            return true;
        }
    }
    return false;
}

Guid AssetMap::findGuid(const std::string& path) {
    std::lock_guard<std::mutex> lock(mapMutex);
    if (path.empty()) {
        return Guid();
    }
    if (resolver) {
        return resolver->findGuid(path);
    }
    ensureLoaded();
    uint32_t index = 0;
    return findLoadedPath(path, index) ? loaded.guidAt(index) : Guid();
}

AssetType AssetMap::findType(const std::string& path) {
    const Guid guid = findGuid(path);
    std::string found;
    AssetType type = AssetType::Unknown;
    return findPath(guid, found, &type) ? type : AssetType::Unknown;
}

std::vector<uint8_t> AssetMap::encode(std::vector<Entry> entries) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.guid.isNull() || entry.path.empty();
    }), entries.end());
    // One entry per GUID: a live one wins over a former one, then the first path in order
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.guid != b.guid) return a.guid < b.guid;
        if (a.former != b.former) return !a.former;
        return a.path < b.path;
    });
    entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.guid == b.guid;
    }), entries.end());

    std::vector<uint8_t> blob;
    std::vector<uint32_t> offsets;
    offsets.reserve(entries.size());
    for (const Entry& entry : entries) {
        offsets.push_back(static_cast<uint32_t>(blob.size()));
        blob.push_back(static_cast<uint8_t>((static_cast<uint8_t>(entry.type) & ~formerFlag) | (entry.former ? formerFlag : 0)));
        blob.insert(blob.end(), entry.path.begin(), entry.path.end());
        blob.push_back(0);
    }

    std::vector<uint8_t> out;
    out.reserve(headerSize + entries.size() * entrySize + blob.size());
    out.insert(out.end(), {'J', 'E', 'A', 'M'});
    putU32(out, mapVersion);
    putU32(out, static_cast<uint32_t>(entries.size()));
    putU32(out, static_cast<uint32_t>(blob.size()));
    for (size_t i = 0; i < entries.size(); i++) {
        uint8_t guidBytes[16];
        entries[i].guid.toBytes(guidBytes);
        out.insert(out.end(), guidBytes, guidBytes + 16);
        putU32(out, offsets[i]);
    }
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

bool AssetMap::loadFromMemory(const uint8_t* bytes, size_t size) {
    std::lock_guard<std::mutex> lock(mapMutex);
    attempted = true;
    loaded = LoadedMap();
    return bytes && parse(std::vector<uint8_t>(bytes, bytes + size), loaded);
}

void AssetMap::reset() {
    std::lock_guard<std::mutex> lock(mapMutex);
    attempted = false;
    loaded = LoadedMap();
}

ScriptAssetRef::ScriptAssetRef() = default;

ScriptAssetRef::ScriptAssetRef(const AssetRef& ref): ref(ref) {
}

ScriptAssetRef ScriptAssetRef::make(const std::string& guidOrPath) {
    if (Guid::isValidText(guidOrPath)) {
        const Guid guid = Guid::parse(guidOrPath);
        std::string path;
        AssetMap::findPath(guid, path);
        return ScriptAssetRef(AssetRef(path, guid));
    }
    return ScriptAssetRef(AssetRef(guidOrPath, AssetMap::findGuid(guidOrPath)));
}

const AssetRef& ScriptAssetRef::getRef() const {
    return ref;
}

std::string ScriptAssetRef::getGuid() const {
    if (!ref.guid.isNull()) {
        return ref.guid.str();
    }
    return AssetMap::findGuid(ref.path).str();
}

std::string ScriptAssetRef::getPath() const {
    std::string path;
    if (!ref.guid.isNull() && AssetMap::findPath(ref.guid, path)) {
        return path;
    }
    return ref.path;
}

std::string ScriptAssetRef::getType() const {
    std::string path;
    AssetType type = AssetType::Unknown;
    if (!ref.guid.isNull() && AssetMap::findPath(ref.guid, path, &type)) {
        return assetTypeName(type);
    }
    return assetTypeName(AssetMap::findType(ref.path));
}

bool ScriptAssetRef::isValid() const {
    std::string path;
    if (!ref.guid.isNull()) {
        return AssetMap::findPath(ref.guid, path);
    }
    return !ref.path.empty() && !AssetMap::findGuid(ref.path).isNull();
}

bool ScriptAssetRef::isEmpty() const {
    return !ref.isSet();
}
