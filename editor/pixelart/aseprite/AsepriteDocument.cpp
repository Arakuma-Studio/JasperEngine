// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The document model's helpers and what the reader and the writer share: errors, the cursor,
// zlib and the pixel layouts of the file.

#include "AsepriteCodecInternal.h"

#include "asset/ContentHash.h"

#include "miniz.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace jasperengine::editor::pixelart::aseprite {

    const char* blendModeName(BlendMode mode) {
        static const char* names[] = {"normal", "multiply", "screen", "overlay", "darken", "lighten", "color_dodge",
                                      "color_burn", "hard_light", "soft_light", "difference", "exclusion", "hue",
                                      "saturation", "color", "luminosity", "addition", "subtract", "divide"};
        const size_t i = static_cast<size_t>(mode);
        return i < std::size(names) ? names[i] : "normal";
    }

    bool parseBlendMode(std::string_view text, BlendMode& out) {
        for (uint16_t i = 0; i <= static_cast<uint16_t>(BlendMode::Divide); i++) {
            if (text == blendModeName(static_cast<BlendMode>(i))) {
                out = static_cast<BlendMode>(i);
                return true;
            }
        }
        return false;
    }

    const char* tagDirectionName(TagDirection dir) {
        switch (dir) {
            case TagDirection::Forward: return "forward";
            case TagDirection::Reverse: return "reverse";
            case TagDirection::PingPong: return "pingpong";
            case TagDirection::PingPongReverse: return "pingpong_reverse";
        }
        return "forward";
    }

    const char* colorModeName(ColorMode mode) {
        switch (mode) {
            case ColorMode::Rgba: return "rgba";
            case ColorMode::Grayscale: return "grayscale";
            case ColorMode::Indexed: return "indexed";
        }
        return "rgba";
    }

    const char* propertyTypeName(PropertyType type) {
        switch (type) {
            case PropertyType::Bool: return "bool";
            case PropertyType::Int8: return "int8";
            case PropertyType::Uint8: return "uint8";
            case PropertyType::Int16: return "int16";
            case PropertyType::Uint16: return "uint16";
            case PropertyType::Int32: return "int32";
            case PropertyType::Uint32: return "uint32";
            case PropertyType::Int64: return "int64";
            case PropertyType::Uint64: return "uint64";
            case PropertyType::Fixed: return "fixed";
            case PropertyType::Float: return "float";
            case PropertyType::Double: return "double";
            case PropertyType::String: return "string";
            case PropertyType::Point: return "point";
            case PropertyType::Size: return "size";
            case PropertyType::Rect: return "rect";
            case PropertyType::Vector: return "vector";
            case PropertyType::Map: return "map";
            case PropertyType::Uuid: return "uuid";
        }
        return "unknown";
    }

    std::string Uuid::toString() const {
        static const char* hex = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < bytes.size(); i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
            s.push_back(hex[bytes[i] >> 4]);
            s.push_back(hex[bytes[i] & 15]);
        }
        return s;
    }

    // ---- property values

    PropertyValue PropertyValue::boolean(bool b) { return {PropertyType::Bool, b, 0}; }

    PropertyValue PropertyValue::integer(int64_t v) {
        // Aseprite's reduce_int_type_size: the smallest type holding the value, unsigned for >= 0
        PropertyValue p;
        if (v >= 0) {
            const uint64_t u = static_cast<uint64_t>(v);
            p.type = u <= 0xff ? PropertyType::Uint8 : u <= 0xffff ? PropertyType::Uint16 : u <= 0xffffffffull ? PropertyType::Uint32 : PropertyType::Uint64;
            p.value = u;
        } else {
            p.type = v >= INT8_MIN ? PropertyType::Int8 : v >= INT16_MIN ? PropertyType::Int16 : v >= INT32_MIN ? PropertyType::Int32 : PropertyType::Int64;
            p.value = v;
        }
        return p;
    }

    PropertyValue PropertyValue::number(double v) { return {PropertyType::Double, v, 0}; }
    PropertyValue PropertyValue::string(std::string s) { return {PropertyType::String, std::move(s), 0}; }
    PropertyValue PropertyValue::point(IntPoint p) { return {PropertyType::Point, p, 0}; }
    PropertyValue PropertyValue::size(IntSize s) { return {PropertyType::Size, s, 0}; }
    PropertyValue PropertyValue::rect(IntRect r) { return {PropertyType::Rect, r, 0}; }
    PropertyValue PropertyValue::uuid(Uuid u) { return {PropertyType::Uuid, u, 0}; }

    PropertyValue PropertyValue::vector(PropertyVector elements) {
        PropertyValue p;
        p.type = PropertyType::Vector;
        uint16_t common = elements.empty() ? 0 : static_cast<uint16_t>(elements.front().type);
        for (const PropertyValue& e : elements) {
            if (static_cast<uint16_t>(e.type) != common) common = 0;
        }
        p.vectorElementType = common;
        p.value = std::move(elements);
        return p;
    }

    PropertyValue PropertyValue::map(PropertyMap properties) {
        PropertyValue p;
        p.type = PropertyType::Map;
        p.value = std::move(properties);
        return p;
    }

    bool PropertyValue::operator==(const PropertyValue& o) const { return type == o.type && value == o.value; }

    const PropertyValue* findProperty(const PropertyMap& map, std::string_view name) {
        for (const Property& p : map) {
            if (p.name == name) return &p.value;
        }
        return nullptr;
    }

    nlohmann::json toJson(const PropertyValue& value) {
        struct Visitor {
            nlohmann::json operator()(bool b) const { return b; }
            nlohmann::json operator()(int64_t v) const { return v; }
            nlohmann::json operator()(uint64_t v) const { return v; }
            nlohmann::json operator()(double v) const { return v; }
            nlohmann::json operator()(const std::string& s) const { return s; }
            nlohmann::json operator()(const IntPoint& p) const { return {{"x", p.x}, {"y", p.y}}; }
            nlohmann::json operator()(const IntSize& s) const { return {{"w", s.w}, {"h", s.h}}; }
            nlohmann::json operator()(const IntRect& r) const { return {{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}}; }
            nlohmann::json operator()(const Uuid& u) const { return u.toString(); }
            nlohmann::json operator()(const PropertyVector& v) const {
                nlohmann::json a = nlohmann::json::array();
                for (const PropertyValue& e : v) a.push_back(toJson(e));
                return a;
            }
            nlohmann::json operator()(const PropertyMap& m) const { return toJson(m); }
        };
        return std::visit(Visitor{}, value.value);
    }

    nlohmann::json toJson(const PropertyMap& map) {
        nlohmann::json o = nlohmann::json::object();
        for (const Property& p : map) {
            o[p.name] = toJson(p.value);
        }
        return o;
    }

    bool UserData::empty() const {
        if (text && !text->empty()) return false;
        if (color && (*color)[3] != 0) return false;
        for (const auto& [key, map] : properties) {
            if (!map.empty()) return false;
        }
        return true;
    }

    const PropertyMap* UserData::userProperties() const {
        auto it = properties.find(0);
        return it == properties.end() ? nullptr : &it->second;
    }

    bool Tilemap::operator==(const Tilemap& o) const {
        return columns == o.columns && rows == o.rows && bitsPerTile == o.bitsPerTile && idMask == o.idMask &&
               xFlipMask == o.xFlipMask && yFlipMask == o.yFlipMask && diagonalMask == o.diagonalMask && tiles == o.tiles;
    }

    const SliceKey* Slice::keyAt(int frame) const {
        const SliceKey* found = nullptr;
        for (const SliceKey& key : keys) {
            if (static_cast<int64_t>(key.frame) <= frame) {
                found = &key;
            } else {
                break;
            }
        }
        if (found && found->hidden()) return nullptr;
        return found;
    }

    PixelRgba Tag::color() const {
        if (userData.color && (*userData.color)[3] != 0) return *userData.color;
        return legacyColor;
    }

    PixelImage Tileset::tile(int index) const {
        if (index < 0 || index >= tileCount || tiles.empty()) return PixelImage();
        return tiles.crop({0, index * tileHeight, tileWidth, tileHeight});
    }

    // ---- document

    const Cel* Document::celRecord(int layer, int frame) const {
        auto it = std::lower_bound(cels.begin(), cels.end(), std::make_pair(frame, layer),
                                   [](const Cel& c, const std::pair<int, int>& key) {
                                       return c.frame < key.first || (c.frame == key.first && c.layer < key.second);
                                   });
        if (it != cels.end() && it->frame == frame && it->layer == layer) return &*it;
        // a document an editor built out of order: a linear search keeps it correct
        for (const Cel& c : cels) {
            if (c.frame == frame && c.layer == layer) return &c;
        }
        return nullptr;
    }

    Cel* Document::celRecord(int layer, int frame) {
        return const_cast<Cel*>(static_cast<const Document*>(this)->celRecord(layer, frame));
    }

    const Cel* Document::cel(int layer, int frame) const {
        const Cel* c = celRecord(layer, frame);
        // links point at a frame holding the image; a chain is followed a bounded number of times
        for (int hops = 0; c && c->linkedFrame >= 0 && hops < 4; hops++) {
            c = c->linkedFrame == c->frame ? nullptr : celRecord(layer, c->linkedFrame);
        }
        if (c && c->linkedFrame >= 0) return nullptr;
        return c;
    }

    const std::vector<PaletteEntry>& Document::paletteAt(int frame) const {
        const std::vector<PaletteEntry>* current = &palette;
        for (const auto& [from, entries] : paletteChanges) {
            if (from <= frame) current = &entries;
            else break;
        }
        return *current;
    }

    std::vector<uint8_t> Document::paletteRgba(int frame) const {
        const std::vector<PaletteEntry>& p = paletteAt(frame);
        std::vector<uint8_t> out(p.size() * 4);
        for (size_t i = 0; i < p.size(); i++) {
            std::memcpy(out.data() + i * 4, p[i].color.data(), 4);
        }
        return out;
    }

    std::string Document::layerPath(int layer) const {
        std::string path;
        for (int guard = 0; layer >= 0 && layer < static_cast<int>(layers.size()) && guard <= static_cast<int>(layers.size()); guard++) {
            path = path.empty() ? layers[layer].name : layers[layer].name + "/" + path;
            layer = layers[layer].parent;
        }
        return path;
    }

    int Document::findLayer(std::string_view nameOrPath) const {
        // a path first ("b" is the top-level "b" even when a group above holds another "b"),
        // then the first layer with that name
        for (int i = 0; i < static_cast<int>(layers.size()); i++) {
            if (layerPath(i) == nameOrPath) return i;
        }
        for (int i = 0; i < static_cast<int>(layers.size()); i++) {
            if (layers[i].name == nameOrPath) return i;
        }
        return -1;
    }

    std::vector<int> Document::children(int group) const {
        std::vector<int> out;
        for (int i = 0; i < static_cast<int>(layers.size()); i++) {
            if (layers[i].parent == group) out.push_back(i);
        }
        return out;
    }

    bool Document::visibleHierarchy(int layer) const {
        for (int guard = 0; layer >= 0 && layer < static_cast<int>(layers.size()) && guard <= static_cast<int>(layers.size()); guard++) {
            if (!layers[layer].visible()) return false;
            layer = layers[layer].parent;
        }
        return true;
    }

    bool Document::hasBackground() const {
        for (const Layer& l : layers) {
            if (l.background()) return true;
        }
        return false;
    }

    void Document::rebuildTreeFromLevels() {
        // NOTE.1: a layer's level relative to the one read before it; a jump deeper than one
        // level goes under the previous layer (as Aseprite's reader does)
        std::vector<int> stack;   // stack[level] = the layer last seen at that level
        for (int i = 0; i < static_cast<int>(layers.size()); i++) {
            int level = std::max(0, layers[i].childLevel);
            if (level > static_cast<int>(stack.size())) level = static_cast<int>(stack.size());
            stack.resize(static_cast<size_t>(level));
            int parent = level == 0 ? -1 : stack[static_cast<size_t>(level) - 1];
            if (parent >= 0 && !layers[parent].isGroup()) {
                // a child of a non-group: Aseprite would fail to add it; keep it beside its "parent"
                parent = layers[parent].parent;
            }
            layers[i].parent = parent;
            stack.push_back(i);
        }
        rebuildLevelsFromTree();
    }

    void Document::rebuildLevelsFromTree() {
        for (int i = 0; i < static_cast<int>(layers.size()); i++) {
            int level = 0;
            for (int p = layers[i].parent, guard = 0; p >= 0 && p < static_cast<int>(layers.size()) && guard <= static_cast<int>(layers.size());
                 p = layers[p].parent, guard++) {
                level++;
            }
            layers[i].childLevel = level;
        }
    }

    void Document::sortCels() {
        std::stable_sort(cels.begin(), cels.end(), [](const Cel& a, const Cel& b) {
            return a.frame < b.frame || (a.frame == b.frame && a.layer < b.layer);
        });
    }

}

namespace jasperengine::editor::pixelart::aseprite::detail {

    void fail(const char* kind, std::string message, std::string fix, std::string field, size_t offset) {
        CodecError e;
        e.kind = kind;
        e.message = std::move(message);
        e.fix = std::move(fix);
        e.field = std::move(field);
        e.offset = offset;
        throw Failure(std::move(e));
    }

    std::string hex16(uint16_t value) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "0x%04X", value);
        return buffer;
    }

    std::string chunkField(uint16_t type, size_t offset) {
        return "chunk " + hex16(type) + " @ " + std::to_string(offset);
    }

    void Cursor::need(size_t n) const {
        if (n <= limit - pos && pos <= limit) return;
        if (type == 0) {
            fail("truncated", "the file ends at byte " + std::to_string(limit) + " in the middle of a header",
                 "the file was cut short (a save still in progress or a broken copy): save it again from Aseprite", "header", pos);
        }
        fail("corrupt_chunk", "chunk " + hex16(type) + " at byte " + std::to_string(start - 6) + " is shorter than its data",
             "the file is damaged: open it in Aseprite and save it again", chunkField(type, start - 6), pos);
    }

    uint32_t Cursor::count(uint32_t value, size_t itemBytes, const char* what) {
        if (itemBytes > 0 && value > remaining() / itemBytes) {
            fail("corrupt_chunk", std::string("chunk ") + hex16(type) + " declares " + std::to_string(value) + " " + what +
                 ", more than its " + std::to_string(remaining()) + " remaining bytes hold",
                 "the file is damaged: open it in Aseprite and save it again", chunkField(type, start - 6), pos);
        }
        return value;
    }

    bool inflateBounded(std::span<const uint8_t> compressed, uint8_t* out, size_t expected, size_t& produced, std::string& error) {
        produced = 0;
        // a chunk is at most 4 GiB and a decoded image at most 1 GiB (R6), so both fit the
        // stream's 32-bit counters
        if (compressed.size() > 0xffffffffull || expected > 0xffffffffull) {
            error = "compressed data too large";
            return false;
        }
        mz_stream stream;
        std::memset(&stream, 0, sizeof(stream));
        if (mz_inflateInit(&stream) != MZ_OK) {
            error = "zlib could not start";
            return false;
        }
        stream.next_in = compressed.data();
        stream.avail_in = static_cast<unsigned int>(compressed.size());
        stream.next_out = out;
        stream.avail_out = static_cast<unsigned int>(expected);
        bool ok = true;
        for (;;) {
            const unsigned int inBefore = stream.avail_in;
            const unsigned int outBefore = stream.avail_out;
            const int status = mz_inflate(&stream, MZ_NO_FLUSH);
            produced = expected - stream.avail_out;
            if (status == MZ_STREAM_END || stream.avail_out == 0) break;
            if (status != MZ_OK && status != MZ_BUF_ERROR) {
                error = std::string("zlib: ") + (stream.msg ? stream.msg : "corrupt stream");
                ok = false;
                break;
            }
            // no progress: the input ended before the image did
            if (stream.avail_in == inBefore && stream.avail_out == outBefore) break;
            if (stream.avail_in == 0) break;
        }
        mz_inflateEnd(&stream);
        return ok;
    }

    bool deflateBytes(std::span<const uint8_t> raw, std::vector<uint8_t>& out) {
        out.clear();
        // miniz's stream sizes are 32-bit
        if (raw.size() > 0xffffffffull / 2) return false;
        mz_stream stream;
        std::memset(&stream, 0, sizeof(stream));
        if (mz_deflateInit(&stream, MZ_DEFAULT_LEVEL) != MZ_OK) return false;
        out.resize(mz_deflateBound(&stream, static_cast<mz_ulong>(raw.size())) + 16);
        stream.next_in = raw.data();
        stream.avail_in = static_cast<unsigned int>(raw.size());
        stream.next_out = out.data();
        stream.avail_out = static_cast<unsigned int>(out.size());
        const int status = mz_deflate(&stream, MZ_FINISH);
        out.resize(stream.total_out);
        mz_deflateEnd(&stream);
        if (status != MZ_STREAM_END) {
            out.clear();
            return false;
        }
        return true;
    }

    int bytesPerPixel(ColorMode mode) {
        switch (mode) {
            case ColorMode::Rgba: return 4;
            case ColorMode::Grayscale: return 2;
            case ColorMode::Indexed: return 1;
        }
        return 4;
    }

    bool imageFileBytes(const PixelImage& image, ColorMode mode, const std::vector<PaletteEntry>& palette, int transparentIndex,
                        std::vector<uint8_t>& out, std::string* error) {
        const size_t n = static_cast<size_t>(image.width()) * image.height();
        std::span<const uint8_t> rgba = image.pixels();
        switch (mode) {
            case ColorMode::Rgba:
                out.assign(rgba.begin(), rgba.end());
                return true;
            case ColorMode::Grayscale:
                out.resize(n * 2);
                for (size_t i = 0; i < n; i++) {
                    out[i * 2] = rgba[i * 4];
                    out[i * 2 + 1] = rgba[i * 4 + 3];
                }
                return true;
            case ColorMode::Indexed: {
                if (image.hasIndices()) {
                    std::span<const uint8_t> idx = image.indices();
                    out.assign(idx.begin(), idx.end());
                    return true;
                }
                // an edited cel lost its plane: every colour must be a palette entry
                out.resize(n);
                std::unordered_map<uint32_t, uint8_t> exact;
                for (size_t i = palette.size(); i-- > 0;) {
                    if (static_cast<int>(i) == transparentIndex || i > 255) continue;
                    const PixelRgba& c = palette[i].color;
                    exact[static_cast<uint32_t>(c[0]) | (c[1] << 8) | (c[2] << 16) | (static_cast<uint32_t>(c[3]) << 24)] = static_cast<uint8_t>(i);
                }
                for (size_t i = 0; i < n; i++) {
                    const uint8_t* p = rgba.data() + i * 4;
                    if (p[3] == 0 && transparentIndex >= 0) {
                        out[i] = static_cast<uint8_t>(transparentIndex);
                        continue;
                    }
                    auto it = exact.find(static_cast<uint32_t>(p[0]) | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
                    if (it == exact.end()) {
                        if (error) {
                            char buffer[64];
                            std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
                            *error = std::string("the colour ") + buffer + " at (" + std::to_string(i % image.width()) + ", " +
                                     std::to_string(i / image.width()) + ") is not in the palette of this indexed sprite";
                        }
                        return false;
                    }
                    out[i] = it->second;
                }
                return true;
            }
        }
        return false;
    }

    PixelImage imageFromFileBytes(int w, int h, ColorMode mode, std::span<const uint8_t> raw, std::span<const uint8_t> paletteRgba,
                                  int transparentIndex) {
        const size_t n = static_cast<size_t>(w) * h;
        if (mode == ColorMode::Indexed) {
            std::vector<uint8_t> indices(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(std::min(n, raw.size())));
            indices.resize(n, static_cast<uint8_t>(std::max(transparentIndex, 0)));
            return PixelImage::fromIndices(w, h, std::move(indices), paletteRgba, transparentIndex);
        }
        PixelImage image(w, h);
        std::span<uint8_t> px = image.editPixels();
        if (mode == ColorMode::Rgba) {
            std::memcpy(px.data(), raw.data(), std::min(raw.size(), px.size()));
        } else {
            const size_t have = std::min(n, raw.size() / 2);
            for (size_t i = 0; i < have; i++) {
                px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = raw[i * 2];
                px[i * 4 + 3] = raw[i * 2 + 1];
            }
        }
        return image;
    }

    uint64_t HashCache::imageHash(const Document& doc, const Cel& cel) {
        const void* key = cel.image.get();
        if (!key) return 0;
        if (provenance) {
            auto it = provenance->imageHashes.find(key);
            if (it != provenance->imageHashes.end()) return it->second;
        }
        auto it = computed.find(key);
        if (it != computed.end()) return it->second;
        ContentHash hasher;
        const PixelImage& image = *cel.image;
        const uint32_t size[2] = {static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height())};
        hasher.update(size, sizeof(size));
        if (doc.mode == ColorMode::Indexed && image.hasIndices()) {
            hasher.update(image.indices().data(), image.indices().size());
        } else if (doc.mode == ColorMode::Grayscale) {
            std::vector<uint8_t> raw;
            imageFileBytes(image, doc.mode, doc.palette, -1, raw, nullptr);
            hasher.update(raw.data(), raw.size());
        } else {
            // RGBA, or an indexed cel without its plane (it hashes unlike any planed image: re-encoded)
            const uint8_t marker = doc.mode == ColorMode::Indexed ? 0xEE : 0;
            hasher.update(&marker, 1);
            hasher.update(image.pixels().data(), image.pixels().size());
        }
        const uint64_t h = hasher.digest();
        computed[key] = h;
        return h;
    }

    uint64_t HashCache::tilemapHash(const Tilemap& tilemap) {
        const void* key = &tilemap;
        if (provenance) {
            auto it = provenance->imageHashes.find(key);
            if (it != provenance->imageHashes.end()) return it->second;
        }
        auto it = computed.find(key);
        if (it != computed.end()) return it->second;
        ContentHash hasher;
        const int32_t header[3] = {tilemap.columns, tilemap.rows, tilemap.bitsPerTile};
        hasher.update(header, sizeof(header));
        hasher.update(tilemap.tiles.data(), tilemap.tiles.size() * sizeof(uint32_t));
        const uint64_t h = hasher.digest();
        computed[key] = h;
        return h;
    }

}
