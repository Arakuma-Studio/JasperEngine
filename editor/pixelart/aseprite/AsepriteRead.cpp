// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The reader (docs/specs/S44-aseprite-import.md R1-R6). The chunk walk and the rules for what
// a user data chunk belongs to follow Aseprite's own decoder (Aseprite Document IO Library,
// src/dio/aseprite_decoder.cpp, MIT; NOTICE.md); every length, count and decompressed size is
// checked against the bytes left and R6's limits, and what upstream skips (unknown chunks, the
// path chunk, user data with no owner, cels of missing layers) is kept as opaque chunks.

#include "AsepriteCodecInternal.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <new>
#include <thread>

namespace jasperengine::editor::pixelart::aseprite {

    namespace {

        using namespace detail;

        constexpr uint16_t kFileMagic = 0xA5E0;
        constexpr uint16_t kFrameMagic = 0xF1FA;

        // what the next user data chunk belongs to (Aseprite's last_object_with_user_data)
        struct Owner {
            enum class Kind { None, Sprite, Layer, Cel, Tag, Slice, Tileset } kind = Kind::Sprite;
            int a = -1, b = -1;
        };

        struct PendingImage {
            std::shared_ptr<PixelImage> image;
            int layer = 0, frame = 0;
        };

        // a compressed cel, inflated in finish() with the others, on several threads (R26: a
        // 256x256, 64-frame, 8-layer file is 128 MB of pixels to inflate)
        struct DeferredCel {
            std::shared_ptr<PixelImage> image;
            std::span<const uint8_t> compressed;
            size_t expected = 0;
            int w = 0, h = 0;
            size_t chunkStart = 0;
            size_t produced = 0;
            std::string error;
        };

        class Reader {
        public:
            Reader(std::span<const uint8_t> bytes, const ReadOptions& options, std::vector<CodecError>& warnings)
                : data(bytes), options(options), warnings(warnings) {}

            Document run();

        private:
            std::span<const uint8_t> data;
            ReadOptions options;
            std::vector<CodecError>& warnings;

            Document doc;
            std::unique_ptr<Provenance> prov;
            HeaderSnapshot snapshot;
            std::vector<PaletteEntry> currentPalette;
            bool newPaletteSeen = false;
            Owner owner;
            int tileUserDataLeft = 0;      // tile user data chunks still expected after a tileset's
            int tileUserDataTileset = -1;
            uint64_t decodedBytes = 0;
            uint64_t paletteBytes = 0;     // the palettes kept, frame 0's and each change's
            std::vector<PendingImage> pending;
            std::vector<DeferredCel> deferred;
            void inflateDeferred();
            ChunkAnchor anchor;            // the last object chunk read in the frame

            void warn(const char* kind, std::string message, std::string fix, std::string field, size_t offset) {
                CodecError w;
                w.kind = kind;
                w.message = std::move(message);
                w.fix = std::move(fix);
                w.field = std::move(field);
                w.offset = offset;
                warnings.push_back(std::move(w));
            }

            void readHeader();
            void readFrame(int frame, size_t& pos);
            // returns the chunk's reference (the provenance record)
            ChunkRef readChunk(int frame, uint16_t type, size_t chunkStart, size_t chunkEnd);

            ChunkRef keepOpaque(int frame, uint16_t type, size_t chunkStart, size_t chunkEnd);
            void readOldPalette(Cursor& c, bool sixBit);
            void readPalette(Cursor& c);
            void commitPalette(int frame, size_t offset);
            ChunkRef readLayer(Cursor& c, size_t chunkStart);
            ChunkRef readCel(Cursor& c, int frame, size_t chunkStart, size_t chunkEnd);
            void readCelExtra(Cursor& c, int frame);
            void readColorProfile(Cursor& c);
            void readExternalFiles(Cursor& c);
            void readMask(Cursor& c, int frame);
            void readTags(Cursor& c);
            ChunkRef readUserData(Cursor& c, int frame, size_t chunkStart, size_t chunkEnd);
            void readUserDataBody(Cursor& c, UserData& out);
            PropertyValue readValue(Cursor& c, uint16_t type, int depth);
            PropertyMap readMap(Cursor& c, int depth);
            void readSliceBody(Cursor& c, Slice& slice, size_t chunkStart);
            void readTileset(Cursor& c, size_t chunkStart);

            std::vector<uint8_t> inflateInto(Cursor& c, size_t expected, size_t chunkStart, uint16_t type);
            void budget(uint64_t bytes, size_t offset);
            void finish();
        };

        void Reader::budget(uint64_t bytes, size_t offset) {
            decodedBytes += bytes;
            if (decodedBytes > kMaxDecodedPixelBytes) {
                fail("too_large", "the sprite decodes to more than 1 GiB of pixels",
                     "split the file (fewer frames or layers) or crop the canvas", "", offset);
            }
        }

        void Reader::readHeader() {
            if (data.size() < 128) {
                if (data.size() >= 6 && (data[4] | (data[5] << 8)) == kFileMagic) {
                    fail("truncated", "the file is " + std::to_string(data.size()) + " bytes, shorter than its 128-byte header",
                         "the file was cut short (a save still in progress?): save it again from Aseprite", "header", data.size());
                }
                fail("not_aseprite", "the file is not an Aseprite file (it is shorter than the 128-byte header)",
                     "open an .aseprite or .ase file saved by Aseprite", "header", 0);
            }
            Cursor c(data, 0, 128, 0);
            c.u32();   // file size: the frames say where they end
            const uint16_t magic = c.u16();
            if (magic != kFileMagic) {
                fail("not_aseprite", "the file is not an Aseprite file (magic " + hex16(magic) + ", expected 0xA5E0)",
                     "open an .aseprite or .ase file saved by Aseprite", "header", 4);
            }
            snapshot.frames = c.u16();
            snapshot.width = c.u16();
            snapshot.height = c.u16();
            snapshot.depth = c.u16();
            snapshot.flags = c.u32();
            snapshot.speed = c.u16();
            c.u32();
            c.u32();
            uint8_t transparent = c.u8();
            c.skip(3);
            uint16_t ncolors = c.u16();
            uint8_t pw = c.u8();
            uint8_t ph = c.u8();
            snapshot.gridX = c.i16();
            snapshot.gridY = c.i16();
            snapshot.gridW = c.u16();
            snapshot.gridH = c.u16();

            if (snapshot.depth != 32 && snapshot.depth != 16 && snapshot.depth != 8) {
                fail("unsupported_depth", "the colour depth is " + std::to_string(snapshot.depth) + " bits (Aseprite writes 32, 16 or 8)",
                     "open the file in Aseprite and save it in RGB, Grayscale or Indexed mode", "header.depth", 12);
            }
            if (snapshot.width < 1 || snapshot.height < 1) {
                fail("corrupt_chunk", "the canvas is " + std::to_string(snapshot.width) + "x" + std::to_string(snapshot.height),
                     "the file is damaged: open it in Aseprite and save it again", "header", 8);
            }
            if (snapshot.width > kMaxCanvasSide || snapshot.height > kMaxCanvasSide) {
                fail("too_large", "the canvas is " + std::to_string(snapshot.width) + "x" + std::to_string(snapshot.height) +
                     ", above the 16384x16384 limit", "crop or scale the sprite in Aseprite", "header", 8);
            }
            if (snapshot.depth != 8) transparent = 0;
            if (ncolors == 0) ncolors = 256;
            if (pw == 0 || ph == 0) pw = ph = 1;
            snapshot.transparentIndex = transparent;
            snapshot.paletteSize = ncolors;
            snapshot.pixelWidth = pw;
            snapshot.pixelHeight = ph;

            doc.width = snapshot.width;
            doc.height = snapshot.height;
            doc.mode = static_cast<ColorMode>(snapshot.depth);
            doc.headerFlags = snapshot.flags;
            doc.transparentIndex = transparent;
            doc.pixelWidth = pw;
            doc.pixelHeight = ph;
            doc.grid = {snapshot.gridX, snapshot.gridY, snapshot.gridW, snapshot.gridH};
            doc.speed = snapshot.speed;
            // Aseprite starts from a palette of `ncolors` black entries the palette chunks change
            currentPalette.assign(ncolors, PaletteEntry{});
            doc.palette = currentPalette;
            // R1: every frame gets the header speed; frames whose duration is > 0 override it
            doc.frameDurations.assign(snapshot.frames, snapshot.speed);
        }

        Document Reader::run() {
            if (options.provenance && options.pixels) {
                prov = std::make_unique<Provenance>();
            }
            readHeader();
            size_t pos = 128;
            for (int frame = 0; frame < snapshot.frames; frame++) {
                readFrame(frame, pos);
            }
            finish();
            if (prov) {
                prov->framesEnd = pos;
                prov->header = snapshot;
                // the fingerprints, from the decoded document as the writer will see it
                HashCache hashes;
                for (FrameRecord& fr : prov->frames) {
                    for (ChunkRecord& rec : fr.chunks) {
                        rec.fingerprint = fingerprintOf(doc, rec.ref, rec.type, hashes);
                    }
                }
                for (const Cel& cel : doc.cels) {
                    if (cel.image) prov->keepAlive.push_back(cel.image);
                    if (cel.tilemap) prov->keepAlive.push_back(cel.tilemap);
                }
                prov->imageHashes = std::move(hashes.computed);
                prov->bytes = std::make_shared<const std::vector<uint8_t>>(data.begin(), data.end());
                doc.provenance = std::shared_ptr<const Provenance>(prov.release());
            }
            return std::move(doc);
        }

        void Reader::readFrame(int frame, size_t& pos) {
            if (pos > data.size() || data.size() - pos < 16) {
                fail("truncated", "the file ends after " + std::to_string(frame) + " of its " + std::to_string(snapshot.frames) + " frames",
                     "the file was cut short (a save still in progress?): save it again from Aseprite", "frame " + std::to_string(frame), pos);
            }
            Cursor h(data, pos, pos + 16, 0);
            const uint32_t frameSize = h.u32();
            const uint16_t magic = h.u16();
            const uint16_t oldCount = h.u16();
            const uint16_t duration = h.u16();
            h.skip(2);
            const uint32_t newCount = h.u32();
            if (magic != kFrameMagic) {
                fail("corrupt_chunk", "frame " + std::to_string(frame) + " at byte " + std::to_string(pos) + " has the magic " + hex16(magic) +
                     " (expected 0xF1FA)", "the file is damaged: open it in Aseprite and save it again", "frame " + std::to_string(frame), pos + 4);
            }
            if (frameSize < 16 || frameSize > data.size() - pos) {
                fail(frameSize > data.size() - pos ? "truncated" : "corrupt_chunk",
                     "frame " + std::to_string(frame) + " declares " + std::to_string(frameSize) + " bytes but " +
                     std::to_string(data.size() - pos) + " are left",
                     "the file was cut short or damaged: save it again from Aseprite", "frame " + std::to_string(frame), pos);
            }
            // aseprite_decoder: the new count is used when the old one is 0xFFFF (or 0)
            uint32_t chunks = oldCount;
            if ((oldCount == 0xFFFF && oldCount < newCount) || (oldCount == 0 && newCount > 0)) chunks = newCount;
            if (duration > 0) doc.frameDurations[static_cast<size_t>(frame)] = duration;

            FrameRecord record;
            record.offset = pos;
            record.size = frameSize;
            record.rawDuration = duration;
            const size_t frameEnd = pos + frameSize;
            size_t cp = pos + 16;
            anchor = {};
            for (uint32_t i = 0; i < chunks; i++) {
                if (frameEnd - cp < 6) {
                    fail("corrupt_chunk", "frame " + std::to_string(frame) + " declares " + std::to_string(chunks) +
                         " chunks but its bytes end after " + std::to_string(i),
                         "the file is damaged: open it in Aseprite and save it again", "frame " + std::to_string(frame), cp);
                }
                Cursor ch(data, cp, frameEnd, 0);
                const uint32_t chunkSize = ch.u32();
                const uint16_t type = ch.u16();
                if (chunkSize < 6 || chunkSize > frameEnd - cp) {
                    fail("corrupt_chunk", "chunk " + hex16(type) + " at byte " + std::to_string(cp) + " declares " + std::to_string(chunkSize) +
                         " bytes, outside its frame", "the file is damaged: open it in Aseprite and save it again", chunkField(type, cp), cp);
                }
                const ChunkRef ref = readChunk(frame, type, cp, cp + chunkSize);
                if (prov) {
                    record.chunks.push_back({ref, type, cp, chunkSize, 0});
                }
                cp += chunkSize;
            }
            if (prov) prov->frames.push_back(std::move(record));
            pos = frameEnd;
        }

        ChunkRef Reader::keepOpaque(int frame, uint16_t type, size_t chunkStart, size_t chunkEnd) {
            OpaqueChunk o;
            o.type = type;
            o.frame = frame;
            o.anchor = anchor;
            o.body.assign(data.begin() + static_cast<std::ptrdiff_t>(chunkStart + 6), data.begin() + static_cast<std::ptrdiff_t>(chunkEnd));
            doc.opaque.push_back(std::move(o));
            return {ChunkRef::Kind::Opaque, static_cast<int>(doc.opaque.size()) - 1};
        }

        ChunkRef Reader::readChunk(int frame, uint16_t type, size_t chunkStart, size_t chunkEnd) {
            Cursor c(data, chunkStart + 6, chunkEnd, type);
            // tile user data chunks come right after a tileset's own one (aseprite_decoder's readTilesData)
            if (tileUserDataLeft > 0) {
                if (type == kChunkUserData) {
                    Tileset& ts = doc.tilesets[static_cast<size_t>(tileUserDataTileset)];
                    const int tile = ts.tileCount - tileUserDataLeft;
                    UserData ud;
                    readUserDataBody(c, ud);
                    if (static_cast<int>(ts.tileUserData.size()) <= tile) ts.tileUserData.resize(static_cast<size_t>(tile) + 1);
                    ts.tileUserData[static_cast<size_t>(tile)] = std::move(ud);
                    tileUserDataLeft--;
                    return userDataRef(UserDataOwner::Tile, tileUserDataTileset, tile);
                }
                tileUserDataLeft = 0;
            }

            switch (type) {
                case kChunkOldPalette:
                case kChunkOldPalette64:
                case kChunkOldPalette64Doc:
                    // R1: a 0x2019 chunk wins over the old ones (aseprite_decoder ignores old
                    // chunks once it has read a new one)
                    if (!newPaletteSeen) {
                        readOldPalette(c, type != kChunkOldPalette);
                        commitPalette(frame, chunkStart);
                    }
                    anchor = {ChunkAnchor::Kind::Palette};
                    return {ChunkRef::Kind::Palette, frame, type};
                case kChunkPalette:
                    readPalette(c);
                    commitPalette(frame, chunkStart);
                    newPaletteSeen = true;
                    anchor = {ChunkAnchor::Kind::Palette};
                    return {ChunkRef::Kind::Palette, frame, type};
                case kChunkLayer:
                    return readLayer(c, chunkStart);
                case kChunkCel:
                    return readCel(c, frame, chunkStart, chunkEnd);
                case kChunkCelExtra: {
                    if (owner.kind == Owner::Kind::Cel && owner.b == frame) {
                        readCelExtra(c, frame);
                        return {ChunkRef::Kind::CelExtra, owner.a, owner.b};
                    }
                    return keepOpaque(frame, type, chunkStart, chunkEnd);
                }
                case kChunkColorProfile:
                    if (doc.colorProfile) return keepOpaque(frame, type, chunkStart, chunkEnd);
                    readColorProfile(c);
                    return {ChunkRef::Kind::ColorProfile};
                case kChunkExternalFiles:
                    if (!doc.externalFiles.empty()) return keepOpaque(frame, type, chunkStart, chunkEnd);
                    readExternalFiles(c);
                    return {ChunkRef::Kind::ExternalFiles};
                case kChunkMask:
                    readMask(c, frame);
                    return {ChunkRef::Kind::Mask, static_cast<int>(doc.masks.size()) - 1};
                case kChunkTags: {
                    if (!doc.tags.empty()) return keepOpaque(frame, type, chunkStart, chunkEnd);
                    readTags(c);
                    owner = doc.tags.empty() ? Owner{Owner::Kind::None} : Owner{Owner::Kind::Tag, 0};
                    anchor = {ChunkAnchor::Kind::Tags};
                    return {ChunkRef::Kind::Tags};
                }
                case kChunkOldSlices: {
                    const uint32_t n = c.count(c.u32(), 20, "slices");
                    c.skip(8);
                    const int first = static_cast<int>(doc.slices.size());
                    for (uint32_t i = 0; i < n && !c.atEnd(); i++) {
                        Slice slice;
                        readSliceBody(c, slice, chunkStart);
                        doc.slices.push_back(std::move(slice));
                    }
                    // the deprecated chunk does not change what the next user data belongs to
                    return {ChunkRef::Kind::OldSlices, first, static_cast<int>(doc.slices.size()) - first};
                }
                case kChunkSlice: {
                    Slice slice;
                    readSliceBody(c, slice, chunkStart);
                    doc.slices.push_back(std::move(slice));
                    const int index = static_cast<int>(doc.slices.size()) - 1;
                    owner = {Owner::Kind::Slice, index};
                    anchor = {ChunkAnchor::Kind::Slice, index};
                    return {ChunkRef::Kind::Slice, index};
                }
                case kChunkUserData:
                    return readUserData(c, frame, chunkStart, chunkEnd);
                case kChunkTileset: {
                    readTileset(c, chunkStart);
                    const int index = static_cast<int>(doc.tilesets.size()) - 1;
                    owner = {Owner::Kind::Tileset, index};
                    anchor = {ChunkAnchor::Kind::Tileset, index};
                    return {ChunkRef::Kind::Tileset, index};
                }
                default:
                    // R5: unknown chunks and the never-used path chunk are kept as they are
                    return keepOpaque(frame, type, chunkStart, chunkEnd);
            }
        }

        void Reader::readOldPalette(Cursor& c, bool sixBit) {
            const uint16_t packets = c.u16();
            int skip = 0;
            for (uint16_t p = 0; p < packets; p++) {
                skip += c.u8();
                int size = c.u8();
                if (size == 0) size = 256;
                if (skip + size > 256) {
                    fail("corrupt_chunk", "an old palette chunk addresses entry " + std::to_string(skip + size - 1) + " (255 at most)",
                         "the file is damaged: save it again from Aseprite", chunkField(sixBit ? kChunkOldPalette64 : kChunkOldPalette, c.offset()), c.offset());
                }
                c.count(static_cast<uint32_t>(size), 3, "palette entries");
                for (int i = skip; i < skip + size; i++) {
                    uint8_t r = c.u8(), g = c.u8(), b = c.u8();
                    if (sixBit) {
                        // Aseprite's scale_6bits_to_8bits (bit replication); values above 63 are clamped
                        auto scale = [](uint8_t v) { v = std::min<uint8_t>(v, 63); return static_cast<uint8_t>((v << 2) | (v >> 4)); };
                        r = scale(r);
                        g = scale(g);
                        b = scale(b);
                    }
                    if (i >= static_cast<int>(currentPalette.size())) currentPalette.resize(static_cast<size_t>(i) + 1);
                    currentPalette[static_cast<size_t>(i)].color = {r, g, b, 255};
                }
            }
        }

        void Reader::readPalette(Cursor& c) {
            const uint32_t newSize = c.u32();
            const uint32_t from = c.u32();
            const uint32_t to = c.u32();
            c.skip(8);
            if (newSize > 65536 || (from <= to && to >= 65536)) {
                fail("corrupt_chunk", "the palette declares " + std::to_string(std::max(newSize, to + 1)) + " entries (65536 at most)",
                     "the file is damaged: save it again from Aseprite", chunkField(kChunkPalette, c.offset()), c.offset());
            }
            if (newSize > 0) currentPalette.resize(newSize);
            if (from > to) return;
            c.count(to - from + 1, 6, "palette entries");
            if (to >= currentPalette.size()) currentPalette.resize(static_cast<size_t>(to) + 1);
            for (uint32_t i = from; i <= to; i++) {
                const uint16_t flags = c.u16();
                PaletteEntry& e = currentPalette[i];
                e.color = {c.u8(), c.u8(), c.u8(), c.u8()};
                if (flags & 1) {
                    e.name = c.string();
                } else {
                    e.name.reset();
                }
            }
        }

        void Reader::commitPalette(int frame, size_t offset) {
            // every palette kept is a copy of the whole palette (a later frame's change too): R6's
            // budget counts them, so a file of many one-entry changes to a 65,536-entry palette
            // stops with too_large instead of asking for gigabytes
            uint64_t bytes = static_cast<uint64_t>(currentPalette.size()) * sizeof(PaletteEntry);
            for (const PaletteEntry& e : currentPalette) {
                if (e.name) bytes += e.name->size();
            }
            if (frame != 0 && currentPalette == doc.paletteAt(frame)) return;
            paletteBytes += bytes;
            if (paletteBytes > kMaxPaletteBytes) {
                fail("too_large", "the sprite's palettes (one per frame where it changes) take more than 64 MiB",
                     "use fewer palette changes, or a smaller palette", chunkField(kChunkPalette, offset), offset);
            }
            budget(bytes, offset);
            if (frame == 0) {
                doc.palette = currentPalette;
            } else {
                doc.paletteChanges[frame] = currentPalette;
            }
        }

        ChunkRef Reader::readLayer(Cursor& c, size_t chunkStart) {
            Layer layer;
            const uint16_t flags = c.u16();
            const uint16_t type = c.u16();
            const uint16_t childLevel = c.u16();
            c.u16();
            c.u16();
            const uint16_t blend = c.u16();
            const uint8_t opacity = c.u8();
            c.skip(3);
            layer.name = c.string();
            layer.type = static_cast<LayerType>(type);
            if (layer.type == LayerType::Tilemap) layer.tilesetIndex = c.u32();
            if (doc.headerFlags & kHeaderLayerUuids) {
                Uuid uuid;
                std::span<const uint8_t> b = c.bytes(16);
                std::copy(b.begin(), b.end(), uuid.bytes.begin());
                layer.uuid = uuid;
            }
            layer.flags = flags;
            layer.childLevel = childLevel;
            // NOTE.6 and aseprite_decoder: blend and opacity hold for image and tilemap layers,
            // for groups only with header flag 2, never for the background; opacity only with flag 1
            const bool isImage = layer.type == LayerType::Image || layer.type == LayerType::Tilemap;
            const bool composeGroups = (doc.headerFlags & kHeaderGroupBlend) != 0;
            if ((isImage || (layer.type == LayerType::Group && composeGroups)) && !(flags & kLayerBackground)) {
                layer.blend = static_cast<BlendMode>(blend);
                layer.opacity = (doc.headerFlags & kHeaderLayerOpacity) ? opacity : 255;
                if (blend > static_cast<uint16_t>(BlendMode::Divide)) {
                    warn("corrupt_chunk", "layer '" + layer.name + "' has the unknown blend mode " + std::to_string(blend) + " (drawn as normal)",
                         "pick a blend mode in Aseprite and save again", chunkField(kChunkLayer, chunkStart), chunkStart);
                }
            }
            if (type > static_cast<uint16_t>(LayerType::Tilemap)) {
                warn("corrupt_chunk", "layer '" + layer.name + "' has the unknown type " + std::to_string(type) + " (not drawn)",
                     "the file comes from a newer Aseprite: open it there and save it again", chunkField(kChunkLayer, chunkStart), chunkStart);
            }
            doc.layers.push_back(std::move(layer));
            const int index = static_cast<int>(doc.layers.size()) - 1;
            owner = {Owner::Kind::Layer, index};
            anchor = {ChunkAnchor::Kind::Layer, index};
            return {ChunkRef::Kind::Layer, index};
        }

        std::vector<uint8_t> Reader::inflateInto(Cursor& c, size_t expected, size_t chunkStart, uint16_t type) {
            std::span<const uint8_t> compressed = c.bytes(c.remaining());
            // zlib expands at most ~1032:1, so a claim beyond that is corrupt before anything is
            // allocated for it: a small hostile file cannot ask for gigabytes
            if (expected / 1032 > compressed.size() + 64) {
                fail("corrupt_chunk", "chunk " + hex16(type) + " at byte " + std::to_string(chunkStart) + " claims " + std::to_string(expected) +
                     " pixel bytes from " + std::to_string(compressed.size()) + " compressed ones",
                     "the file is damaged: open it in Aseprite and save it again", chunkField(type, chunkStart), chunkStart);
            }
            std::vector<uint8_t> raw(expected);
            size_t produced = 0;
            std::string error;
            if (!inflateBounded(compressed, raw.data(), expected, produced, error)) {
                fail("corrupt_chunk", "chunk " + hex16(type) + " at byte " + std::to_string(chunkStart) + " holds damaged pixels (" + error + ")",
                     "the file is damaged: open it in Aseprite and save it again", chunkField(type, chunkStart), chunkStart);
            }
            if (produced < expected) {
                warn("corrupt_chunk", "chunk " + hex16(type) + " at byte " + std::to_string(chunkStart) + " holds " + std::to_string(produced) +
                     " of its " + std::to_string(expected) + " pixel bytes (the rest is transparent)",
                     "save the file again from Aseprite", chunkField(type, chunkStart), chunkStart);
            }
            return raw;
        }

        ChunkRef Reader::readCel(Cursor& c, int frame, size_t chunkStart, size_t chunkEnd) {
            Cel cel;
            const uint16_t layerIndex = c.u16();
            cel.layer = layerIndex;
            cel.frame = frame;
            cel.x = c.i16();
            cel.y = c.i16();
            cel.opacity = c.u8();
            const uint16_t celType = c.u16();
            cel.zIndex = c.i16();
            c.skip(5);

            if (layerIndex >= doc.layers.size() || doc.layers[layerIndex].isGroup() ||
                static_cast<uint16_t>(doc.layers[layerIndex].type) > static_cast<uint16_t>(LayerType::Tilemap)) {
                warn("corrupt_chunk", "frame " + std::to_string(frame) + " has a cel on layer " + std::to_string(layerIndex) +
                     (layerIndex >= doc.layers.size() ? ", which does not exist" : ", which holds no pixels") + " (kept, not drawn)",
                     "open the file in Aseprite and save it again", chunkField(kChunkCel, chunkStart), chunkStart);
                owner = {Owner::Kind::None};
                return keepOpaque(frame, kChunkCel, chunkStart, chunkEnd);
            }
            if (doc.celRecord(cel.layer, frame)) {
                warn("corrupt_chunk", "frame " + std::to_string(frame) + " has two cels on layer '" + doc.layers[layerIndex].name + "' (the second is kept, not drawn)",
                     "open the file in Aseprite and save it again", chunkField(kChunkCel, chunkStart), chunkStart);
                owner = {Owner::Kind::None};
                return keepOpaque(frame, kChunkCel, chunkStart, chunkEnd);
            }
            const Layer& layer = doc.layers[layerIndex];
            const bool tilemapLayer = layer.type == LayerType::Tilemap;

            switch (celType) {
                case 0:
                case 2: {
                    const uint16_t w = c.u16();
                    const uint16_t h = c.u16();
                    if (tilemapLayer) {
                        warn("corrupt_chunk", "the tilemap layer '" + layer.name + "' has an image cel on frame " + std::to_string(frame),
                             "open the file in Aseprite and save it again", chunkField(kChunkCel, chunkStart), chunkStart);
                    }
                    if (!options.pixels) break;
                    const size_t bpp = static_cast<size_t>(bytesPerPixel(doc.mode));
                    const size_t expected = static_cast<size_t>(w) * h * bpp;
                    budget(static_cast<uint64_t>(w) * h * 4 + expected, chunkStart);
                    std::vector<uint8_t> raw;
                    if (celType == 0) {
                        std::span<const uint8_t> b = c.bytes(std::min(expected, c.remaining()));
                        raw.assign(b.begin(), b.end());
                        if (raw.size() < expected) {
                            warn("corrupt_chunk", "the raw cel at byte " + std::to_string(chunkStart) + " is shorter than its pixels (the rest is transparent)",
                                 "save the file again from Aseprite", chunkField(kChunkCel, chunkStart), chunkStart);
                            raw.resize(expected, 0);
                        }
                    } else {
                        // checked now (a claim beyond zlib's ratio allocates nothing), inflated
                        // in finish() with the other cels
                        std::span<const uint8_t> compressed = c.bytes(c.remaining());
                        if (expected / 1032 > compressed.size() + 64) {
                            fail("corrupt_chunk", "chunk " + hex16(kChunkCel) + " at byte " + std::to_string(chunkStart) + " claims " +
                                 std::to_string(expected) + " pixel bytes from " + std::to_string(compressed.size()) + " compressed ones",
                                 "the file is damaged: open it in Aseprite and save it again", chunkField(kChunkCel, chunkStart), chunkStart);
                        }
                        auto image = std::make_shared<PixelImage>();
                        deferred.push_back({image, compressed, expected, w, h, chunkStart, 0, std::string()});
                        if (doc.mode == ColorMode::Indexed) pending.push_back({image, cel.layer, frame});
                        cel.image = image;
                        break;
                    }
                    auto image = std::make_shared<PixelImage>(
                        imageFromFileBytes(w, h, doc.mode, raw, std::span<const uint8_t>(), -1));
                    if (doc.mode == ColorMode::Indexed) pending.push_back({image, cel.layer, frame});
                    cel.image = image;
                    break;
                }
                case 1: {
                    cel.linkedFrame = c.u16();
                    const Cel* target = doc.celRecord(cel.layer, cel.linkedFrame);
                    if (!target || cel.linkedFrame >= frame) {
                        warn("missing_link", "the cel of layer '" + layer.name + "' on frame " + std::to_string(frame) + " links to frame " +
                             std::to_string(cel.linkedFrame) + ", which has no cel there (an empty cel)",
                             "unlink the cel in Aseprite and save again", chunkField(kChunkCel, chunkStart), chunkStart);
                    } else {
                        // links share the image they point at (a link to a link points at its target)
                        const Cel* resolved = doc.cel(cel.layer, cel.linkedFrame);
                        if (resolved) {
                            cel.image = resolved->image;
                            cel.tilemap = resolved->tilemap;
                        }
                    }
                    break;
                }
                case 3: {
                    const uint16_t w = c.u16();
                    const uint16_t h = c.u16();
                    auto tm = std::make_shared<Tilemap>();
                    tm->columns = w;
                    tm->rows = h;
                    tm->bitsPerTile = c.u16();
                    tm->idMask = c.u32();
                    tm->xFlipMask = c.u32();
                    tm->yFlipMask = c.u32();
                    tm->diagonalMask = c.u32();
                    c.skip(10);
                    if (!tilemapLayer) {
                        warn("corrupt_chunk", "the image layer '" + layer.name + "' has a tilemap cel on frame " + std::to_string(frame),
                             "open the file in Aseprite and save it again", chunkField(kChunkCel, chunkStart), chunkStart);
                    }
                    if (tm->bitsPerTile != 8 && tm->bitsPerTile != 16 && tm->bitsPerTile != 32) {
                        fail("corrupt_chunk", "a tilemap cel uses " + std::to_string(tm->bitsPerTile) + " bits per tile (8, 16 or 32 are valid)",
                             "open the file in Aseprite and save it again", chunkField(kChunkCel, chunkStart), chunkStart);
                    }
                    if (!options.pixels) break;
                    const size_t tileBytes = tm->bitsPerTile / 8;
                    const size_t expected = static_cast<size_t>(w) * h * tileBytes;
                    budget(static_cast<uint64_t>(w) * h * 4, chunkStart);
                    std::vector<uint8_t> raw = inflateInto(c, expected, chunkStart, kChunkCel);
                    tm->tiles.resize(static_cast<size_t>(w) * h);
                    for (size_t i = 0; i < tm->tiles.size(); i++) {
                        uint32_t v = 0;
                        for (size_t b = 0; b < tileBytes; b++) v |= static_cast<uint32_t>(raw[i * tileBytes + b]) << (8 * b);
                        tm->tiles[i] = v;
                    }
                    cel.tilemap = tm;
                    break;
                }
                default:
                    warn("corrupt_chunk", "a cel has the unknown type " + std::to_string(celType) + " (kept, not drawn)",
                         "the file comes from a newer Aseprite: open it there and save it again", chunkField(kChunkCel, chunkStart), chunkStart);
                    owner = {Owner::Kind::None};
                    return keepOpaque(frame, kChunkCel, chunkStart, chunkEnd);
            }

            if (!options.pixels) {
                owner = {Owner::Kind::None};
                return {ChunkRef::Kind::Cel, cel.layer, frame};
            }
            doc.cels.push_back(std::move(cel));
            const Cel& stored = doc.cels.back();
            owner = {Owner::Kind::Cel, stored.layer, frame};
            anchor = {ChunkAnchor::Kind::Cel, stored.layer, frame};
            return {ChunkRef::Kind::Cel, stored.layer, frame};
        }

        void Reader::readCelExtra(Cursor& c, int frame) {
            Cel* cel = doc.celRecord(owner.a, frame);
            if (!cel) return;
            CelExtra extra;
            extra.flags = c.u32();
            extra.x = c.i32();
            extra.y = c.i32();
            extra.w = c.i32();
            extra.h = c.i32();
            cel->extra = extra;
        }

        void Reader::readColorProfile(Cursor& c) {
            ColorProfile p;
            p.type = c.u16();
            p.flags = c.u16();
            p.gamma = c.i32();
            c.skip(8);
            if (p.type == 2) {
                const uint32_t length = c.count(c.u32(), 1, "ICC bytes");
                std::span<const uint8_t> b = c.bytes(length);
                p.icc.assign(b.begin(), b.end());
            }
            if (p.type > 2) {
                warn("corrupt_chunk", "the colour profile has the unknown type " + std::to_string(p.type) + " (pixels used as sRGB)",
                     "set the colour profile to sRGB in Aseprite", chunkField(kChunkColorProfile, c.offset()), c.offset());
            }
            doc.colorProfile = std::move(p);
        }

        void Reader::readExternalFiles(Cursor& c) {
            const uint32_t n = c.count(c.u32(), 14, "external files");
            c.skip(8);
            for (uint32_t i = 0; i < n; i++) {
                ExternalFile f;
                f.id = c.u32();
                f.type = c.u8();
                c.skip(7);
                f.name = c.string();
                doc.externalFiles.push_back(std::move(f));
            }
        }

        void Reader::readMask(Cursor& c, int frame) {
            Mask m;
            m.frame = frame;
            m.x = c.i16();
            m.y = c.i16();
            m.w = c.u16();
            m.h = c.u16();
            c.skip(8);
            m.name = c.string();
            const size_t bytes = static_cast<size_t>(m.h) * ((static_cast<size_t>(m.w) + 7) / 8);
            std::span<const uint8_t> b = c.bytes(std::min(bytes, c.remaining()));
            m.bitmap.assign(b.begin(), b.end());
            m.bitmap.resize(bytes, 0);
            doc.masks.push_back(std::move(m));
        }

        void Reader::readTags(Cursor& c) {
            const uint16_t n = c.u16();
            c.skip(8);
            for (uint16_t i = 0; i < n && !c.atEnd(); i++) {
                Tag tag;
                tag.from = c.u16();
                tag.to = c.u16();
                uint8_t dir = c.u8();
                if (dir > 3) {
                    warn("corrupt_chunk", "a tag has the unknown direction " + std::to_string(dir) + " (read as forward)",
                         "set the tag's direction in Aseprite", chunkField(kChunkTags, c.offset()), c.offset());
                    dir = 0;
                }
                tag.direction = static_cast<TagDirection>(dir);
                tag.repeat = c.u16();
                c.skip(6);
                const uint8_t r = c.u8(), g = c.u8(), b = c.u8();
                c.u8();
                tag.legacyColor = {r, g, b, 255};
                tag.name = c.string();
                if (tag.from > tag.to || tag.to >= doc.frameCount()) {
                    warn("corrupt_chunk", "the tag '" + tag.name + "' spans frames " + std::to_string(tag.from) + "-" + std::to_string(tag.to) +
                         " of a " + std::to_string(doc.frameCount()) + "-frame sprite", "fix the tag's range in Aseprite",
                         chunkField(kChunkTags, c.offset()), c.offset());
                }
                doc.tags.push_back(std::move(tag));
            }
        }

        PropertyValue Reader::readValue(Cursor& c, uint16_t type, int depth) {
            if (depth > kMaxUserDataDepth) {
                fail("corrupt_chunk", "user data properties nest deeper than 128 levels", "flatten the properties in Aseprite",
                     chunkField(kChunkUserData, c.offset()), c.offset());
            }
            PropertyValue v;
            v.type = static_cast<PropertyType>(type);
            switch (v.type) {
                case PropertyType::Bool: v.value = c.u8() != 0; break;
                case PropertyType::Int8: v.value = static_cast<int64_t>(static_cast<int8_t>(c.u8())); break;
                case PropertyType::Uint8: v.value = static_cast<uint64_t>(c.u8()); break;
                case PropertyType::Int16: v.value = static_cast<int64_t>(c.i16()); break;
                case PropertyType::Uint16: v.value = static_cast<uint64_t>(c.u16()); break;
                case PropertyType::Int32: v.value = static_cast<int64_t>(c.i32()); break;
                case PropertyType::Uint32: v.value = static_cast<uint64_t>(c.u32()); break;
                case PropertyType::Int64: v.value = static_cast<int64_t>(c.u64()); break;
                case PropertyType::Uint64: v.value = c.u64(); break;
                case PropertyType::Fixed: v.value = static_cast<double>(c.i32()) / 65536.0; break;
                case PropertyType::Float: {
                    const uint32_t bits = c.u32();
                    float f;
                    std::memcpy(&f, &bits, 4);
                    v.value = static_cast<double>(f);
                    break;
                }
                case PropertyType::Double: {
                    const uint64_t bits = c.u64();
                    double d;
                    std::memcpy(&d, &bits, 8);
                    v.value = d;
                    break;
                }
                case PropertyType::String: v.value = c.string(); break;
                case PropertyType::Point: {
                    IntPoint p;
                    p.x = c.i32();
                    p.y = c.i32();
                    v.value = p;
                    break;
                }
                case PropertyType::Size: {
                    IntSize s;
                    s.w = c.i32();
                    s.h = c.i32();
                    v.value = s;
                    break;
                }
                case PropertyType::Rect: {
                    IntRect r;
                    r.x = c.i32();
                    r.y = c.i32();
                    r.w = c.i32();
                    r.h = c.i32();
                    v.value = r;
                    break;
                }
                case PropertyType::Vector: {
                    const uint32_t n = c.u32();
                    const uint16_t elementType = c.u16();
                    v.vectorElementType = elementType;
                    // every element takes at least one byte (two with its own type)
                    c.count(n, elementType == 0 ? 3 : 1, "vector elements");
                    PropertyVector elements;
                    elements.reserve(n);
                    for (uint32_t i = 0; i < n; i++) {
                        const uint16_t t = elementType == 0 ? c.u16() : elementType;
                        elements.push_back(readValue(c, t, depth + 1));
                    }
                    v.value = std::move(elements);
                    break;
                }
                case PropertyType::Map: v.value = readMap(c, depth + 1); break;
                case PropertyType::Uuid: {
                    Uuid u;
                    std::span<const uint8_t> b = c.bytes(16);
                    std::copy(b.begin(), b.end(), u.bytes.begin());
                    v.value = u;
                    break;
                }
                default:
                    fail("corrupt_chunk", "a user data property has the unknown type " + hex16(type),
                         "the file comes from a newer Aseprite: open it there and save it again", chunkField(kChunkUserData, c.offset()), c.offset());
            }
            return v;
        }

        PropertyMap Reader::readMap(Cursor& c, int depth) {
            if (depth > kMaxUserDataDepth) {
                fail("corrupt_chunk", "user data properties nest deeper than 128 levels", "flatten the properties in Aseprite",
                     chunkField(kChunkUserData, c.offset()), c.offset());
            }
            const uint32_t n = c.count(c.u32(), 4, "properties");
            PropertyMap map;
            map.reserve(n);
            for (uint32_t i = 0; i < n; i++) {
                Property p;
                p.name = c.string();
                const uint16_t type = c.u16();
                p.value = readValue(c, type, depth + 1);
                map.push_back(std::move(p));
            }
            return map;
        }

        void Reader::readUserDataBody(Cursor& c, UserData& out) {
            const uint32_t flags = c.u32();
            if (flags & 1) out.text = c.string();
            if (flags & 2) out.color = PixelRgba{c.u8(), c.u8(), c.u8(), c.u8()};
            if (flags & 4) {
                const size_t start = c.offset();
                const uint32_t size = c.u32();
                if (size < 8 || size - 4 > c.remaining()) {
                    fail("corrupt_chunk", "the user data properties declare " + std::to_string(size) + " bytes, more than the chunk holds",
                         "the file is damaged: open it in Aseprite and save it again", chunkField(kChunkUserData, start), start);
                }
                const uint32_t maps = c.count(c.u32(), 8, "property maps");
                for (uint32_t i = 0; i < maps; i++) {
                    const uint32_t key = c.u32();
                    out.properties[key] = readMap(c, 0);
                }
            }
        }

        ChunkRef Reader::readUserData(Cursor& c, int frame, size_t chunkStart, size_t chunkEnd) {
            UserData ud;
            const Owner target = owner;
            switch (target.kind) {
                case Owner::Kind::Sprite:
                    // the sprite stays the owner (aseprite_decoder): a second chunk replaces the first
                    doc.spriteUserData = UserData();
                    readUserDataBody(c, doc.spriteUserData);
                    return userDataRef(UserDataOwner::Sprite);
                case Owner::Kind::Layer:
                    doc.layers[static_cast<size_t>(target.a)].userData = UserData();
                    readUserDataBody(c, doc.layers[static_cast<size_t>(target.a)].userData);
                    return userDataRef(UserDataOwner::Layer, target.a);
                case Owner::Kind::Cel: {
                    // a link's user data stays on the link record (Aseprite writes none for links)
                    readUserDataBody(c, ud);
                    if (Cel* cel = doc.celRecord(target.a, target.b)) cel->userData = std::move(ud);
                    return userDataRef(UserDataOwner::Cel, target.a, target.b);
                }
                case Owner::Kind::Tag: {
                    Tag& tag = doc.tags[static_cast<size_t>(target.a)];
                    tag.userData = UserData();
                    readUserDataBody(c, tag.userData);
                    const int next = target.a + 1;
                    owner = next < static_cast<int>(doc.tags.size()) ? Owner{Owner::Kind::Tag, next} : Owner{Owner::Kind::None};
                    return userDataRef(UserDataOwner::Tag, target.a);
                }
                case Owner::Kind::Slice:
                    doc.slices[static_cast<size_t>(target.a)].userData = UserData();
                    readUserDataBody(c, doc.slices[static_cast<size_t>(target.a)].userData);
                    return userDataRef(UserDataOwner::Slice, target.a);
                case Owner::Kind::Tileset: {
                    Tileset& ts = doc.tilesets[static_cast<size_t>(target.a)];
                    ts.userData = UserData();
                    readUserDataBody(c, ts.userData);
                    tileUserDataLeft = ts.tileCount;
                    tileUserDataTileset = target.a;
                    owner = {Owner::Kind::None};
                    return userDataRef(UserDataOwner::Tileset, target.a);
                }
                case Owner::Kind::None:
                    break;
            }
            // no owner (after a cel that could not be read, after the last tag): kept as it is
            return keepOpaque(frame, kChunkUserData, chunkStart, chunkEnd);
        }

        void Reader::readSliceBody(Cursor& c, Slice& slice, size_t chunkStart) {
            const uint32_t keys = c.u32();
            slice.flags = c.u32();
            c.u32();
            slice.name = c.string();
            const size_t keyBytes = 20 + ((slice.flags & 1) ? 16 : 0) + ((slice.flags & 2) ? 8 : 0);
            c.count(keys, keyBytes, "slice keys");
            for (uint32_t k = 0; k < keys; k++) {
                SliceKey key;
                key.frame = c.u32();
                key.bounds.x = c.i32();
                key.bounds.y = c.i32();
                key.bounds.w = static_cast<int>(std::min<uint32_t>(c.u32(), INT32_MAX));
                key.bounds.h = static_cast<int>(std::min<uint32_t>(c.u32(), INT32_MAX));
                if (slice.flags & 1) {
                    IntRect center;
                    center.x = c.i32();
                    center.y = c.i32();
                    center.w = static_cast<int>(std::min<uint32_t>(c.u32(), INT32_MAX));
                    center.h = static_cast<int>(std::min<uint32_t>(c.u32(), INT32_MAX));
                    key.center = center;
                }
                if (slice.flags & 2) {
                    IntPoint pivot;
                    pivot.x = c.i32();
                    pivot.y = c.i32();
                    key.pivot = pivot;
                }
                if (key.frame >= static_cast<uint32_t>(doc.frameCount())) {
                    warn("slice_key_past_end", "the slice '" + slice.name + "' has a key on frame " + std::to_string(key.frame) + " of a " +
                         std::to_string(doc.frameCount()) + "-frame sprite (it never shows)", "remove the key in Aseprite",
                         chunkField(kChunkSlice, chunkStart), chunkStart);
                }
                slice.keys.push_back(key);
            }
        }

        void Reader::readTileset(Cursor& c, size_t chunkStart) {
            Tileset ts;
            ts.id = c.u32();
            ts.flags = c.u32();
            const uint32_t count = c.u32();
            ts.tileWidth = c.u16();
            ts.tileHeight = c.u16();
            ts.baseIndex = c.i16();
            c.skip(14);
            ts.name = c.string();
            if (count > 0x7fffffffu || ts.tileWidth < 1 || ts.tileHeight < 1) {
                fail("corrupt_chunk", "the tileset '" + ts.name + "' declares " + std::to_string(count) + " tiles of " + std::to_string(ts.tileWidth) +
                     "x" + std::to_string(ts.tileHeight), "the file is damaged: open it in Aseprite and save it again",
                     chunkField(kChunkTileset, chunkStart), chunkStart);
            }
            ts.tileCount = static_cast<int>(count);
            if (ts.flags & 1) {
                ExternalTileset ext;
                ext.fileId = c.u32();
                ext.tilesetId = c.u32();
                ts.external = ext;
            }
            if ((ts.flags & 2) && count > 0) {
                const uint32_t length = c.u32();
                if (length > c.remaining()) {
                    fail("corrupt_chunk", "the tileset '" + ts.name + "' declares " + std::to_string(length) + " compressed bytes, more than its chunk holds",
                         "the file is damaged: open it in Aseprite and save it again", chunkField(kChunkTileset, chunkStart), chunkStart);
                }
                const uint64_t height = static_cast<uint64_t>(ts.tileHeight) * count;
                if (height > static_cast<uint64_t>(PixelImage::kMaxSide)) {
                    fail("too_large", "the tileset '" + ts.name + "' holds " + std::to_string(count) + " tiles, a strip taller than 65535 pixels",
                         "split the tileset in Aseprite", chunkField(kChunkTileset, chunkStart), chunkStart);
                }
                if (options.pixels) {
                    const size_t bpp = static_cast<size_t>(bytesPerPixel(doc.mode));
                    const size_t expected = static_cast<size_t>(ts.tileWidth) * height * bpp;
                    budget(static_cast<uint64_t>(ts.tileWidth) * height * 4 + expected, chunkStart);
                    Cursor sub(data, c.offset(), c.offset() + length, kChunkTileset);
                    std::vector<uint8_t> raw = inflateInto(sub, expected, chunkStart, kChunkTileset);
                    // tileset pixels keep their indices; transparent index clear (palette applied at finish)
                    ts.tiles = imageFromFileBytes(ts.tileWidth, static_cast<int>(height), doc.mode, raw, std::span<const uint8_t>(), -1);
                }
                c.skip(length);
            }
            doc.tilesets.push_back(std::move(ts));
        }

        void Reader::inflateDeferred() {
            if (deferred.empty()) return;
            size_t total = 0;
            for (const DeferredCel& d : deferred) total += d.expected;
            // threads pay off on big files only
            const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
            const unsigned threads = total < (8u << 20) ? 1u : std::min({hardware, 8u, static_cast<unsigned>(deferred.size())});
            std::atomic<size_t> next{0};
            const ColorMode mode = doc.mode;
            auto work = [this, &next, mode]() {
                for (;;) {
                    const size_t i = next.fetch_add(1);
                    if (i >= deferred.size()) return;
                    DeferredCel& d = deferred[i];
                    try {
                        std::vector<uint8_t> raw(d.expected);
                        std::string error;
                        if (!inflateBounded(d.compressed, raw.data(), d.expected, d.produced, error)) {
                            d.error = error.empty() ? std::string("not a zlib stream") : error;
                            continue;
                        }
                        *d.image = imageFromFileBytes(d.w, d.h, mode, raw, std::span<const uint8_t>(), -1);
                    } catch (const std::bad_alloc&) {
                        d.error = "out of memory";
                    }
                }
            };
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < threads; t++) pool.emplace_back(work);
            work();
            for (std::thread& t : pool) t.join();
            // what went wrong, in the file's order
            for (const DeferredCel& d : deferred) {
                if (!d.error.empty()) {
                    fail("corrupt_chunk", "chunk " + hex16(kChunkCel) + " at byte " + std::to_string(d.chunkStart) + " holds damaged pixels (" + d.error + ")",
                         "the file is damaged: open it in Aseprite and save it again", chunkField(kChunkCel, d.chunkStart), d.chunkStart);
                }
                if (d.produced < d.expected) {
                    warn("corrupt_chunk", "chunk " + hex16(kChunkCel) + " at byte " + std::to_string(d.chunkStart) + " holds " + std::to_string(d.produced) +
                         " of its " + std::to_string(d.expected) + " pixel bytes (the rest is transparent)",
                         "save the file again from Aseprite", chunkField(kChunkCel, d.chunkStart), d.chunkStart);
                }
            }
            deferred.clear();
        }

        void Reader::finish() {
            inflateDeferred();
            // parents from the child levels (levels that jump are normalized; an unchanged layer
            // still writes back its original chunk)
            doc.rebuildTreeFromLevels();
            doc.sortCels();
            if (doc.mode != ColorMode::Indexed) return;
            // indexed pixels: the RGBA is the palette lookup of the cel's frame, the transparent
            // index clear except on the background layer
            for (PendingImage& p : pending) {
                const std::vector<uint8_t> rgba = doc.paletteRgba(p.frame);
                const bool background = p.layer < static_cast<int>(doc.layers.size()) && doc.layers[static_cast<size_t>(p.layer)].background();
                p.image->repaint(rgba, background ? -1 : doc.transparentIndex);
            }
            const std::vector<uint8_t> rgba = doc.paletteRgba(0);
            for (Tileset& ts : doc.tilesets) {
                if (!ts.tiles.empty()) ts.tiles.repaint(rgba, doc.transparentIndex);
            }
        }

    }

    ReadResult read(std::span<const uint8_t> bytes, const ReadOptions& options) {
        ReadResult result;
        try {
            Reader reader(bytes, options, result.warnings);
            result.document = reader.run();
        } catch (const detail::Failure& f) {
            result.document.reset();
            result.error = f.error;
        } catch (const std::bad_alloc&) {
            result.document.reset();
            result.error.kind = "too_large";
            result.error.message = "the sprite needs more memory than is available";
            result.error.fix = "split the file (fewer frames or layers) or crop the canvas";
        } catch (const std::length_error&) {
            result.document.reset();
            result.error.kind = "too_large";
            result.error.message = "the sprite needs more memory than is available";
            result.error.fix = "split the file (fewer frames or layers) or crop the canvas";
        }
        return result;
    }

    ReadResult readFile(const std::filesystem::path& path, const ReadOptions& options) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            ReadResult result;
            result.error.kind = "import_failed";
            result.error.message = "cannot open " + path.generic_string();
            result.error.fix = "check that the file exists and can be read";
            result.error.field = "path";
            return result;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return read(bytes, options);
    }

}
