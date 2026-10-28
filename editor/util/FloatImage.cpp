// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/FloatImage.h"

#include "util/FileUtils.h"

#include "stb_image.h"
#include "tinyexr.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace jasperengine::editor {

    namespace {

        const uint8_t exrMagic[4] = {0x76, 0x2f, 0x31, 0x01};
        const uint8_t pngMagic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

        void setError(std::string* error, const std::string& text) {
            if (error)
                *error = text;
        }

        std::string lowerExtension(const std::string& name) {
            const size_t dot = name.find_last_of('.');
            const size_t slash = name.find_last_of("/\\");
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                return "";
            std::string ext = name.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        // tinyexr's message (freed here) in parentheses, or its return code
        std::string exrReason(const char* message, int code) {
            std::string reason;
            if (message) {
                reason = message;
                FreeEXRErrorMessage(message);
                while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r' || reason.back() == ' '))
                    reason.pop_back();
            }
            if (reason.empty())
                reason = code != 0 ? "tinyexr error " + std::to_string(code) : std::string("no reason given");
            return reason;
        }

        // R, G, B, A, Y of an EXR channel name: the part after the last '.', any case
        char channelRole(const char* name) {
            const char* dot = std::strrchr(name, '.');
            const char* base = dot ? dot + 1 : name;
            if (base[0] == '\0' || base[1] != '\0')
                return 0;
            const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
            return (c == 'R' || c == 'G' || c == 'B' || c == 'A' || c == 'Y') ? c : 0;
        }

        bool readExr(const uint8_t* bytes, size_t size, const std::string& name, FloatImage& out, std::string* error) {
            EXRVersion version;
            int ret = ParseEXRVersionFromMemory(&version, bytes, size);
            if (ret != TINYEXR_SUCCESS) {
                setError(error, name + ": not a valid EXR file (its version header can't be read, tinyexr error " + std::to_string(ret) + ")");
                return false;
            }
            if (version.multipart || version.non_image) {
                setError(error, name + ": multi-part and deep EXR images are not supported (export a single-part scanline or tiled image)");
                return false;
            }

            EXRHeader header;
            InitEXRHeader(&header);
            const char* message = nullptr;
            ret = ParseEXRHeaderFromMemory(&header, &version, bytes, size, &message);
            if (ret != TINYEXR_SUCCESS) {
                setError(error, name + ": corrupt or truncated EXR header (" + exrReason(message, ret) + ")");
                FreeEXRHeader(&header);
                return false;
            }

            // the channel of each role, -1 when absent
            int roles[5] = {-1, -1, -1, -1, -1}; // R G B A Y
            std::string names;
            for (int c = 0; c < header.num_channels; c++) {
                const EXRChannelInfo& info = header.channels[c];
                names += (names.empty() ? "" : ", ") + std::string(info.name);
                if (info.x_sampling != 1 || info.y_sampling != 1) {
                    setError(error, name + ": subsampled EXR channels are not supported (channel '" + info.name + "')");
                    FreeEXRHeader(&header);
                    return false;
                }
                const char role = channelRole(info.name);
                const int slot = role == 'R' ? 0 : role == 'G' ? 1 : role == 'B' ? 2 : role == 'A' ? 3 : role == 'Y' ? 4 : -1;
                if (slot >= 0 && roles[slot] < 0)
                    roles[slot] = c;
                // half is converted to float on load (exactly); float and uint stay as they are
                if (header.pixel_types[c] == TINYEXR_PIXELTYPE_HALF)
                    header.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
            }

            // what feeds r, g, b, a (-1: the default, 0 for colour and 1 for alpha)
            int source[4] = {-1, -1, -1, -1};
            int channelsUsed = 0;
            if (roles[0] >= 0 || roles[1] >= 0 || roles[2] >= 0) {
                for (int i = 0; i < 4; i++) {
                    source[i] = roles[i];
                    channelsUsed += roles[i] >= 0 ? 1 : 0;
                }
            } else if (roles[4] >= 0 || header.num_channels == 1) {
                const int grey = roles[4] >= 0 ? roles[4] : 0;
                source[0] = source[1] = source[2] = grey;
                source[3] = (roles[4] >= 0) ? roles[3] : -1;
                channelsUsed = source[3] >= 0 ? 2 : 1;
            } else {
                setError(error, name + ": no R, G, B or Y channel in the EXR (channels: " + names + ")");
                FreeEXRHeader(&header);
                return false;
            }

            EXRImage image;
            InitEXRImage(&image);
            message = nullptr;
            ret = LoadEXRImageFromMemory(&image, &header, bytes, size, &message);
            if (ret != TINYEXR_SUCCESS) {
                setError(error, name + ": corrupt or truncated EXR data (" + exrReason(message, ret) + ")");
                FreeEXRHeader(&header);
                return false;
            }
            if (image.width <= 0 || image.height <= 0 || (!image.images && !image.tiles)) {
                setError(error, name + ": the EXR image is empty");
                FreeEXRImage(&image);
                FreeEXRHeader(&header);
                return false;
            }

            out.width = image.width;
            out.height = image.height;
            out.rgba.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4, 0.0f);
            out.sourceChannels = channelsUsed;
            out.sourceBits = 0;
            out.sourceFloat = true;
            for (int i = 0; i < 4; i++) {
                if (source[i] < 0)
                    continue;
                // pixel_types now holds the loaded (requested) type
                const int type = header.pixel_types[source[i]];
                out.sourceBits = std::max(out.sourceBits, header.channels[source[i]].pixel_type == TINYEXR_PIXELTYPE_HALF ? 16 : 32);
                if (type == TINYEXR_PIXELTYPE_UINT)
                    out.sourceFloat = false;
            }

            auto valueOf = [&](int channel, const unsigned char* plane, size_t index) -> float {
                if (header.pixel_types[channel] == TINYEXR_PIXELTYPE_UINT)
                    return static_cast<float>(reinterpret_cast<const uint32_t*>(plane)[index]);
                return reinterpret_cast<const float*>(plane)[index];
            };
            auto store = [&](unsigned char* const* planes, size_t srcIndex, size_t dstTexel) {
                float* texel = &out.rgba[dstTexel * 4];
                for (int i = 0; i < 4; i++)
                    texel[i] = source[i] >= 0 ? valueOf(source[i], planes[source[i]], srcIndex) : (i == 3 ? 1.0f : 0.0f);
            };

            if (image.tiles) {
                // level 0 (the image itself; next_level holds the mipmaps): tiles of
                // tile_size_x * tile_size_y samples, the last ones of a row/column cut
                const size_t tileW = static_cast<size_t>(header.tile_size_x);
                const size_t tileH = static_cast<size_t>(header.tile_size_y);
                for (int t = 0; t < image.num_tiles; t++) {
                    const EXRTile& tile = image.tiles[t];
                    for (int j = 0; j < tile.height; j++) {
                        const size_t y = static_cast<size_t>(tile.offset_y) * tileH + static_cast<size_t>(j);
                        if (y >= static_cast<size_t>(out.height))
                            continue;
                        for (int i = 0; i < tile.width; i++) {
                            const size_t x = static_cast<size_t>(tile.offset_x) * tileW + static_cast<size_t>(i);
                            if (x >= static_cast<size_t>(out.width))
                                continue;
                            store(tile.images, static_cast<size_t>(i) + static_cast<size_t>(j) * tileW, y * static_cast<size_t>(out.width) + x);
                        }
                    }
                }
            } else {
                const size_t count = static_cast<size_t>(out.width) * static_cast<size_t>(out.height);
                for (size_t p = 0; p < count; p++)
                    store(image.images, p, p);
            }

            FreeEXRImage(&image);
            FreeEXRHeader(&header);
            return true;
        }

        bool readStb(const uint8_t* bytes, size_t size, const std::string& name, bool png, FloatImage& out, std::string* error) {
            const char* kind = png ? "PNG" : "TGA";
            if (size > static_cast<size_t>(INT_MAX)) {
                setError(error, name + ": the file is too large");
                return false;
            }
            const int length = static_cast<int>(size);
            int width = 0, height = 0, channels = 0;
            if (!stbi_info_from_memory(bytes, length, &width, &height, &channels)) {
                const char* reason = stbi_failure_reason();
                setError(error, name + ": corrupt or truncated " + kind + " (" + (reason ? reason : "unknown") + ")");
                return false;
            }
            const bool sixteen = png && stbi_is_16_bit_from_memory(bytes, length);
            void* pixels = sixteen ? static_cast<void*>(stbi_load_16_from_memory(bytes, length, &width, &height, &channels, 0))
                                   : static_cast<void*>(stbi_load_from_memory(bytes, length, &width, &height, &channels, 0));
            if (!pixels) {
                const char* reason = stbi_failure_reason();
                setError(error, name + ": corrupt or truncated " + kind + " (" + (reason ? reason : "unknown") + ")");
                return false;
            }
            if (width <= 0 || height <= 0 || channels < 1 || channels > 4) {
                stbi_image_free(pixels);
                setError(error, name + ": unsupported " + kind + " layout (" + std::to_string(channels) + " channels)");
                return false;
            }

            const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
            out.width = width;
            out.height = height;
            out.sourceChannels = channels;
            out.sourceBits = sixteen ? 16 : 8;
            out.sourceFloat = false;
            out.rgba.resize(count * 4);
            if (!sixteen)
                out.raw8.resize(count * 4);

            const float scale = sixteen ? 65535.0f : 255.0f;
            const uint32_t opaque = sixteen ? 65535u : 255u;
            for (size_t p = 0; p < count; p++) {
                uint32_t v[4] = {0, 0, 0, opaque};
                uint32_t s[4] = {0, 0, 0, 0};
                for (int c = 0; c < channels; c++)
                    s[c] = sixteen ? static_cast<const uint16_t*>(pixels)[p * channels + c] : static_cast<const uint8_t*>(pixels)[p * channels + c];
                if (channels <= 2) {
                    v[0] = v[1] = v[2] = s[0];
                    if (channels == 2)
                        v[3] = s[1];
                } else {
                    v[0] = s[0];
                    v[1] = s[1];
                    v[2] = s[2];
                    if (channels == 4)
                        v[3] = s[3];
                }
                for (int c = 0; c < 4; c++) {
                    out.rgba[p * 4 + c] = static_cast<float>(v[c]) / scale;
                    if (!sixteen)
                        out.raw8[p * 4 + c] = static_cast<uint8_t>(v[c]);
                }
            }
            stbi_image_free(pixels);
            return true;
        }

        bool writeBytes(const std::string& path, const unsigned char* bytes, size_t size, std::string* error) {
            std::ofstream file(FileUtils::pathFromUtf8(path), std::ios::binary | std::ios::trunc);
            if (!file) {
                setError(error, path + ": can't be written");
                return false;
            }
            file.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
            file.close();
            if (!file) {
                setError(error, path + ": can't be written");
                return false;
            }
            return true;
        }

        int exrCompression(ExrWriteOptions::Compression compression) {
            switch (compression) {
                case ExrWriteOptions::Compression::None:  return TINYEXR_COMPRESSIONTYPE_NONE;
                case ExrWriteOptions::Compression::Rle:   return TINYEXR_COMPRESSIONTYPE_RLE;
                case ExrWriteOptions::Compression::Zips:  return TINYEXR_COMPRESSIONTYPE_ZIPS;
                case ExrWriteOptions::Compression::Zip:   return TINYEXR_COMPRESSIONTYPE_ZIP;
                case ExrWriteOptions::Compression::Piz:   return TINYEXR_COMPRESSIONTYPE_PIZ;
                case ExrWriteOptions::Compression::Pxr24: return TINYEXR_COMPRESSIONTYPE_PXR24;
                case ExrWriteOptions::Compression::B44:   return TINYEXR_COMPRESSIONTYPE_B44;
                case ExrWriteOptions::Compression::B44a:  return TINYEXR_COMPRESSIONTYPE_B44A;
            }
            return TINYEXR_COMPRESSIONTYPE_ZIP;
        }

        // IEEE binary16, rounded to nearest, ties to even (overflow to infinity, NaN kept a NaN)
        uint16_t floatToHalf(float value) {
            uint32_t x;
            std::memcpy(&x, &value, sizeof(x));
            const uint32_t sign = (x >> 16) & 0x8000u;
            const uint32_t exponent = (x >> 23) & 0xFFu;
            const uint32_t mantissa = x & 0x7FFFFFu;
            if (exponent == 0xFF)
                return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u | (mantissa >> 13) : 0u));
            const int e = static_cast<int>(exponent) - 127 + 15;
            if (e >= 31)
                return static_cast<uint16_t>(sign | 0x7C00u);
            if (e <= 0) {
                // subnormal half (or zero): the implicit bit joins the mantissa
                if (e < -10)
                    return static_cast<uint16_t>(sign);
                const uint32_t full = mantissa | 0x800000u;
                const int shift = 14 - e;
                uint32_t half = full >> shift;
                const uint32_t rest = full & ((1u << shift) - 1u);
                const uint32_t halfway = 1u << (shift - 1);
                if (rest > halfway || (rest == halfway && (half & 1u)))
                    half++;
                return static_cast<uint16_t>(sign | half);
            }
            uint32_t half = sign | (static_cast<uint32_t>(e) << 10) | (mantissa >> 13);
            const uint32_t rest = mantissa & 0x1FFFu;
            if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
                half++;   // a carry into the exponent is right, up to infinity
            return static_cast<uint16_t>(half);
        }

        uint32_t toUint(float v) {
            if (!(v > 0.0f))
                return 0;
            if (v >= 4294967295.0f)
                return 0xFFFFFFFFu;
            return static_cast<uint32_t>(std::lround(static_cast<double>(v)));
        }

    } // namespace

    bool readFloatImageFromMemory(const uint8_t* bytes, size_t size, const std::string& name, FloatImage& out, std::string* error) {
        out = FloatImage();
        if (!bytes || size == 0) {
            setError(error, name + ": the file is empty");
            return false;
        }
        const std::string ext = lowerExtension(name);
        bool ok = false;
        if (size >= sizeof(exrMagic) && std::memcmp(bytes, exrMagic, sizeof(exrMagic)) == 0) {
            ok = readExr(bytes, size, name, out, error);
        } else if (size >= sizeof(pngMagic) && std::memcmp(bytes, pngMagic, sizeof(pngMagic)) == 0) {
            ok = readStb(bytes, size, name, true, out, error);
        } else if (ext == ".tga") {
            ok = readStb(bytes, size, name, false, out, error);
        } else if (ext == ".exr" || ext == ".png") {
            setError(error, name + ": not " + (ext == ".exr" ? std::string("an EXR") : std::string("a PNG")) + " file (its magic number is missing)");
        } else {
            setError(error, name + ": unsupported image format (EXR, PNG and TGA are read)");
        }
        if (!ok)
            out = FloatImage();
        return ok;
    }

    bool readFloatImage(const std::string& path, FloatImage& out, std::string* error) {
        out = FloatImage();
        const std::filesystem::path filePath = FileUtils::pathFromUtf8(path);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(filePath, ec)) {
            setError(error, path + ": file not found");
            return false;
        }
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            setError(error, path + ": can't be read");
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (file.bad()) {
            setError(error, path + ": can't be read");
            return false;
        }
        return readFloatImageFromMemory(bytes.data(), bytes.size(), path, out, error);
    }

    bool writeExr(const std::string& path, const FloatImage& image, bool half, std::string* error) {
        ExrWriteOptions options;
        options.pixelType = half ? ExrWriteOptions::PixelType::Half : ExrWriteOptions::PixelType::Float;
        return writeExr(path, image, options, error);
    }

    bool writeExr(const std::string& path, const FloatImage& image, const ExrWriteOptions& options, std::string* error) {
        const size_t count = static_cast<size_t>(std::max(image.width, 0)) * static_cast<size_t>(std::max(image.height, 0));
        if (image.width <= 0 || image.height <= 0 || image.rgba.size() != count * 4) {
            setError(error, path + ": the image is empty or its rgba data isn't width * height * 4 floats");
            return false;
        }
        if (options.tiled && (options.tileSize <= 0 || options.tileSize > image.width || options.tileSize > image.height)) {
            setError(error, path + ": tile size " + std::to_string(options.tileSize) + " must be 1 to the image's smaller side");
            return false;
        }

        // channels in alphabetical order (as EXR readers expect), each with its rgba component
        const char* names1[] = {"Y"};
        const int comps1[] = {0};
        const char* names2[] = {"A", "Y"};
        const int comps2[] = {3, 0};
        const char* names3[] = {"B", "G", "R"};
        const int comps3[] = {2, 1, 0};
        const char* names4[] = {"A", "B", "G", "R"};
        const int comps4[] = {3, 2, 1, 0};
        const int channels = (image.sourceChannels >= 1 && image.sourceChannels <= 4) ? image.sourceChannels : 4;
        const char* const* names = channels == 1 ? names1 : channels == 2 ? names2 : channels == 3 ? names3 : names4;
        const int* comps = channels == 1 ? comps1 : channels == 2 ? comps2 : channels == 3 ? comps3 : comps4;

        // samples are converted here (half: round to nearest even), so tinyexr stores them as given
        const int storedType = options.pixelType == ExrWriteOptions::PixelType::Uint ? TINYEXR_PIXELTYPE_UINT :
                               options.pixelType == ExrWriteOptions::PixelType::Half ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
        const size_t sampleBytes = storedType == TINYEXR_PIXELTYPE_HALF ? 2 : 4;

        // one plane per channel
        std::vector<std::vector<uint8_t>> planes(static_cast<size_t>(channels), std::vector<uint8_t>(count * sampleBytes));
        for (int c = 0; c < channels; c++) {
            for (size_t p = 0; p < count; p++) {
                const float v = image.rgba[p * 4 + static_cast<size_t>(comps[c])];
                uint8_t* sample = &planes[c][p * sampleBytes];
                if (storedType == TINYEXR_PIXELTYPE_HALF) {
                    const uint16_t h = floatToHalf(v);
                    std::memcpy(sample, &h, sizeof(h));
                } else if (storedType == TINYEXR_PIXELTYPE_UINT) {
                    const uint32_t u = toUint(v);
                    std::memcpy(sample, &u, sizeof(u));
                } else {
                    std::memcpy(sample, &v, sizeof(v));
                }
            }
        }

        EXRHeader header;
        InitEXRHeader(&header);
        std::vector<EXRChannelInfo> channelInfos(static_cast<size_t>(channels));
        std::vector<int> pixelTypes(static_cast<size_t>(channels), storedType);
        std::vector<int> requestedTypes(static_cast<size_t>(channels), storedType);
        for (int c = 0; c < channels; c++) {
            std::memset(&channelInfos[c], 0, sizeof(EXRChannelInfo));
            std::snprintf(channelInfos[c].name, sizeof(channelInfos[c].name), "%s", names[c]);
            channelInfos[c].pixel_type = storedType;
            channelInfos[c].x_sampling = 1;
            channelInfos[c].y_sampling = 1;
        }
        header.num_channels = channels;
        header.channels = channelInfos.data();
        header.pixel_types = pixelTypes.data();
        header.requested_pixel_types = requestedTypes.data();
        header.compression_type = exrCompression(options.compression);
        header.data_window.max_x = header.display_window.max_x = image.width - 1;
        header.data_window.max_y = header.display_window.max_y = image.height - 1;

        EXRImage exrImage;
        InitEXRImage(&exrImage);
        exrImage.width = image.width;
        exrImage.height = image.height;
        exrImage.num_channels = channels;

        std::vector<unsigned char*> planePointers(static_cast<size_t>(channels));
        // tiled: per tile, one tileSize x tileSize buffer per channel (the cut tiles of the last
        // row/column use their top-left part)
        std::vector<EXRTile> tiles;
        std::vector<std::vector<uint8_t>> tileData;
        std::vector<std::vector<unsigned char*>> tilePointers;
        if (options.tiled) {
            const int size = options.tileSize;
            const int tilesX = (image.width + size - 1) / size;
            const int tilesY = (image.height + size - 1) / size;
            header.tiled = 1;
            header.tile_size_x = size;
            header.tile_size_y = size;
            header.tile_level_mode = TINYEXR_TILE_ONE_LEVEL;
            header.tile_rounding_mode = TINYEXR_TILE_ROUND_DOWN;
            tiles.resize(static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY));
            tileData.resize(tiles.size() * static_cast<size_t>(channels));
            tilePointers.resize(tiles.size());
            for (int ty = 0; ty < tilesY; ty++) {
                for (int tx = 0; tx < tilesX; tx++) {
                    const size_t t = static_cast<size_t>(ty) * static_cast<size_t>(tilesX) + static_cast<size_t>(tx);
                    EXRTile& tile = tiles[t];
                    std::memset(&tile, 0, sizeof(EXRTile));
                    tile.offset_x = tx;
                    tile.offset_y = ty;
                    tile.width = std::min(size, image.width - tx * size);
                    tile.height = std::min(size, image.height - ty * size);
                    tilePointers[t].resize(static_cast<size_t>(channels));
                    for (int c = 0; c < channels; c++) {
                        std::vector<uint8_t>& buffer = tileData[t * static_cast<size_t>(channels) + static_cast<size_t>(c)];
                        buffer.assign(static_cast<size_t>(size) * static_cast<size_t>(size) * sampleBytes, 0u);
                        for (int j = 0; j < tile.height; j++) {
                            // one tile row: tile.width samples
                            const size_t src = static_cast<size_t>(ty * size + j) * static_cast<size_t>(image.width) + static_cast<size_t>(tx * size);
                            std::memcpy(&buffer[static_cast<size_t>(j) * static_cast<size_t>(size) * sampleBytes], &planes[c][src * sampleBytes],
                                        static_cast<size_t>(tile.width) * sampleBytes);
                        }
                        tilePointers[t][c] = reinterpret_cast<unsigned char*>(buffer.data());
                    }
                    tile.images = tilePointers[t].data();
                }
            }
            exrImage.tiles = tiles.data();
            exrImage.num_tiles = static_cast<int>(tiles.size());
        } else {
            for (int c = 0; c < channels; c++)
                planePointers[c] = reinterpret_cast<unsigned char*>(planes[c].data());
            exrImage.images = planePointers.data();
        }

        unsigned char* memory = nullptr;
        const char* message = nullptr;
        const size_t written = SaveEXRImageToMemory(&exrImage, &header, &memory, &message);
        if (written == 0 || !memory) {
            setError(error, path + ": the EXR could not be encoded (" + exrReason(message, 0) + ")");
            if (memory)
                std::free(memory);
            return false;
        }
        if (message)
            FreeEXRErrorMessage(message);
        const bool ok = writeBytes(path, memory, written, error);
        std::free(memory);
        return ok;
    }

} // namespace jasperengine::editor
