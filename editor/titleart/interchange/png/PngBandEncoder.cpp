// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The band encoder and titleAdler32Combine are ported from Pachislop src/io/pngWorker.ts and
// src/io/pngChunk.ts (adler32Combine, itself zlib's adler32_combine) by SafEight, used with the
// author's permission.

#include "PngBandEncoder.h"

#include "jobs/JobSystem.h"
#include "miniz.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr uint32_t ADLER_BASE = 65521;

    void put32(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back((uint8_t)(v >> 24));
        out.push_back((uint8_t)(v >> 16));
        out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)v);
    }

    void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
        put32(out, (uint32_t)data.size());
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, out.data() + start, out.size() - start);
        put32(out, (uint32_t)crc);
    }

    std::vector<uint8_t> be32(uint32_t v) {
        return {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    }

    struct Band {
        std::vector<uint8_t> raw;           // filtered rows
        std::vector<uint8_t> deflated;
        uint32_t adler = 1;
        bool ok = false;
    };

    mz_bool collect(const void* buffer, int length, void* user) {
        auto* out = static_cast<std::vector<uint8_t>*>(user);
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        out->insert(out->end(), bytes, bytes + length);
        return MZ_TRUE;
    }

    // the filtered rows [y0, y1) of the image: a Sub filter byte, then each byte minus the byte
    // one pixel to its left (the 16-bit samples big-endian, as PNG stores them)
    void filterRows(const texsynth::Image& image, int y0, int y1, std::vector<uint8_t>& out) {
        const size_t bpp = image.bytesPerPixel();
        const size_t rowBytes = bpp * (size_t)image.width;
        out.resize((rowBytes + 1) * (size_t)(y1 - y0));
        std::vector<uint8_t> row(rowBytes);
        for (int y = y0; y < y1; y++) {
            const uint8_t* src = image.pixel(0, y);
            if (image.bitDepth == 16) {
                for (size_t i = 0; i < rowBytes; i += 2) {
                    uint16_t v;
                    std::memcpy(&v, src + i, 2);
                    row[i] = (uint8_t)(v >> 8);
                    row[i + 1] = (uint8_t)v;
                }
            } else {
                std::memcpy(row.data(), src, rowBytes);
            }
            uint8_t* dst = out.data() + (rowBytes + 1) * (size_t)(y - y0);
            dst[0] = 1;     // Sub
            for (size_t i = 0; i < rowBytes; i++) dst[1 + i] = (uint8_t)(row[i] - (i >= bpp ? row[i - bpp] : 0));
        }
    }

    void deflateBand(Band& band, int level, bool last) {
        auto compressor = std::make_unique<tdefl_compressor>();
        const mz_uint flags = tdefl_create_comp_flags_from_zip_params(level, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
        if (tdefl_init(compressor.get(), &collect, &band.deflated, (int)flags) != TDEFL_STATUS_OKAY) return;
        const tdefl_status status = tdefl_compress_buffer(compressor.get(), band.raw.data(), band.raw.size(), last ? TDEFL_FINISH : TDEFL_SYNC_FLUSH);
        band.ok = last ? status == TDEFL_STATUS_DONE : status == TDEFL_STATUS_OKAY;
        band.adler = titleAdler32(band.raw.data(), band.raw.size(), 1);
    }

}

uint32_t editor::titleAdler32(const uint8_t* bytes, size_t size, uint32_t adler) {
    uint32_t a = adler & 0xffff, b = adler >> 16;
    // 3800 bytes between reductions keep b below 2^32
    for (size_t i = 0; i < size;) {
        const size_t end = std::min(size, i + 3800);
        for (; i < end; i++) {
            a += bytes[i];
            b += a;
        }
        a %= ADLER_BASE;
        b %= ADLER_BASE;
    }
    return (b << 16) | a;
}

uint32_t editor::titleAdler32Combine(uint32_t adlerA, uint32_t adlerB, size_t lengthB) {
    const uint32_t rem = (uint32_t)(lengthB % ADLER_BASE);
    uint32_t a = adlerA & 0xffff;
    uint32_t b = (uint32_t)(((uint64_t)rem * a) % ADLER_BASE);
    a += (adlerB & 0xffff) + ADLER_BASE - 1;
    b += (adlerA >> 16) + (adlerB >> 16) + ADLER_BASE - rem;
    if (a >= ADLER_BASE) a -= ADLER_BASE;
    if (a >= ADLER_BASE) a -= ADLER_BASE;
    if (b >= 2 * ADLER_BASE) b -= 2 * ADLER_BASE;
    if (b >= ADLER_BASE) b -= ADLER_BASE;
    return (b << 16) | a;
}

bool editor::encodeTitlePng(const texsynth::Image& image, const TitlePngOptions& options, std::vector<uint8_t>& out, std::string& error) {
    if (image.width <= 0 || image.height <= 0 || image.channels < 1 || image.channels > 4 || (image.bitDepth != 8 && image.bitDepth != 16) ||
        image.data.size() != image.bytesPerPixel() * (size_t)image.width * (size_t)image.height) {
        error = "not an image the PNG writer takes (1-4 channels, 8 or 16 bits)";
        return false;
    }
    const int bandRows = std::max(1, options.bandRows);
    const int bands = (image.height + bandRows - 1) / bandRows;
    std::vector<Band> work((size_t)bands);
    const auto encodeBand = [&](size_t i) {
        const int y0 = (int)i * bandRows, y1 = std::min(image.height, y0 + bandRows);
        filterRows(image, y0, y1, work[i].raw);
        deflateBand(work[i], std::clamp(options.level, 1, 9), i + 1 == work.size());
        work[i].raw.clear();
        work[i].raw.shrink_to_fit();
    };
    // the adler of each band is over its raw bytes: keep their sizes before they are dropped
    std::vector<size_t> rawSizes((size_t)bands);
    for (int i = 0; i < bands; i++) {
        const int y0 = i * bandRows, y1 = std::min(image.height, y0 + bandRows);
        rawSizes[(size_t)i] = (image.bytesPerPixel() * (size_t)image.width + 1) * (size_t)(y1 - y0);
    }
    if (options.parallel && bands > 1) {
        jobs::parallelFor((size_t)bands, [&](size_t i) { encodeBand(i); });
    } else {
        for (size_t i = 0; i < work.size(); i++) encodeBand(i);
    }
    std::vector<uint8_t> idat = {0x78, 0x9c};
    uint32_t adler = 1;
    for (size_t i = 0; i < work.size(); i++) {
        if (!work[i].ok) {
            error = "deflating band " + std::to_string(i) + " failed";
            return false;
        }
        idat.insert(idat.end(), work[i].deflated.begin(), work[i].deflated.end());
        adler = titleAdler32Combine(adler, work[i].adler, rawSizes[i]);
    }
    const std::vector<uint8_t> tail = be32(adler);
    idat.insert(idat.end(), tail.begin(), tail.end());

    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.assign(signature, signature + 8);
    static const uint8_t colorTypes[5] = {0, 0, 4, 2, 6};
    std::vector<uint8_t> ihdr = be32((uint32_t)image.width);
    const std::vector<uint8_t> h = be32((uint32_t)image.height);
    ihdr.insert(ihdr.end(), h.begin(), h.end());
    ihdr.push_back((uint8_t)image.bitDepth);
    ihdr.push_back(colorTypes[image.channels]);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    if (options.srgbChunks) {
        chunk(out, "sRGB", {0});    // perceptual
        chunk(out, "gAMA", be32(45455));
        // white D65, then red, green, blue (x, y) x 100000 (PNG specification 11.3.3.4)
        std::vector<uint8_t> chrm;
        for (uint32_t v : {31270u, 32900u, 64000u, 33000u, 30000u, 60000u, 15000u, 6000u}) {
            const std::vector<uint8_t> b = be32(v);
            chrm.insert(chrm.end(), b.begin(), b.end());
        }
        chunk(out, "cHRM", chrm);
    }
    if (options.dpi > 0) {
        const uint32_t ppm = (uint32_t)std::lround(options.dpi / 0.0254);
        std::vector<uint8_t> phys = be32(ppm);
        const std::vector<uint8_t> y = be32(ppm);
        phys.insert(phys.end(), y.begin(), y.end());
        phys.push_back(1);      // the unit is the metre
        chunk(out, "pHYs", phys);
    }
    if (!options.recipeKeyword.empty()) {
        // iTXt: keyword, NUL, compression flag 0, method 0, empty language tag and translated keyword
        std::vector<uint8_t> itxt(options.recipeKeyword.begin(), options.recipeKeyword.end());
        itxt.push_back(0);
        itxt.push_back(0);
        itxt.push_back(0);
        itxt.push_back(0);
        itxt.push_back(0);
        itxt.insert(itxt.end(), options.recipeText.begin(), options.recipeText.end());
        chunk(out, "iTXt", itxt);
    }
    chunk(out, "IDAT", idat);
    chunk(out, "IEND", {});
    return true;
}
