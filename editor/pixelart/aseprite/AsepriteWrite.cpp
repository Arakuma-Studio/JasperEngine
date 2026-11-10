// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The writer (docs/specs/S44-aseprite-import.md R7). A new document gets Aseprite 1.3's layout
// (the chunk order of the Aseprite Document IO Library's encoder, src/dio/aseprite_encoder.cpp,
// MIT; NOTICE.md); a document read from a file keeps that file's chunk order, and every chunk
// whose fingerprint still matches is written back as its original bytes, so an untouched file
// round-trips byte for byte and an edit changes only the chunks (and frame headers) it touched.
// Output is deterministic: no clocks, no hash-map iteration order, miniz at a fixed level.

#include "AsepriteCodecInternal.h"

#include "asset/ContentHash.h"
#include "io/AtomicFile.h"

#include <thread>
#include <atomic>
#include <algorithm>
#include <unordered_set>

namespace jasperengine::editor::pixelart::aseprite {

    namespace detail {

        namespace {

            uint8_t scale6(uint8_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

            bool representable(const std::vector<PaletteEntry>& palette, uint16_t type) {
                if (type == kChunkPalette) return true;
                if (palette.size() > 256) return false;
                for (const PaletteEntry& e : palette) {
                    if (e.color[3] != 255 || e.name) return false;
                    if (type != kChunkOldPalette) {
                        for (int c = 0; c < 3; c++) {
                            if (scale6(static_cast<uint8_t>(e.color[c] >> 2)) != e.color[c]) return false;
                        }
                    }
                }
                return true;
            }

            // the palette chunk Aseprite 1.3.5+ writes: the old 8-bit one while it can hold the
            // palette, else the new one (we also keep entry names, which only the new one holds)
            uint16_t defaultPaletteType(const std::vector<PaletteEntry>& palette) {
                return representable(palette, kChunkOldPalette) ? kChunkOldPalette : kChunkPalette;
            }

            void encodeValue(Out& o, const PropertyValue& v, int depth, bool& tooDeep);

            uint16_t vectorTypeOf(const PropertyValue& v) {
                const PropertyVector& elements = std::get<PropertyVector>(v.value);
                if (v.vectorElementType == 0) return 0;
                for (const PropertyValue& e : elements) {
                    if (static_cast<uint16_t>(e.type) != v.vectorElementType) {
                        // the declared type no longer fits: a common one, else each element typed
                        const uint16_t first = elements.empty() ? 0 : static_cast<uint16_t>(elements.front().type);
                        for (const PropertyValue& x : elements) {
                            if (static_cast<uint16_t>(x.type) != first) return 0;
                        }
                        return first;
                    }
                }
                return v.vectorElementType;
            }

            void encodeMap(Out& o, const PropertyMap& map, int depth, bool& tooDeep) {
                if (depth > kMaxUserDataDepth) {
                    tooDeep = true;
                    o.u32(0);
                    return;
                }
                o.u32(static_cast<uint32_t>(map.size()));
                for (const Property& p : map) {
                    o.string(p.name);
                    o.u16(static_cast<uint16_t>(p.value.type));
                    encodeValue(o, p.value, depth + 1, tooDeep);
                }
            }

            int64_t asSigned(const PropertyValue& v) {
                if (const int64_t* i = std::get_if<int64_t>(&v.value)) return *i;
                if (const uint64_t* u = std::get_if<uint64_t>(&v.value)) return static_cast<int64_t>(*u);
                if (const double* d = std::get_if<double>(&v.value)) return static_cast<int64_t>(*d);
                if (const bool* b = std::get_if<bool>(&v.value)) return *b ? 1 : 0;
                return 0;
            }

            uint64_t asUnsigned(const PropertyValue& v) {
                if (const uint64_t* u = std::get_if<uint64_t>(&v.value)) return *u;
                return static_cast<uint64_t>(asSigned(v));
            }

            double asDouble(const PropertyValue& v) {
                if (const double* d = std::get_if<double>(&v.value)) return *d;
                if (const int64_t* i = std::get_if<int64_t>(&v.value)) return static_cast<double>(*i);
                if (const uint64_t* u = std::get_if<uint64_t>(&v.value)) return static_cast<double>(*u);
                return 0;
            }

            void encodeValue(Out& o, const PropertyValue& v, int depth, bool& tooDeep) {
                if (depth > kMaxUserDataDepth) {
                    tooDeep = true;
                    return;
                }
                switch (v.type) {
                    case PropertyType::Bool: {
                        const bool* b = std::get_if<bool>(&v.value);
                        o.u8(b ? (*b ? 1 : 0) : (asSigned(v) != 0 ? 1 : 0));
                        break;
                    }
                    case PropertyType::Int8:
                    case PropertyType::Uint8: o.u8(static_cast<uint32_t>(asUnsigned(v) & 0xff)); break;
                    case PropertyType::Int16:
                    case PropertyType::Uint16: o.u16(static_cast<uint32_t>(asUnsigned(v) & 0xffff)); break;
                    case PropertyType::Int32:
                    case PropertyType::Uint32: o.u32(static_cast<uint32_t>(asUnsigned(v))); break;
                    case PropertyType::Int64:
                    case PropertyType::Uint64: o.u64(asUnsigned(v)); break;
                    case PropertyType::Fixed: o.u32(static_cast<uint32_t>(static_cast<int32_t>(std::llround(asDouble(v) * 65536.0)))); break;
                    case PropertyType::Float: {
                        const float f = static_cast<float>(asDouble(v));
                        uint32_t bits;
                        std::memcpy(&bits, &f, 4);
                        o.u32(bits);
                        break;
                    }
                    case PropertyType::Double: {
                        const double d = asDouble(v);
                        uint64_t bits;
                        std::memcpy(&bits, &d, 8);
                        o.u64(bits);
                        break;
                    }
                    case PropertyType::String: {
                        const std::string* s = std::get_if<std::string>(&v.value);
                        o.string(s ? *s : std::string());
                        break;
                    }
                    case PropertyType::Point: {
                        const IntPoint* p = std::get_if<IntPoint>(&v.value);
                        o.u32(static_cast<uint32_t>(p ? p->x : 0));
                        o.u32(static_cast<uint32_t>(p ? p->y : 0));
                        break;
                    }
                    case PropertyType::Size: {
                        const IntSize* s = std::get_if<IntSize>(&v.value);
                        o.u32(static_cast<uint32_t>(s ? s->w : 0));
                        o.u32(static_cast<uint32_t>(s ? s->h : 0));
                        break;
                    }
                    case PropertyType::Rect: {
                        const IntRect* r = std::get_if<IntRect>(&v.value);
                        o.u32(static_cast<uint32_t>(r ? r->x : 0));
                        o.u32(static_cast<uint32_t>(r ? r->y : 0));
                        o.u32(static_cast<uint32_t>(r ? r->w : 0));
                        o.u32(static_cast<uint32_t>(r ? r->h : 0));
                        break;
                    }
                    case PropertyType::Vector: {
                        const PropertyVector* elements = std::get_if<PropertyVector>(&v.value);
                        if (!elements) {
                            o.u32(0);
                            o.u16(0);
                            break;
                        }
                        const uint16_t elementType = vectorTypeOf(v);
                        o.u32(static_cast<uint32_t>(elements->size()));
                        o.u16(elementType);
                        for (const PropertyValue& e : *elements) {
                            if (elementType == 0) o.u16(static_cast<uint16_t>(e.type));
                            encodeValue(o, e, depth + 1, tooDeep);
                        }
                        break;
                    }
                    case PropertyType::Map: {
                        const PropertyMap* map = std::get_if<PropertyMap>(&v.value);
                        encodeMap(o, map ? *map : PropertyMap(), depth + 1, tooDeep);
                        break;
                    }
                    case PropertyType::Uuid: {
                        const Uuid* u = std::get_if<Uuid>(&v.value);
                        o.raw(u ? std::span<const uint8_t>(u->bytes) : std::span<const uint8_t>(Uuid().bytes));
                        break;
                    }
                }
            }

            bool encodeUserData(Out& o, const UserData& ud, CodecError* error) {
                uint32_t flags = 0;
                if (ud.text) flags |= 1;
                if (ud.color) flags |= 2;
                if (!ud.properties.empty()) flags |= 4;
                o.u32(flags);
                if (ud.text) o.string(*ud.text);
                if (ud.color) o.raw(*ud.color);
                if (flags & 4) {
                    const size_t start = o.size();
                    o.u32(0);
                    o.u32(static_cast<uint32_t>(ud.properties.size()));
                    bool tooDeep = false;
                    for (const auto& [key, map] : ud.properties) {
                        o.u32(key);
                        encodeMap(o, map, 0, tooDeep);
                    }
                    o.patch32(start, static_cast<uint32_t>(o.size() - start));
                    if (tooDeep) {
                        if (error) {
                            error->kind = "invalid_document";
                            error->message = "user data properties nest deeper than 128 levels";
                            error->fix = "flatten the nested properties";
                            error->field = "user_data";
                        }
                        return false;
                    }
                }
                return true;
            }

            void encodeSliceBody(Out& o, const Slice& slice) {
                uint32_t computed = 0;
                for (const SliceKey& k : slice.keys) {
                    if (k.center) computed |= 1;
                    if (k.pivot) computed |= 2;
                }
                const uint32_t flags = (slice.flags & ~3u) | computed;
                o.u32(static_cast<uint32_t>(slice.keys.size()));
                o.u32(flags);
                o.u32(0);
                o.string(slice.name);
                for (const SliceKey& k : slice.keys) {
                    o.u32(k.frame);
                    o.u32(static_cast<uint32_t>(k.bounds.x));
                    o.u32(static_cast<uint32_t>(k.bounds.y));
                    o.u32(static_cast<uint32_t>(std::max(0, k.bounds.w)));
                    o.u32(static_cast<uint32_t>(std::max(0, k.bounds.h)));
                    if (flags & 1) {
                        const IntRect c = k.center.value_or(IntRect{});
                        o.u32(static_cast<uint32_t>(c.x));
                        o.u32(static_cast<uint32_t>(c.y));
                        o.u32(static_cast<uint32_t>(std::max(0, c.w)));
                        o.u32(static_cast<uint32_t>(std::max(0, c.h)));
                    }
                    if (flags & 2) {
                        const IntPoint p = k.pivot.value_or(IntPoint{});
                        o.u32(static_cast<uint32_t>(p.x));
                        o.u32(static_cast<uint32_t>(p.y));
                    }
                }
            }

            void hashInto(Out& o, uint64_t h) { o.u64(h); }

            void setError(CodecError* error, const std::string& message, const std::string& fix, const std::string& field) {
                if (!error) return;
                error->kind = "invalid_document";
                error->message = message;
                error->fix = fix;
                error->field = field;
            }

            const std::vector<PaletteEntry>* previousPalette(const Document& doc, int frame) {
                if (frame <= 0) return nullptr;
                return &doc.paletteAt(frame - 1);
            }

        }

        uint16_t chunkTypeOf(const Document& doc, const ChunkRef& ref) {
            switch (ref.kind) {
                case ChunkRef::Kind::Palette: return static_cast<uint16_t>(ref.b);
                case ChunkRef::Kind::Layer: return kChunkLayer;
                case ChunkRef::Kind::Cel: return kChunkCel;
                case ChunkRef::Kind::CelExtra: return kChunkCelExtra;
                case ChunkRef::Kind::ColorProfile: return kChunkColorProfile;
                case ChunkRef::Kind::ExternalFiles: return kChunkExternalFiles;
                case ChunkRef::Kind::Mask: return kChunkMask;
                case ChunkRef::Kind::Tags: return kChunkTags;
                case ChunkRef::Kind::Slice: return kChunkSlice;
                case ChunkRef::Kind::OldSlices: return kChunkOldSlices;
                case ChunkRef::Kind::Tileset: return kChunkTileset;
                case ChunkRef::Kind::UserData: return kChunkUserData;
                case ChunkRef::Kind::Opaque:
                    return ref.a >= 0 && ref.a < static_cast<int>(doc.opaque.size()) ? doc.opaque[static_cast<size_t>(ref.a)].type : 0;
            }
            return 0;
        }

        bool encodeBody(const Document& doc, const ChunkRef& ref, uint16_t type, HashCache& hashes, bool fingerprint,
                        std::vector<uint8_t>& body, CodecError* error) {
            Out o;
            switch (ref.kind) {
                case ChunkRef::Kind::Palette: {
                    const std::vector<PaletteEntry>& palette = doc.paletteAt(ref.a);
                    if (type == kChunkPalette) {
                        size_t from = 0, to = palette.empty() ? 0 : palette.size() - 1;
                        if (const std::vector<PaletteEntry>* prev = previousPalette(doc, ref.a)) {
                            // later frames write the changed range, as Aseprite does
                            size_t first = SIZE_MAX, last = 0;
                            for (size_t i = 0; i < palette.size(); i++) {
                                if (i >= prev->size() || !(palette[i] == (*prev)[i])) {
                                    first = std::min(first, i);
                                    last = i;
                                }
                            }
                            if (first != SIZE_MAX) {
                                from = first;
                                to = last;
                            }
                        }
                        o.u32(static_cast<uint32_t>(palette.size()));
                        o.u32(static_cast<uint32_t>(from));
                        o.u32(palette.empty() ? 0u : static_cast<uint32_t>(to));
                        o.zeros(8);
                        for (size_t i = from; !palette.empty() && i <= to; i++) {
                            const PaletteEntry& e = palette[i];
                            o.u16(e.name ? 1 : 0);
                            o.raw(e.color);
                            if (e.name) o.string(*e.name);
                        }
                    } else {
                        const bool sixBit = type != kChunkOldPalette;
                        const size_t n = std::min<size_t>(palette.size(), 256);
                        o.u16(1);
                        o.u8(0);
                        o.u8(n == 256 ? 0 : static_cast<uint32_t>(n));
                        for (size_t i = 0; i < n; i++) {
                            for (int c = 0; c < 3; c++) {
                                const uint8_t v = palette[i].color[static_cast<size_t>(c)];
                                o.u8(sixBit ? static_cast<uint32_t>(v >> 2) : v);
                            }
                        }
                    }
                    break;
                }
                case ChunkRef::Kind::Layer: {
                    if (ref.a < 0 || ref.a >= static_cast<int>(doc.layers.size())) return false;
                    const Layer& layer = doc.layers[static_cast<size_t>(ref.a)];
                    const uint16_t rawType = static_cast<uint16_t>(layer.type);
                    const bool saveBlend = layer.type == LayerType::Image || layer.type == LayerType::Tilemap ||
                                           (layer.type == LayerType::Group && (doc.headerFlags & kHeaderGroupBlend));
                    o.u16(layer.flags);
                    o.u16(rawType);
                    o.u16(static_cast<uint32_t>(std::max(0, layer.childLevel)));
                    o.u16(0);
                    o.u16(0);
                    o.u16(saveBlend ? static_cast<uint16_t>(layer.blend) : 0);
                    o.u8(saveBlend ? layer.opacity : 0);
                    o.zeros(3);
                    o.string(layer.name);
                    if (layer.type == LayerType::Tilemap) o.u32(layer.tilesetIndex);
                    if (doc.headerFlags & kHeaderLayerUuids) o.raw(layer.uuid.value_or(Uuid()).bytes);
                    break;
                }
                case ChunkRef::Kind::Cel: {
                    const Cel* cel = doc.celRecord(ref.a, ref.b);
                    if (!cel) return false;
                    o.u16(static_cast<uint32_t>(cel->layer));
                    o.u16(static_cast<uint32_t>(static_cast<uint16_t>(cel->x)));
                    o.u16(static_cast<uint32_t>(static_cast<uint16_t>(cel->y)));
                    o.u8(cel->opacity);
                    if (cel->linkedFrame >= 0) {
                        o.u16(1);
                        o.u16(static_cast<uint32_t>(static_cast<uint16_t>(cel->zIndex)));
                        o.zeros(5);
                        o.u16(static_cast<uint32_t>(cel->linkedFrame));
                    } else if (cel->tilemap) {
                        const Tilemap& tm = *cel->tilemap;
                        o.u16(3);
                        o.u16(static_cast<uint32_t>(static_cast<uint16_t>(cel->zIndex)));
                        o.zeros(5);
                        o.u16(static_cast<uint32_t>(tm.columns));
                        o.u16(static_cast<uint32_t>(tm.rows));
                        o.u16(tm.bitsPerTile);
                        o.u32(tm.idMask);
                        o.u32(tm.xFlipMask);
                        o.u32(tm.yFlipMask);
                        o.u32(tm.diagonalMask);
                        o.zeros(10);
                        if (fingerprint) {
                            hashInto(o, hashes.tilemapHash(tm));
                        } else {
                            const size_t tileBytes = std::max<size_t>(1, tm.bitsPerTile / 8);
                            std::vector<uint8_t> raw(tm.tiles.size() * tileBytes);
                            for (size_t i = 0; i < tm.tiles.size(); i++) {
                                for (size_t b = 0; b < tileBytes; b++) raw[i * tileBytes + b] = static_cast<uint8_t>(tm.tiles[i] >> (8 * b));
                            }
                            std::vector<uint8_t> z;
                            if (!deflateBytes(raw, z)) {
                                setError(error, "the tilemap of layer " + std::to_string(cel->layer) + ", frame " + std::to_string(cel->frame) +
                                         " could not be compressed", "make the tilemap smaller", "cels");
                                return false;
                            }
                            o.raw(z);
                        }
                    } else {
                        o.u16(2);
                        o.u16(static_cast<uint32_t>(static_cast<uint16_t>(cel->zIndex)));
                        o.zeros(5);
                        const PixelImage* image = cel->image.get();
                        const int w = image ? image->width() : 0;
                        const int h = image ? image->height() : 0;
                        o.u16(static_cast<uint32_t>(w));
                        o.u16(static_cast<uint32_t>(h));
                        if (image && !image->empty()) {
                            if (fingerprint) {
                                hashInto(o, hashes.imageHash(doc, *cel));
                            } else {
                                const bool background = doc.layers.size() > static_cast<size_t>(cel->layer) && doc.layers[static_cast<size_t>(cel->layer)].background();
                                auto ahead = hashes.compressedCels.find(cel);
                                if (ahead != hashes.compressedCels.end() && !ahead->second.empty()) {
                                    o.raw(ahead->second);
                                    break;
                                }
                                std::vector<uint8_t> raw;
                                std::string why;
                                if (!imageFileBytes(*image, doc.mode, doc.paletteAt(cel->frame), background ? -1 : doc.transparentIndex, raw, &why)) {
                                    setError(error, why + " (layer '" + doc.layers[static_cast<size_t>(cel->layer)].name + "', frame " + std::to_string(cel->frame) + ")",
                                             "use palette colours only, or add the colour to the palette first", "cels");
                                    return false;
                                }
                                std::vector<uint8_t> z;
                                if (!deflateBytes(raw, z)) {
                                    setError(error, "the cel of layer '" + doc.layers[static_cast<size_t>(cel->layer)].name + "', frame " +
                                             std::to_string(cel->frame) + " could not be compressed", "make the cel smaller", "cels");
                                    return false;
                                }
                                o.raw(z);
                            }
                        }
                    }
                    break;
                }
                case ChunkRef::Kind::CelExtra: {
                    const Cel* cel = doc.celRecord(ref.a, ref.b);
                    if (!cel || !cel->extra) return false;
                    o.u32(cel->extra->flags);
                    o.u32(static_cast<uint32_t>(cel->extra->x));
                    o.u32(static_cast<uint32_t>(cel->extra->y));
                    o.u32(static_cast<uint32_t>(cel->extra->w));
                    o.u32(static_cast<uint32_t>(cel->extra->h));
                    o.zeros(16);
                    break;
                }
                case ChunkRef::Kind::ColorProfile: {
                    if (!doc.colorProfile) return false;
                    const ColorProfile& p = *doc.colorProfile;
                    o.u16(p.type);
                    o.u16(p.flags);
                    o.u32(static_cast<uint32_t>(p.gamma));
                    o.zeros(8);
                    if (p.type == 2) {
                        o.u32(static_cast<uint32_t>(p.icc.size()));
                        o.raw(p.icc);
                    }
                    break;
                }
                case ChunkRef::Kind::ExternalFiles: {
                    o.u32(static_cast<uint32_t>(doc.externalFiles.size()));
                    o.zeros(8);
                    for (const ExternalFile& f : doc.externalFiles) {
                        o.u32(f.id);
                        o.u8(f.type);
                        o.zeros(7);
                        o.string(f.name);
                    }
                    break;
                }
                case ChunkRef::Kind::Mask: {
                    if (ref.a < 0 || ref.a >= static_cast<int>(doc.masks.size())) return false;
                    const Mask& m = doc.masks[static_cast<size_t>(ref.a)];
                    o.u16(static_cast<uint32_t>(static_cast<uint16_t>(m.x)));
                    o.u16(static_cast<uint32_t>(static_cast<uint16_t>(m.y)));
                    o.u16(static_cast<uint32_t>(m.w));
                    o.u16(static_cast<uint32_t>(m.h));
                    o.zeros(8);
                    o.string(m.name);
                    o.raw(m.bitmap);
                    break;
                }
                case ChunkRef::Kind::Tags: {
                    o.u16(static_cast<uint32_t>(doc.tags.size()));
                    o.zeros(8);
                    for (const Tag& t : doc.tags) {
                        o.u16(static_cast<uint32_t>(t.from));
                        o.u16(static_cast<uint32_t>(t.to));
                        o.u8(static_cast<uint8_t>(t.direction));
                        o.u16(t.repeat);
                        o.zeros(6);
                        o.u8(t.legacyColor[0]);
                        o.u8(t.legacyColor[1]);
                        o.u8(t.legacyColor[2]);
                        o.u8(0);
                        o.string(t.name);
                    }
                    break;
                }
                case ChunkRef::Kind::Slice: {
                    if (ref.a < 0 || ref.a >= static_cast<int>(doc.slices.size())) return false;
                    encodeSliceBody(o, doc.slices[static_cast<size_t>(ref.a)]);
                    break;
                }
                case ChunkRef::Kind::OldSlices: {
                    if (ref.a < 0 || ref.a + ref.b > static_cast<int>(doc.slices.size())) return false;
                    o.u32(static_cast<uint32_t>(ref.b));
                    o.zeros(8);
                    for (int i = 0; i < ref.b; i++) encodeSliceBody(o, doc.slices[static_cast<size_t>(ref.a + i)]);
                    break;
                }
                case ChunkRef::Kind::Tileset: {
                    if (ref.a < 0 || ref.a >= static_cast<int>(doc.tilesets.size())) return false;
                    const Tileset& ts = doc.tilesets[static_cast<size_t>(ref.a)];
                    uint32_t flags = ts.flags & ~3u;
                    if (ts.external) flags |= 1;
                    if (!ts.tiles.empty() || (ts.flags & 2)) flags |= 2;
                    o.u32(ts.id);
                    o.u32(flags);
                    o.u32(static_cast<uint32_t>(ts.tileCount));
                    o.u16(static_cast<uint32_t>(ts.tileWidth));
                    o.u16(static_cast<uint32_t>(ts.tileHeight));
                    o.u16(static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(ts.baseIndex))));
                    o.zeros(14);
                    o.string(ts.name);
                    if (flags & 1) {
                        const ExternalTileset ext = ts.external.value_or(ExternalTileset{});
                        o.u32(ext.fileId);
                        o.u32(ext.tilesetId);
                    }
                    if ((flags & 2) && ts.tileCount > 0) {
                        if (ts.tiles.width() != ts.tileWidth || ts.tiles.height() != ts.tileHeight * ts.tileCount) {
                            setError(error, "the tileset '" + ts.name + "' holds a " + std::to_string(ts.tiles.width()) + "x" + std::to_string(ts.tiles.height()) +
                                     " strip for " + std::to_string(ts.tileCount) + " tiles of " + std::to_string(ts.tileWidth) + "x" + std::to_string(ts.tileHeight),
                                     "make the strip tileWidth x (tileHeight * tileCount)", "tilesets");
                            return false;
                        }
                        std::vector<uint8_t> raw;
                        std::string why;
                        if (!imageFileBytes(ts.tiles, doc.mode, doc.palette, doc.transparentIndex, raw, &why)) {
                            setError(error, why + " (tileset '" + ts.name + "')", "use palette colours only", "tilesets");
                            return false;
                        }
                        if (fingerprint) {
                            o.u32(0);
                            hashInto(o, ContentHash::of(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size())));
                        } else {
                            std::vector<uint8_t> z;
                            if (!deflateBytes(raw, z)) {
                                setError(error, "the tileset '" + ts.name + "' could not be compressed", "use fewer or smaller tiles", "tilesets");
                                return false;
                            }
                            o.u32(static_cast<uint32_t>(z.size()));
                            o.raw(z);
                        }
                    }
                    break;
                }
                case ChunkRef::Kind::UserData: {
                    const UserData* ud = nullptr;
                    switch (static_cast<UserDataOwner>(ref.b)) {
                        case UserDataOwner::Sprite: ud = &doc.spriteUserData; break;
                        case UserDataOwner::Layer:
                            if (ref.a >= 0 && ref.a < static_cast<int>(doc.layers.size())) ud = &doc.layers[static_cast<size_t>(ref.a)].userData;
                            break;
                        case UserDataOwner::Cel:
                            if (const Cel* cel = doc.celRecord(ref.a, ref.c)) ud = &cel->userData;
                            break;
                        case UserDataOwner::Tag:
                            if (ref.a >= 0 && ref.a < static_cast<int>(doc.tags.size())) ud = &doc.tags[static_cast<size_t>(ref.a)].userData;
                            break;
                        case UserDataOwner::Slice:
                            if (ref.a >= 0 && ref.a < static_cast<int>(doc.slices.size())) ud = &doc.slices[static_cast<size_t>(ref.a)].userData;
                            break;
                        case UserDataOwner::Tileset:
                            if (ref.a >= 0 && ref.a < static_cast<int>(doc.tilesets.size())) ud = &doc.tilesets[static_cast<size_t>(ref.a)].userData;
                            break;
                        case UserDataOwner::Tile: {
                            if (ref.a >= 0 && ref.a < static_cast<int>(doc.tilesets.size())) {
                                const Tileset& ts = doc.tilesets[static_cast<size_t>(ref.a)];
                                static const UserData empty;
                                ud = ref.c >= 0 && ref.c < static_cast<int>(ts.tileUserData.size()) ? &ts.tileUserData[static_cast<size_t>(ref.c)] : &empty;
                            }
                            break;
                        }
                    }
                    if (!ud) return false;
                    if (!encodeUserData(o, *ud, error)) return false;
                    break;
                }
                case ChunkRef::Kind::Opaque: {
                    if (ref.a < 0 || ref.a >= static_cast<int>(doc.opaque.size())) return false;
                    o.raw(doc.opaque[static_cast<size_t>(ref.a)].body);
                    break;
                }
            }
            body = std::move(o.bytes);
            return true;
        }

        uint64_t fingerprintOf(const Document& doc, const ChunkRef& ref, uint16_t type, HashCache& hashes) {
            std::vector<uint8_t> body;
            if (!encodeBody(doc, ref, type, hashes, true, body, nullptr)) return 0;
            ContentHash hasher;
            const uint8_t header[2] = {static_cast<uint8_t>(type & 0xff), static_cast<uint8_t>(type >> 8)};
            hasher.update(header, 2);
            hasher.update(body.data(), body.size());
            return hasher.digest() | 1;   // never 0, the "could not encode" value
        }

    }

    namespace {

        using namespace detail;

        struct Planned {
            ChunkRef ref;
            uint16_t type = 0;
            const ChunkRecord* original = nullptr;   // the record whose bytes may be reused
        };

        // The refs a provenance frame holds, for "the file had this chunk" questions
        class FrameRefs {
        public:
            explicit FrameRefs(const FrameRecord* frame) {
                if (!frame) return;
                for (const ChunkRecord& r : frame->chunks) refs.push_back(r.ref);
            }
            bool has(const ChunkRef& ref) const { return std::find(refs.begin(), refs.end(), ref) != refs.end(); }
            std::vector<uint16_t> paletteTypes() const {
                std::vector<uint16_t> types;
                for (const ChunkRef& r : refs) {
                    if (r.kind == ChunkRef::Kind::Palette && std::find(types.begin(), types.end(), static_cast<uint16_t>(r.b)) == types.end()) {
                        types.push_back(static_cast<uint16_t>(r.b));
                    }
                }
                return types;
            }
            const std::vector<ChunkRef>& all() const { return refs; }

        private:
            std::vector<ChunkRef> refs;
        };

        class Writer {
        public:
            Writer(const Document& doc, CodecError* error): doc(doc), error(error), prov(doc.provenance.get()) {
                hashes.provenance = prov;
                // slices stay in the frame the file had them in (Aseprite writes them in frame 0)
                sliceFrame.assign(doc.slices.size(), 0);
                if (prov) {
                    for (size_t f = 0; f < prov->frames.size(); f++) {
                        for (const ChunkRecord& r : prov->frames[f].chunks) {
                            if (r.ref.kind == ChunkRef::Kind::Slice && r.ref.a >= 0 && r.ref.a < static_cast<int>(doc.slices.size())) {
                                sliceFrame[static_cast<size_t>(r.ref.a)] = static_cast<int>(f);
                            }
                        }
                    }
                }
            }

            bool run(std::vector<uint8_t>& out);

        private:
            const Document& doc;
            CodecError* error;
            const Provenance* prov;
            HashCache hashes;
            std::vector<int> sliceFrame;

            const FrameRecord* provFrame(int f) const {
                return prov && f >= 0 && f < static_cast<int>(prov->frames.size()) ? &prov->frames[static_cast<size_t>(f)] : nullptr;
            }
            const ChunkRecord* findRecord(const FrameRecord* fr, const ChunkRef& ref) const {
                if (!fr) return nullptr;
                for (const ChunkRecord& r : fr->chunks) {
                    if (r.ref == ref) return &r;
                }
                return nullptr;
            }
            bool fingerprintMatches(const ChunkRecord& rec) {
                return rec.fingerprint != 0 && fingerprintOf(doc, rec.ref, rec.type, hashes) == rec.fingerprint;
            }

            std::vector<Planned> canonical(int frame);
            std::vector<Planned> merge(int frame, std::vector<Planned> wanted);
            void paletteChunks(int frame, const FrameRefs& had, std::vector<Planned>& out);
            void compressAhead(const std::vector<std::vector<Planned>>& plans);
            bool writeHeader(std::vector<uint8_t>& out);
        };

        void Writer::compressAhead(const std::vector<std::vector<Planned>>& plans) {
            struct Job {
                const Cel* cel;
                std::vector<uint8_t> out;
            };
            std::vector<Job> jobs;
            size_t total = 0;
            for (const std::vector<Planned>& plan : plans) {
                for (const Planned& p : plan) {
                    if (p.ref.kind != ChunkRef::Kind::Cel) continue;
                    if (p.original && p.original->type == p.type && fingerprintMatches(*p.original)) continue;
                    const Cel* cel = doc.celRecord(p.ref.a, p.ref.b);
                    if (!cel || cel->linkedFrame >= 0 || cel->tilemap || !cel->image || cel->image->empty()) continue;
                    if (hashes.compressedCels.count(cel)) continue;
                    hashes.compressedCels[cel];
                    jobs.push_back({cel, {}});
                    total += static_cast<size_t>(cel->image->width()) * cel->image->height() * 4;
                }
            }
            // threads pay off on big documents only; a cel that fails here is encoded (and its
            // error reported) by encodeBody as before
            const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
            const unsigned threads = total < (8u << 20) ? 0u : std::min({hardware, 8u, static_cast<unsigned>(jobs.size())});
            if (threads == 0) {
                hashes.compressedCels.clear();
                return;
            }
            std::atomic<size_t> next{0};
            auto work = [this, &jobs, &next]() {
                for (;;) {
                    const size_t i = next.fetch_add(1);
                    if (i >= jobs.size()) return;
                    const Cel& cel = *jobs[i].cel;
                    const bool background = doc.layers.size() > static_cast<size_t>(cel.layer) && doc.layers[static_cast<size_t>(cel.layer)].background();
                    std::vector<uint8_t> raw;
                    if (!imageFileBytes(*cel.image, doc.mode, doc.paletteAt(cel.frame), background ? -1 : doc.transparentIndex, raw, nullptr)) continue;
                    deflateBytes(raw, jobs[i].out);
                }
            };
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < threads; t++) pool.emplace_back(work);
            work();
            for (std::thread& t : pool) t.join();
            for (Job& job : jobs) hashes.compressedCels[job.cel] = std::move(job.out);
        }

        // Frame 0 always has a palette chunk, a later frame where its palette changes; the
        // chunk types the file used there stay while they can hold the palette (or still match)
        void Writer::paletteChunks(int frame, const FrameRefs& had, std::vector<Planned>& out) {
            const std::vector<PaletteEntry>& palette = doc.paletteAt(frame);
            const bool needed = !palette.empty() && (frame == 0 || doc.paletteChanges.count(frame) > 0);
            const FrameRecord* fr = provFrame(frame);
            std::vector<uint16_t> types;
            for (uint16_t t : had.paletteTypes()) {
                const ChunkRef ref{ChunkRef::Kind::Palette, frame, t};
                const ChunkRecord* rec = findRecord(fr, ref);
                if (rec && fingerprintMatches(*rec)) {
                    types.push_back(t);
                } else if (needed && representable(palette, t)) {
                    types.push_back(t);
                }
            }
            if (types.empty() && needed) types.push_back(defaultPaletteType(palette));
            for (uint16_t t : types) out.push_back({{ChunkRef::Kind::Palette, frame, t}, t, nullptr});
        }

        std::vector<Planned> Writer::canonical(int frame) {
            std::vector<Planned> list;
            const FrameRefs had(provFrame(frame));
            const bool fresh = prov == nullptr;
            auto add = [&](const ChunkRef& ref) { list.push_back({ref, chunkTypeOf(doc, ref), nullptr}); };
            auto addUserData = [&](const ChunkRef& ref, const UserData& ud) {
                if (!ud.empty() || had.has(ref)) add(ref);
            };
            // opaque chunks go after the object they followed in the file
            auto addOpaque = [&](const ChunkAnchor& a) {
                for (size_t i = 0; i < doc.opaque.size(); i++) {
                    if (doc.opaque[i].frame == frame && doc.opaque[i].anchor == a) add({ChunkRef::Kind::Opaque, static_cast<int>(i)});
                }
            };
            addOpaque({ChunkAnchor::Kind::FrameStart});

            if (frame == 0) {
                if (!doc.externalFiles.empty()) add({ChunkRef::Kind::ExternalFiles});
                if (doc.colorProfile) add({ChunkRef::Kind::ColorProfile});
                paletteChunks(0, had, list);
                addOpaque({ChunkAnchor::Kind::Palette});
                addUserData(userDataRef(UserDataOwner::Sprite), doc.spriteUserData);
                for (int t = 0; t < static_cast<int>(doc.tilesets.size()); t++) {
                    const Tileset& ts = doc.tilesets[static_cast<size_t>(t)];
                    add({ChunkRef::Kind::Tileset, t});
                    // tile user data follows the tileset's own, in tile order (positional): a
                    // new document writes them all, as Aseprite does; a read one the ones it had
                    // and up to the last tile that has some
                    int lastTile = -1;
                    for (int i = 0; i < ts.tileCount; i++) {
                        const bool nonEmpty = i < static_cast<int>(ts.tileUserData.size()) && !ts.tileUserData[static_cast<size_t>(i)].empty();
                        if (fresh || nonEmpty || had.has(userDataRef(UserDataOwner::Tile, t, i))) lastTile = i;
                    }
                    const ChunkRef tsRef = userDataRef(UserDataOwner::Tileset, t);
                    if (fresh || lastTile >= 0 || !ts.userData.empty() || had.has(tsRef)) {
                        add(tsRef);
                        for (int i = 0; i <= lastTile; i++) add(userDataRef(UserDataOwner::Tile, t, i));
                    }
                    addOpaque({ChunkAnchor::Kind::Tileset, t});
                }
                if (!doc.tags.empty()) {
                    add({ChunkRef::Kind::Tags});
                    int lastTag = -1;
                    for (int i = 0; i < static_cast<int>(doc.tags.size()); i++) {
                        if (fresh || !doc.tags[static_cast<size_t>(i)].userData.empty() || had.has(userDataRef(UserDataOwner::Tag, i))) lastTag = i;
                    }
                    for (int i = 0; i <= lastTag; i++) add(userDataRef(UserDataOwner::Tag, i));
                    addOpaque({ChunkAnchor::Kind::Tags});
                }
                for (int l = 0; l < static_cast<int>(doc.layers.size()); l++) {
                    add({ChunkRef::Kind::Layer, l});
                    addUserData(userDataRef(UserDataOwner::Layer, l), doc.layers[static_cast<size_t>(l)].userData);
                    addOpaque({ChunkAnchor::Kind::Layer, l});
                }
                // slices the file held in one deprecated chunk stay there while they are unchanged
                std::vector<bool> covered(doc.slices.size(), false);
                for (const ChunkRef& r : had.all()) {
                    if (r.kind != ChunkRef::Kind::OldSlices || r.a < 0 || r.a + r.b > static_cast<int>(doc.slices.size())) continue;
                    const ChunkRecord* rec = findRecord(provFrame(frame), r);
                    if (rec && fingerprintMatches(*rec)) {
                        for (int i = r.a; i < r.a + r.b; i++) covered[static_cast<size_t>(i)] = true;
                        add(r);
                    }
                }
                for (int s = 0; s < static_cast<int>(doc.slices.size()); s++) {
                    if (covered[static_cast<size_t>(s)] || sliceFrame[static_cast<size_t>(s)] != 0) continue;
                    add({ChunkRef::Kind::Slice, s});
                    addUserData(userDataRef(UserDataOwner::Slice, s), doc.slices[static_cast<size_t>(s)].userData);
                    addOpaque({ChunkAnchor::Kind::Slice, s});
                }
            } else {
                // a later frame's palette chunk (where the palette changes)
                paletteChunks(frame, had, list);
                addOpaque({ChunkAnchor::Kind::Palette});
                for (int s = 0; s < static_cast<int>(doc.slices.size()); s++) {
                    if (sliceFrame[static_cast<size_t>(s)] != frame) continue;
                    add({ChunkRef::Kind::Slice, s});
                    addUserData(userDataRef(UserDataOwner::Slice, s), doc.slices[static_cast<size_t>(s)].userData);
                    addOpaque({ChunkAnchor::Kind::Slice, s});
                }
            }
            for (int m = 0; m < static_cast<int>(doc.masks.size()); m++) {
                if (doc.masks[static_cast<size_t>(m)].frame == frame) add({ChunkRef::Kind::Mask, m});
            }
            for (const Cel& cel : doc.cels) {
                if (cel.frame != frame) continue;
                add({ChunkRef::Kind::Cel, cel.layer, frame});
                if (cel.extra) add({ChunkRef::Kind::CelExtra, cel.layer, frame});
                addUserData(userDataRef(UserDataOwner::Cel, cel.layer, frame), cel.userData);
                addOpaque({ChunkAnchor::Kind::Cel, cel.layer, frame});
            }
            // opaque chunks whose anchor is gone, and those that followed something else
            for (size_t i = 0; i < doc.opaque.size(); i++) {
                if (doc.opaque[i].frame != frame) continue;
                const ChunkRef ref{ChunkRef::Kind::Opaque, static_cast<int>(i)};
                bool present = false;
                for (const Planned& p : list) present = present || p.ref == ref;
                if (!present) add(ref);
            }
            return list;
        }

        // The file's chunk order for what still exists, the canonical position (after the chunk
        // the canonical order puts before it) for what is new
        std::vector<Planned> Writer::merge(int frame, std::vector<Planned> wanted) {
            const FrameRecord* fr = provFrame(frame);
            if (!fr) return wanted;
            std::vector<Planned> out;
            std::vector<bool> placed(wanted.size(), false);
            for (const ChunkRecord& rec : fr->chunks) {
                for (size_t i = 0; i < wanted.size(); i++) {
                    if (wanted[i].ref == rec.ref) {
                        Planned p = wanted[i];
                        p.original = &rec;
                        out.push_back(p);
                        placed[i] = true;
                        break;
                    }
                }
            }
            for (size_t i = 0; i < wanted.size(); i++) {
                if (placed[i]) continue;
                // after the nearest earlier wanted chunk already in the output
                size_t at = 0;
                for (size_t j = i; j-- > 0;) {
                    if (!placed[j]) continue;
                    for (size_t k = out.size(); k-- > 0;) {
                        if (out[k].ref == wanted[j].ref) {
                            at = k + 1;
                            break;
                        }
                    }
                    break;
                }
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), wanted[i]);
                placed[i] = true;
            }
            return out;
        }

        bool Writer::writeHeader(std::vector<uint8_t>& out) {
            out.assign(128, 0);
            const HeaderSnapshot* snap = prov ? &prov->header : nullptr;
            if (prov && prov->bytes && prov->bytes->size() >= 128) {
                std::copy(prov->bytes->begin(), prov->bytes->begin() + 128, out.begin());
            }
            auto put16 = [&](size_t at, uint32_t v) {
                out[at] = static_cast<uint8_t>(v);
                out[at + 1] = static_cast<uint8_t>(v >> 8);
            };
            auto put32 = [&](size_t at, uint32_t v) {
                for (int i = 0; i < 4; i++) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
            };
            put16(4, 0xA5E0);
            put16(6, static_cast<uint32_t>(doc.frameCount()));
            put16(8, static_cast<uint32_t>(doc.width));
            put16(10, static_cast<uint32_t>(doc.height));
            put16(12, static_cast<uint32_t>(static_cast<uint16_t>(doc.mode)));
            put32(14, doc.headerFlags);
            put16(18, doc.speed);
            // transparent index, palette size and pixel ratio keep their raw bytes while their
            // meaning is unchanged (0 colours means 256 in old files, 0:0 means 1:1)
            if (!snap || snap->transparentIndex != doc.transparentIndex || snap->depth != static_cast<uint16_t>(doc.mode)) {
                out[28] = doc.mode == ColorMode::Indexed ? doc.transparentIndex : 0;
            }
            const size_t paletteSize = doc.palette.size();
            if (!snap || snap->paletteSize != paletteSize) {
                put16(32, static_cast<uint32_t>(std::min<size_t>(paletteSize, 0xffff)));
            }
            if (!snap || snap->pixelWidth != doc.pixelWidth || snap->pixelHeight != doc.pixelHeight) {
                out[34] = doc.pixelWidth;
                out[35] = doc.pixelHeight;
            }
            if (!snap || snap->gridX != doc.grid.x || snap->gridY != doc.grid.y || snap->gridW != doc.grid.w || snap->gridH != doc.grid.h) {
                put16(36, static_cast<uint32_t>(static_cast<uint16_t>(doc.grid.x)));
                put16(38, static_cast<uint32_t>(static_cast<uint16_t>(doc.grid.y)));
                put16(40, static_cast<uint32_t>(doc.grid.w));
                put16(42, static_cast<uint32_t>(doc.grid.h));
            }
            return true;
        }

        bool Writer::run(std::vector<uint8_t>& out) {
            // validation the format needs
            if (doc.width < 1 || doc.height < 1 || doc.width > kMaxCanvasSide || doc.height > kMaxCanvasSide) {
                setError(error, "the canvas is " + std::to_string(doc.width) + "x" + std::to_string(doc.height) + " (1 to 16384 per side)",
                         "give the sprite a size between 1 and 16384", "size");
                return false;
            }
            if (doc.frameCount() < 1 || doc.frameCount() > kMaxFrames) {
                setError(error, "the sprite has " + std::to_string(doc.frameCount()) + " frames (1 to 65535)", "give it at least one frame", "frames");
                return false;
            }
            for (const Cel& cel : doc.cels) {
                if (cel.layer < 0 || cel.layer >= static_cast<int>(doc.layers.size()) || cel.frame < 0 || cel.frame >= doc.frameCount()) {
                    setError(error, "a cel is on layer " + std::to_string(cel.layer) + ", frame " + std::to_string(cel.frame) + ", which does not exist",
                             "remove the cel or add the layer/frame first", "cels");
                    return false;
                }
                const LayerType type = doc.layers[static_cast<size_t>(cel.layer)].type;
                if (type != LayerType::Image && type != LayerType::Tilemap) {
                    setError(error, "a cel is on the group layer '" + doc.layers[static_cast<size_t>(cel.layer)].name + "'",
                             "put the cel on an image or tilemap layer", "cels");
                    return false;
                }
                const std::string where = " (layer " + std::to_string(cel.layer) + ", frame " + std::to_string(cel.frame) + ")";
                if (cel.image && (cel.image->width() > 0xffff || cel.image->height() > 0xffff)) {
                    setError(error, "a cel is larger than 65535 pixels a side" + where, "crop the cel", "cels");
                    return false;
                }
                // the file keeps the position in 16 bits
                if (cel.x < -32768 || cel.x > 32767 || cel.y < -32768 || cel.y > 32767) {
                    setError(error, "a cel is at " + std::to_string(cel.x) + ", " + std::to_string(cel.y) + where + " (-32768 to 32767)",
                             "move the cel nearer the canvas", "cels");
                    return false;
                }
                if (cel.linkedFrame >= 0 && (cel.linkedFrame >= cel.frame || !doc.celRecord(cel.layer, cel.linkedFrame))) {
                    setError(error, "a cel links to frame " + std::to_string(cel.linkedFrame) + where + ", which has no earlier cel on its layer",
                             "link to an earlier frame's cel", "cels");
                    return false;
                }
                if (cel.tilemap) {
                    const Tilemap& tm = *cel.tilemap;
                    if (tm.columns < 0 || tm.rows < 0 || tm.columns > 0xffff || tm.rows > 0xffff ||
                        tm.tiles.size() != static_cast<size_t>(tm.columns) * static_cast<size_t>(tm.rows) ||
                        (tm.bitsPerTile != 8 && tm.bitsPerTile != 16 && tm.bitsPerTile != 32)) {
                        setError(error, "a tilemap" + where + " is " + std::to_string(tm.columns) + "x" + std::to_string(tm.rows) + " with " +
                                 std::to_string(tm.tiles.size()) + " tiles at " + std::to_string(tm.bitsPerTile) + " bits",
                                 "give it columns x rows tiles (each side up to 65535) of 8, 16 or 32 bits", "cels");
                        return false;
                    }
                }
            }

            // the plans, and the image cels to encode again compressed ahead on several threads
            // (R26: every cel of a big file changed is deflating 128 MB)
            std::vector<std::vector<Planned>> plans(static_cast<size_t>(doc.frameCount()));
            for (int f = 0; f < doc.frameCount(); f++) plans[static_cast<size_t>(f)] = merge(f, canonical(f));
            compressAhead(plans);

            std::vector<uint8_t> file;
            writeHeader(file);
            for (int f = 0; f < doc.frameCount(); f++) {
                const FrameRecord* fr = provFrame(f);
                std::vector<Planned>& plan = plans[static_cast<size_t>(f)];
                const uint16_t duration = doc.frameDurations[static_cast<size_t>(f)];

                // an untouched frame is copied whole (its header and any slack after its chunks)
                bool untouched = fr && fr->chunks.size() == plan.size() &&
                                 (fr->rawDuration == duration || (fr->rawDuration == 0 && duration == doc.speed && prov->header.speed == doc.speed));
                std::vector<std::vector<uint8_t>> bodies(plan.size());
                std::vector<bool> reuse(plan.size(), false);
                for (size_t i = 0; i < plan.size(); i++) {
                    const Planned& p = plan[i];
                    if (p.original && p.original->type == p.type && fingerprintMatches(*p.original)) {
                        reuse[i] = true;
                        if (untouched && &fr->chunks[i] != p.original) untouched = false;
                        continue;
                    }
                    untouched = false;
                    if (!encodeBody(doc, p.ref, p.type, hashes, false, bodies[i], error)) {
                        if (error && error->kind.empty()) {
                            setError(error, "a chunk could not be encoded", "check the document", "");
                        }
                        return false;
                    }
                }
                if (untouched) {
                    file.insert(file.end(), prov->bytes->begin() + static_cast<std::ptrdiff_t>(fr->offset),
                                prov->bytes->begin() + static_cast<std::ptrdiff_t>(fr->offset + fr->size));
                    continue;
                }
                Out frame;
                frame.u32(0);
                frame.u16(0xF1FA);
                frame.u16(static_cast<uint32_t>(std::min<size_t>(plan.size(), 0xFFFF)));
                frame.u16(fr && fr->rawDuration == 0 && duration == doc.speed ? 0 : duration);
                if (fr) {
                    frame.u8((*prov->bytes)[fr->offset + 10]);
                    frame.u8((*prov->bytes)[fr->offset + 11]);
                } else {
                    frame.zeros(2);
                }
                frame.u32(static_cast<uint32_t>(plan.size()));
                for (size_t i = 0; i < plan.size(); i++) {
                    if (reuse[i]) {
                        const ChunkRecord* rec = plan[i].original;
                        frame.raw(std::span<const uint8_t>(prov->bytes->data() + rec->offset, rec->size));
                    } else {
                        if (bodies[i].size() > 0xffffffffull - 6) {
                            setError(error, "a chunk is larger than 4 GiB", "split the sprite", "");
                            return false;
                        }
                        frame.u32(static_cast<uint32_t>(bodies[i].size() + 6));
                        frame.u16(plan[i].type);
                        frame.raw(bodies[i]);
                    }
                }
                frame.patch32(0, static_cast<uint32_t>(frame.size()));
                file.insert(file.end(), frame.bytes.begin(), frame.bytes.end());
            }
            // bytes the file had after its last frame stay there
            if (prov && prov->bytes && prov->framesEnd < prov->bytes->size() && prov->frames.size() == static_cast<size_t>(doc.frameCount())) {
                file.insert(file.end(), prov->bytes->begin() + static_cast<std::ptrdiff_t>(prov->framesEnd), prov->bytes->end());
            }
            if (file.size() > 0xffffffffull) {
                setError(error, "the file would be larger than 4 GiB", "split the sprite", "");
                return false;
            }
            // the size field: kept when it still matches (a file whose field disagreed with its length keeps that)
            const uint32_t size = static_cast<uint32_t>(file.size());
            const bool sameLength = prov && prov->bytes && prov->bytes->size() == file.size();
            if (!sameLength) {
                for (int i = 0; i < 4; i++) file[static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));
            }
            out = std::move(file);
            return true;
        }

    }

    bool write(const Document& document, std::vector<uint8_t>& out, CodecError* error) {
        if (error) *error = CodecError();
        if (document.palette.empty()) {
            // a file always has a palette: without one a reader (Aseprite's, ours) starts from the
            // header's 256 black entries, so they are written out, and a re-save is the same bytes
            Document withPalette = document;
            withPalette.palette.assign(256, PaletteEntry{});
            Writer writer(withPalette, error);
            return writer.run(out);
        }
        Writer writer(document, error);
        return writer.run(out);
    }

    bool writeFile(const Document& document, const std::filesystem::path& path, CodecError* error) {
        std::vector<uint8_t> bytes;
        if (!write(document, bytes, error)) return false;
        const io::WriteResult result = io::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!result.ok()) {
            if (error) {
                error->kind = "import_failed";
                error->message = "could not write " + path.generic_string() + ": " + result.message;
                error->fix = "check the folder exists and is writable";
                error->field = "path";
            }
            return false;
        }
        return true;
    }

}
