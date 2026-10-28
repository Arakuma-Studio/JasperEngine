// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26 R10 (AC7 PickCache.DiskCache*): the editor's mesh BVH disk cache. A tree built for a
// large mesh is written to .jasperengine/cache/meshbvh/<hash>.bvh and loaded instead of built
// next time; a corrupt, truncated or stale file is rebuilt, never trusted.

#include "picking/BvhDiskCache.h"
#include "picking/MeshBvh.h"
#include "picking/PickCache.h"
#include "picking/PickingTestUtil.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct TempDirectory {
        fs::path path;
        TempDirectory(){
            path = fs::temp_directory_path() / ("je-bvh-cache-" + std::to_string(std::random_device{}()));
            fs::create_directories(path);
        }
        ~TempDirectory(){
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    // the closest hits of rays through both trees are the same
    void expectSameHits(const MeshBvh& a, const MeshBvh& b){
        for (const test::TestRay& ray : test::randomRays(200, 77)){
            MeshBvhRay query;
            query.origin = ray.origin;
            query.direction = ray.direction;
            MeshBvhHit ha, hb;
            const bool hitA = a.raycast(query, ha);
            const bool hitB = b.raycast(query, hb);
            ASSERT_EQ(hitA, hitB);
            if (hitA){
                EXPECT_EQ(ha.triangle, hb.triangle);
                EXPECT_EQ(ha.t, hb.t);
            }
        }
    }

    // a PickCache build of `geometry` (the disk cache asked first, given what was built)
    const MeshBvh* buildThroughCache(PickCache& cache, uint64_t hash, PickGeometry geometry){
        PickCache::EntryPtr entry = cache.create(hash, std::move(geometry), true);
        return cache.wait(entry);
    }

}

TEST(PickCache, DiskCacheRoundTrip){
    TempDirectory dir;
    BvhDiskCache disk(dir.path);
    const PickGeometry geometry = test::randomGeometry(5000, 3);
    const MeshBvh built = MeshBvh::build(geometry);
    const uint64_t hash = 0x1234abcdull;
    disk.store(hash, built);
    ASSERT_TRUE(fs::exists(disk.pathFor(hash)));

    MeshBvh loaded;
    ASSERT_TRUE(disk.load(hash, geometry, loaded));
    expectSameHits(built, loaded);

    // through the cache: a large mesh (65,536 triangles or more) is stored once built, and the
    // next session loads it instead of building
    PickCache& cache = PickCache::get();
    cache.clear();
    cache.setDiskCache(&disk);
    const PickGeometry big = test::randomGeometry(PickCache::DISK_CACHE_TRIANGLES, 9);
    const uint64_t bigHash = 0x5151ull;
    const PickCacheStats before = cache.stats();
    const MeshBvh* first = buildThroughCache(cache, bigHash, big);
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(fs::exists(disk.pathFor(bigHash))) << "the large tree was written";
    cache.clear();
    const MeshBvh* second = buildThroughCache(cache, bigHash, big);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(cache.stats().diskLoads, before.diskLoads + 1) << "the second build is a disk load";
    cache.clear();
    cache.setDiskCache(nullptr);
}

TEST(PickCache, DiskCacheRejectsCorrupt){
    TempDirectory dir;
    BvhDiskCache disk(dir.path);
    const PickGeometry geometry = test::randomGeometry(3000, 5);
    const uint64_t hash = 77;
    disk.store(hash, MeshBvh::build(geometry));
    const fs::path path = disk.pathFor(hash);
    const uintmax_t size = fs::file_size(path);

    // a flipped byte in the payload
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekg((std::streamoff)(size / 2));
        char byte = 0;
        file.read(&byte, 1);
        byte = (char)(byte ^ 0x5A);
        file.seekp((std::streamoff)(size / 2));
        file.write(&byte, 1);
    }
    MeshBvh loaded;
    EXPECT_FALSE(disk.load(hash, geometry, loaded));

    // truncated
    disk.store(hash, MeshBvh::build(geometry));
    fs::resize_file(path, size - 17);
    EXPECT_FALSE(disk.load(hash, geometry, loaded));
    fs::resize_file(path, 10);
    EXPECT_FALSE(disk.load(hash, geometry, loaded));

    // another mesh's file under this hash (the triangle count differs)
    disk.store(hash, MeshBvh::build(test::randomGeometry(2999, 6)));
    EXPECT_FALSE(disk.load(hash, geometry, loaded));

    // a bad file of a large mesh is rebuilt through the cache, never trusted
    const PickGeometry big = test::randomGeometry(PickCache::DISK_CACHE_TRIANGLES, 12);
    const uint64_t bigHash = 0xbad5ull;
    disk.store(bigHash, MeshBvh::build(test::randomGeometry(PickCache::DISK_CACHE_TRIANGLES + 1, 13)));
    PickCache& cache = PickCache::get();
    cache.clear();
    cache.setDiskCache(&disk);
    const uint64_t loadsBefore = cache.stats().diskLoads;
    const MeshBvh* tree = buildThroughCache(cache, bigHash, big);
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(cache.stats().diskLoads, loadsBefore);
    expectSameHits(*tree, MeshBvh::build(big));
    // and replaced by a good one
    MeshBvh reloaded;
    EXPECT_TRUE(disk.load(bigHash, big, reloaded));
    cache.clear();
    cache.setDiskCache(nullptr);
}

TEST(PickCache, DiskCacheRejectsStaleVersion){
    TempDirectory dir;
    BvhDiskCache disk(dir.path);
    const PickGeometry geometry = test::randomGeometry(3000, 8);
    const uint64_t hash = 99;
    disk.store(hash, MeshBvh::build(geometry));
    const fs::path path = disk.pathFor(hash);
    MeshBvh loaded;
    ASSERT_TRUE(disk.load(hash, geometry, loaded));

    // the header's version (after the magic) from another format
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t stale = BvhDiskCache::FILE_VERSION + 1;
        file.seekp(4);
        file.write(reinterpret_cast<const char*>(&stale), sizeof(stale));
    }
    EXPECT_FALSE(disk.load(hash, geometry, loaded));

    // and its magic
    disk.store(hash, MeshBvh::build(geometry));
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t magic = 0x12345678u;
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    }
    EXPECT_FALSE(disk.load(hash, geometry, loaded));
}

TEST(PickCache, DiskCacheEvictsLeastRecentlyUsed){
    TempDirectory dir;
    const PickGeometry geometry = test::randomGeometry(3000, 4);
    const MeshBvh bvh = MeshBvh::build(geometry);
    BvhDiskCache probe(dir.path);
    probe.store(1, bvh);
    const uintmax_t fileSize = fs::file_size(probe.pathFor(1));
    // room for two files
    BvhDiskCache disk(dir.path, fileSize * 2 + fileSize / 2);
    disk.store(2, bvh);
    // 1 is used (a load touches it), so 2 is the least recently used one
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MeshBvh loaded;
    ASSERT_TRUE(disk.load(1, geometry, loaded));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    disk.store(3, bvh);
    EXPECT_TRUE(fs::exists(disk.pathFor(1)));
    EXPECT_FALSE(fs::exists(disk.pathFor(2)));
    EXPECT_TRUE(fs::exists(disk.pathFor(3)));
}
