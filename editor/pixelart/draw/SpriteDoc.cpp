// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteDoc.h"

#include "miniz.h"
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        namespace ase = aseprite;

        const uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

        std::string lowerExtension(const std::filesystem::path& path) {
            std::string ext = path.extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return ext;
        }

        uint32_t be32(const uint8_t* p) {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }

        void putBe32(std::vector<uint8_t>& out, uint32_t v) {
            out.push_back(static_cast<uint8_t>(v >> 24));
            out.push_back(static_cast<uint8_t>(v >> 16));
            out.push_back(static_cast<uint8_t>(v >> 8));
            out.push_back(static_cast<uint8_t>(v));
        }

        void putChunk(std::vector<uint8_t>& out, const char* type, std::span<const uint8_t> data) {
            putBe32(out, static_cast<uint32_t>(data.size()));
            const size_t typeAt = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, out.data() + typeAt, data.size() + 4);
            putBe32(out, static_cast<uint32_t>(crc));
        }

        PixelProblem pngProblem(const std::string& message) {
            return {"not_a_sprite", "png", message, "pass an .aseprite, .ase or 8-bit .png file"};
        }

        uint8_t paeth(int a, int b, int c) {
            const int p = a + b - c;
            const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
            return static_cast<uint8_t>(pb <= pc ? b : c);
        }

        // the index plane of a non-interlaced palette PNG (1, 2, 4 or 8 bits)
        bool decodePaletteIndices(std::span<const uint8_t> zdata, int w, int h, int depth, std::vector<uint8_t>& indices) {
            const size_t stride = (static_cast<size_t>(w) * depth + 7) / 8;
            const size_t expected = (stride + 1) * static_cast<size_t>(h);
            std::vector<uint8_t> raw(expected);
            mz_ulong length = static_cast<mz_ulong>(expected);
            if (mz_uncompress(raw.data(), &length, zdata.data(), static_cast<mz_ulong>(zdata.size())) != MZ_OK || length != expected) return false;
            std::vector<uint8_t> prev(stride, 0), row(stride);
            indices.assign(static_cast<size_t>(w) * h, 0);
            for (int y = 0; y < h; y++) {
                const uint8_t* in = raw.data() + static_cast<size_t>(y) * (stride + 1);
                const uint8_t filter = in[0];
                for (size_t i = 0; i < stride; i++) {
                    const int a = i >= 1 ? row[i - 1] : 0, b = prev[i], c = i >= 1 ? prev[i - 1] : 0;
                    int v = in[i + 1];
                    switch (filter) {
                        case 0: break;
                        case 1: v += a; break;
                        case 2: v += b; break;
                        case 3: v += (a + b) / 2; break;
                        case 4: v += paeth(a, b, c); break;
                        default: return false;
                    }
                    row[i] = static_cast<uint8_t>(v);
                }
                for (int x = 0; x < w; x++) {
                    uint8_t index = 0;
                    if (depth == 8) {
                        index = row[x];
                    } else {
                        const int perByte = 8 / depth;
                        const int shift = 8 - depth * (x % perByte + 1);
                        index = static_cast<uint8_t>((row[x / perByte] >> shift) & ((1 << depth) - 1));
                    }
                    indices[static_cast<size_t>(y) * w + x] = index;
                }
                prev.swap(row);
            }
            return true;
        }

        bool decodePng(std::span<const uint8_t> bytes, SpriteDoc& out, PixelProblem& problem) {
            if (bytes.size() < 8 || std::memcmp(bytes.data(), kPngSignature, 8) != 0) {
                problem = pngProblem("the file is not a PNG");
                return false;
            }
            PngLayout layout;
            int w = 0, h = 0, depth = 0, colorType = 0, interlace = 0;
            std::vector<uint8_t> plte, trns, zdata;
            bool idatPlaced = false, ended = false;
            size_t pos = 8;
            while (pos < bytes.size()) {
                if (bytes.size() - pos < 12) break;
                const uint32_t length = be32(&bytes[pos]);
                if (length > bytes.size() - pos - 12) {
                    problem = pngProblem("a PNG chunk at byte " + std::to_string(pos) + " runs past the end of the file");
                    return false;
                }
                const uint8_t* type = &bytes[pos + 4];
                const uint8_t* data = &bytes[pos + 8];
                const size_t end = pos + 12 + length;
                PngChunk chunk;
                if (std::memcmp(type, "IHDR", 4) == 0 && length >= 13) {
                    w = static_cast<int>(be32(data));
                    h = static_cast<int>(be32(data + 4));
                    depth = data[8];
                    colorType = data[9];
                    interlace = data[12];
                    chunk.role = PngChunk::Role::Ihdr;
                } else if (std::memcmp(type, "PLTE", 4) == 0) {
                    plte.assign(data, data + length);
                    layout.hadPalette = true;
                    chunk.role = PngChunk::Role::Plte;
                } else if (std::memcmp(type, "tRNS", 4) == 0) {
                    trns.assign(data, data + length);
                    chunk.role = PngChunk::Role::Trns;
                } else if (std::memcmp(type, "IDAT", 4) == 0) {
                    zdata.insert(zdata.end(), data, data + length);
                    if (idatPlaced) {
                        pos = end;
                        continue;
                    }
                    idatPlaced = true;
                    chunk.role = PngChunk::Role::Idat;
                } else if (std::memcmp(type, "IEND", 4) == 0) {
                    layout.iend.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.begin() + static_cast<std::ptrdiff_t>(end));
                    layout.trailing.assign(bytes.begin() + static_cast<std::ptrdiff_t>(end), bytes.end());
                    ended = true;
                    break;
                } else {
                    chunk.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.begin() + static_cast<std::ptrdiff_t>(end));
                }
                layout.chunks.push_back(std::move(chunk));
                pos = end;
            }
            if (!ended || w <= 0 || h <= 0 || !idatPlaced) {
                problem = pngProblem("the PNG has no IHDR, IDAT or IEND chunk");
                return false;
            }
            if (w > ase::kMaxCanvasSide || h > ase::kMaxCanvasSide) {
                problem = {"canvas_too_large", "size", "the PNG is " + std::to_string(w) + "x" + std::to_string(h) + ", above 16384x16384",
                           "split it into smaller images"};
                return false;
            }
            if (depth == 16) {
                problem = {"unsupported_feature", "png", "16-bit PNGs are not edited (their precision would be lost)",
                           "save it as an 8-bit PNG, or use create_sprite {from: <the png>} to make an .aseprite"};
                return false;
            }
            ase::Document& doc = out.doc;
            doc = ase::Document();
            doc.width = w;
            doc.height = h;
            doc.frameDurations = {100};
            ase::Layer layer;
            layer.name = "Layer 1";
            doc.layers.push_back(layer);
            ase::Cel cel;
            cel.layer = 0;
            cel.frame = 0;
            if (colorType == 3) {
                doc.mode = ase::ColorMode::Indexed;
                const size_t entries = plte.size() / 3;
                doc.palette.resize(entries);
                int transparent = -1;
                for (size_t i = 0; i < entries; i++) {
                    const uint8_t alpha = i < trns.size() ? trns[i] : 255;
                    doc.palette[i].color = {plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2], alpha};
                    if (alpha == 0 && transparent < 0) transparent = static_cast<int>(i);
                }
                // a palette PNG without a clear entry is opaque everywhere: a background layer keeps
                // index 0 a colour
                if (transparent < 0) {
                    doc.layers[0].flags |= ase::kLayerBackground;
                    doc.transparentIndex = 0;
                } else {
                    doc.transparentIndex = static_cast<uint8_t>(transparent);
                }
                std::vector<uint8_t> indices;
                bool decoded = interlace == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8) &&
                               decodePaletteIndices(zdata, w, h, depth, indices);
                if (!decoded) {
                    // interlaced: the colours back to the first entry holding them
                    int cw = 0, ch = 0, comp = 0;
                    stbi_uc* rgba = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &cw, &ch, &comp, 4);
                    if (!rgba || cw != w || ch != h) {
                        if (rgba) stbi_image_free(rgba);
                        problem = pngProblem("the PNG's pixels do not decode");
                        return false;
                    }
                    indices.assign(static_cast<size_t>(w) * h, 0);
                    for (size_t p = 0; p < indices.size(); p++) {
                        for (size_t i = 0; i < entries; i++) {
                            const PixelRgba& c = doc.palette[i].color;
                            if (std::memcmp(c.data(), rgba + p * 4, 4) == 0) {
                                indices[p] = static_cast<uint8_t>(i);
                                break;
                            }
                        }
                    }
                    stbi_image_free(rgba);
                }
                for (uint8_t& i : indices) {
                    if (i >= entries) i = 0;
                }
                const std::vector<uint8_t> paletteRgba = doc.paletteRgba(0);
                cel.image = std::make_shared<PixelImage>(PixelImage::fromIndices(
                    w, h, std::move(indices), paletteRgba, doc.layers[0].background() ? -1 : doc.transparentIndex));
            } else {
                doc.mode = ase::ColorMode::Rgba;
                std::string error;
                std::optional<PixelImage> image = PixelImage::decodePng(bytes, &error);
                if (!image) {
                    problem = pngProblem("the PNG's pixels do not decode: " + error);
                    return false;
                }
                cel.image = std::make_shared<PixelImage>(std::move(*image));
                // an RGB PNG's suggested palette stays reference only (R25)
                const size_t entries = plte.size() / 3;
                for (size_t i = 0; i < entries; i++) doc.palette.push_back({{plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2], 255}, std::nullopt});
            }
            doc.cels.push_back(std::move(cel));
            out.kind = SpriteKind::Png;
            out.png = std::move(layout);
            return true;
        }

        bool encodePng(const SpriteDoc& sprite, std::vector<uint8_t>& out, PixelProblem& problem) {
            const ase::Document& doc = sprite.doc;
            const int w = doc.width, h = doc.height;
            const bool indexed = doc.mode == ase::ColorMode::Indexed;
            const PixelImage canvas = celCanvas(doc, 0, 0);
            if (canvas.empty()) {
                problem = {"invalid_document", "png", "the PNG has no pixel layer", ""};
                return false;
            }
            std::vector<uint8_t> raw;
            const size_t bpp = indexed ? 1 : 4;
            raw.reserve((static_cast<size_t>(w) * bpp + 1) * h);
            for (int y = 0; y < h; y++) {
                raw.push_back(0);
                if (indexed) {
                    const std::span<const uint8_t> plane = canvas.indices();
                    raw.insert(raw.end(), plane.begin() + static_cast<std::ptrdiff_t>(y) * w, plane.begin() + static_cast<std::ptrdiff_t>(y + 1) * w);
                } else {
                    raw.insert(raw.end(), canvas.row(y), canvas.row(y) + static_cast<size_t>(w) * 4);
                }
            }
            std::vector<uint8_t> zdata(mz_compressBound(static_cast<mz_ulong>(raw.size())));
            mz_ulong zlength = static_cast<mz_ulong>(zdata.size());
            if (mz_compress2(zdata.data(), &zlength, raw.data(), static_cast<mz_ulong>(raw.size()), 6) != MZ_OK) {
                problem = {"invalid_document", "png", "the PNG's pixels could not be compressed", ""};
                return false;
            }
            zdata.resize(zlength);
            std::vector<uint8_t> ihdr;
            putBe32(ihdr, static_cast<uint32_t>(w));
            putBe32(ihdr, static_cast<uint32_t>(h));
            ihdr.insert(ihdr.end(), {8, static_cast<uint8_t>(indexed ? 3 : 6), 0, 0, 0});
            std::vector<uint8_t> plte, trns;
            if (indexed) {
                const bool clear = !doc.layers.empty() && !doc.layers[0].background();
                for (size_t i = 0; i < doc.palette.size(); i++) {
                    const PixelRgba& c = doc.palette[i].color;
                    plte.insert(plte.end(), {c[0], c[1], c[2]});
                    trns.push_back(clear && i == doc.transparentIndex ? 0 : c[3]);
                }
                while (!trns.empty() && trns.back() == 255) trns.pop_back();
            }
            out.assign(kPngSignature, kPngSignature + 8);
            bool wroteIhdr = false, wrotePlte = false, wroteTrns = false;
            auto writePalette = [&]() {
                if (!indexed || wrotePlte) return;
                putChunk(out, "PLTE", plte);
                wrotePlte = true;
            };
            auto writeTrns = [&]() {
                if (!indexed || wroteTrns) return;
                writePalette();
                if (!trns.empty()) putChunk(out, "tRNS", trns);
                wroteTrns = true;
            };
            const PngLayout& layout = sprite.png;
            auto writeHeader = [&]() {
                if (wroteIhdr) return;
                putChunk(out, "IHDR", ihdr);
                wroteIhdr = true;
            };
            for (const PngChunk& chunk : layout.chunks) {
                switch (chunk.role) {
                    case PngChunk::Role::Ihdr: writeHeader(); break;
                    case PngChunk::Role::Plte:
                        writeHeader();
                        if (indexed) writePalette();
                        else if (layout.hadPalette) {
                            // an RGBA PNG's suggested palette, rebuilt from the reference palette
                            std::vector<uint8_t> suggested;
                            for (const ase::PaletteEntry& e : doc.palette) suggested.insert(suggested.end(), {e.color[0], e.color[1], e.color[2]});
                            if (!suggested.empty()) putChunk(out, "PLTE", suggested);
                        }
                        break;
                    case PngChunk::Role::Trns:
                        writeHeader();
                        writeTrns();
                        break;
                    case PngChunk::Role::Idat:
                        writeHeader();
                        writeTrns();
                        putChunk(out, "IDAT", zdata);
                        break;
                    case PngChunk::Role::Keep:
                        if (!wroteIhdr) writeHeader();
                        out.insert(out.end(), chunk.bytes.begin(), chunk.bytes.end());
                        break;
                }
            }
            if (!wroteIhdr) writeHeader();
            if (layout.iend.empty()) putChunk(out, "IEND", {});
            else out.insert(out.end(), layout.iend.begin(), layout.iend.end());
            out.insert(out.end(), layout.trailing.begin(), layout.trailing.end());
            return true;
        }

    }

    bool isSpritePath(const std::filesystem::path& path) {
        const std::string ext = lowerExtension(path);
        return ext == ".aseprite" || ext == ".ase" || ext == ".png";
    }

    bool isPngPath(const std::filesystem::path& path) {
        return lowerExtension(path) == ".png";
    }

    bool decodeSprite(const std::filesystem::path& path, std::span<const uint8_t> bytes, SpriteDoc& out, PixelProblem& problem, bool provenance) {
        out = SpriteDoc();
        if (isPngPath(path)) return decodePng(bytes, out, problem);
        ase::ReadOptions options;
        options.provenance = provenance;
        ase::ReadResult read = ase::read(bytes, options);
        if (!read.ok()) {
            problem = read.error;
            if (problem.kind == "not_aseprite") problem.kind = "not_a_sprite";
            return false;
        }
        out.kind = SpriteKind::Aseprite;
        out.doc = std::move(*read.document);
        return true;
    }

    bool encodeSprite(const SpriteDoc& sprite, std::vector<uint8_t>& out, PixelProblem& problem) {
        if (sprite.isPng()) return encodePng(sprite, out, problem);
        ase::CodecError error;
        if (!ase::write(sprite.doc, out, &error)) {
            problem = error;
            return false;
        }
        return true;
    }

    PixelProblem editRefusal(const SpriteDoc& sprite) {
        if (sprite.indexed() && !sprite.doc.paletteChanges.empty()) {
            return {"unsupported_feature", "palette", "the palette changes on frame " + std::to_string(sprite.doc.paletteChanges.begin()->first + 1) +
                    ", which edits cannot keep in step", "flatten the palette changes in Aseprite (one palette for every frame), then edit"};
        }
        return {};
    }

    PixelImage blankCanvas(const aseprite::Document& doc, int layer) {
        if (doc.mode != ase::ColorMode::Indexed) return PixelImage(doc.width, doc.height);
        const bool background = layer >= 0 && layer < static_cast<int>(doc.layers.size()) && doc.layers[static_cast<size_t>(layer)].background();
        const std::vector<uint8_t> paletteRgba = doc.paletteRgba(0);
        return PixelImage::fromIndices(doc.width, doc.height, std::vector<uint8_t>(static_cast<size_t>(doc.width) * doc.height, doc.transparentIndex),
                                       paletteRgba, background ? -1 : doc.transparentIndex);
    }

    PixelImage celCanvas(const aseprite::Document& doc, int layer, int frame) {
        if (layer < 0 || layer >= static_cast<int>(doc.layers.size()) || doc.layers[static_cast<size_t>(layer)].type != ase::LayerType::Image) return PixelImage();
        PixelImage canvas = blankCanvas(doc, layer);
        const ase::Cel* cel = doc.cel(layer, frame);
        if (cel && cel->image && !cel->image->empty()) canvas.blit(*cel->image, cel->x, cel->y);
        return canvas;
    }

    std::string hexColor(const PixelRgba& c) {
        char buffer[16];
        if (c[3] == 255) std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", c[0], c[1], c[2]);
        else std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);
        return buffer;
    }

}
