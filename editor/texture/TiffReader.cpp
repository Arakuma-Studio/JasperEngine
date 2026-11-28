// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Written from the TIFF 6.0 specification (Adobe, 1992) and its Technical Note 2 (Deflate).

#include "TiffReader.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr uint32_t TIFF_MAX_SIDE = 32768;
    constexpr uint64_t TIFF_MAX_PIXELS = 1ull << 27;
    constexpr uint64_t TIFF_MAX_CHUNK_PIXELS = 1ull << 26;
    constexpr uint32_t TIFF_MAX_ENTRY_VALUES = 1u << 24;

    enum TiffTag : uint16_t {
        TAG_WIDTH = 256, TAG_HEIGHT = 257, TAG_BITS = 258, TAG_COMPRESSION = 259, TAG_PHOTOMETRIC = 262,
        TAG_FILL_ORDER = 266, TAG_STRIP_OFFSETS = 273, TAG_SAMPLES = 277, TAG_ROWS_PER_STRIP = 278,
        TAG_STRIP_BYTES = 279, TAG_PLANAR = 284, TAG_PREDICTOR = 317, TAG_TILE_WIDTH = 322, TAG_TILE_HEIGHT = 323,
        TAG_TILE_OFFSETS = 324, TAG_TILE_BYTES = 325, TAG_EXTRA_SAMPLES = 338, TAG_SAMPLE_FORMAT = 339,
    };

    struct TiffSource {
        const uint8_t* data = nullptr;
        size_t size = 0;
        bool little = true;

        bool has(uint64_t offset, uint64_t length) const { return offset <= size && length <= size - offset; }
        uint16_t u16(uint64_t at) const {
            const uint8_t* p = data + at;
            return little ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[0] << 8 | p[1]);
        }
        uint32_t u32(uint64_t at) const {
            const uint8_t* p = data + at;
            return little ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
                          : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
        }
    };

    struct TiffEntry {
        uint16_t type = 0;
        uint32_t count = 0;
        uint64_t valueAt = 0;           // where the values are (inline or at the offset)
    };

    struct TiffIfd {
        const TiffSource* src = nullptr;
        std::vector<std::pair<uint16_t, TiffEntry>> entries;

        const TiffEntry* find(uint16_t tag) const {
            for (const auto& [t, e] : entries) if (t == tag) return &e;
            return nullptr;
        }
        // the unsigned integer values of a BYTE, SHORT or LONG entry; false when it has another type
        bool values(uint16_t tag, std::vector<uint32_t>& out) const {
            out.clear();
            const TiffEntry* e = find(tag);
            if (!e) return true;
            const int width = e->type == 1 ? 1 : e->type == 3 ? 2 : e->type == 4 ? 4 : 0;
            if (width == 0) return false;
            out.reserve(e->count);
            for (uint32_t i = 0; i < e->count; i++) {
                const uint64_t at = e->valueAt + (uint64_t)i * width;
                out.push_back(width == 1 ? src->data[at] : width == 2 ? src->u16(at) : src->u32(at));
            }
            return true;
        }
    };

    std::string refuse(const std::string& what) {
        return what + " is not supported (" + TIFF_SUPPORTED_SET + ")";
    }

    const char* compressionName(uint32_t c) {
        switch (c) {
            case 2: return "CCITT RLE";
            case 3: return "CCITT Group 3";
            case 4: return "CCITT Group 4";
            case 6: return "old-style JPEG";
            case 7: return "JPEG";
            case 34712: return "JPEG 2000";
            case 34887: return "LERC";
            case 34925: return "LZMA";
            case 50000: return "Zstandard";
            case 50001: return "WebP";
            default: return "unknown";
        }
    }

    const char* photometricName(uint32_t p) {
        switch (p) {
            case 3: return "palette";
            case 4: return "transparency mask";
            case 5: return "CMYK (separated)";
            case 6: return "YCbCr";
            case 8: return "CIE L*a*b*";
            case 9: return "ICC L*a*b*";
            case 10: return "ITU L*a*b*";
            case 32844: case 32845: return "LogLuv";
            default: return "unknown";
        }
    }

    bool readIfd(const TiffSource& src, uint64_t offset, TiffIfd& ifd, std::string& error) {
        ifd.src = &src;
        if (offset < 8 || !src.has(offset, 2)) {
            error = "the TIFF's first image directory (offset " + std::to_string(offset) + ") is past the end of the file";
            return false;
        }
        const uint16_t count = src.u16(offset);
        if (!src.has(offset + 2, (uint64_t)count * 12)) {
            error = "the TIFF's image directory of " + std::to_string(count) + " entries runs past the end of the file";
            return false;
        }
        for (uint16_t i = 0; i < count; i++) {
            const uint64_t at = offset + 2 + (uint64_t)i * 12;
            TiffEntry e;
            const uint16_t tag = src.u16(at);
            e.type = src.u16(at + 2);
            e.count = src.u32(at + 4);
            static const int typeSizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
            const int typeSize = e.type < 14 ? typeSizes[e.type] : 0;
            if (typeSize == 0) continue;    // unknown types are skipped, as the specification asks
            if (e.count > TIFF_MAX_ENTRY_VALUES) {
                error = "TIFF tag " + std::to_string(tag) + " has " + std::to_string(e.count) + " values";
                return false;
            }
            const uint64_t bytes = (uint64_t)e.count * typeSize;
            e.valueAt = bytes <= 4 ? at + 8 : src.u32(at + 8);
            if (!src.has(e.valueAt, bytes)) {
                error = "TIFF tag " + std::to_string(tag) + "'s values (offset " + std::to_string(e.valueAt) + ") are past the end of the file";
                return false;
            }
            ifd.entries.emplace_back(tag, e);
        }
        return true;
    }

    // one value of an entry that must hold one (or a default when absent)
    bool single(const TiffIfd& ifd, uint16_t tag, uint32_t fallback, uint32_t& out, std::string& error) {
        std::vector<uint32_t> v;
        if (!ifd.values(tag, v)) {
            error = "TIFF tag " + std::to_string(tag) + " has a type that is not an unsigned integer";
            return false;
        }
        out = v.empty() ? fallback : v[0];
        return true;
    }

    // TIFF LZW: MSB-first codes of 9 to 12 bits, Clear 256, EndOfInformation 257, the width growing
    // one code early. Stops when `out` is full.
    bool inflateLzw(const uint8_t* in, size_t n, uint8_t* out, size_t expected, std::string& error) {
        if (n >= 2 && in[0] == 0 && (in[1] & 1)) {
            error = refuse("old-style (pre-6.0) LZW");
            return false;
        }
        struct Code {
            uint16_t prefix;
            uint16_t length;
            uint8_t byte;
            uint8_t first;
        };
        std::vector<Code> table(4096);
        for (int i = 0; i < 256; i++) table[(size_t)i] = {0, 1, (uint8_t)i, (uint8_t)i};
        size_t bitPos = 0, written = 0;
        int width = 9, next = 258, old = -1;
        auto readCode = [&](int& code) {
            if (bitPos + (size_t)width > n * 8) return false;
            uint32_t v = 0;
            for (int b = 0; b < width; b++, bitPos++) v = v << 1 | ((in[bitPos >> 3] >> (7 - (bitPos & 7))) & 1u);
            code = (int)v;
            return true;
        };
        // writes code's string; returns its first byte
        auto emit = [&](int code) {
            const Code& c = table[(size_t)code];
            const size_t len = c.length;
            size_t end = std::min(written + len, expected);
            // walk back from the last byte, keeping only the part that fits
            int at = code;
            for (size_t i = written + len; i-- > written;) {
                if (i < end) out[i] = table[(size_t)at].byte;
                at = table[(size_t)at].prefix;
            }
            written = end;
            return c.first;
        };
        int code = 0;
        while (written < expected && readCode(code)) {
            if (code == 257) break;
            if (code == 256) {
                width = 9;
                next = 258;
                old = -1;
                continue;
            }
            if (old < 0) {
                if (code > 255) {
                    error = "the TIFF's LZW data starts with code " + std::to_string(code);
                    return false;
                }
                emit(code);
                old = code;
                continue;
            }
            uint8_t first = 0;
            if (code < next) {
                first = emit(code);
            } else if (code == next && next < 4096) {
                first = table[(size_t)old].first;
            } else {
                error = "the TIFF's LZW data has code " + std::to_string(code) + " before it is defined";
                return false;
            }
            if (next < 4096) {
                table[(size_t)next] = {(uint16_t)old, (uint16_t)(table[(size_t)old].length + 1), first, table[(size_t)old].first};
                if (code == next) emit(code);
                next++;
                if (next == (1 << width) - 1 && width < 12) width++;
            }
            old = code;
        }
        if (written != expected) {
            error = "the TIFF's LZW data ends after " + std::to_string(written) + " of " + std::to_string(expected) + " bytes";
            return false;
        }
        return true;
    }

    bool inflateZlib(const uint8_t* in, size_t n, uint8_t* out, size_t expected, std::string& error) {
        mz_stream stream;
        std::memset(&stream, 0, sizeof(stream));
        if (mz_inflateInit(&stream) != MZ_OK) {
            error = "the Deflate decoder did not start";
            return false;
        }
        stream.next_in = in;
        stream.avail_in = (unsigned int)std::min<size_t>(n, 0xffffffffu);
        stream.next_out = out;
        stream.avail_out = (unsigned int)expected;
        const int status = mz_inflate(&stream, MZ_FINISH);
        const size_t written = expected - stream.avail_out;
        mz_inflateEnd(&stream);
        // a stream that fills the chunk is enough, whatever follows it
        if (written != expected || (status != MZ_STREAM_END && status != MZ_OK && status != MZ_BUF_ERROR)) {
            error = "the TIFF's Deflate data inflates to " + std::to_string(written) + " of " + std::to_string(expected) + " bytes";
            return false;
        }
        return true;
    }

    bool inflatePackBits(const uint8_t* in, size_t n, uint8_t* out, size_t expected, std::string& error) {
        size_t i = 0, written = 0;
        while (written < expected && i < n) {
            const int8_t header = (int8_t)in[i++];
            if (header >= 0) {
                const size_t count = (size_t)header + 1;
                if (i + count > n || written + count > expected) break;
                std::memcpy(out + written, in + i, count);
                i += count;
                written += count;
            } else if (header != -128) {
                const size_t count = (size_t)(1 - header);
                if (i >= n || written + count > expected) break;
                std::memset(out + written, in[i++], count);
                written += count;
            }
        }
        if (written != expected) {
            error = "the TIFF's PackBits data gives " + std::to_string(written) + " of " + std::to_string(expected) + " bytes";
            return false;
        }
        return true;
    }

}

bool editor::readTiff(const uint8_t* data, size_t size, TiffImage& out, std::string& error) {
    out = TiffImage();
    TiffSource src{data, size, true};
    if (size < 8 || !((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))) {
        error = "not a TIFF file (no II or MM byte order mark)";
        return false;
    }
    src.little = data[0] == 'I';
    const uint16_t magic = src.u16(2);
    if (magic == 43) {
        error = refuse("BigTIFF");
        return false;
    }
    if (magic != 42) {
        error = "not a TIFF file (version " + std::to_string(magic) + ", not 42)";
        return false;
    }
    TiffIfd ifd;
    if (!readIfd(src, src.u32(4), ifd, error)) return false;

    uint32_t width = 0, height = 0, compression = 1, photometric = 0, samples = 1, planar = 1, predictor = 1, fillOrder = 1;
    if (!single(ifd, TAG_WIDTH, 0, width, error) || !single(ifd, TAG_HEIGHT, 0, height, error) ||
        !single(ifd, TAG_COMPRESSION, 1, compression, error) || !single(ifd, TAG_SAMPLES, 1, samples, error) ||
        !single(ifd, TAG_PLANAR, 1, planar, error) || !single(ifd, TAG_PREDICTOR, 1, predictor, error) ||
        !single(ifd, TAG_FILL_ORDER, 1, fillOrder, error))
        return false;
    if (!ifd.find(TAG_PHOTOMETRIC)) {
        error = "the TIFF has no PhotometricInterpretation";
        return false;
    }
    if (!single(ifd, TAG_PHOTOMETRIC, 0, photometric, error)) return false;
    if (width == 0 || height == 0 || width > TIFF_MAX_SIDE || height > TIFF_MAX_SIDE || (uint64_t)width * height > TIFF_MAX_PIXELS) {
        error = "a TIFF of " + std::to_string(width) + " x " + std::to_string(height) + " px is empty or over the reader's " +
                std::to_string(TIFF_MAX_SIDE) + " px a side and 2^27 pixels";
        return false;
    }
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 32773) {
        error = refuse("TIFF compression " + std::to_string(compression) + " (" + compressionName(compression) + ")");
        return false;
    }
    if (photometric > 2) {
        error = refuse("TIFF photometric interpretation " + std::to_string(photometric) + " (" + photometricName(photometric) + ")");
        return false;
    }
    if (fillOrder != 1) {
        error = refuse("TIFF FillOrder " + std::to_string(fillOrder));
        return false;
    }
    if (planar != 1 && planar != 2) {
        error = "the TIFF's PlanarConfiguration " + std::to_string(planar) + " is unknown";
        return false;
    }
    const uint32_t colours = photometric == 2 ? 3 : 1;
    if (samples < colours || samples > 16) {
        error = "a TIFF with " + std::to_string(samples) + " samples a pixel for " + std::to_string(colours) + " colour channels";
        return false;
    }
    std::vector<uint32_t> bits, formats, extras;
    if (!ifd.values(TAG_BITS, bits) || !ifd.values(TAG_SAMPLE_FORMAT, formats) || !ifd.values(TAG_EXTRA_SAMPLES, extras)) {
        error = "a TIFF's BitsPerSample, SampleFormat or ExtraSamples is not an unsigned integer";
        return false;
    }
    if (bits.empty()) bits.assign(1, 1);
    const uint32_t depth = bits[0];
    for (uint32_t b : bits) {
        if (b != depth) {
            error = refuse("a TIFF whose samples have different bit depths");
            return false;
        }
    }
    if (depth != 8 && depth != 16) {
        error = refuse(std::to_string(depth) + "-bit TIFF samples");
        return false;
    }
    for (uint32_t f : formats) {
        if (f != 1) {
            error = refuse(std::string("TIFF sample format ") + (f == 2 ? "signed integer" : f == 3 ? "floating point" : std::to_string(f)));
            return false;
        }
    }
    if (predictor != 1 && predictor != 2) {
        error = refuse("TIFF predictor " + std::to_string(predictor) + (predictor == 3 ? " (floating point)" : ""));
        return false;
    }
    // the first extra sample is the alpha; an unspecified one too, as libtiff reads it
    const bool hasAlpha = samples > colours;
    const bool associated = hasAlpha && (extras.empty() || extras[0] != 2);

    // the chunks: strips, or tiles when the tile tags are there
    const bool tiled = ifd.find(TAG_TILE_WIDTH) != nullptr;
    uint32_t chunkWidth = width, chunkHeight = height;
    std::vector<uint32_t> offsets, byteCounts;
    if (tiled) {
        if (!single(ifd, TAG_TILE_WIDTH, 0, chunkWidth, error) || !single(ifd, TAG_TILE_HEIGHT, 0, chunkHeight, error)) return false;
        if (!ifd.values(TAG_TILE_OFFSETS, offsets) || !ifd.values(TAG_TILE_BYTES, byteCounts)) {
            error = "the TIFF's TileOffsets or TileByteCounts is not an unsigned integer";
            return false;
        }
    } else {
        uint32_t rows = height;
        if (!single(ifd, TAG_ROWS_PER_STRIP, height, rows, error)) return false;
        chunkHeight = std::min(std::max(rows, 1u), height);
        if (!ifd.values(TAG_STRIP_OFFSETS, offsets) || !ifd.values(TAG_STRIP_BYTES, byteCounts)) {
            error = "the TIFF's StripOffsets or StripByteCounts is not an unsigned integer";
            return false;
        }
    }
    if (chunkWidth == 0 || chunkHeight == 0 || (uint64_t)chunkWidth * chunkHeight > TIFF_MAX_CHUNK_PIXELS) {
        error = "the TIFF's " + std::string(tiled ? "tiles" : "strips") + " of " + std::to_string(chunkWidth) + " x " +
                std::to_string(chunkHeight) + " px are empty or too large";
        return false;
    }
    const uint32_t across = (width + chunkWidth - 1) / chunkWidth, down = (height + chunkHeight - 1) / chunkHeight;
    const uint32_t planes = planar == 2 ? samples : 1;
    const uint32_t chunkSamples = planar == 2 ? 1 : samples;
    const uint64_t chunkCount = (uint64_t)across * down * planes;
    if (offsets.size() < chunkCount || byteCounts.size() < chunkCount) {
        error = "the TIFF lists " + std::to_string(std::min(offsets.size(), byteCounts.size())) + " " + (tiled ? "tiles" : "strips") +
                " of the " + std::to_string(chunkCount) + " it needs";
        return false;
    }

    const int bps = depth == 16 ? 2 : 1;
    const uint32_t maxValue = depth == 16 ? 65535 : 255;
    // the samples kept (colours, alpha) of every pixel, native order; other extra samples are dropped
    const uint32_t kept = colours + (hasAlpha ? 1 : 0);
    std::vector<uint16_t> pixels((size_t)width * height * kept, 0);
    std::vector<uint8_t> chunk;
    std::vector<uint16_t> values((size_t)chunkWidth * chunkSamples);
    for (uint32_t plane = 0; plane < std::min(planes, kept); plane++) {
        for (uint32_t cy = 0; cy < down; cy++) {
            for (uint32_t cx = 0; cx < across; cx++) {
                const size_t index = ((size_t)plane * down + cy) * across + cx;
                // a strip holds only the rows left; a tile is always whole
                const uint32_t rows = tiled ? chunkHeight : std::min(chunkHeight, height - cy * chunkHeight);
                const size_t rowBytes = (size_t)chunkWidth * chunkSamples * bps;
                const size_t expected = rowBytes * rows;
                const uint64_t at = offsets[index], length = byteCounts[index];
                if (!src.has(at, length)) {
                    error = "TIFF " + std::string(tiled ? "tile " : "strip ") + std::to_string(index) + " (offset " + std::to_string(at) +
                            ", " + std::to_string(length) + " bytes) is past the end of the file";
                    return false;
                }
                chunk.assign(expected, 0);
                const uint8_t* in = data + at;
                bool ok = true;
                switch (compression) {
                    case 1:
                        if (length < expected) {
                            error = "TIFF " + std::string(tiled ? "tile " : "strip ") + std::to_string(index) + " holds " + std::to_string(length) +
                                    " of its " + std::to_string(expected) + " bytes";
                            ok = false;
                        } else {
                            std::memcpy(chunk.data(), in, expected);
                        }
                        break;
                    case 5: ok = inflateLzw(in, (size_t)length, chunk.data(), expected, error); break;
                    case 8: case 32946: ok = inflateZlib(in, (size_t)length, chunk.data(), expected, error); break;
                    default: ok = inflatePackBits(in, (size_t)length, chunk.data(), expected, error); break;
                }
                if (!ok) return false;
                for (uint32_t y = 0; y < rows; y++) {
                    const uint8_t* row = chunk.data() + (size_t)y * rowBytes;
                    for (size_t i = 0; i < values.size(); i++) {
                        values[i] = bps == 1 ? row[i]
                                  : src.little ? (uint16_t)(row[i * 2] | row[i * 2 + 1] << 8) : (uint16_t)(row[i * 2] << 8 | row[i * 2 + 1]);
                    }
                    // Predictor 2 differences each sample from the same sample of the pixel to its left
                    if (predictor == 2) {
                        for (size_t i = chunkSamples; i < values.size(); i++)
                            values[i] = bps == 1 ? (uint8_t)(values[i] + values[i - chunkSamples]) : (uint16_t)(values[i] + values[i - chunkSamples]);
                    }
                    const uint32_t py = cy * chunkHeight + y;
                    if (py >= height) break;
                    for (uint32_t x = 0; x < chunkWidth; x++) {
                        const uint32_t px = cx * chunkWidth + x;
                        if (px >= width) break;
                        uint16_t* dst = &pixels[((size_t)py * width + px) * kept];
                        if (planar == 2) dst[plane] = values[x];
                        else std::memcpy(dst, &values[(size_t)x * samples], kept * sizeof(uint16_t));
                    }
                }
            }
        }
    }

    out.width = (int)width;
    out.height = (int)height;
    out.bitDepth = (int)depth;
    out.rgba.assign((size_t)width * height * 4 * bps, 0);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        const uint16_t* p = &pixels[i * kept];
        uint32_t v[4];
        if (colours == 1) {
            const uint32_t grey = photometric == 0 ? maxValue - p[0] : p[0];
            v[0] = v[1] = v[2] = grey;
        } else {
            v[0] = p[0];
            v[1] = p[1];
            v[2] = p[2];
        }
        v[3] = hasAlpha ? p[colours] : maxValue;
        if (associated) {
            for (int c = 0; c < 3; c++) v[c] = v[3] ? std::min(maxValue, (v[c] * maxValue + v[3] / 2) / v[3]) : 0;
        }
        for (int c = 0; c < 4; c++) {
            if (bps == 1) {
                out.rgba[i * 4 + c] = (uint8_t)v[c];
            } else {
                const uint16_t s = (uint16_t)v[c];
                std::memcpy(out.rgba.data() + (i * 4 + c) * 2, &s, 2);
            }
        }
    }
    return true;
}

std::vector<uint8_t> editor::tiffToRgba8(const TiffImage& image) {
    if (image.bitDepth != 16) return image.rgba;
    std::vector<uint8_t> out(image.rgba.size() / 2);
    for (size_t i = 0; i < out.size(); i++) {
        uint16_t v;
        std::memcpy(&v, image.rgba.data() + i * 2, 2);
        out[i] = (uint8_t)((v * 255u + 32767u) / 65535u);
    }
    return out;
}
