// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ResourcePack.h"

#include "JPak.h"
#include "Log.h"
#include "System.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#ifndef _WIN32
#include <sys/types.h>
#endif

using namespace jasperengine;

namespace {

    constexpr char PACK_MAGIC[] = {'D', 'X', 'P', 'K', '1'};
#ifndef JASPERENGINE_EDITOR
    // the editor reads its projects' files, never a pack
    constexpr const char* PACK_FILENAME = "resources.pak";
    constexpr const char* CONTENT_PACK_FILENAME = "content.jpak";
#endif

#ifdef _WIN32
    typedef __int64 pack_offset_t;
#else
    typedef off_t pack_offset_t;
#endif

    // Still 32-bit on the armeabi-v7a and x86 ABIs, so a larger pack is rejected
    constexpr uint64_t MAX_PACK_OFFSET = static_cast<uint64_t>(std::numeric_limits<pack_offset_t>::max());

    struct PackEntry {
        uint64_t offset = 0;
        uint64_t size = 0;
        uint8_t key = 0;
        uint32_t shift = 0;
    };

    struct PackState {
        std::string filename;
        std::map<std::string, PackEntry> entries;
        // the .jpak content pack (mounted, or found next to the executable)
        std::unique_ptr<JPak> content;
    };

    // Same static-destruction guard as TextureDataPool::getMap()
    PackState& state() {
        static PackState* packState = new PackState();
        return *packState;
    }

    // Filled once, then read-only, so lookups take no lock
    std::once_flag loadFlag;

    // Keys are '/'-separated and relative to the export root. "." and ".." resolve
    // like FileData::simplifyPath, so a glTF "models/../textures/a.png" still hits.
    std::string normalizePath(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');

        std::vector<std::string> parts;
        for (size_t start = 0; start <= path.size(); ) {
            const size_t end = path.find('/', start);
            const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);

            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }

            if (end == std::string::npos) break;
            start = end + 1;
        }

        std::string result;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) result += '/';
            result += parts[i];
        }
        return result;
    }

    bool startsWith(const std::string& value, const std::string& prefix) {
        return value.rfind(prefix, 0) == 0;
    }

    // Follows FileData::getSystemPath, but only the two packed roots have keys
    std::string getPackKey(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.empty()) return "";

        if (startsWith(path, "asset://")) return "assets/" + normalizePath(path.substr(8));
        if (startsWith(path, "lua://")) return "lua/" + normalizePath(path.substr(6));
        if (startsWith(path, "shader://")) return "assets/shaders/" + normalizePath(path.substr(9));
        if (startsWith(path, "content://")) return normalizePath(path.substr(10));

        if (path.find("://") != std::string::npos) return "";
        if (path.front() == '/' || (path.size() > 1 && path[1] == ':')) return "";

        return "assets/" + normalizePath(path);
    }

    bool readExact(FILE* file, void* dst, size_t size) {
        return std::fread(dst, 1, size, file) == size;
    }

    bool readU8(FILE* file, uint8_t& value) {
        return readExact(file, &value, 1);
    }

    bool readU16(FILE* file, uint16_t& value) {
        unsigned char bytes[2];
        if (!readExact(file, bytes, sizeof(bytes))) return false;
        value = static_cast<uint16_t>(bytes[0])
              | (static_cast<uint16_t>(bytes[1]) << 8);
        return true;
    }

    bool readU32(FILE* file, uint32_t& value) {
        unsigned char bytes[4];
        if (!readExact(file, bytes, sizeof(bytes))) return false;
        value = static_cast<uint32_t>(bytes[0])
              | (static_cast<uint32_t>(bytes[1]) << 8)
              | (static_cast<uint32_t>(bytes[2]) << 16)
              | (static_cast<uint32_t>(bytes[3]) << 24);
        return true;
    }

    bool readU64(FILE* file, uint64_t& value) {
        unsigned char bytes[8];
        if (!readExact(file, bytes, sizeof(bytes))) return false;
        value = 0;
        for (int i = 0; i < 8; i++) {
            value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
        }
        return true;
    }

    bool seekFile(FILE* file, uint64_t offset) {
#ifdef _WIN32
        return _fseeki64(file, static_cast<pack_offset_t>(offset), SEEK_SET) == 0;
#else
        return fseeko(file, static_cast<pack_offset_t>(offset), SEEK_SET) == 0;
#endif
    }

    bool getFileSize(FILE* file, uint64_t& size) {
#ifdef _WIN32
        if (_fseeki64(file, 0, SEEK_END) != 0) return false;
        const pack_offset_t end = _ftelli64(file);
#else
        if (fseeko(file, 0, SEEK_END) != 0) return false;
        const pack_offset_t end = ftello(file);
#endif
        if (end < 0) return false;

        size = static_cast<uint64_t>(end);
        return seekFile(file, 0);
    }

    // (games only: the editor reads the project's files, loadPackOnce)
    [[maybe_unused]] bool loadPack(const std::string& filename) {
        FILE* file = System::instance().platformFopen(filename.c_str(), "rb");
        if (!file) return false;

        uint64_t packSize = 0;
        char magic[sizeof(PACK_MAGIC)];
        uint32_t fileCount = 0;
        bool ok = getFileSize(file, packSize)
            && packSize <= MAX_PACK_OFFSET
            && readExact(file, magic, sizeof(magic))
            && std::equal(std::begin(PACK_MAGIC), std::end(PACK_MAGIC), magic)
            && readU32(file, fileCount);

        std::map<std::string, PackEntry> entries;
        for (uint32_t i = 0; ok && i < fileCount; i++) {
            uint16_t pathLength = 0;
            if (!readU16(file, pathLength) || pathLength == 0) {
                ok = false;
                break;
            }

            std::string path(pathLength, '\0');
            if (!readExact(file, path.data(), pathLength)) {
                ok = false;
                break;
            }

            PackEntry entry;
            if (!readU64(file, entry.offset)
                || !readU64(file, entry.size)
                || !readU8(file, entry.key)
                || !readU32(file, entry.shift)) {
                ok = false;
                break;
            }

            // Bounds come from the file, so a corrupt one must not become an allocation
            if (entry.offset > packSize || entry.size > packSize - entry.offset
                || entry.size > UINT32_MAX) {
                ok = false;
                break;
            }

            entries[normalizePath(path)] = entry;
        }

        std::fclose(file);

        if (!ok) return false;

        state().filename = filename;
        state().entries = std::move(entries);
        return true;
    }

    void loadPackOnce() {
#ifndef JASPERENGINE_EDITOR
        if (!System::hasInstance()) return;
        // a content pack beside the executable (desktop) or at the asset root (an APK), unless
        // the player mounted its own
        if (!state().content) {
            for (const std::string& candidate : {std::string(CONTENT_PACK_FILENAME),
                                                 System::instance().getAssetPath() + "/../" + CONTENT_PACK_FILENAME}) {
                FILE* probe = System::instance().platformFopen(candidate.c_str(), "rb");
                if (!probe) continue;
                std::fclose(probe);
                auto pack = std::make_unique<JPak>();
                std::string error;
                if (pack->openFile(candidate, &error)) {
                    state().content = std::move(pack);
                } else {
                    Log::error("Content pack %s: %s", candidate.c_str(), error.c_str());
                }
                break;
            }
        }

        // Beside the executable on desktop, at the asset root inside an APK
        if (loadPack(PACK_FILENAME)) return;

        // Used as given: absolute in a macOS bundle, or wherever JASPERENGINE_ASSET_PATH points
        const std::string assetRoot = System::instance().getAssetPath();
        if (!assetRoot.empty()) {
            loadPack(assetRoot + "/" + PACK_FILENAME);
        }
#endif
    }

    const PackEntry* findEntry(const std::string& path) {
        std::call_once(loadFlag, loadPackOnce);

        const PackState& pack = state();
        if (pack.entries.empty()) return nullptr;

        const std::string key = getPackKey(path);
        if (key.empty()) return nullptr;

        auto it = pack.entries.find(key);
        return it != pack.entries.end() ? &it->second : nullptr;
    }

    // Inverse of Exporter::obfuscate, which rotates right and then XORs
    void deobfuscate(unsigned char* data, size_t size, uint8_t key, uint32_t shift) {
        if (size == 0) return;

        for (size_t i = 0; i < size; i++) {
            data[i] ^= key;
        }

        shift %= size;
        if (shift > 0) {
            std::rotate(data, data + shift, data + size);
        }
    }

    const JPak* contentEntryPack(const std::string& path, std::string& key) {
        std::call_once(loadFlag, loadPackOnce);
        const JPak* pack = state().content.get();
        if (!pack) return nullptr;
        key = getPackKey(path);
        if (key.empty() || !pack->find(key)) return nullptr;
        return pack;
    }

    bool mountPack(std::unique_ptr<JPak> pack) {
        // the automatic lookup must not replace (or race) the mounted pack later
        std::call_once(loadFlag, loadPackOnce);
        state().content = std::move(pack);
        return true;
    }

}

bool ResourcePack::mountContentPack(const std::filesystem::path& path, std::string* error) {
    auto pack = std::make_unique<JPak>();
    if (!pack->openFile(path, error)) return false;
    return mountPack(std::move(pack));
}

bool ResourcePack::mountContentPack(std::span<const uint8_t> bytes, std::string* error) {
    auto pack = std::make_unique<JPak>();
    if (!pack->openMemory(bytes, error)) return false;
    return mountPack(std::move(pack));
}

const JPak* ResourcePack::contentPack() {
    std::call_once(loadFlag, loadPackOnce);
    return state().content.get();
}

std::string ResourcePack::packKey(const std::string& path) {
    return getPackKey(path);
}

bool ResourcePack::contains(const std::string& path) {
    std::string key;
    return contentEntryPack(path, key) != nullptr || findEntry(path) != nullptr;
}

bool ResourcePack::size(const std::string& path, uint64_t& outSize) {
    const PackEntry* entry = findEntry(path);
    if (!entry) return false;
    outSize = entry->size;
    return true;
}

bool ResourcePack::view(const std::string& path, std::span<const uint8_t>& out) {
    std::string key;
    const JPak* pack = contentEntryPack(path, key);
    return pack && pack->view(key, out, nullptr);
}

bool ResourcePack::read(const std::string& path, std::vector<unsigned char>& outData) {
    std::string key;
    if (const JPak* pack = contentEntryPack(path, key)) {
        std::string error;
        if (pack->read(key, outData, &error)) return true;
        Log::error("%s", error.c_str());
        return false;
    }
    const PackEntry* entry = findEntry(path);
    if (!entry) return false;

    FILE* file = System::instance().platformFopen(state().filename.c_str(), "rb");
    if (!file) return false;

    bool ok = seekFile(file, entry->offset);
    if (ok) {
        outData.resize(static_cast<size_t>(entry->size));
        ok = entry->size == 0 || readExact(file, outData.data(), static_cast<size_t>(entry->size));
    }
    std::fclose(file);

    if (!ok) {
        outData.clear();
        return false;
    }

    deobfuscate(outData.data(), outData.size(), entry->key, entry->shift);
    return true;
}

ResourcePack::RangeReader::~RangeReader() {
    if (file) std::fclose(static_cast<FILE*>(file));
}

bool ResourcePack::RangeReader::open(const std::string& path) {
    const PackEntry* entry = findEntry(path);
    if (!entry) return false;
    FILE* handle = System::instance().platformFopen(state().filename.c_str(), "rb");
    if (!handle) return false;
    if (file) std::fclose(static_cast<FILE*>(file));
    file = handle;
    entryOffset = entry->offset;
    entrySize = entry->size;
    key = entry->key;
    shift = entry->size ? (uint32_t)(entry->shift % entry->size) : 0;
    return true;
}

// Byte i of the entry is stored at (i + shift) % size, XOR key (deobfuscate's rotation, inverted)
bool ResourcePack::RangeReader::readRange(uint64_t from, unsigned char* dst, size_t count) {
    if (!file || from + count > entrySize) return false;
    FILE* handle = static_cast<FILE*>(file);
    size_t done = 0;
    while (done < count) {
        const uint64_t stored = (from + done + shift) % entrySize;
        const size_t run = (size_t)std::min<uint64_t>(count - done, entrySize - stored);
        if (!seekFile(handle, entryOffset + stored) || !readExact(handle, dst + done, run)) return false;
        done += run;
    }
    for (size_t i = 0; i < count; i++) dst[i] ^= key;
    return true;
}

bool ResourcePack::readOwned(const std::string& path, std::unique_ptr<unsigned char[]>& outData,
                             unsigned int& outSize) {
    std::string key;
    if (const JPak* pack = contentEntryPack(path, key)) {
        const JPak::Entry* entry = pack->find(key);
        if (entry->size > UINT32_MAX) return false;
        std::unique_ptr<unsigned char[]> data(new unsigned char[static_cast<size_t>(entry->size) + 1]);
        std::string error;
        if (!pack->read(*entry, data.get(), &error)) {
            Log::error("%s", error.c_str());
            return false;
        }
        outData = std::move(data);
        outSize = static_cast<unsigned int>(entry->size);
        return true;
    }
    const PackEntry* entry = findEntry(path);
    if (!entry) return false;

    auto closeFile = [](FILE* handle) { std::fclose(handle); };
    std::unique_ptr<FILE, decltype(closeFile)> file(
        System::instance().platformFopen(state().filename.c_str(), "rb"), closeFile);
    if (!file) return false;

    bool ok = seekFile(file.get(), entry->offset);
    std::unique_ptr<unsigned char[]> data;
    if (ok) {
        data.reset(new unsigned char[static_cast<size_t>(entry->size)]);
        ok = entry->size == 0 || readExact(file.get(), data.get(), static_cast<size_t>(entry->size));
    }

    if (!ok) return false;

    deobfuscate(data.get(), static_cast<size_t>(entry->size), entry->key, entry->shift);
    outData = std::move(data);
    outSize = static_cast<unsigned int>(entry->size);
    return true;
}
