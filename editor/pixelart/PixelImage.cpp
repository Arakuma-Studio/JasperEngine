// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelImage.h"

#include "aseprite/AsepriteBlend.h"
#include "asset/ContentHash.h"
#include "util/PngWriter.h"

#include "stb_image.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

// stb_image_write's in-memory PNG encoder: compiled into the engine (engine/libs/stb), declared
// only in the implementation part of its header
extern "C" unsigned char* stbi_write_png_to_mem(const unsigned char* pixels, int stride_bytes, int x, int y, int n, int* out_len);

namespace jasperengine::editor::pixelart {

    IntRect IntRect::intersect(const IntRect& o) const {
        const int x0 = std::max(x, o.x);
        const int y0 = std::max(y, o.y);
        const int x1 = std::min(x + w, o.x + o.w);
        const int y1 = std::min(y + h, o.y + o.h);
        if (x1 <= x0 || y1 <= y0) return {};
        return {x0, y0, x1 - x0, y1 - y0};
    }

    IntRect IntRect::unite(const IntRect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const int x0 = std::min(x, o.x);
        const int y0 = std::min(y, o.y);
        const int x1 = std::max(x + w, o.x + o.w);
        const int y1 = std::max(y + h, o.y + o.h);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    PixelImage::PixelImage(int width, int height) {
        if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
            return;
        }
        w = width;
        h = height;
        rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    }

    void PixelImage::lookup(PixelImage& image, std::span<const uint8_t> paletteRgba) {
        const size_t entries = paletteRgba.size() / 4;
        const size_t n = image.idx.size();
        for (size_t i = 0; i < n; i++) {
            const uint8_t index = image.idx[i];
            uint8_t* out = image.rgba.data() + i * 4;
            if (static_cast<int>(index) == image.transparent || index >= entries) {
                out[0] = out[1] = out[2] = out[3] = 0;
            } else {
                std::memcpy(out, paletteRgba.data() + static_cast<size_t>(index) * 4, 4);
            }
        }
    }

    PixelImage PixelImage::fromIndices(int width, int height, std::vector<uint8_t> indices,
                                       std::span<const uint8_t> paletteRgba, int transparentIndex) {
        PixelImage image(width, height);
        if (image.empty() || indices.size() != static_cast<size_t>(width) * height) {
            return PixelImage();
        }
        image.idx = std::move(indices);
        image.indexed = true;
        image.transparent = transparentIndex;
        lookup(image, paletteRgba);
        return image;
    }

    std::span<uint8_t> PixelImage::editPixels(bool* droppedIndices) {
        const bool had = indexed;
        dropIndices();
        if (droppedIndices) *droppedIndices = had;
        return rgba;
    }

    bool PixelImage::setIndices(std::vector<uint8_t> indices, std::span<const uint8_t> paletteRgba, int transparentIndex) {
        if (indices.size() != static_cast<size_t>(w) * h) {
            return false;
        }
        idx = std::move(indices);
        indexed = true;
        transparent = transparentIndex;
        lookup(*this, paletteRgba);
        return true;
    }

    bool PixelImage::repaint(std::span<const uint8_t> paletteRgba, int transparentIndex) {
        if (!indexed) return false;
        transparent = transparentIndex;
        lookup(*this, paletteRgba);
        return true;
    }

    void PixelImage::dropIndices() {
        idx.clear();
        idx.shrink_to_fit();
        indexed = false;
        transparent = -1;
    }

    PixelRgba PixelImage::at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return {0, 0, 0, 0};
        const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
        return {p[0], p[1], p[2], p[3]};
    }

    uint8_t PixelImage::indexAt(int x, int y) const {
        if (!indexed || x < 0 || y < 0 || x >= w || y >= h) return 0;
        return idx[static_cast<size_t>(y) * w + x];
    }

    bool PixelImage::set(int x, int y, PixelRgba color) {
        const bool had = indexed;
        dropIndices();
        if (x < 0 || y < 0 || x >= w || y >= h) return had;
        std::memcpy(rgba.data() + (static_cast<size_t>(y) * w + x) * 4, color.data(), 4);
        return had;
    }

    bool PixelImage::setIndex(int x, int y, uint8_t index, PixelRgba color) {
        if (!indexed || x < 0 || y < 0 || x >= w || y >= h) return false;
        const size_t i = static_cast<size_t>(y) * w + x;
        idx[i] = index;
        if (static_cast<int>(index) == transparent) color = {0, 0, 0, 0};
        std::memcpy(rgba.data() + i * 4, color.data(), 4);
        return true;
    }

    PixelImage PixelImage::crop(const IntRect& rect) const {
        PixelImage out(rect.w, rect.h);
        if (out.empty()) return out;
        const bool inside = rect.x >= 0 && rect.y >= 0 && rect.x + rect.w <= w && rect.y + rect.h <= h;
        const bool keepIndices = indexed && (inside || transparent >= 0);
        if (keepIndices) {
            out.idx.assign(static_cast<size_t>(rect.w) * rect.h, static_cast<uint8_t>(std::max(transparent, 0)));
            out.indexed = true;
            out.transparent = transparent;
        }
        const IntRect src = rect.intersect(bounds());
        for (int y = 0; y < src.h; y++) {
            const int sy = src.y + y;
            const int dy = sy - rect.y;
            const int dx = src.x - rect.x;
            std::memcpy(out.rgba.data() + (static_cast<size_t>(dy) * rect.w + dx) * 4,
                        rgba.data() + (static_cast<size_t>(sy) * w + src.x) * 4, static_cast<size_t>(src.w) * 4);
            if (keepIndices) {
                std::memcpy(out.idx.data() + static_cast<size_t>(dy) * rect.w + dx,
                            idx.data() + static_cast<size_t>(sy) * w + src.x, static_cast<size_t>(src.w));
            }
        }
        return out;
    }

    bool PixelImage::blit(const PixelImage& src, int x, int y) {
        const bool keep = indexed && src.indexed;
        if (!keep) dropIndices();
        const IntRect dst = IntRect{x, y, src.w, src.h}.intersect(bounds());
        for (int row = 0; row < dst.h; row++) {
            const int dy = dst.y + row;
            const int sy = dy - y;
            const int sx = dst.x - x;
            std::memcpy(rgba.data() + (static_cast<size_t>(dy) * w + dst.x) * 4,
                        src.rgba.data() + (static_cast<size_t>(sy) * src.w + sx) * 4, static_cast<size_t>(dst.w) * 4);
            if (keep) {
                std::memcpy(idx.data() + static_cast<size_t>(dy) * w + dst.x,
                            src.idx.data() + static_cast<size_t>(sy) * src.w + sx, static_cast<size_t>(dst.w));
            }
        }
        return keep;
    }

    bool PixelImage::blendOver(const PixelImage& src, int x, int y, uint8_t opacity) {
        const bool had = indexed;
        dropIndices();
        const IntRect dst = IntRect{x, y, src.w, src.h}.intersect(bounds());
        for (int row = 0; row < dst.h; row++) {
            const int dy = dst.y + row;
            const int sy = dy - y;
            uint8_t* d = rgba.data() + (static_cast<size_t>(dy) * w + dst.x) * 4;
            const uint8_t* s = src.rgba.data() + (static_cast<size_t>(sy) * src.w + (dst.x - x)) * 4;
            for (int col = 0; col < dst.w; col++, d += 4, s += 4) {
                const uint32_t out = aseprite::blend::rgbaNormal(aseprite::blend::pack(d), aseprite::blend::pack(s), opacity);
                aseprite::blend::unpack(out, d);
            }
        }
        return had;
    }

    IntRect PixelImage::opaqueBounds(uint8_t alphaThreshold) const {
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++) {
            const uint8_t* p = row(y);
            for (int x = 0; x < w; x++) {
                if (p[x * 4 + 3] > alphaThreshold) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
        }
        if (x1 < 0) return {};
        return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }

    void PixelImage::flipX() {
        for (int y = 0; y < h; y++) {
            uint8_t* p = rgba.data() + static_cast<size_t>(y) * w * 4;
            for (int a = 0, b = w - 1; a < b; a++, b--) {
                for (int c = 0; c < 4; c++) std::swap(p[a * 4 + c], p[b * 4 + c]);
            }
            if (indexed) {
                uint8_t* q = idx.data() + static_cast<size_t>(y) * w;
                std::reverse(q, q + w);
            }
        }
    }

    void PixelImage::flipY() {
        const size_t stride = static_cast<size_t>(w) * 4;
        for (int a = 0, b = h - 1; a < b; a++, b--) {
            std::swap_ranges(rgba.begin() + a * stride, rgba.begin() + (a + 1) * stride, rgba.begin() + b * stride);
            if (indexed) {
                std::swap_ranges(idx.begin() + static_cast<size_t>(a) * w, idx.begin() + static_cast<size_t>(a + 1) * w,
                                 idx.begin() + static_cast<size_t>(b) * w);
            }
        }
    }

    bool PixelImage::fill(PixelRgba color) {
        const bool had = indexed;
        dropIndices();
        for (size_t i = 0; i < rgba.size(); i += 4) {
            std::memcpy(rgba.data() + i, color.data(), 4);
        }
        return had;
    }

    uint64_t PixelImage::hash() const {
        ContentHash hasher;
        uint8_t header[9];
        for (int i = 0; i < 4; i++) {
            header[i] = static_cast<uint8_t>(static_cast<uint32_t>(w) >> (8 * i));
            header[4 + i] = static_cast<uint8_t>(static_cast<uint32_t>(h) >> (8 * i));
        }
        header[8] = indexed ? 1 : 0;
        hasher.update(header, sizeof(header));
        hasher.update(rgba.data(), rgba.size());
        if (indexed) {
            hasher.update(idx.data(), idx.size());
        }
        return hasher.digest();
    }

    bool PixelImage::operator==(const PixelImage& other) const {
        return w == other.w && h == other.h && indexed == other.indexed && rgba == other.rgba &&
               (!indexed || (idx == other.idx && transparent == other.transparent));
    }

    std::optional<PixelImage> PixelImage::decodePng(std::span<const uint8_t> bytes, std::string* error) {
        if (bytes.empty() || bytes.size() > static_cast<size_t>(INT32_MAX)) {
            if (error) *error = "not a readable image (empty)";
            return std::nullopt;
        }
        int width = 0, height = 0, comp = 0;
        const int length = static_cast<int>(bytes.size());
        if (!stbi_info_from_memory(bytes.data(), length, &width, &height, &comp)) {
            const char* reason = stbi_failure_reason();
            if (error) *error = std::string("not a readable image") + (reason ? std::string(" (") + reason + ")" : std::string());
            return std::nullopt;
        }
        if (width > kMaxSide || height > kMaxSide) {
            if (error) *error = "the image is " + std::to_string(width) + "x" + std::to_string(height) + ", at most " +
                                std::to_string(kMaxSide) + " per side";
            return std::nullopt;
        }
        unsigned char* pixels = stbi_load_from_memory(bytes.data(), length, &width, &height, &comp, 4);
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            if (error) *error = std::string("decoding failed") + (reason ? std::string(" (") + reason + ")" : std::string());
            return std::nullopt;
        }
        PixelImage image(width, height);
        if (!image.empty()) {
            std::memcpy(image.rgba.data(), pixels, image.rgba.size());
        }
        stbi_image_free(pixels);
        return image;
    }

    std::optional<PixelImage> PixelImage::loadPng(const std::filesystem::path& path, std::string* error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) *error = "cannot open " + path.generic_string();
            return std::nullopt;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return decodePng(bytes, error);
    }

    std::string PixelImage::encodePng() const {
        if (empty()) return std::string();
        int length = 0;
        unsigned char* encoded = stbi_write_png_to_mem(rgba.data(), w * 4, w, h, 4, &length);
        if (!encoded) return std::string();
        std::string png(reinterpret_cast<const char*>(encoded), static_cast<size_t>(length));
        std::free(encoded);   // STBIW_MALLOC is malloc
        return png;
    }

    bool PixelImage::savePng(const std::filesystem::path& path) const {
        if (empty()) return false;
        return writePng(path, w, h, 4, rgba.data(), w * 4);
    }

}
