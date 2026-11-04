// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_DETOUR_SUPPORT_H
#define NAV_DETOUR_SUPPORT_H

// Engine-internal glue to Recast/Detour (not installed: recastnavigation is a static library
// linked into libjasperengine only): the tile cache's deflate compressor (stb's zlib pair, spec
// R30), its scratch allocator and the mesh process that sets polygon flags and links.

// JASPERENGINE_NAVIGATION's default, for a build that does not define it (the Xcode project):
// tested before it, this file compiled empty there while NavMeshBuilder.cpp called into it
#include "navigation/NavTypes.h"

#if JASPERENGINE_NAVIGATION

#include "navigation/NavBakeInput.h"

#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace jasperengine::nav::detail {

    // zlib streams through stbi_zlib_compress / stbi_zlib_decode_buffer
    struct DeflateCompressor final : dtTileCacheCompressor {
        int maxCompressedSize(const int bufferSize) override;
        dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed,
                          const int maxCompressedSize, int* compressedSize) override;
        dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer,
                            const int maxBufferSize, int* bufferSize) override;
    };

    // a bump allocator reset per tile build (Detour's sample LinearAllocator), growing on demand
    struct LinearAllocator final : dtTileCacheAlloc {
        LinearAllocator();
        ~LinearAllocator() override;
        void reset() override;
        void* alloc(const size_t size) override;
        void free(void* ptr) override;

    private:
        std::vector<std::vector<unsigned char>> blocks;
        size_t top = 0;
    };

    // Each navmesh tile the tile cache builds: area ids back to areas, flags 1 << area, and the
    // off-mesh links of the tile (linksAt returns them for a tile coordinate)
    struct MeshProcess final : dtTileCacheMeshProcess {
        std::function<const std::vector<NavLinkInput>*(int x, int y)> linksAt;
        void process(struct dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override;

    private:
        std::vector<float> verts;
        std::vector<float> radii;
        std::vector<unsigned short> flags;
        std::vector<unsigned char> areas;
        std::vector<unsigned char> dirs;
        std::vector<unsigned int> ids;
    };

    // Converts a built polygon's Recast area to the area and its flag
    // (S55: a runtime volume's ids back to the area they were moved from)
    inline void polyAreaAndFlags(unsigned char recastArea, unsigned char& area, unsigned short& flags) {
        if (recastArea >= FIRST_RUNTIME_VOLUME_RECAST_AREA && recastArea <= LAST_RUNTIME_VOLUME_RECAST_AREA)
            recastArea = (unsigned char)(recastArea - RUNTIME_VOLUME_AREA_OFFSET);
        const unsigned char a = (recastArea >= 1 && recastArea <= AREA_COUNT) ? (unsigned char)(recastArea - 1) : AREA_WALKABLE;
        area = a;
        flags = (unsigned short)(1u << a);
    }

    // Fills the off-mesh connection arrays of `links` for the tile [bmin, bmax) (the start nudged
    // inside, so Detour's classification keeps it)
    void appendLinks(const std::vector<NavLinkInput>& links, const float* bmin, const float* bmax,
                     std::vector<float>& verts, std::vector<float>& radii, std::vector<unsigned short>& flags,
                     std::vector<unsigned char>& areas, std::vector<unsigned char>& dirs, std::vector<unsigned int>& ids);

}

#endif // JASPERENGINE_NAVIGATION

#endif // NAV_DETOUR_SUPPORT_H
