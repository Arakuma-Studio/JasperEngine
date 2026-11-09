// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Tiled importer (docs/specs/S50-grid-tilemap.md R44, D11). A .tmx is first turned into the
// shape of a .tmj (attributes stay text, the checked reads of ImportJsonReader.h take both), so one
// mapper reads both formats.
//
// Cells are converted to y up with the map's height (a finite map's row y becomes H - 1 - y, an
// infinite map's -1 - y), and the layout's axes follow so every tile lands on the pixel Tiled draws
// it, up to one translation of the whole map (TiledImporterTest pins each orientation):
//   - Tiled's isometric maps grow downward on screen, ours upward: (x, y) -> (H - 1 - y, W - 1 - x);
//   - staggered and hexagonal rows: row parity changes when H is even, so the stagger index flips;
//     Tiled's staggered columns move down, ours up, so the index flips on axis X always.

#include "tilemap/import/TiledImporter.h"

#include "tilemap/import/ImportJsonReader.h"
#include "util/Base64.h"

#include "json.hpp"
#include "stb_image.h"
#include "tinyxml2.h"
#include "zstd.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::editor::tilemapimport {

    namespace {

        using nlohmann::json;
        using tinyxml2::XMLAttribute;
        using tinyxml2::XMLDocument;
        using tinyxml2::XMLElement;

        constexpr uint32_t kGidFlipH = 1u << 31;
        constexpr uint32_t kGidFlipV = 1u << 30;
        constexpr uint32_t kGidFlipD = 1u << 29;
        constexpr uint32_t kGidRotateHex120 = 1u << 28;
        constexpr uint32_t kGidMask = 0x0FFFFFFFu;

        // groups nest at most this deep (a .tmj can nest them without bound)
        constexpr int kMaxGroupDepth = 64;

        // ---- .tmx -> the .tmj shape

        json tmxAttributes(const XMLElement* e) {
            json o = json::object();
            for (const XMLAttribute* a = e->FirstAttribute(); a; a = a->Next()) o[a->Name()] = a->Value();
            o["__line"] = e->GetLineNum();
            return o;
        }

        json tmxProperties(const XMLElement* parent) {
            json out = json::array();
            const XMLElement* properties = parent->FirstChildElement("properties");
            if (!properties) return out;
            for (const XMLElement* p = properties->FirstChildElement("property"); p; p = p->NextSiblingElement("property")) {
                json o = tmxAttributes(p);
                if (!o.contains("value")) o["value"] = p->GetText() ? p->GetText() : "";
                out.push_back(std::move(o));
            }
            return out;
        }

        json tmxPoints(const char* text) {
            json out = json::array();
            if (!text) return out;
            const std::string points = text;
            size_t i = 0;
            while (i < points.size()) {
                while (i < points.size() && std::isspace(static_cast<unsigned char>(points[i]))) i++;
                const size_t start = i;
                while (i < points.size() && !std::isspace(static_cast<unsigned char>(points[i]))) i++;
                if (i == start) break;
                const std::string point = points.substr(start, i - start);
                const size_t comma = point.find(',');
                out.push_back({{"x", point.substr(0, comma)}, {"y", comma == std::string::npos ? std::string() : point.substr(comma + 1)}});
            }
            return out;
        }

        json tmxObjects(const XMLElement* group) {
            json out = json::array();
            for (const XMLElement* e = group->FirstChildElement("object"); e; e = e->NextSiblingElement("object")) {
                json o = tmxAttributes(e);
                if (e->FirstChildElement("ellipse")) o["ellipse"] = true;
                if (e->FirstChildElement("point")) o["point"] = true;
                if (const XMLElement* p = e->FirstChildElement("polygon")) o["polygon"] = tmxPoints(p->Attribute("points"));
                if (const XMLElement* p = e->FirstChildElement("polyline")) o["polyline"] = tmxPoints(p->Attribute("points"));
                if (e->FirstChildElement("text")) o["text"] = true;
                out.push_back(std::move(o));
            }
            return out;
        }

        void tmxReadImage(const XMLElement* parent, json& o) {
            if (const XMLElement* image = parent->FirstChildElement("image")) {
                if (const char* source = image->Attribute("source")) o["image"] = source;
                if (const char* width = image->Attribute("width")) o["imagewidth"] = width;
                if (const char* height = image->Attribute("height")) o["imageheight"] = height;
            }
        }

        json tmxTileset(const XMLElement* e) {
            json o = tmxAttributes(e);
            tmxReadImage(e, o);
            if (const XMLElement* offset = e->FirstChildElement("tileoffset")) o["tileoffset"] = tmxAttributes(offset);
            if (e->FirstChildElement("terrains")) o["terrains"] = true;
            json tiles = json::array();
            for (const XMLElement* t = e->FirstChildElement("tile"); t; t = t->NextSiblingElement("tile")) {
                json tile = tmxAttributes(t);
                tile["properties"] = tmxProperties(t);
                tmxReadImage(t, tile);
                if (const XMLElement* group = t->FirstChildElement("objectgroup")) tile["objectgroup"] = {{"objects", tmxObjects(group)}, {"__line", group->GetLineNum()}};
                if (const XMLElement* animation = t->FirstChildElement("animation")) {
                    json frames = json::array();
                    for (const XMLElement* f = animation->FirstChildElement("frame"); f; f = f->NextSiblingElement("frame")) frames.push_back(tmxAttributes(f));
                    tile["animation"] = std::move(frames);
                }
                tiles.push_back(std::move(tile));
            }
            if (!tiles.empty()) o["tiles"] = std::move(tiles);
            if (const XMLElement* wangsets = e->FirstChildElement("wangsets")) {
                json sets = json::array();
                for (const XMLElement* w = wangsets->FirstChildElement("wangset"); w; w = w->NextSiblingElement("wangset")) {
                    json set = tmxAttributes(w);
                    json colors = json::array();
                    for (const XMLElement* c = w->FirstChildElement("wangcolor"); c; c = c->NextSiblingElement("wangcolor")) colors.push_back(tmxAttributes(c));
                    set["colors"] = std::move(colors);
                    json wangTiles = json::array();
                    for (const XMLElement* t = w->FirstChildElement("wangtile"); t; t = t->NextSiblingElement("wangtile")) wangTiles.push_back(tmxAttributes(t));
                    set["wangtiles"] = std::move(wangTiles);
                    sets.push_back(std::move(set));
                }
                o["wangsets"] = std::move(sets);
            }
            return o;
        }

        void tmxDataContent(const XMLElement* element, json& target) {
            if (element->FirstChildElement("tile")) {
                json gids = json::array();
                for (const XMLElement* t = element->FirstChildElement("tile"); t; t = t->NextSiblingElement("tile")) {
                    const char* gid = t->Attribute("gid");
                    gids.push_back(gid ? gid : "0");
                }
                target["data"] = std::move(gids);
                return;
            }
            const char* text = element->GetText();
            target["data"] = text ? text : "";
        }

        json tmxLayers(const XMLElement* parent) {
            json out = json::array();
            for (const XMLElement* e = parent->FirstChildElement(); e; e = e->NextSiblingElement()) {
                const std::string name = e->Name();
                if (name == "layer") {
                    json o = tmxAttributes(e);
                    o["type"] = "tilelayer";
                    if (const XMLElement* data = e->FirstChildElement("data")) {
                        if (const char* encoding = data->Attribute("encoding")) o["encoding"] = encoding;
                        if (const char* compression = data->Attribute("compression")) o["compression"] = compression;
                        if (data->FirstChildElement("chunk")) {
                            json chunks = json::array();
                            for (const XMLElement* c = data->FirstChildElement("chunk"); c; c = c->NextSiblingElement("chunk")) {
                                json chunk = tmxAttributes(c);
                                tmxDataContent(c, chunk);
                                chunks.push_back(std::move(chunk));
                            }
                            o["chunks"] = std::move(chunks);
                        } else {
                            tmxDataContent(data, o);
                        }
                    }
                    out.push_back(std::move(o));
                } else if (name == "objectgroup" || name == "imagelayer" || name == "group") {
                    json o = tmxAttributes(e);
                    o["type"] = name;
                    if (name == "group") o["layers"] = tmxLayers(e);
                    out.push_back(std::move(o));
                }
            }
            return out;
        }

        json parseXmlDocument(std::string_view text, const detail::SourceContext& ctx, XMLDocument& doc) {
            doc.Parse(text.data(), text.size());
            if (doc.Error()) ctx.fail("", std::string("not valid XML: ") + doc.ErrorStr(), doc.ErrorLineNum());
            const XMLElement* root = doc.RootElement();
            if (!root) ctx.fail("", "no root element", 1);
            const std::string name = root->Name();
            if (name == "tileset") return tmxTileset(root);
            if (name != "map") ctx.fail("", "the root element is <" + name + ">, not <map> or <tileset>", root->GetLineNum());
            json o = tmxAttributes(root);
            o["layers"] = tmxLayers(root);
            json tilesets = json::array();
            for (const XMLElement* e = root->FirstChildElement("tileset"); e; e = e->NextSiblingElement("tileset")) tilesets.push_back(tmxTileset(e));
            o["tilesets"] = std::move(tilesets);
            return o;
        }

        bool looksLikeXml(std::string_view text) {
            size_t i = 0;
            // a UTF-8 BOM, then whitespace
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) i = 3;
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
            return i < text.size() && text[i] == '<';
        }

        json parseDocument(std::string_view text, const detail::SourceContext& ctx, XMLDocument& doc) {
            return looksLikeXml(text) ? parseXmlDocument(text, ctx, doc) : detail::parseJsonDocument(text, ctx);
        }

        // ---- tile data

        void inflateZlib(const std::string& input, size_t expected, std::vector<uint8_t>& out, const detail::SourceContext& ctx, const std::string& path) {
            if (input.size() > static_cast<size_t>(INT_MAX)) ctx.fail(path, "the compressed data is too large");   // stb's lengths are int
            out.resize(expected);
            const int n = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), static_cast<int>(expected), input.data(), static_cast<int>(input.size()));
            if (n < 0 || static_cast<size_t>(n) != expected) ctx.fail(path, "the zlib data does not inflate to the layer's size");
        }

        void inflateGzip(const std::string& input, size_t expected, std::vector<uint8_t>& out, const detail::SourceContext& ctx, const std::string& path) {
            // RFC 1952: a 10-byte header, optional fields, the deflate stream, CRC32, ISIZE
            const auto* b = reinterpret_cast<const uint8_t*>(input.data());
            if (input.size() < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8) ctx.fail(path, "not a gzip stream");
            if (input.size() > static_cast<size_t>(INT_MAX)) ctx.fail(path, "the compressed data is too large");
            const uint8_t flags = b[3];
            size_t pos = 10;
            if (flags & 4) {   // FEXTRA
                if (pos + 2 > input.size()) ctx.fail(path, "truncated gzip header");
                const size_t extra = static_cast<size_t>(b[pos]) | (static_cast<size_t>(b[pos + 1]) << 8);
                pos += 2 + extra;
            }
            for (uint8_t flag : {uint8_t(8), uint8_t(16)}) {   // FNAME, FCOMMENT: NUL-terminated
                if (flags & flag) {
                    while (pos < input.size() && b[pos] != 0) pos++;
                    pos++;
                }
            }
            if (flags & 2) pos += 2;   // FHCRC
            if (pos + 8 > input.size()) ctx.fail(path, "truncated gzip header");
            const size_t deflateEnd = input.size() - 8;
            const uint32_t isize = static_cast<uint32_t>(b[deflateEnd + 4]) | (static_cast<uint32_t>(b[deflateEnd + 5]) << 8) |
                                   (static_cast<uint32_t>(b[deflateEnd + 6]) << 16) | (static_cast<uint32_t>(b[deflateEnd + 7]) << 24);
            if (isize != expected) ctx.fail(path, "the gzip data's size does not match the layer's size");
            out.resize(expected);
            const int n = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), static_cast<int>(expected), input.data() + pos, static_cast<int>(deflateEnd - pos));
            if (n < 0 || static_cast<size_t>(n) != expected) ctx.fail(path, "the gzip data does not inflate to the layer's size");
        }

        void inflateZstd(const std::string& input, size_t expected, std::vector<uint8_t>& out, const detail::SourceContext& ctx, const std::string& path) {
            out.resize(expected);
            const size_t n = ZSTD_decompress(out.data(), expected, input.data(), input.size());
            if (ZSTD_isError(n)) ctx.fail(path, std::string("the zstd data does not inflate: ") + ZSTD_getErrorName(n));
            if (n != expected) ctx.fail(path, "the zstd data does not inflate to the layer's size");
        }

        std::string decodeBase64Text(const std::string& text, const detail::SourceContext& ctx, const std::string& path) {
            std::string clean;
            clean.reserve(text.size());
            size_t padding = 0;
            for (char c : text) {
                if (std::isspace(static_cast<unsigned char>(c))) continue;
                if (c == '=') {
                    padding++;
                    continue;
                }
                const auto u = static_cast<unsigned char>(c);
                if (padding > 0 || !(std::isalnum(u) || c == '+' || c == '/')) ctx.fail(path, "not valid base64 (the data has an unexpected character)");
                clean.push_back(c);
            }
            if (padding > 2 || (clean.size() + padding) % 4 != 0) ctx.fail(path, "not valid base64 (its length is wrong)");
            for (size_t i = 0; i < padding; i++) clean.push_back('=');
            const std::vector<unsigned char> decoded = Base64::decode(clean);
            return std::string(reinterpret_cast<const char*>(decoded.data()), decoded.size());
        }

        void decodeGids(const json& data, const std::string& encoding, const std::string& compression, int64_t cells, std::vector<uint32_t>& out,
                        const detail::SourceContext& ctx, const std::string& path, int line) {
            out.clear();
            if (data.is_array()) {
                if (static_cast<int64_t>(data.size()) != cells) ctx.fail(path, "has " + std::to_string(data.size()) + " cells, the layer needs " + std::to_string(cells), line);
                out.reserve(data.size());
                for (size_t i = 0; i < data.size(); i++) {
                    const int64_t gid = detail::asInt(data[i], ctx, detail::indexPath(path, i), line);
                    if (gid < 0 || gid > 0xFFFFFFFFll) ctx.fail(detail::indexPath(path, i), "is not a tile GID", line);
                    out.push_back(static_cast<uint32_t>(gid));
                }
                return;
            }
            if (!data.is_string()) ctx.fail(path, "expected an array or a string", line);
            const std::string& text = data.get_ref<const std::string&>();
            if (encoding == "base64") {
                const std::string decoded = decodeBase64Text(text, ctx, path);
                const size_t expected = static_cast<size_t>(cells) * 4;
                std::vector<uint8_t> bytes;
                if (compression.empty()) {
                    if (decoded.size() != expected) ctx.fail(path, "the base64 data is " + std::to_string(decoded.size()) + " bytes, the layer needs " + std::to_string(expected), line);
                    bytes.assign(decoded.begin(), decoded.end());
                } else if (compression == "zlib") {
                    inflateZlib(decoded, expected, bytes, ctx, path);
                } else if (compression == "gzip") {
                    inflateGzip(decoded, expected, bytes, ctx, path);
                } else if (compression == "zstd") {
                    inflateZstd(decoded, expected, bytes, ctx, path);
                } else {
                    ctx.fail(path, "unknown compression '" + compression + "' (expected zlib, gzip or zstd)", line);
                }
                out.resize(static_cast<size_t>(cells));
                for (size_t i = 0; i < out.size(); i++) {
                    out[i] = static_cast<uint32_t>(bytes[i * 4]) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) |
                             (static_cast<uint32_t>(bytes[i * 4 + 2]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 3]) << 24);
                }
                return;
            }
            if (encoding != "csv" && !encoding.empty()) ctx.fail(path, "unknown encoding '" + encoding + "' (expected csv or base64)", line);
            out.reserve(static_cast<size_t>(cells));
            size_t i = 0;
            while (i < text.size()) {
                while (i < text.size() && (text[i] == ',' || std::isspace(static_cast<unsigned char>(text[i])))) i++;
                if (i >= text.size()) break;
                const size_t start = i;
                while (i < text.size() && text[i] != ',' && !std::isspace(static_cast<unsigned char>(text[i]))) i++;
                if (static_cast<int64_t>(out.size()) >= cells) ctx.fail(path, "has more cells than the layer's " + std::to_string(cells), line);
                int64_t gid = 0;
                if (!detail::parseIntText(text.substr(start, i - start), gid) || gid < 0 || gid > 0xFFFFFFFFll) {
                    ctx.fail(path, "'" + text.substr(start, i - start) + "' is not a tile GID", line);
                }
                out.push_back(static_cast<uint32_t>(gid));
            }
            if (static_cast<int64_t>(out.size()) != cells) ctx.fail(path, "has " + std::to_string(out.size()) + " cells, the layer needs " + std::to_string(cells), line);
        }

        // ---- the mapper

        struct InheritedLayer {
            std::string prefix;
            bool visible = true;
            double opacity = 1.0;
            double offsetX = 0.0;
            double offsetY = 0.0;
        };

        class TiledParser {
        public:
            TiledParser(const std::string& fileName, const FileReader& readFile, ImportPlan& plan): fileName(fileName), readFile(readFile), plan(plan) {}

            void run(std::string_view text) {
                detail::SourceContext ctx;
                ctx.file = fileName;
                plan.sourceFile = fileName;
                plan.format = "tiled";
                XMLDocument doc;
                const json root = parseDocument(text, ctx, doc);
                if (!root.is_object()) ctx.fail("", "the document is not a map object");
                if (detail::getString(root, "type", "map", ctx, "") == "tileset") ctx.fail("type", "this file is a tileset; import the map that uses it");

                ImportGrid grid;
                grid.name = std::filesystem::path(fileName).stem().generic_string();
                if (grid.name.empty()) grid.name = "map";
                readMapHeader(root, grid, ctx);

                const json* tilesets = detail::findKey(root, "tilesets");
                if (tilesets && !tilesets->is_array()) ctx.fail("tilesets", "expected an array", detail::lineOf(root));
                if (tilesets) {
                    for (size_t i = 0; i < tilesets->size(); i++) readTilesetRef((*tilesets)[i], detail::indexPath("tilesets", i), ctx);
                }
                std::sort(gidTable.begin(), gidTable.end());
                for (size_t i = 1; i < gidTable.size(); i++) {
                    if (gidTable[i].first == gidTable[i - 1].first) ctx.fail("tilesets", "two tilesets share the firstgid " + std::to_string(gidTable[i].first), detail::lineOf(root));
                }

                const json* layers = detail::findKey(root, "layers");
                if (!layers || !layers->is_array()) ctx.fail("layers", "expected an array of layers", detail::lineOf(root));
                walkLayers(*layers, "layers", InheritedLayer{}, 0, grid, ctx);

                plan.grids.push_back(std::move(grid));
            }

        private:
            const std::string& fileName;
            const FileReader& readFile;
            ImportPlan& plan;

            int mapWidth = 0;   // 0 when infinite: every row maps through -1 - y
            int mapHeight = 0;
            // the diamond: rows and columns swap and negate (x, y) -> (H - 1 - y, W - 1 - x)
            bool swapAxes = false;
            // isometric, staggered, or hexagonal with no side: tile shape and wang directions
            bool diamondTiles = false;
            bool hexLayout = false;
            bool staggerAxisIsX = false;
            std::vector<std::pair<uint32_t, int>> gidTable;   // firstgid, plan tileset
            std::set<std::string> usedLayerNames;
            int64_t mapCells = 0;

            void warn(std::string message) { plan.warnings.push_back(std::move(message)); }

            void readMapHeader(const json& root, ImportGrid& grid, const detail::SourceContext& ctx) {
                const std::string orientation = detail::getString(root, "orientation", "", ctx, "");
                const int tw = detail::requireInt(root, "tilewidth", 1, 65536, ctx, "");
                const int th = detail::requireInt(root, "tileheight", 1, 65536, ctx, "");
                const bool infinite = detail::getBool(root, "infinite", false, ctx, "");
                TileGridSpec& spec = grid.spec;
                spec.cellWidth = tw;
                spec.cellHeight = th;
                if (orientation == "orthogonal") {
                    spec.layout = TileGridLayout::Rectangle;
                } else if (orientation == "isometric") {
                    spec.layout = TileGridLayout::Isometric;
                    swapAxes = true;
                    diamondTiles = true;
                } else if (orientation == "staggered" || orientation == "hexagonal") {
                    spec.layout = orientation == "hexagonal" ? TileGridLayout::Hexagon : TileGridLayout::IsometricStaggered;
                    diamondTiles = orientation == "staggered";
                    hexLayout = orientation == "hexagonal";
                    const std::string axis = detail::getString(root, "staggeraxis", "y", ctx, "");
                    if (axis != "x" && axis != "y") ctx.fail("staggeraxis", "expected x or y, found '" + axis + "'", detail::lineOf(root));
                    spec.staggerAxis = axis == "x" ? StaggerAxis::X : StaggerAxis::Y;
                    staggerAxisIsX = axis == "x";
                    const std::string index = detail::getString(root, "staggerindex", "odd", ctx, "");
                    if (index != "odd" && index != "even") ctx.fail("staggerindex", "expected odd or even, found '" + index + "'", detail::lineOf(root));
                    spec.staggerIndex = index == "even" ? StaggerIndex::Even : StaggerIndex::Odd;
                    // Tiled rounds a staggered or hexagonal map's tile size down to even (RenderParams)
                    if ((tw & 1) || (th & 1)) {
                        warn("The map's tile size is odd; Tiled draws it rounded down to " + std::to_string(tw & ~1) + "x" + std::to_string(th & ~1));
                        spec.cellWidth = tw & ~1;
                        spec.cellHeight = th & ~1;
                    }
                    if (orientation == "hexagonal") {
                        const int side = detail::boundedInt(root, "hexsidelength", 0, 0, 65536, ctx, "");
                        if (side == 0) {
                            // no side: Tiled's staggered geometry (its hex maths with side 0); a
                            // hexSideLength of 0 here would mean a regular hexagon instead
                            warn("The hexagonal map has no hexsidelength; it imports as a staggered map, which is the same geometry");
                            spec.layout = TileGridLayout::IsometricStaggered;
                            hexLayout = false;
                            diamondTiles = true;
                        } else {
                            spec.hexSideLength = side;
                        }
                    }
                } else {
                    ctx.fail("orientation", "unknown orientation '" + orientation + "' (expected orthogonal, isometric, staggered or hexagonal)", detail::lineOf(root));
                }

                if (!infinite) {
                    mapWidth = detail::requireInt(root, "width", 1, 1 << 24, ctx, "");
                    mapHeight = detail::requireInt(root, "height", 1, 1 << 24, ctx, "");
                }
                // rows flip through H - 1 - y (H 0 when infinite), so row parity moves when H is even;
                // columns keep theirs, but Tiled's staggered columns move down and ours up
                if (spec.layout == TileGridLayout::IsometricStaggered || spec.layout == TileGridLayout::Hexagon) {
                    const bool flipIndex = spec.staggerAxis == StaggerAxis::X || (mapHeight & 1) == 0;
                    if (flipIndex) spec.staggerIndex = spec.staggerIndex == StaggerIndex::Odd ? StaggerIndex::Even : StaggerIndex::Odd;
                }
            }

            // ---- tilesets

            void readTilesetRef(const json& ref, const std::string& path, const detail::SourceContext& ctx) {
                if (!ref.is_object()) ctx.fail(path, "expected a tileset object", detail::lineOf(ref));
                const int firstGid = detail::requireInt(ref, "firstgid", 1, static_cast<int>(kGidMask), ctx, path);
                ImportTileset tileset;
                const std::string source = detail::getString(ref, "source", "", ctx, path);
                if (source.empty()) {
                    convertTileset(ref, path, std::string(), ctx, tileset);
                } else {
                    const std::string external = detail::joinMapPath(std::string(), source);
                    std::string text;
                    if (!readFile || !readFile(external, text)) {
                        ImportProblem p;
                        p.kind = "tileset_missing";
                        p.field = detail::keyPath(path, "source");
                        p.message = "cannot read the external tileset '" + external + "'";
                        p.fix = "put the tileset next to the map, or embed it in Tiled";
                        p.file = fileName;
                        p.line = detail::lineOf(ref);
                        throw detail::ParseFailure{std::move(p)};
                    }
                    detail::SourceContext externalCtx;
                    externalCtx.file = external;
                    XMLDocument doc;
                    const json root = parseDocument(text, externalCtx, doc);
                    if (!root.is_object()) externalCtx.fail("", "the document is not a tileset object");
                    const std::string baseDir = std::filesystem::path(external).parent_path().generic_string();
                    convertTileset(root, "", baseDir, externalCtx, tileset);
                    tileset.externalFile = external;
                }
                gidTable.emplace_back(static_cast<uint32_t>(firstGid), static_cast<int>(plan.tilesets.size()));
                plan.tilesets.push_back(std::move(tileset));
            }

            void convertTileset(const json& ts, const std::string& path, const std::string& baseDir, const detail::SourceContext& ctx, ImportTileset& out) {
                TilesetData& data = out.data;
                data.name = detail::getString(ts, "name", "", ctx, path);
                if (data.name.empty()) data.name = "tileset" + std::to_string(plan.tilesets.size());
                data.tileWidth = detail::requireInt(ts, "tilewidth", 1, 65536, ctx, path);
                data.tileHeight = detail::requireInt(ts, "tileheight", 1, 65536, ctx, path);
                data.shape = hexLayout ? (staggerAxisIsX ? TileShape::HexFlat : TileShape::HexPointy) : (diamondTiles ? TileShape::Isometric : TileShape::Square);

                const int margin = detail::boundedInt(ts, "margin", 0, 0, 65536, ctx, path);
                const int spacing = detail::boundedInt(ts, "spacing", 0, 0, 65536, ctx, path);
                int offsetX = 0, offsetY = 0;
                if (const json* offset = detail::findKey(ts, "tileoffset")) {
                    offsetX = detail::boundedInt(*offset, "x", 0, -65536, 65536, ctx, detail::keyPath(path, "tileoffset"));
                    offsetY = -detail::boundedInt(*offset, "y", 0, -65536, 65536, ctx, detail::keyPath(path, "tileoffset"));
                }
                if (detail::findKey(ts, "terrains")) plan.skipped.push_back({"wang_set", data.name + ": old-format terrains", 1});

                const std::string image = detail::getString(ts, "image", "", ctx, path);
                const json* tiles = detail::findKey(ts, "tiles");
                if (tiles && !tiles->is_array()) ctx.fail(detail::keyPath(path, "tiles"), "expected an array", detail::lineOf(ts));

                std::unordered_map<int32_t, size_t> index;
                std::map<int32_t, const json*> entries;
                if (tiles) {
                    for (size_t i = 0; i < tiles->size(); i++) {
                        const std::string tilePath = detail::indexPath(detail::keyPath(path, "tiles"), i);
                        const json& t = (*tiles)[i];
                        if (!t.is_object()) ctx.fail(tilePath, "expected a tile object", detail::lineOf(ts));
                        const int id = detail::requireInt(t, "id", 0, MAX_TILE_ID, ctx, tilePath);
                        if (!entries.emplace(id, &t).second) ctx.fail(detail::keyPath(tilePath, "id"), "the id " + std::to_string(id) + " appears twice", detail::lineOf(t));
                    }
                }

                if (!image.empty()) {
                    const int imageWidth = detail::boundedInt(ts, "imagewidth", 0, 0, 1 << 24, ctx, path);
                    const int imageHeight = detail::boundedInt(ts, "imageheight", 0, 0, 1 << 24, ctx, path);
                    int columns = detail::boundedInt(ts, "columns", 0, 0, 1 << 24, ctx, path);
                    if (columns == 0 && imageWidth > 0) columns = std::max(0, (imageWidth - 2 * margin + spacing) / (data.tileWidth + spacing));
                    if (columns == 0) ctx.fail(detail::keyPath(path, "columns"), "the image tileset needs columns or an image width", detail::lineOf(ts));
                    int64_t tileCount = detail::getInt(ts, "tilecount", -1, ctx, path);
                    if (tileCount < 0) {
                        if (imageHeight == 0) ctx.fail(detail::keyPath(path, "tilecount"), "the image tileset needs tilecount or an image height", detail::lineOf(ts));
                        const int rows = std::max(0, (imageHeight - 2 * margin + spacing) / (data.tileHeight + spacing));
                        tileCount = static_cast<int64_t>(columns) * rows;
                    }
                    if (tileCount > detail::kMaxTilesetTiles) ctx.fail(detail::keyPath(path, "tilecount"), std::to_string(tileCount) + " tiles is too many for one tileset", detail::lineOf(ts));

                    TilesetSource source;
                    source.id = 0;
                    source.kind = TilesetSourceKind::Texture;
                    source.asset = detail::joinMapPath(baseDir, image);
                    source.tileWidth = data.tileWidth;
                    source.tileHeight = data.tileHeight;
                    source.margin = margin;
                    source.spacing = spacing;
                    // Tiled numbers every position: a skip_empty would renumber the ids
                    source.skipEmpty = false;
                    out.images.push_back({0, source.asset.path, imageWidth, imageHeight});
                    data.sources.push_back(std::move(source));

                    data.tiles.reserve(static_cast<size_t>(tileCount));
                    for (int64_t id = 0; id < tileCount; id++) {
                        TileDef tile;
                        tile.id = static_cast<int32_t>(id);
                        tile.source = 0;
                        tile.atX = static_cast<int>(id % columns);
                        tile.atY = static_cast<int>(id / columns);
                        tile.offsetX = offsetX;
                        tile.offsetY = offsetY;
                        data.tiles.push_back(std::move(tile));
                        index[static_cast<int32_t>(id)] = static_cast<size_t>(id);
                    }
                    data.nextId = static_cast<int32_t>(tileCount);
                } else {
                    // an image collection: one source per image, the tile ids as Tiled numbered them
                    for (const auto& [id, entry] : entries) {
                        const std::string entryPath = detail::keyPath(path, "tiles") + "[id " + std::to_string(id) + "]";
                        const std::string tileImage = detail::getString(*entry, "image", "", ctx, entryPath);
                        if (tileImage.empty()) {
                            warn("Tileset '" + data.name + "': tile " + std::to_string(id) + " has no image and is skipped");
                            continue;
                        }
                        TilesetSource source;
                        source.id = static_cast<int>(data.sources.size());
                        source.kind = TilesetSourceKind::Texture;
                        source.asset = detail::joinMapPath(baseDir, tileImage);
                        source.tileWidth = detail::boundedInt(*entry, "imagewidth", 0, 0, 1 << 24, ctx, entryPath);
                        source.tileHeight = detail::boundedInt(*entry, "imageheight", 0, 0, 1 << 24, ctx, entryPath);
                        if (source.tileWidth == 0) source.tileWidth = data.tileWidth;
                        if (source.tileHeight == 0) source.tileHeight = data.tileHeight;
                        source.skipEmpty = false;
                        out.images.push_back({source.id, source.asset.path, source.tileWidth, source.tileHeight});

                        TileDef tile;
                        tile.id = id;
                        tile.source = source.id;
                        tile.offsetX = offsetX;
                        tile.offsetY = offsetY;
                        index[id] = data.tiles.size();
                        data.tiles.push_back(std::move(tile));
                        data.sources.push_back(std::move(source));
                        data.nextId = std::max(data.nextId, id + 1);
                    }
                    if (!data.sources.empty()) {
                        warn("Tileset '" + data.name + "' is an image collection: it makes " + std::to_string(data.sources.size()) + " sources, one per image");
                    }
                }

                std::map<std::string, PropertySchemaEntry> schema;
                for (const auto& [id, entry] : entries) {
                    const auto it = index.find(id);
                    if (it == index.end()) {
                        if (!image.empty()) warn("Tileset '" + data.name + "': tile " + std::to_string(id) + " is past the tile count and is skipped");
                        continue;
                    }
                    TileDef& tile = data.tiles[it->second];
                    const std::string tilePath = detail::keyPath(path, "tiles") + "[id " + std::to_string(id) + "]";
                    const int tileImageHeight = image.empty() ? data.sources[static_cast<size_t>(tile.source)].tileHeight : data.tileHeight;
                    readTileExtras(*entry, tilePath, tile, data, tileImageHeight, index, schema, ctx);
                }
                data.propertySchema.reserve(schema.size());
                for (auto& [name, entry] : schema) data.propertySchema.push_back(std::move(entry));

                if (const json* wangsets = detail::findKey(ts, "wangsets")) {
                    readWangSets(*wangsets, detail::keyPath(path, "wangsets"), data, index, ctx);
                }
            }

            void readTileExtras(const json& entry, const std::string& path, TileDef& tile, const TilesetData& data, int tileImageHeight,
                                const std::unordered_map<int32_t, size_t>& index, std::map<std::string, PropertySchemaEntry>& schema, const detail::SourceContext& ctx) {
                const double probability = detail::getDouble(entry, "probability", 1.0, ctx, path);
                tile.probability = probability < 0.0 || !std::isfinite(probability) ? 1.0f : static_cast<float>(probability);

                if (const json* frames = detail::findKey(entry, "animation")) {
                    if (!frames->is_array()) ctx.fail(detail::keyPath(path, "animation"), "expected an array", detail::lineOf(entry));
                    for (size_t i = 0; i < frames->size(); i++) {
                        const std::string framePath = detail::indexPath(detail::keyPath(path, "animation"), i);
                        const int frameTile = detail::requireInt((*frames)[i], "tileid", 0, MAX_TILE_ID, ctx, framePath);
                        const int duration = detail::boundedInt((*frames)[i], "duration", 100, 0, 3600000, ctx, framePath);
                        if (!index.count(frameTile)) {
                            warn("Tileset '" + data.name + "': tile " + std::to_string(tile.id) + "'s animation names the missing tile " + std::to_string(frameTile));
                            continue;
                        }
                        TileAnimationFrame frame;
                        frame.tile = frameTile;
                        frame.ms = std::max(1, duration);
                        tile.animation.push_back(frame);
                    }
                }

                if (const json* group = detail::findKey(entry, "objectgroup")) {
                    const json* objects = detail::findKey(*group, "objects");
                    if (objects) readCollision(*objects, detail::keyPath(detail::keyPath(path, "objectgroup"), "objects"), tile, data.name, tileImageHeight, ctx);
                }

                if (const json* properties = detail::findKey(entry, "properties")) {
                    if (!properties->is_array()) ctx.fail(detail::keyPath(path, "properties"), "expected an array", detail::lineOf(entry));
                    for (size_t i = 0; i < properties->size(); i++) readProperty((*properties)[i], detail::indexPath(detail::keyPath(path, "properties"), i), tile, data.name, schema, ctx);
                }
            }

            void readProperty(const json& p, const std::string& path, TileDef& tile, const std::string& tilesetName, std::map<std::string, PropertySchemaEntry>& schema, const detail::SourceContext& ctx) {
                if (!p.is_object()) ctx.fail(path, "expected a property object");
                const std::string name = detail::getString(p, "name", "", ctx, path);
                if (name.empty()) ctx.fail(detail::keyPath(path, "name"), "the property has no name", detail::lineOf(p));
                const std::string type = detail::getString(p, "type", "string", ctx, path);
                const json* valueNode = detail::findKey(p, "value");

                TilePropertyValue value;
                if (type == "bool") {
                    value.type = TilePropertyType::Bool;
                    value.b = valueNode ? detail::asBool(*valueNode, ctx, detail::keyPath(path, "value"), detail::lineOf(p)) : false;
                } else if (type == "int" || type == "object") {
                    value.type = TilePropertyType::Int;
                    value.i = valueNode ? detail::asInt(*valueNode, ctx, detail::keyPath(path, "value"), detail::lineOf(p)) : 0;
                } else if (type == "float") {
                    value.type = TilePropertyType::Float;
                    value.f = valueNode ? detail::asDouble(*valueNode, ctx, detail::keyPath(path, "value"), detail::lineOf(p)) : 0.0;
                } else if (type == "color") {
                    value.type = TilePropertyType::Color;
                    const std::string text = valueNode ? detail::asString(*valueNode, ctx, detail::keyPath(path, "value"), detail::lineOf(p)) : std::string();
                    if (!parseColor(text, value.color)) {
                        warn("Tileset '" + tilesetName + "': tile " + std::to_string(tile.id) + "'s colour property '" + name + "' has no colour and is skipped");
                        return;
                    }
                } else if (type == "string" || type == "file") {
                    value.type = TilePropertyType::String;
                    value.s = valueNode ? detail::asString(*valueNode, ctx, detail::keyPath(path, "value"), detail::lineOf(p)) : std::string();
                } else {
                    warn("Tileset '" + tilesetName + "': tile " + std::to_string(tile.id) + "'s property '" + name + "' of type " + type + " is skipped");
                    return;
                }

                auto [it, inserted] = schema.try_emplace(name);
                if (inserted) {
                    it->second.name = name;
                    it->second.type = value.type;
                    it->second.defaultValue.type = value.type;
                } else if (it->second.type != value.type) {
                    warn("Tileset '" + tilesetName + "': tile " + std::to_string(tile.id) + "'s property '" + name + "' changes type and is skipped");
                    return;
                }
                tile.properties[name] = std::move(value);
            }

            bool parseColor(const std::string& text, Vector4& out) const {
                std::string hex = text;
                if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
                if (hex.size() != 6 && hex.size() != 8) return false;
                unsigned values[4] = {255, 255, 255, 255};
                const size_t offset = hex.size() == 8 ? 2 : 0;
                for (size_t i = 0; i < hex.size(); i += 2) {
                    const auto parseHex = [](char c) {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int a = parseHex(hex[i]);
                    const int b = parseHex(hex[i + 1]);
                    if (a < 0 || b < 0) return false;
                    unsigned value = static_cast<unsigned>(a * 16 + b);
                    if (offset == 2 && i == 0) values[3] = value;
                    else values[(i - offset) / 2] = value;
                }
                out = Vector4(values[0] / 255.0f, values[1] / 255.0f, values[2] / 255.0f, values[3] / 255.0f);
                return true;
            }

            // ---- collision

            void readCollision(const json& objects, const std::string& path, TileDef& tile, const std::string& tilesetName, int tileImageHeight, const detail::SourceContext& ctx) {
                if (!objects.is_array()) ctx.fail(path, "expected an array of objects");
                int rounded = 0;
                for (size_t i = 0; i < objects.size(); i++) {
                    const json& o = objects[i];
                    const std::string objectPath = detail::indexPath(path, i);
                    if (!o.is_object()) ctx.fail(objectPath, "expected an object");
                    const std::string objectName = tilesetName + " tile " + std::to_string(tile.id);
                    if (detail::findKey(o, "polyline")) { plan.skipped.push_back({"collision", objectName + ": polyline"}); continue; }
                    if (detail::findKey(o, "point")) { plan.skipped.push_back({"collision", objectName + ": point"}); continue; }
                    if (detail::findKey(o, "text")) { plan.skipped.push_back({"collision", objectName + ": text"}); continue; }
                    if (detail::findKey(o, "template")) { plan.skipped.push_back({"collision", objectName + ": template"}); continue; }

                    const double x = detail::getDouble(o, "x", 0.0, ctx, objectPath);
                    const double y = detail::getDouble(o, "y", 0.0, ctx, objectPath);
                    const double width = detail::getDouble(o, "width", 0.0, ctx, objectPath);
                    const double height = detail::getDouble(o, "height", 0.0, ctx, objectPath);
                    const double rotation = detail::getDouble(o, "rotation", 0.0, ctx, objectPath);
                    std::vector<std::pair<double, double>> points;
                    if (const json* polygon = detail::findKey(o, "polygon")) {
                        if (!polygon->is_array()) ctx.fail(detail::keyPath(objectPath, "polygon"), "expected an array of points", detail::lineOf(o));
                        for (size_t p = 0; p < polygon->size(); p++) {
                            const std::string pointPath = detail::indexPath(detail::keyPath(objectPath, "polygon"), p);
                            points.emplace_back(detail::getDouble((*polygon)[p], "x", 0.0, ctx, pointPath), detail::getDouble((*polygon)[p], "y", 0.0, ctx, pointPath));
                        }
                    } else if (detail::findKey(o, "ellipse") && detail::asBool(*detail::findKey(o, "ellipse"), ctx, detail::keyPath(objectPath, "ellipse"), detail::lineOf(o))) {
                        for (int s = 0; s < 16; s++) {
                            const double angle = 3.14159265358979323846 * 2.0 * s / 16.0;
                            points.emplace_back(width * 0.5 + width * 0.5 * std::cos(angle), height * 0.5 + height * 0.5 * std::sin(angle));
                        }
                    } else {
                        points = {{0.0, 0.0}, {width, 0.0}, {width, height}, {0.0, height}};
                    }

                    const double radians = rotation * 3.14159265358979323846 / 180.0;
                    const double c = std::cos(radians), s = std::sin(radians);
                    TilePolygon polygon;
                    for (const auto& [px, py] : points) {
                        const double rx = x + px * c - py * s;
                        const double ry = y + px * s + py * c;
                        const double up = static_cast<double>(tileImageHeight) - ry;
                        if (!std::isfinite(rx) || !std::isfinite(up) || std::fabs(rx) > 1.0e6 || std::fabs(up) > 1.0e6) {
                            ctx.fail(objectPath, "the shape is too far from its tile", detail::lineOf(o));
                        }
                        const int32_t ix = static_cast<int32_t>(std::lround(rx));
                        const int32_t iy = static_cast<int32_t>(std::lround(up));
                        if (std::fabs(rx - ix) > 1.0e-6 || std::fabs(up - iy) > 1.0e-6) rounded++;
                        if (polygon.points.empty() || polygon.points.back() != std::array<int32_t, 2>{ix, iy}) polygon.points.push_back({ix, iy});
                    }
                    if (polygon.points.size() > 1 && polygon.points.front() == polygon.points.back()) polygon.points.pop_back();
                    int64_t area = 0;
                    const size_t n = polygon.points.size();
                    for (size_t p = 0; p < n; p++) {
                        const auto& a = polygon.points[p];
                        const auto& b = polygon.points[(p + 1) % n];
                        area += static_cast<int64_t>(a[0]) * b[1] - static_cast<int64_t>(b[0]) * a[1];
                    }
                    if (n < 3 || area == 0) {
                        plan.skipped.push_back({"collision", objectName + ": empty shape"});
                        continue;
                    }
                    // tiles' polygons are counter-clockwise (the collider builder's winding)
                    if (area < 0) std::reverse(polygon.points.begin(), polygon.points.end());
                    tile.collision.mode = TileCollisionMode::Custom;
                    tile.collision.polygons.push_back(std::move(polygon));
                }
                if (rounded > 0) warn("Tileset '" + tilesetName + "': tile " + std::to_string(tile.id) + "'s collision points were rounded to the pixel grid");
            }

            // ---- wang sets

            // Tiled's wang id runs clockwise from the top; our peering slots n ne e se s sw w nw run
            // the same way, but on (staggered) isometric maps Tiled's "top" is our "e" (x and y swap
            // and negate): the slot moves two steps
            void readWangSets(const json& sets, const std::string& path, TilesetData& data, const std::unordered_map<int32_t, size_t>& index, const detail::SourceContext& ctx) {
                if (!sets.is_array()) ctx.fail(path, "expected an array");
                const int rotation = diamondTiles ? 2 : 0;
                for (size_t i = 0; i < sets.size(); i++) {
                    const json& set = sets[i];
                    const std::string setPath = detail::indexPath(path, i);
                    if (!set.is_object()) ctx.fail(setPath, "expected an object");
                    const std::string name = detail::getString(set, "name", "", ctx, setPath);
                    const std::string type = detail::getString(set, "type", "", ctx, setPath);
                    if (hexLayout) {
                        plan.skipped.push_back({"wang_set", data.name + "/" + name + ": hexagonal"});
                        continue;
                    }
                    if (type != "edge" && type != "corner" && type != "mixed") {
                        warn("Wang set '" + name + "' of tileset '" + data.name + "' has the unknown type '" + type + "' and is skipped");
                        plan.skipped.push_back({"wang_set", data.name + "/" + name});
                        continue;
                    }
                    const json* colors = detail::findKey(set, "colors");
                    if (!colors || !colors->is_array() || colors->empty()) {
                        // Tiled < 1.5 wrote wangedgecolor and wangcornercolor elements instead
                        plan.skipped.push_back({"wang_set", data.name + "/" + name + ": old format"});
                        continue;
                    }
                    if (colors->size() > 254) ctx.fail(detail::keyPath(setPath, "colors"), "too many colours", detail::lineOf(set));
                    if (terrainTotal(data) + static_cast<int>(colors->size()) > MAX_TILESET_TERRAINS) {
                        warn("Wang set '" + name + "' of tileset '" + data.name + "' is skipped: the tileset would pass " + std::to_string(MAX_TILESET_TERRAINS) + " terrains");
                        plan.skipped.push_back({"wang_set", data.name + "/" + name + ": too many terrains"});
                        continue;
                    }
                    TerrainSetDef terrainSet;
                    terrainSet.name = name.empty() ? "Wang " + std::to_string(i) : name;
                    terrainSet.mode = type == "edge" ? TerrainMode::Sides : TerrainMode::Blob;
                    for (size_t c = 0; c < colors->size(); c++) {
                        TerrainDef terrain;
                        terrain.name = detail::getString((*colors)[c], "name", "Terrain " + std::to_string(c), ctx, detail::indexPath(detail::keyPath(setPath, "colors"), c));
                        terrain.color = detail::normalizeColor(detail::getString((*colors)[c], "color", "", ctx, detail::indexPath(detail::keyPath(setPath, "colors"), c)));
                        terrainSet.terrains.push_back(std::move(terrain));
                    }
                    const int setIndex = static_cast<int>(data.terrainSets.size());
                    data.terrainSets.push_back(std::move(terrainSet));

                    const json* wangTiles = detail::findKey(set, "wangtiles");
                    if (!wangTiles || !wangTiles->is_array()) continue;
                    for (size_t t = 0; t < wangTiles->size(); t++) {
                        const json& wt = (*wangTiles)[t];
                        const std::string wtPath = detail::indexPath(detail::keyPath(setPath, "wangtiles"), t);
                        if (!wt.is_object()) ctx.fail(wtPath, "expected an object");
                        const int tileId = detail::requireInt(wt, "tileid", 0, MAX_TILE_ID, ctx, wtPath);
                        const auto it = index.find(tileId);
                        if (it == index.end()) continue;
                        TileDef& tile = data.tiles[it->second];
                        if (tile.terrain.set >= 0) {
                            warn("Tile " + std::to_string(tileId) + " of tileset '" + data.name + "' is in two wang sets; it keeps the first");
                            continue;
                        }
                        int values[8] = {0};
                        if (!readWangId(wt, values)) {
                            plan.skipped.push_back({"wang_set", data.name + "/" + name + ": old wang id on tile " + std::to_string(tileId)});
                            continue;
                        }
                        int counts[256] = {0};
                        TileTerrain terrain;
                        terrain.set = setIndex;
                        for (int w = 0; w < 8; w++) {
                            const bool edgeSlot = w % 2 == 0;
                            const bool used = type == "mixed" || (type == "edge" && edgeSlot) || (type == "corner" && !edgeSlot);
                            if (values[w] < 0 || values[w] > static_cast<int>(colors->size())) ctx.fail(wtPath, "a wang id value is outside the set's colours", detail::lineOf(wt));
                            if (!used) continue;
                            const int slot = (w + rotation) % 8;
                            terrain.peering[static_cast<size_t>(slot)] = values[w] == 0 ? PEERING_NONE : static_cast<int16_t>(values[w] - 1);
                            if (values[w] > 0) counts[values[w] - 1]++;
                        }
                        int best = -1;
                        for (int c = 0; c < 256; c++) {
                            if (counts[c] > 0 && (best < 0 || counts[c] > counts[best])) best = c;
                        }
                        terrain.center = best;
                        tile.terrain = terrain;
                    }
                    if (type == "corner") warn("Terrain set '" + data.terrainSets[static_cast<size_t>(setIndex)].name + "' of tileset '" + data.name + "' comes from a corner wang set: imported as blob with sides any");
                }
            }

            static int terrainTotal(const TilesetData& data) {
                int total = 0;
                for (const TerrainSetDef& s : data.terrainSets) total += static_cast<int>(s.terrains.size());
                return total;
            }

            bool readWangId(const json& wt, int values[8]) const {
                const json* id = detail::findKey(wt, "wangid");
                if (!id) return false;
                if (id->is_array()) {
                    if (id->size() != 8) return false;
                    for (size_t i = 0; i < 8; i++) {
                        if (!(*id)[i].is_number_integer()) return false;
                        values[i] = (*id)[i].get<int>();
                    }
                    return true;
                }
                if (!id->is_string()) return false;
                const std::string& text = id->get_ref<const std::string&>();
                if (text.rfind("0x", 0) == 0) return false;   // Tiled < 1.5's packed hex form
                size_t pos = 0;
                for (int i = 0; i < 8; i++) {
                    const size_t comma = text.find(',', pos);
                    if ((comma == std::string::npos) != (i == 7)) return false;
                    int64_t value = 0;
                    const std::string item = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    if (!detail::parseIntText(item, value) || value < 0 || value > 255) return false;
                    values[i] = static_cast<int>(value);
                    pos = comma + 1;
                }
                return true;
            }

            // ---- layers

            void walkLayers(const json& layers, const std::string& path, const InheritedLayer& inherited, int depth, ImportGrid& grid, const detail::SourceContext& ctx) {
                if (depth > kMaxGroupDepth) ctx.fail(path, "groups nest deeper than " + std::to_string(kMaxGroupDepth));
                for (size_t i = 0; i < layers.size(); i++) {
                    const json& layer = layers[i];
                    const std::string layerPath = detail::indexPath(path, i);
                    if (!layer.is_object()) ctx.fail(layerPath, "expected a layer object");
                    const std::string name = detail::getString(layer, "name", "", ctx, layerPath);
                    const std::string fullName = inherited.prefix + name;
                    const std::string type = detail::getString(layer, "type", "", ctx, layerPath);

                    const double parallaxX = detail::getDouble(layer, "parallaxx", 1.0, ctx, layerPath);
                    const double parallaxY = detail::getDouble(layer, "parallaxy", 1.0, ctx, layerPath);
                    if (parallaxX != 1.0 || parallaxY != 1.0) plan.skipped.push_back({"parallax", fullName, 1});

                    InheritedLayer here = inherited;
                    here.visible = inherited.visible && detail::getBool(layer, "visible", true, ctx, layerPath);
                    here.opacity = inherited.opacity * detail::getDouble(layer, "opacity", 1.0, ctx, layerPath);
                    here.offsetX = inherited.offsetX + detail::getDouble(layer, "offsetx", 0.0, ctx, layerPath);
                    here.offsetY = inherited.offsetY + detail::getDouble(layer, "offsety", 0.0, ctx, layerPath);

                    if (type == "objectgroup") {
                        plan.skipped.push_back({"object_layer", fullName, 1});
                    } else if (type == "imagelayer") {
                        plan.skipped.push_back({"image_layer", fullName, 1});
                    } else if (type == "group") {
                        const json* children = detail::findKey(layer, "layers");
                        if (children && !children->is_array()) ctx.fail(detail::keyPath(layerPath, "layers"), "expected an array", detail::lineOf(layer));
                        if (children) {
                            InheritedLayer child = here;
                            child.prefix = fullName + "/";
                            walkLayers(*children, detail::keyPath(layerPath, "layers"), child, depth + 1, grid, ctx);
                        }
                    } else if (type == "tilelayer" || type == "layer") {
                        if (const std::string tint = detail::getString(layer, "tintcolor", "", ctx, layerPath); !tint.empty()) plan.skipped.push_back({"tint", fullName, 1});
                        readTileLayer(layer, layerPath, fullName, here, grid, ctx);
                    } else {
                        warn("Layer '" + fullName + "' has the unknown type '" + type + "' and is skipped");
                    }
                }
            }

            struct Bucket {
                std::vector<ImportCell> cells;
            };

            void readTileLayer(const json& layer, const std::string& path, const std::string& fullName, const InheritedLayer& inherited, ImportGrid& grid, const detail::SourceContext& ctx) {
                const int line = detail::lineOf(layer);
                std::map<int, Bucket> buckets;
                int64_t hexRotations = 0;
                int64_t badGids = 0;
                int chunkCount = 0;
                std::vector<uint32_t> gids;

                const std::string encoding = detail::getString(layer, "encoding", "", ctx, path);
                const std::string compression = detail::getString(layer, "compression", "", ctx, path);
                const auto consume = [&](int64_t originX, int64_t originY, int width, int height) {
                    for (int row = 0; row < height; row++) {
                        for (int col = 0; col < width; col++) {
                            const uint32_t raw = gids[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col)];
                            emit(raw, originX + col, originY + row, buckets, hexRotations, badGids, ctx, path, line);
                        }
                    }
                };
                const auto checkedCells = [&](int64_t width, int64_t height, const std::string& where) {
                    if (width < 1 || height < 1 || width > detail::kMaxLayerCells || height > detail::kMaxLayerCells || width * height > detail::kMaxLayerCells) ctx.fail(where, "the layer's size is out of range", line);
                    mapCells += width * height;
                    if (mapCells > detail::kMaxMapCells) ctx.fail(where, "the map has more than " + std::to_string(detail::kMaxMapCells) + " cells", line);
                };

                if (const json* chunks = detail::findKey(layer, "chunks")) {
                    if (!chunks->is_array()) ctx.fail(detail::keyPath(path, "chunks"), "expected an array", line);
                    for (size_t c = 0; c < chunks->size(); c++) {
                        const json& chunk = (*chunks)[c];
                        const std::string chunkPath = detail::indexPath(detail::keyPath(path, "chunks"), c);
                        if (!chunk.is_object()) ctx.fail(chunkPath, "expected a chunk object", line);
                        const int64_t cx = checkedCoord(chunk, "x", chunkPath, ctx);
                        const int64_t cy = checkedCoord(chunk, "y", chunkPath, ctx);
                        const int width = detail::requireInt(chunk, "width", 1, 1 << 24, ctx, chunkPath);
                        const int height = detail::requireInt(chunk, "height", 1, 1 << 24, ctx, chunkPath);
                        checkedCells(width, height, chunkPath);
                        const json* data = detail::findKey(chunk, "data");
                        if (!data) ctx.fail(detail::keyPath(chunkPath, "data"), "the chunk has no data", detail::lineOf(chunk));
                        decodeGids(*data, encoding, compression, static_cast<int64_t>(width) * height, gids, ctx, detail::keyPath(chunkPath, "data"), detail::lineOf(chunk));
                        consume(cx, cy, width, height);
                        chunkCount++;
                    }
                } else {
                    const int width = mapWidth > 0 ? detail::boundedInt(layer, "width", mapWidth, 1, 1 << 24, ctx, path) : detail::requireInt(layer, "width", 1, 1 << 24, ctx, path);
                    const int height = mapHeight > 0 ? detail::boundedInt(layer, "height", mapHeight, 1, 1 << 24, ctx, path) : detail::requireInt(layer, "height", 1, 1 << 24, ctx, path);
                    checkedCells(width, height, path);
                    const json* data = detail::findKey(layer, "data");
                    if (!data) ctx.fail(detail::keyPath(path, "data"), "the tile layer has no data", line);
                    decodeGids(*data, encoding, compression, static_cast<int64_t>(width) * height, gids, ctx, detail::keyPath(path, "data"), line);
                    consume(0, 0, width, height);
                }

                if (hexRotations > 0) plan.skipped.push_back({"hex_rotation", fullName, static_cast<int>(hexRotations)});
                if (badGids > 0) warn("Layer '" + fullName + "': " + std::to_string(badGids) + " cells name a GID no tileset covers and are empty");

                if (buckets.empty()) {
                    ImportLayer empty;
                    empty.name = uniqueLayerName(fullName);
                    applyInherited(empty, inherited);
                    empty.chunks = chunkCount;
                    grid.layers.push_back(std::move(empty));
                    return;
                }
                bool first = true;
                for (auto& [tilesetIndex, bucket] : buckets) {
                    ImportLayer out;
                    const std::string& tilesetName = plan.tilesets[static_cast<size_t>(tilesetIndex)].data.name;
                    out.name = uniqueLayerName(first ? fullName : fullName + " (" + tilesetName + ")");
                    if (!first) warn("Layer '" + fullName + "' uses several tilesets; the cells of '" + tilesetName + "' form the layer '" + out.name + "'");
                    first = false;
                    out.tileset = tilesetIndex;
                    out.cells = std::move(bucket.cells);
                    std::sort(out.cells.begin(), out.cells.end(), [](const ImportCell& a, const ImportCell& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
                    out.chunks = chunkCount;
                    applyInherited(out, inherited);
                    grid.layers.push_back(std::move(out));
                }
            }

            static void applyInherited(ImportLayer& layer, const InheritedLayer& inherited) {
                layer.visible = inherited.visible;
                layer.opacity = static_cast<float>(std::clamp(inherited.opacity, 0.0, 1.0));
                layer.offsetX = inherited.offsetX;
                layer.offsetY = -inherited.offsetY;   // y up
            }

            std::string uniqueLayerName(const std::string& name) {
                if (usedLayerNames.insert(name).second) return name;
                for (int i = 2;; i++) {
                    std::string candidate = name + " (" + std::to_string(i) + ")";
                    if (usedLayerNames.insert(candidate).second) {
                        warn("Two layers are called '" + name + "'; the later one is '" + candidate + "'");
                        return candidate;
                    }
                }
            }

            int64_t checkedCoord(const json& o, const char* key, const std::string& path, const detail::SourceContext& ctx) {
                const int64_t v = detail::getInt(o, key, 0, ctx, path);
                if (v < -detail::kMaxCoordinate || v > detail::kMaxCoordinate) ctx.fail(detail::keyPath(path, key), "is out of range", detail::lineOf(o));
                return v;
            }

            void emit(uint32_t raw, int64_t tiledX, int64_t tiledY, std::map<int, Bucket>& buckets, int64_t& hexRotations, int64_t& badGids, const detail::SourceContext& ctx, const std::string& path, int line) {
                const uint32_t gid = raw & kGidMask;
                if (gid == 0) return;
                if (gidTable.empty() || gid < gidTable.front().first) {
                    badGids++;
                    return;
                }
                auto it = std::upper_bound(gidTable.begin(), gidTable.end(), std::make_pair(gid, INT32_MAX));
                --it;
                const uint32_t local = gid - it->first;
                if (local > static_cast<uint32_t>(MAX_TILE_ID)) {
                    badGids++;
                    return;
                }
                uint32_t flags = 0;
                if (raw & kGidFlipH) flags |= TILE_FLIP_X;
                if (raw & kGidFlipV) flags |= TILE_FLIP_Y;
                if (raw & kGidRotateHex120) hexRotations++;
                if (raw & kGidFlipD) {
                    // on a hexagonal map Tiled's D bit is the 60-degree rotation: reported, as bit 28,
                    // because hexagon cells accept no transpose (R5)
                    if (hexLayout) hexRotations++;
                    else flags |= TILE_TRANSPOSE;
                }
                const int64_t yUpRow = static_cast<int64_t>(mapHeight) - 1 - tiledY;
                int64_t x, y;
                if (swapAxes) {
                    x = yUpRow;
                    y = static_cast<int64_t>(mapWidth) - 1 - tiledX;
                } else {
                    x = tiledX;
                    y = yUpRow;
                }
                if (x < -detail::kMaxCoordinate || x > detail::kMaxCoordinate || y < -detail::kMaxCoordinate || y > detail::kMaxCoordinate) ctx.fail(path, "a cell is out of range", line);
                buckets[it->second].cells.push_back(ImportCell{static_cast<int32_t>(x), static_cast<int32_t>(y), makeCellValue(static_cast<int32_t>(local), flags), 0});
            }

        };

    }

    bool parseTiled(std::string_view text, const std::string& fileName, const FileReader& readFile, ImportPlan& out, std::vector<ImportProblem>& problems) {
        out = ImportPlan();
        try {
            TiledParser parser(fileName, readFile, out);
            parser.run(text);
            return true;
        } catch (detail::ParseFailure& failure) {
            problems.push_back(std::move(failure.problem));
        } catch (const std::exception& e) {
            ImportProblem p;
            p.message = std::string("the file could not be read: ") + e.what();
            p.file = fileName;
            problems.push_back(std::move(p));
        }
        return false;
    }

}
