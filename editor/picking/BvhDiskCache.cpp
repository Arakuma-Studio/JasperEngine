// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BvhDiskCache.h"

#include "io/AtomicFile.h"
#include "picking/PickGeometry.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct FileHeader {
        uint32_t magic;
        uint32_t version;
        uint64_t hash;
        uint32_t triangles;
        uint32_t reserved;
        uint64_t payloadSize;
        uint64_t payloadHash;
    };
    static_assert(sizeof(FileHeader) == 40, "BvhDiskCache header layout: bump FILE_VERSION on a change");

}

BvhDiskCache::BvhDiskCache(fs::path directory, uint64_t capacityBytes): directory(std::move(directory)), capacity(capacityBytes){
}

fs::path BvhDiskCache::directoryFor(const fs::path& projectPath){
    return projectPath / ".jasperengine" / "cache" / "meshbvh";
}

fs::path BvhDiskCache::pathFor(uint64_t hash) const{
    char name[32];
    std::snprintf(name, sizeof(name), "%016" PRIx64 ".bvh", hash);
    return directory / name;
}

bool BvhDiskCache::load(uint64_t hash, const PickGeometry& geometry, MeshBvh& bvh){
    const fs::path path = pathFor(hash);
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size < sizeof(FileHeader)){
        return false;
    }
    std::vector<uint8_t> bytes((size_t)size);
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)bytes.size())){
            return false;
        }
    }
    FileHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    const std::span<const uint8_t> payload(bytes.data() + sizeof(header), bytes.size() - sizeof(header));
    if (header.magic != FILE_MAGIC || header.version != FILE_VERSION || header.hash != hash ||
            header.triangles != geometry.getTriangleCount() || header.payloadSize != payload.size() ||
            header.payloadHash != picking::hashBytes(payload.data(), payload.size(), hash)){
        return false;
    }
    if (!bvh.deserialize(payload, geometry)){
        return false;
    }
    // used: the last to go
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    return true;
}

void BvhDiskCache::store(uint64_t hash, const MeshBvh& bvh){
    std::vector<uint8_t> payload;
    bvh.serialize(payload);
    FileHeader header{};
    header.magic = FILE_MAGIC;
    header.version = FILE_VERSION;
    header.hash = hash;
    header.triangles = bvh.getSubmeshFirstTriangle().empty() ? bvh.getTriangleCount() : bvh.getSubmeshFirstTriangle().back();
    header.payloadSize = payload.size();
    header.payloadHash = picking::hashBytes(payload.data(), payload.size(), hash);

    std::string bytes(sizeof(header) + payload.size(), '\0');
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!payload.empty()){
        std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
    }
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    if (!io::writeFileAtomic(pathFor(hash), bytes, options).ok()){
        return;
    }
    enforceCapacity();
}

uint64_t BvhDiskCache::enforceCapacity(){
    std::lock_guard<std::mutex> lock(mutex);
    struct File { fs::path path; uintmax_t size; fs::file_time_type time; };
    std::vector<File> files;
    uint64_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)){
        if (!it->is_regular_file(ec) || it->path().extension() != ".bvh"){
            continue;
        }
        const uintmax_t size = it->file_size(ec);
        const fs::file_time_type time = it->last_write_time(ec);
        if (ec){
            ec.clear();
            continue;
        }
        files.push_back({it->path(), size, time});
        total += size;
    }
    if (total <= capacity){
        return total;
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b){ return a.time < b.time; });
    for (const File& file : files){
        if (total <= capacity){
            break;
        }
        if (fs::remove(file.path, ec)){
            total -= file.size;
        }
    }
    return total;
}
