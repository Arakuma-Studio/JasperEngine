// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The `.aseprite` / `.ase` codec (docs/specs/S44-aseprite-import.md §4.1, R1-R9): the document
// model, the reader, the writer, the composition and the JSON description. Editor-only, with no
// dependency on Project, ImGui or the GPU: the importer runs it on jobs, the CLI without a
// project, and S46/S47 edit documents with it.
//
// Reading keeps everything (every chunk of the published format, unknown chunks as opaque bytes
// anchored to their frame and the object read before them) and remembers each chunk's original
// bytes with a fingerprint of what it decoded to. Writing re-encodes only the chunks whose
// meaning changed, so write(read(f)) == f byte for byte, and an edit changes only the chunks (and
// frame headers) it touched. Code that edits a document changes the plain structs below and never
// manages bytes; Provenance is the writer's.
//
// Errors are typed (CodecError: {kind, message, fix, field} plus the byte offset): not_aseprite,
// truncated, corrupt_chunk, too_large, unsupported_depth, invalid_document. Recoverable problems
// (a link to a missing cel, a slice key past the last frame) are warnings.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"

#include "json.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace jasperengine::editor::pixelart::aseprite {

    // the limits of R6: a file asking for more is refused with too_large
    inline constexpr int kMaxCanvasSide = 16384;
    inline constexpr int kMaxFrames = 65535;
    inline constexpr uint64_t kMaxDecodedPixelBytes = 1ull << 30;   // 1 GiB of decoded pixels
    inline constexpr uint64_t kMaxPaletteBytes = 64ull << 20;       // the palettes kept (frame 0 and each change)
    inline constexpr int kMaxUserDataDepth = 128;

    enum class ColorMode : uint16_t { Rgba = 32, Grayscale = 16, Indexed = 8 };
    enum class LayerType : uint16_t { Image = 0, Group = 1, Tilemap = 2 };
    enum class BlendMode : uint16_t {
        Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, HardLight, SoftLight,
        Difference, Exclusion, Hue, Saturation, Color, Luminosity, Addition, Subtract, Divide
    };   // 0..18, the file values
    enum class TagDirection : uint8_t { Forward, Reverse, PingPong, PingPongReverse };

    // header flags
    inline constexpr uint32_t kHeaderLayerOpacity = 1;
    inline constexpr uint32_t kHeaderGroupBlend = 2;
    inline constexpr uint32_t kHeaderLayerUuids = 4;

    // layer flags
    inline constexpr uint16_t kLayerVisible = 1;
    inline constexpr uint16_t kLayerEditable = 2;
    inline constexpr uint16_t kLayerLockMovement = 4;
    inline constexpr uint16_t kLayerBackground = 8;
    inline constexpr uint16_t kLayerPreferLinked = 16;
    inline constexpr uint16_t kLayerCollapsed = 32;
    inline constexpr uint16_t kLayerReference = 64;

    // chunk types
    inline constexpr uint16_t kChunkOldPalette = 0x0004;          // 8-bit components
    inline constexpr uint16_t kChunkOldPalette64 = 0x000B;        // 6-bit components (FLI_COLOR, what Aseprite reads)
    inline constexpr uint16_t kChunkOldPalette64Doc = 0x0011;     // the same, as the published spec numbers it
    inline constexpr uint16_t kChunkLayer = 0x2004;
    inline constexpr uint16_t kChunkCel = 0x2005;
    inline constexpr uint16_t kChunkCelExtra = 0x2006;
    inline constexpr uint16_t kChunkColorProfile = 0x2007;
    inline constexpr uint16_t kChunkExternalFiles = 0x2008;
    inline constexpr uint16_t kChunkMask = 0x2016;
    inline constexpr uint16_t kChunkPath = 0x2017;
    inline constexpr uint16_t kChunkTags = 0x2018;
    inline constexpr uint16_t kChunkPalette = 0x2019;
    inline constexpr uint16_t kChunkUserData = 0x2020;
    inline constexpr uint16_t kChunkOldSlices = 0x2021;           // deprecated multi-slice chunk
    inline constexpr uint16_t kChunkSlice = 0x2022;
    inline constexpr uint16_t kChunkTileset = 0x2023;

    const char* blendModeName(BlendMode mode);         // "normal", "multiply", ... "divide"
    bool parseBlendMode(std::string_view text, BlendMode& out);
    const char* tagDirectionName(TagDirection dir);    // "forward", "reverse", "pingpong", "pingpong_reverse"
    const char* colorModeName(ColorMode mode);         // "rgba", "grayscale", "indexed"

    struct Uuid {
        std::array<uint8_t, 16> bytes{};
        bool operator==(const Uuid& o) const { return bytes == o.bytes; }
        std::string toString() const;   // 8-4-4-4-12 lowercase hex
    };

    // ---- user data (R4): text, colour and property maps with Aseprite's 19 property types

    enum class PropertyType : uint16_t {
        Bool = 0x01, Int8 = 0x02, Uint8 = 0x03, Int16 = 0x04, Uint16 = 0x05, Int32 = 0x06, Uint32 = 0x07,
        Int64 = 0x08, Uint64 = 0x09, Fixed = 0x0A, Float = 0x0B, Double = 0x0C, String = 0x0D, Point = 0x0E,
        Size = 0x0F, Rect = 0x10, Vector = 0x11, Map = 0x12, Uuid = 0x13
    };
    const char* propertyTypeName(PropertyType type);   // "bool", "int8", ... "uuid"

    struct PropertyValue;
    struct Property;
    using PropertyVector = std::vector<PropertyValue>;
    using PropertyMap = std::vector<Property>;   // in file order; names unique

    // One value. `type` is the file's type code, so an int16 stays an int16 on write; `value`
    // holds every integer as int64_t (signed types) or uint64_t (unsigned ones), Fixed/Float/
    // Double as double (Fixed: the 16.16 value / 65536, exact).
    struct PropertyValue {
        PropertyType type = PropertyType::Bool;
        std::variant<bool, int64_t, uint64_t, double, std::string, IntPoint, IntSize, IntRect, Uuid, PropertyVector, PropertyMap> value;
        // vectors: the element type the file declared (0: each element has its own); the writer
        // recomputes it for a vector whose elements changed
        uint16_t vectorElementType = 0;

        static PropertyValue boolean(bool b);
        // the smallest integer type that holds the value, as Aseprite stores integers
        static PropertyValue integer(int64_t v);
        static PropertyValue number(double v);        // Double
        static PropertyValue string(std::string s);
        static PropertyValue point(IntPoint p);
        static PropertyValue size(IntSize s);
        static PropertyValue rect(IntRect r);
        static PropertyValue uuid(Uuid u);
        static PropertyValue vector(PropertyVector elements);
        static PropertyValue map(PropertyMap properties);

        bool operator==(const PropertyValue& o) const;
        bool operator!=(const PropertyValue& o) const { return !(*this == o); }
    };

    struct Property {
        std::string name;
        PropertyValue value;
        bool operator==(const Property& o) const { return name == o.name && value == o.value; }
    };

    const PropertyValue* findProperty(const PropertyMap& map, std::string_view name);
    // JSON of a value: numbers, strings, booleans; point {x, y}, size {w, h}, rect {x, y, w, h},
    // uuid "8-4-4-4-12", vectors as arrays, maps as objects
    nlohmann::json toJson(const PropertyValue& value);
    nlohmann::json toJson(const PropertyMap& map);

    struct UserData {
        std::optional<std::string> text;
        std::optional<PixelRgba> color;
        // key 0: the user properties; others: an extension's (its External Files entry id)
        std::map<uint32_t, PropertyMap> properties;

        bool empty() const;
        bool operator==(const UserData& o) const { return text == o.text && color == o.color && properties == o.properties; }
        bool operator!=(const UserData& o) const { return !(*this == o); }
        const PropertyMap* userProperties() const;
    };

    // ---- structure

    struct Layer {
        std::string name;
        LayerType type = LayerType::Image;
        uint16_t flags = kLayerVisible | kLayerEditable;
        int childLevel = 0;
        int parent = -1;                  // index of the group holding it, -1 at the top
        BlendMode blend = BlendMode::Normal;
        uint8_t opacity = 255;
        uint32_t tilesetIndex = 0;        // tilemap layers
        std::optional<Uuid> uuid;         // when the header says layers have UUIDs
        UserData userData;

        bool visible() const { return (flags & kLayerVisible) != 0; }
        bool reference() const { return (flags & kLayerReference) != 0; }
        bool background() const { return (flags & kLayerBackground) != 0; }
        bool isGroup() const { return type == LayerType::Group; }
    };

    struct Tilemap {
        int columns = 0, rows = 0;
        uint16_t bitsPerTile = 32;
        uint32_t idMask = 0x1fffffff, xFlipMask = 0x80000000, yFlipMask = 0x40000000, diagonalMask = 0x20000000;
        std::vector<uint32_t> tiles;      // columns x rows, rows top first, raw file values

        uint32_t at(int column, int row) const { return tiles[static_cast<size_t>(row) * columns + column]; }
        bool operator==(const Tilemap& o) const;
    };

    // cel extra (0x2006): precise bounds in 16.16 fixed point, kept raw
    struct CelExtra {
        uint32_t flags = 1;
        int32_t x = 0, y = 0, w = 0, h = 0;
        bool operator==(const CelExtra& o) const { return flags == o.flags && x == o.x && y == o.y && w == o.w && h == o.h; }
    };

    struct Cel {
        int layer = 0, frame = 0;
        int x = 0, y = 0;
        uint8_t opacity = 255;
        int16_t zIndex = 0;
        int linkedFrame = -1;                        // >= 0: a linked cel (shares that frame's cel)
        // image cels (shared by their links). Indexed documents keep the index plane (RGBA: the
        // palette lookup, the transparent index clear on non-background layers); grayscale ones
        // keep gray RGBA (r = g = b = value).
        std::shared_ptr<const PixelImage> image;
        std::shared_ptr<const Tilemap> tilemap;      // tilemap cels
        std::optional<CelExtra> extra;
        UserData userData;
    };

    struct SliceKey {
        uint32_t frame = 0;
        IntRect bounds;                   // w == 0 or h == 0: hidden from this frame on
        std::optional<IntRect> center;    // 9-patch centre, relative to the bounds
        std::optional<IntPoint> pivot;    // relative to the bounds' origin
        bool hidden() const { return bounds.w == 0 || bounds.h == 0; }
        bool operator==(const SliceKey& o) const { return frame == o.frame && bounds == o.bounds && center == o.center && pivot == o.pivot; }
    };

    struct Slice {
        std::string name;
        uint32_t flags = 0;               // 1: 9-patch, 2: pivot (recomputed from the keys on write)
        std::vector<SliceKey> keys;       // ascending frames; with a centre (or pivot) on one key the file stores one on every key
        UserData userData;
        // the key in force on a frame (keys hold until the next one); nullptr before the first
        // key or where the slice is hidden
        const SliceKey* keyAt(int frame) const;
    };

    struct Tag {
        int from = 0, to = 0;
        TagDirection direction = TagDirection::Forward;
        uint16_t repeat = 0;              // 0: unspecified (infinite in Aseprite's UI, once on export)
        std::string name;
        PixelRgba legacyColor{0, 0, 0, 255};   // the deprecated RGB of the tags chunk
        UserData userData;                // its colour is the tag colour Aseprite shows
        PixelRgba color() const;          // the user data colour, else the legacy one
    };

    struct ExternalTileset { uint32_t fileId = 0, tilesetId = 0; };

    struct Tileset {
        uint32_t id = 0;
        uint32_t flags = 2 | 4;           // 1 external, 2 embedded, 4 tile 0 is empty, 8/16/32 auto-flip matching
        int tileWidth = 16, tileHeight = 16;
        int baseIndex = 1;
        int tileCount = 0;
        std::string name;
        std::optional<ExternalTileset> external;
        PixelImage tiles;                 // tileWidth x (tileHeight * tileCount), embedded tilesets
        UserData userData;
        std::vector<UserData> tileUserData;   // one per tile (may be shorter: the rest are empty)

        bool zeroIsEmpty() const { return (flags & 4) != 0; }
        PixelImage tile(int index) const;
    };

    struct PaletteEntry {
        PixelRgba color{0, 0, 0, 255};
        std::optional<std::string> name;
        bool operator==(const PaletteEntry& o) const { return color == o.color && name == o.name; }
    };

    struct ExternalFile {
        uint32_t id = 0;
        uint8_t type = 0;                 // 0 palette, 1 tileset, 2 properties extension, 3 tile management extension
        std::string name;
        bool operator==(const ExternalFile& o) const { return id == o.id && type == o.type && name == o.name; }
    };

    struct ColorProfile {
        uint16_t type = 1;                // 0 none, 1 sRGB, 2 embedded ICC
        uint16_t flags = 0;               // 1: fixed gamma
        int32_t gamma = 0;                // 16.16
        std::vector<uint8_t> icc;
        bool operator==(const ColorProfile& o) const { return type == o.type && flags == o.flags && gamma == o.gamma && icc == o.icc; }
    };

    // the deprecated mask chunk (0x2016), kept as read
    struct Mask {
        int frame = 0;
        int x = 0, y = 0, w = 0, h = 0;
        std::string name;
        std::vector<uint8_t> bitmap;      // h * ceil(w / 8) bytes, MSB first
        bool operator==(const Mask& o) const { return frame == o.frame && x == o.x && y == o.y && w == o.w && h == o.h && name == o.name && bitmap == o.bitmap; }
    };

    // What an opaque chunk came after, so it is written back in place
    struct ChunkAnchor {
        enum class Kind : uint8_t { FrameStart, Layer, Cel, Tags, Slice, Tileset, Palette, Other };
        Kind kind = Kind::FrameStart;
        int a = -1, b = -1;               // layer index; cel layer/frame; slice / tileset index
        bool operator==(const ChunkAnchor& o) const { return kind == o.kind && a == o.a && b == o.b; }
    };

    // A chunk the codec does not interpret (an unknown type, the unused path chunk, a user data
    // chunk with no owner): its body (after the 6-byte size/type header) written back as is
    struct OpaqueChunk {
        uint16_t type = 0;
        int frame = 0;
        ChunkAnchor anchor;
        std::vector<uint8_t> body;
        bool operator==(const OpaqueChunk& o) const { return type == o.type && frame == o.frame && anchor == o.anchor && body == o.body; }
    };

    // The writer's record of the file a document was read from (R7); internal
    struct Provenance;

    struct Document {
        int width = 0, height = 0;
        ColorMode mode = ColorMode::Rgba;
        uint32_t headerFlags = kHeaderLayerOpacity;
        uint8_t transparentIndex = 0;     // indexed documents
        uint8_t pixelWidth = 1, pixelHeight = 1;
        IntRect grid{0, 0, 16, 16};
        uint16_t speed = 100;             // the deprecated header speed (ms)
        std::vector<Layer> layers;        // file order (NOTE.2 layer indices)
        std::vector<uint16_t> frameDurations;   // ms, one per frame
        std::vector<Cel> cels;            // sorted by (frame, layer)
        std::vector<Tag> tags;
        std::vector<PaletteEntry> palette;      // the frame 0 palette
        // palettes that change on a later frame (rare): frame -> the whole palette from there on
        std::map<int, std::vector<PaletteEntry>> paletteChanges;
        std::vector<Slice> slices;
        std::vector<Tileset> tilesets;
        std::vector<ExternalFile> externalFiles;
        std::optional<ColorProfile> colorProfile;
        std::vector<Mask> masks;
        UserData spriteUserData;
        std::vector<OpaqueChunk> opaque;  // R5
        std::shared_ptr<const Provenance> provenance;

        int frameCount() const { return static_cast<int>(frameDurations.size()); }
        // the cel of a layer on a frame, following links (nullptr: no cel)
        const Cel* cel(int layer, int frame) const;
        // the stored cel record itself (a link stays a link)
        const Cel* celRecord(int layer, int frame) const;
        Cel* celRecord(int layer, int frame);
        // the palette in force on a frame
        const std::vector<PaletteEntry>& paletteAt(int frame) const;
        // the palette as RGBA bytes (4 per entry)
        std::vector<uint8_t> paletteRgba(int frame = 0) const;
        // "Body/Arm": the names of a layer's groups and its own, '/' separated
        std::string layerPath(int layer) const;
        // a layer by path ("Body/Arm", or a top-level name), else the first with that name; -1 when none
        int findLayer(std::string_view nameOrPath) const;
        // the layers directly in a group (-1: the top level), in file order
        std::vector<int> children(int group) const;
        // visible here and in every group above it
        bool visibleHierarchy(int layer) const;
        bool hasBackground() const;
        // recomputes every layer's parent from the child levels (NOTE.1) and every child level
        // from the parents, so code that inserted layers can call it after setting one of them
        void rebuildTreeFromLevels();
        void rebuildLevelsFromTree();
        // keeps `cels` sorted by (frame, layer)
        void sortCels();
    };

    struct CodecError : PixelProblem {
        size_t offset = 0;                // the byte offset of the problem in the file
    };

    struct ReadOptions {
        // false: skip pixel data (cels keep their size, `image` holds no pixels); a quick scan for
        // the asset database
        bool pixels = true;
        // false: no provenance (a read-only use: the importer, describe, render)
        bool provenance = true;
    };

    struct ReadResult {
        std::optional<Document> document;
        std::vector<CodecError> warnings;
        CodecError error;                 // kind empty when the read succeeded
        bool ok() const { return document.has_value(); }
    };

    ReadResult read(std::span<const uint8_t> bytes, const ReadOptions& options = {});
    ReadResult readFile(const std::filesystem::path& path, const ReadOptions& options = {});

    // Encodes a document (deterministic: same document, same bytes, on every platform). False
    // with *error (invalid_document) when the document cannot be written, e.g. an indexed cel
    // whose colour is not in the palette or a cel on a missing layer.
    bool write(const Document& document, std::vector<uint8_t>& out, CodecError* error);
    // write() then an atomic file replace
    bool writeFile(const Document& document, const std::filesystem::path& path, CodecError* error);

    // ---- composition (R8)

    // How groups combine. FromFile: composed (their opacity and blend applied) when the header
    // says groups have them (flag 2, what the artist saw in Aseprite), else flattened. Flatten is
    // what Aseprite 1.3's command-line --sheet export always does (its Render never composes groups).
    enum class GroupMode { FromFile, Flatten, Compose };

    struct RenderOptions {
        bool includeHidden = false;       // hidden layers too (as if visible)
        bool includeReference = false;    // reference layers too
        bool keepIndices = false;         // indexed documents: keep the composed index plane
        bool newBlend = true;             // false: Aseprite's classic blending (--classic-blend)
        bool legacyHsl = false;           // Hue/Saturation as Aseprite <= 1.3.12 drew them (--legacy-hsl)
        std::vector<int> layers;          // only these layers (and their groups); empty: by the rules above
        int soloLayer = -1;               // >= 0: that layer (or group) alone
        bool zIndex = true;               // apply cel z-index (the importer's layers_as_sprites turns it off)
        GroupMode groups = GroupMode::FromFile;
    };

    // The frame exactly as Aseprite's `--sheet` export draws it. An out-of-range frame gives an
    // empty image.
    PixelImage renderFrame(const Document& document, int frame, const RenderOptions& options = {});
    // A tilemap cel's pixels from its tileset (flips applied), at the cel's size in pixels
    PixelImage renderTilemap(const Document& document, const Tilemap& tilemap, uint32_t tilesetIndex);

    // ---- description (R9): the one JSON shape of get_asset_info, S47's get_sprite and the CLI

    enum class Detail { Summary, Full, Cels };
    nlohmann::json describe(const Document& document, Detail detail);

}
