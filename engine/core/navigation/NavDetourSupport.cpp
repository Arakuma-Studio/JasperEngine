// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavDetourSupport.h"

#if JASPERENGINE_NAVIGATION

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "stb_image.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// stb_image_write.c defines it (C linkage) without declaring it in the header's API part
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

using namespace jasperengine;
using namespace jasperengine::nav;
using namespace jasperengine::nav::detail;

namespace {
    // stb's best effort (its quality is the hash chain length)
    constexpr int DEFLATE_QUALITY = 8;
    constexpr size_t ALLOCATOR_BLOCK = 256 * 1024;
}

int DeflateCompressor::maxCompressedSize(const int bufferSize) {
    // a stored deflate stream grows by 5 bytes per 64 KiB block, plus the zlib header and adler
    return bufferSize + bufferSize / 64 + 64;
}

dtStatus DeflateCompressor::compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed,
                                     const int maxCompressedSize, int* compressedSize) {
    int length = 0;
    unsigned char* stream = stbi_zlib_compress(const_cast<unsigned char*>(buffer), bufferSize, &length, DEFLATE_QUALITY);
    if (!stream)
        return DT_FAILURE | DT_OUT_OF_MEMORY;
    if (length > maxCompressedSize) {
        std::free(stream);
        return DT_FAILURE | DT_BUFFER_TOO_SMALL;
    }
    std::memcpy(compressed, stream, (size_t)length);
    std::free(stream);
    *compressedSize = length;
    return DT_SUCCESS;
}

dtStatus DeflateCompressor::decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer,
                                       const int maxBufferSize, int* bufferSize) {
    const int length = stbi_zlib_decode_buffer(reinterpret_cast<char*>(buffer), maxBufferSize,
                                               reinterpret_cast<const char*>(compressed), compressedSize);
    if (length < 0)
        return DT_FAILURE | DT_INVALID_PARAM;
    *bufferSize = length;
    return DT_SUCCESS;
}

LinearAllocator::LinearAllocator() = default;
LinearAllocator::~LinearAllocator() = default;

void LinearAllocator::reset() {
    // keep the largest block: the next tile needs about as much
    if (blocks.size() > 1) {
        auto largest = std::max_element(blocks.begin(), blocks.end(),
            [](const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) { return a.size() < b.size(); });
        std::vector<unsigned char> keep = std::move(*largest);
        blocks.clear();
        blocks.push_back(std::move(keep));
    }
    top = 0;
}

void* LinearAllocator::alloc(const size_t size) {
    const size_t aligned = (size + 15) & ~(size_t)15;
    if (blocks.empty() || top + aligned > blocks.back().size()) {
        blocks.emplace_back(std::max(ALLOCATOR_BLOCK, aligned));
        top = 0;
    }
    void* pointer = blocks.back().data() + top;
    top += aligned;
    return pointer;
}

void LinearAllocator::free(void*) {
    // freed by the next reset
}

void detail::appendLinks(const std::vector<NavLinkInput>& links, const float* bmin, const float* bmax,
                         std::vector<float>& verts, std::vector<float>& radii, std::vector<unsigned short>& flags,
                         std::vector<unsigned char>& areas, std::vector<unsigned char>& dirs, std::vector<unsigned int>& ids) {
    for (const NavLinkInput& link : links) {
        // Detour keeps a connection in the tile whose [bmin, bmax) holds its start: one that sits
        // on the edge is brought a hair inside
        const float epsilon = 1e-4f * std::max(1.0f, bmax[0] - bmin[0]);
        float start[3] = {link.start[0], link.start[1], link.start[2]};
        start[0] = std::clamp(start[0], bmin[0] + epsilon, bmax[0] - epsilon);
        start[2] = std::clamp(start[2], bmin[2] + epsilon, bmax[2] - epsilon);
        verts.insert(verts.end(), start, start + 3);
        verts.insert(verts.end(), link.end, link.end + 3);
        radii.push_back(link.radius);
        flags.push_back((unsigned short)(1u << link.area));
        areas.push_back(link.polyArea);
        dirs.push_back(link.bidirectional ? (unsigned char)DT_OFFMESH_CON_BIDIR : (unsigned char)0);
        ids.push_back(link.userId);
    }
}

void MeshProcess::process(dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) {
    for (int i = 0; i < params->polyCount; i++)
        polyAreaAndFlags(polyAreas[i], polyAreas[i], polyFlags[i]);

    verts.clear();
    radii.clear();
    flags.clear();
    areas.clear();
    dirs.clear();
    ids.clear();
    const std::vector<NavLinkInput>* links = linksAt ? linksAt(params->tileX, params->tileY) : nullptr;
    if (links && !links->empty()) {
        appendLinks(*links, params->bmin, params->bmax, verts, radii, flags, areas, dirs, ids);
        params->offMeshConVerts = verts.data();
        params->offMeshConRad = radii.data();
        params->offMeshConFlags = flags.data();
        params->offMeshConAreas = areas.data();
        params->offMeshConDir = dirs.data();
        params->offMeshConUserID = ids.data();
        params->offMeshConCount = (int)radii.size();
    } else {
        params->offMeshConCount = 0;
    }
}

namespace {

    bool isHex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    bool isWordChar(char c) {
        return isHex(c) || (c >= 'g' && c <= 'z') || (c >= 'G' && c <= 'Z') || c == '_';
    }

    // A Recast message with its pointer values (%p: "0x55d0...", glibc's "(nil)", MSVC's 8 or 16
    // upper-case digits) written as "?", so two bakes of the same input report the same text
    std::string withoutPointers(const char* msg, int len) {
        std::string out;
        out.reserve((size_t)len);
        int i = 0;
        while (i < len) {
            const bool wordStart = i == 0 || !isWordChar(msg[i - 1]);
            if (wordStart && msg[i] == '0' && i + 2 < len && (msg[i + 1] == 'x' || msg[i + 1] == 'X') && isHex(msg[i + 2])) {
                int j = i + 2;
                while (j < len && isHex(msg[j]))
                    j++;
                if (j == len || !isWordChar(msg[j])) {
                    out += '?';
                    i = j;
                    continue;
                }
            }
            if (wordStart && len - i >= 5 && std::memcmp(msg + i, "(nil)", 5) == 0) {
                out += '?';
                i += 5;
                continue;
            }
            if (wordStart && isHex(msg[i])) {
                int j = i;
                bool upper = true;
                while (j < len && isHex(msg[j])) {
                    upper = upper && !(msg[j] >= 'a' && msg[j] <= 'f');
                    j++;
                }
                if ((j - i == 8 || j - i == 16) && upper && (j == len || !isWordChar(msg[j]))) {
                    out += '?';
                    i = j;
                    continue;
                }
                out.append(msg + i, (size_t)(j - i));
                i = j;
                continue;
            }
            out += msg[i++];
        }
        return out;
    }

    // rcBuildContours grows its buffer and says so: an allocation notice, not a finding
    constexpr const char* CONTOUR_GROWTH = "rcBuildContours: Expanding max contours";

}

NavRecastLog::NavRecastLog(): rcContext(true) {
    enableTimer(false);
}

void NavRecastLog::takeMessages(std::vector<std::string>& out) {
    for (std::string& message : messages)
        out.push_back(std::move(message));
    if (dropped > 0)
        out.push_back("... and " + std::to_string(dropped) + " more Recast messages");
    messages.clear();
    dropped = 0;
}

void NavRecastLog::doResetLog() {
    messages.clear();
    lastError.clear();
    dropped = 0;
    errors = 0;
}

void NavRecastLog::doLog(const rcLogCategory category, const char* msg, const int len) {
    if (category != RC_LOG_WARNING && category != RC_LOG_ERROR)
        return;
    const size_t growth = std::strlen(CONTOUR_GROWTH);
    if (len >= (int)growth && std::memcmp(msg, CONTOUR_GROWTH, growth) == 0)
        return;
    std::string text = withoutPointers(msg, len);
    if (category == RC_LOG_ERROR) {
        errors++;
        lastError = text;
    }
    if (messages.size() < MAX_MESSAGES)
        messages.push_back(std::move(text));
    else
        dropped++;
}

#endif // JASPERENGINE_NAVIGATION
