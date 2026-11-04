// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SynthImage.h"

#include "util/PngWriter.h"
#include "util/SHA1.h"

#include "stb_image.h"
#include "stb_image_resize2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

// stb_image_write's in-memory PNG encoder: compiled into the engine (engine/libs/stb), declared
// only in the implementation part of its header
extern "C" unsigned char* stbi_write_png_to_mem(const unsigned char* pixels, int stride_bytes, int x, int y, int n, int* out_len);

namespace fs = std::filesystem;

namespace jasperengine::editor::texsynth {

    Image Image::make(int width, int height, int channels, int bitDepth) {
        Image image;
        if (width <= 0 || height <= 0 || channels < 1 || channels > 4 || (bitDepth != 8 && bitDepth != 16)) {
            return image;
        }
        image.width = width;
        image.height = height;
        image.channels = channels;
        image.bitDepth = bitDepth;
        image.data.assign(image.pixelCount() * image.bytesPerPixel(), 0);
        return image;
    }

    std::string describeFormat(const Image& image) {
        static const char* names[5] = {"?", "grey", "grey+alpha", "RGB", "RGBA"};
        const int c = (image.channels >= 1 && image.channels <= 4) ? image.channels : 0;
        return std::to_string(image.bitDepth) + "-bit " + names[c];
    }

    std::string sha1Hex(const uint8_t* bytes, size_t size) {
        return SHA1::hash(std::string(reinterpret_cast<const char*>(bytes), size));
    }

    bool loadImageFromMemory(const uint8_t* bytes, size_t size, Image& out, std::string& error, int maxSide) {
        out = Image();
        if (!bytes || size == 0 || size > static_cast<size_t>(INT32_MAX)) {
            error = "empty or too large file";
            return false;
        }
        const int length = static_cast<int>(size);
        if (stbi_is_hdr_from_memory(bytes, length)) {
            error = "HDR images are not supported (8- and 16-bit only)";
            return false;
        }
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_memory(bytes, length, &w, &h, &comp)) {
            const char* reason = stbi_failure_reason();
            error = std::string("not a readable image") + (reason ? std::string(" (") + reason + ")" : std::string());
            return false;
        }
        // the header gave the size: a too large image is refused before it is decoded
        if (maxSide > 0 && (w > maxSide || h > maxSide)) {
            error = "the image is " + std::to_string(w) + "x" + std::to_string(h) + ", at most " + std::to_string(maxSide) + " per side (scale it down)";
            return false;
        }
        const bool sixteen = stbi_is_16_bit_from_memory(bytes, length) != 0;
        void* pixels = sixteen ? static_cast<void*>(stbi_load_16_from_memory(bytes, length, &w, &h, &comp, 0))
                               : static_cast<void*>(stbi_load_from_memory(bytes, length, &w, &h, &comp, 0));
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            error = std::string("decoding failed") + (reason ? std::string(" (") + reason + ")" : std::string());
            return false;
        }
        out = Image::make(w, h, comp, sixteen ? 16 : 8);
        if (out.empty()) {
            stbi_image_free(pixels);
            error = "unsupported image layout";
            return false;
        }
        std::memcpy(out.data.data(), pixels, out.data.size());
        stbi_image_free(pixels);
        return true;
    }

    bool probeImage(const fs::path& path, int& width, int& height) {
        int comp = 0;
        width = height = 0;
        // through an fs::path stream (a narrow path string is not the file's name on Windows
        // when it has non-ASCII characters); stb reads only the header
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        stbi_io_callbacks callbacks;
        callbacks.read = [](void* user, char* data, int size) {
            std::ifstream& in = *static_cast<std::ifstream*>(user);
            in.read(data, size);
            return static_cast<int>(in.gcount());
        };
        callbacks.skip = [](void* user, int n) {
            std::ifstream& in = *static_cast<std::ifstream*>(user);
            in.clear();
            in.seekg(n, std::ios::cur);
        };
        callbacks.eof = [](void* user) {
            std::ifstream& in = *static_cast<std::ifstream*>(user);
            return in.peek() == std::ifstream::traits_type::eof() ? 1 : 0;
        };
        return stbi_info_from_callbacks(&callbacks, &file, &width, &height, &comp) != 0;
    }

    bool loadImage(const fs::path& path, Image& out, std::string& error, std::string* sha1, int maxSide) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "cannot open " + path.string();
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (sha1) *sha1 = sha1Hex(bytes.data(), bytes.size());
        if (!loadImageFromMemory(bytes.data(), bytes.size(), out, error, maxSide)) {
            error = path.string() + ": " + error;
            return false;
        }
        return true;
    }

    bool encodePng(const Image& image, std::vector<uint8_t>& png) {
        png.clear();
        if (image.empty() || image.channels < 1 || image.channels > 4) return false;
        if (image.bitDepth == 16) {
            return encodePng16(image.width, image.height, image.channels, image.data.data(), image.data.size(), png);
        }
        if (image.bitDepth != 8) return false;
        int length = 0;
        unsigned char* encoded = stbi_write_png_to_mem(image.data.data(), static_cast<int>(image.width * image.bytesPerPixel()),
                                                       image.width, image.height, image.channels, &length);
        if (!encoded) return false;
        png.assign(encoded, encoded + length);
        std::free(encoded);   // STBIW_MALLOC is malloc
        return true;
    }

    namespace {
        inline uint16_t read16(const uint8_t* p) {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        inline void write16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
    }

    Image toRgba8(const Image& image) {
        Image out = Image::make(image.width, image.height, 4, 8);
        if (out.empty() || image.empty()) return out;
        const size_t n = image.pixelCount();
        const int c = image.channels;
        const size_t bpp = image.bytesPerPixel();
        for (size_t i = 0; i < n; i++) {
            const uint8_t* src = image.data.data() + i * bpp;
            uint8_t s[4] = {0, 0, 0, 255};
            for (int k = 0; k < c; k++) {
                s[k] = image.bitDepth == 16 ? static_cast<uint8_t>((read16(src + 2 * k) + 128) / 257) : src[k];
            }
            uint8_t* dst = out.data.data() + i * 4;
            if (c <= 2) {
                dst[0] = dst[1] = dst[2] = s[0];
                dst[3] = c == 2 ? s[1] : 255;
            } else {
                dst[0] = s[0];
                dst[1] = s[1];
                dst[2] = s[2];
                dst[3] = c == 4 ? s[3] : 255;
            }
        }
        return out;
    }

    bool widenTo(const Image& image, int channels, int bitDepth, Image& out) {
        const bool srcColour = image.channels >= 3, srcAlpha = image.channels == 2 || image.channels == 4;
        const bool dstColour = channels >= 3, dstAlpha = channels == 2 || channels == 4;
        if ((srcColour && !dstColour) || (srcAlpha && !dstAlpha) || image.bitDepth > bitDepth) return false;
        if (image.channels == channels && image.bitDepth == bitDepth) {
            out = image;
            return true;
        }
        out = Image::make(image.width, image.height, channels, bitDepth);
        if (out.empty()) return false;
        const uint32_t opaque = bitDepth == 16 ? 65535u : 255u;
        const size_t n = image.pixelCount();
        for (size_t i = 0; i < n; i++) {
            const uint8_t* src = image.data.data() + i * image.bytesPerPixel();
            uint32_t s[4] = {0, 0, 0, opaque};
            for (int k = 0; k < image.channels; k++) {
                const uint32_t v = image.bitDepth == 16 ? read16(src + 2 * k) : src[k];
                s[k] = (image.bitDepth == 8 && bitDepth == 16) ? v * 257u : v;
            }
            // to grey/RGB + alpha slots: g, a | r, g, b, a
            uint32_t colour[3], alpha = opaque;
            if (image.channels <= 2) {
                colour[0] = colour[1] = colour[2] = s[0];
                if (image.channels == 2) alpha = s[1];
            } else {
                colour[0] = s[0];
                colour[1] = s[1];
                colour[2] = s[2];
                if (image.channels == 4) alpha = s[3];
            }
            uint32_t d[4];
            int dc = 0;
            if (dstColour) {
                d[dc++] = colour[0];
                d[dc++] = colour[1];
                d[dc++] = colour[2];
            } else {
                d[dc++] = colour[0];
            }
            if (dstAlpha) d[dc++] = alpha;
            uint8_t* dst = out.data.data() + i * out.bytesPerPixel();
            for (int k = 0; k < dc; k++) {
                if (bitDepth == 16) write16(dst + 2 * k, static_cast<uint16_t>(d[k]));
                else dst[k] = static_cast<uint8_t>(d[k]);
            }
        }
        return true;
    }

    void unionFormat(const std::vector<const Image*>& images, int& channels, int& bitDepth) {
        bool colour = false, alpha = false;
        bitDepth = 8;
        for (const Image* image : images) {
            if (!image) continue;
            colour = colour || image->channels >= 3;
            alpha = alpha || image->channels == 2 || image->channels == 4;
            bitDepth = std::max(bitDepth, image->bitDepth);
        }
        channels = colour ? (alpha ? 4 : 3) : (alpha ? 2 : 1);
    }

    Image resizeRgba8(const Image& image, int width, int height) {
        if (image.width == width && image.height == height) return image;
        Image out = Image::make(width, height, 4, 8);
        if (out.empty() || image.empty()) return out;
        // channels independent (no alpha weighting: guides and masks are data), clamped edges
        stbir_resize(image.data.data(), image.width, image.height, 0, out.data.data(), width, height, 0, STBIR_4CHANNEL,
                     STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_TRIANGLE);
        return out;
    }

    Image blurRgba8(const Image& image, float sigma, const std::atomic<bool>* cancel) {
        if (image.empty() || sigma <= 0.0f) return image;
        const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
        std::vector<float> kernel(2 * radius + 1);
        float sum = 0.0f;
        for (int i = -radius; i <= radius; i++) {
            kernel[i + radius] = std::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
            sum += kernel[i + radius];
        }
        for (float& k : kernel) k /= sum;
        const int w = image.width, h = image.height;
        // the horizontal pass of the rows the vertical pass reads, in a ring of 2 * radius + 1 rows
        // (row r in slot r % ring): the same sums in the same order as a full-size pass, a
        // fraction of its memory (16 bytes per pixel at 8192 x 8192 would be 1 GB)
        const int ring = 2 * radius + 1;
        std::vector<float> rows(static_cast<size_t>(ring) * w * 4);
        std::vector<int> rowIn(ring, -1);
        auto horizontal = [&](int y) -> const float* {
            float* t = rows.data() + static_cast<size_t>(y % ring) * w * 4;
            if (rowIn[y % ring] == y) return t;
            rowIn[y % ring] = y;
            for (int x = 0; x < w; x++) {
                float acc[4] = {0, 0, 0, 0};
                for (int i = -radius; i <= radius; i++) {
                    const int sx = std::clamp(x + i, 0, w - 1);
                    const uint8_t* p = image.data.data() + (static_cast<size_t>(y) * w + sx) * 4;
                    for (int c = 0; c < 4; c++) acc[c] += kernel[i + radius] * p[c];
                }
                for (int c = 0; c < 4; c++) t[static_cast<size_t>(x) * 4 + c] = acc[c];
            }
            return t;
        };
        Image out = Image::make(w, h, 4, 8);
        std::vector<const float*> taps(ring);
        for (int y = 0; y < h; y++) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return Image();
            // rows y - radius .. y + radius (clamped): at most `ring` distinct ones, all in the ring
            for (int i = -radius; i <= radius; i++) taps[i + radius] = horizontal(std::clamp(y + i, 0, h - 1));
            for (int x = 0; x < w; x++) {
                float acc[4] = {0, 0, 0, 0};
                for (int i = -radius; i <= radius; i++) {
                    const float* t = taps[i + radius] + static_cast<size_t>(x) * 4;
                    for (int c = 0; c < 4; c++) acc[c] += kernel[i + radius] * t[c];
                }
                uint8_t* d = out.data.data() + (static_cast<size_t>(y) * w + x) * 4;
                for (int c = 0; c < 4; c++) d[c] = static_cast<uint8_t>(std::clamp(std::lround(acc[c]), 0L, 255L));
            }
        }
        return out;
    }

    Image greyRgba8(const Image& image) {
        Image out = image;
        const size_t n = image.pixelCount();
        for (size_t i = 0; i < n; i++) {
            uint8_t* p = out.data.data() + i * 4;
            const float luma = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            const uint8_t g = static_cast<uint8_t>(std::clamp(std::lround(luma), 0L, 255L));
            p[0] = p[1] = p[2] = g;
            p[3] = 255;
        }
        return out;
    }

    void matchHistogramRgba8(Image& image, const Image& reference) {
        if (image.empty() || reference.empty()) return;
        auto cdf = [](const Image& img) {
            std::vector<double> hist(256, 0.0);
            const size_t n = img.pixelCount();
            for (size_t i = 0; i < n; i++) hist[img.data[i * 4]] += 1.0;
            for (int i = 1; i < 256; i++) hist[i] += hist[i - 1];
            for (double& v : hist) v /= static_cast<double>(n);
            return hist;
        };
        const std::vector<double> source = cdf(image), target = cdf(reference);
        uint8_t lut[256];
        for (int v = 0; v < 256; v++) {
            // the first target level whose cumulative share reaches the source's
            int t = 0;
            while (t < 255 && target[t] < source[v]) t++;
            lut[v] = static_cast<uint8_t>(t);
        }
        const size_t n = image.pixelCount();
        for (size_t i = 0; i < n; i++) {
            uint8_t* p = image.data.data() + i * 4;
            p[0] = p[1] = p[2] = lut[p[0]];
            p[3] = 255;
        }
    }

}
