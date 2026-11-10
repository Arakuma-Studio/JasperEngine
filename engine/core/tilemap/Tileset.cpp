// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/Tileset.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

namespace {

    using TsJson = nlohmann::json;

    const char* const TS_SQUARE_KEYS[8] = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};
    const char* const TS_POINTY_KEYS[12] = {"e", "ne", "nw", "w", "sw", "se", "c_ne", "c_n", "c_nw", "c_sw", "c_s", "c_se"};
    const char* const TS_FLAT_KEYS[12] = {"ne", "n", "nw", "sw", "s", "se", "c_e", "c_ne", "c_nw", "c_w", "c_sw", "c_se"};

    bool tsIsHex(TileShape shape){
        return shape == TileShape::HexPointy || shape == TileShape::HexFlat;
    }

    int tsKeyCount(TileShape shape){
        return tsIsHex(shape) ? 12 : 8;
    }

    const char* tsOneWayName(TileOneWayDirection direction){
        switch (direction){
            case TileOneWayDirection::Up: return "up";
            case TileOneWayDirection::Down: return "down";
            case TileOneWayDirection::Left: return "left";
            case TileOneWayDirection::Right: return "right";
        }
        return "up";
    }

    // ---- writing: by hand, because the order of §6.2 and one tile per line are the format

    std::string tsQuoted(const std::string& text){
        return TsJson(text).dump(-1, ' ', false, TsJson::error_handler_t::replace);
    }

    // through nlohmann's float overload: the shortest text that reads back as this float (a double
    // path would print 0.3f as 0.30000001192...)
    std::string tsFloatText(float value){
        char buffer[64];
        char* end = nlohmann::detail::to_chars(buffer, buffer + sizeof(buffer), std::isfinite(value) ? value : 0.0f);
        return std::string(buffer, end);
    }

    std::string tsDoubleText(double value){
        return TsJson(std::isfinite(value) ? value : 0.0).dump();
    }

    class TsObject{
    public:
        explicit TsObject(std::string& target): out(target){
            out.push_back('{');
        }
        void raw(const char* key, const std::string& value){
            quoted(std::string("\"") + key + "\"", value);
        }
        void quoted(const std::string& key, const std::string& value){
            if (!first)
                out += ", ";
            first = false;
            out += key;
            out += ": ";
            out += value;
        }
        void end(){
            out.push_back('}');
        }

    private:
        std::string& out;
        bool first = true;
    };

    std::string tsColorText(const Vector4& color){
        const auto channel = [](float value){
            return (int)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
        };
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", channel(color.x), channel(color.y), channel(color.z), channel(color.w));
        return buffer;
    }

    std::string tsAssetRefText(const AssetRef& ref){
        std::string text;
        TsObject object(text);
        object.raw("path", tsQuoted(ref.path));
        if (!ref.guid.isNull())
            object.raw("guid", tsQuoted(ref.guid.str()));
        object.end();
        return text;
    }

    std::string tsPairText(int64_t a, int64_t b){
        return "[" + std::to_string(a) + ", " + std::to_string(b) + "]";
    }

    std::string tsPropertyText(const TilePropertyValue& value){
        switch (value.type){
            case TilePropertyType::Bool: return value.b ? "true" : "false";
            case TilePropertyType::Int: return std::to_string(value.i);
            case TilePropertyType::Float: return tsDoubleText(value.f);
            case TilePropertyType::String: return tsQuoted(value.s);
            case TilePropertyType::Color: return tsQuoted(tsColorText(value.color));
        }
        return "null";
    }

    std::string tsSourceText(const TilesetSource& source){
        std::string text;
        TsObject object(text);
        object.raw("id", std::to_string(source.id));
        object.raw("kind", tsQuoted(tileset::sourceKindName(source.kind)));
        switch (source.kind){
            case TilesetSourceKind::Texture:
                object.raw("texture", tsAssetRefText(source.asset));
                if (source.tileWidth != 0 || source.tileHeight != 0)
                    object.raw("tile_size", tsPairText(source.tileWidth, source.tileHeight));
                object.raw("margin", std::to_string(source.margin));
                object.raw("spacing", std::to_string(source.spacing));
                object.raw("skip_empty", source.skipEmpty ? "true" : "false");
                break;
            case TilesetSourceKind::Sheet:{
                object.raw("sheet", tsAssetRefText(source.asset));
                std::string frames = "[";
                for (size_t i = 0; i < source.frames.size(); i++)
                    frames += (i ? ", " : "") + tsQuoted(source.frames[i]);
                object.raw("frames", frames + "]");
                break;
            }
            case TilesetSourceKind::Aseprite:
                object.raw("file", tsAssetRefText(source.asset));
                object.raw("tileset", std::to_string(source.asepriteTileset));
                break;
        }
        object.end();
        return text;
    }

    std::string tsTerrainSetText(const TerrainSetDef& set){
        std::string text;
        TsObject object(text);
        object.raw("name", tsQuoted(set.name));
        object.raw("mode", tsQuoted(tileset::terrainModeName(set.mode)));
        if (set.twoSurface)
            object.raw("two_surface", "true");
        std::string terrains = "[";
        for (size_t i = 0; i < set.terrains.size(); i++){
            std::string entry;
            TsObject terrain(entry);
            terrain.raw("name", tsQuoted(set.terrains[i].name));
            terrain.raw("color", tsQuoted(set.terrains[i].color));
            terrain.end();
            terrains += (i ? ", " : "") + entry;
        }
        object.raw("terrains", terrains + "]");
        object.end();
        return text;
    }

    std::string tsPeeringValueText(int16_t value){
        if (value == PEERING_ANY)
            return "\"any\"";
        if (value == PEERING_NONE)
            return "\"none\"";
        return std::to_string(value);
    }

    std::string tsPolygonsText(const std::vector<TilePolygon>& polygons){
        std::string text = "[";
        for (size_t p = 0; p < polygons.size(); p++){
            text += p ? ", [" : "[";
            for (size_t i = 0; i < polygons[p].points.size(); i++)
                text += (i ? ", " : "") + tsPairText(polygons[p].points[i][0], polygons[p].points[i][1]);
            text += "]";
        }
        return text + "]";
    }

    std::string tsCollisionText(const TileCollision& collision){
        std::string text;
        TsObject object(text);
        object.raw("mode", tsQuoted(tileset::collisionModeName(collision.mode)));
        if (collision.mode == TileCollisionMode::Alpha || collision.threshold != 128)
            object.raw("threshold", std::to_string(collision.threshold));
        object.raw("kind", tsQuoted(tileset::collisionKindName(collision.kind)));
        if (collision.oneWay != TileOneWayDirection::Up)
            object.raw("direction", tsQuoted(tsOneWayName(collision.oneWay)));
        if (!collision.polygons.empty())
            object.raw("polygons", tsPolygonsText(collision.polygons));
        object.end();
        return text;
    }

    std::string tsTerrainText(const TilesetData& data, const TileTerrain& terrain){
        std::string text;
        TsObject object(text);
        object.raw("set", std::to_string(terrain.set));
        if (terrain.center != -1)
            object.raw("center", std::to_string(terrain.center));
        std::vector<int> slots;
        if (terrain.set >= 0 && (size_t)terrain.set < data.terrainSets.size())
            slots = tileset::peeringSlots(data.shape, data.terrainSets[(size_t)terrain.set].mode);
        std::string peering;
        TsObject values(peering);
        for (int slot = 0; slot < tsKeyCount(data.shape); slot++){
            const bool used = std::find(slots.begin(), slots.end(), slot) != slots.end();
            if (used || terrain.peering[(size_t)slot] != PEERING_ANY)
                values.raw(tileset::peeringKey(data.shape, slot), tsPeeringValueText(terrain.peering[(size_t)slot]));
        }
        values.end();
        object.raw("peering", peering);
        object.end();
        return text;
    }

    std::string tsTileText(const TilesetData& data, const TileDef& tile){
        std::string text;
        TsObject object(text);
        object.raw("id", std::to_string(tile.id));
        object.raw("source", std::to_string(tile.source));
        if (tile.frame.empty() || tile.atX != 0 || tile.atY != 0)
            object.raw("at", tsPairText(tile.atX, tile.atY));
        if (!tile.frame.empty())
            object.raw("frame", tsQuoted(tile.frame));
        if (!tile.name.empty())
            object.raw("name", tsQuoted(tile.name));
        if (tile.offsetX != 0 || tile.offsetY != 0)
            object.raw("offset", tsPairText(tile.offsetX, tile.offsetY));
        if (tile.probability != 1.0f)
            object.raw("probability", tsFloatText(tile.probability));
        if (!tile.properties.empty()){
            std::string properties;
            TsObject values(properties);
            for (const auto& [name, value] : tile.properties)
                values.quoted(tsQuoted(name), tsPropertyText(value));
            values.end();
            object.raw("properties", properties);
        }
        if (!(tile.collision == TileCollision()))
            object.raw("collision", tsCollisionText(tile.collision));
        if (!tile.animation.empty()){
            std::string frames = "[";
            for (size_t i = 0; i < tile.animation.size(); i++){
                std::string entry;
                TsObject frame(entry);
                frame.raw("tile", std::to_string(tile.animation[i].tile));
                frame.raw("ms", std::to_string(tile.animation[i].ms));
                frame.end();
                frames += (i ? ", " : "") + entry;
            }
            object.raw("animation", frames + "]");
        }
        if (tile.randomStart)
            object.raw("random_start", "true");
        if (!(tile.terrain == TileTerrain()))
            object.raw("terrain", tsTerrainText(data, tile.terrain));
        if (tile.navArea != -1)
            object.raw("nav_area", std::to_string(tile.navArea));
        object.end();
        return text;
    }

    std::string tsStampText(const TileStamp& stamp){
        std::string text;
        TsObject object(text);
        object.raw("name", tsQuoted(stamp.name));
        object.raw("size", tsPairText(stamp.width, stamp.height));
        std::string cells = "[";
        for (size_t i = 0; i < stamp.cells.size(); i++){
            const TileStampCell& cell = stamp.cells[i];
            cells += i ? ", [" : "[";
            cells += std::to_string(cell.x) + ", " + std::to_string(cell.y) + ", " + std::to_string(cell.tile);
            std::string flags;
            if (cell.flags & TILE_FLIP_X) flags += 'x';
            if (cell.flags & TILE_FLIP_Y) flags += 'y';
            if (cell.flags & TILE_TRANSPOSE) flags += 't';
            if (!flags.empty())
                cells += ", \"" + flags + "\"";
            cells += "]";
        }
        object.raw("cells", cells + "]");
        object.end();
        return text;
    }

    template<class T, class F>
    void tsWriteArray(std::string& out, const char* key, const std::vector<T>& items, F&& itemText, bool last){
        out += "  \"";
        out += key;
        out += "\": [";
        if (!items.empty()){
            out += "\n";
            for (size_t i = 0; i < items.size(); i++){
                out += "    ";
                out += itemText(items[i]);
                out += i + 1 < items.size() ? ",\n" : "\n";
            }
            out += "  ";
        }
        out += last ? "]\n" : "],\n";
    }

    // ---- reading

    // nlohmann's DOM builder without exceptions, keeping where the text stopped being JSON
    class TsSaxParser : public nlohmann::detail::json_sax_dom_parser<TsJson>{
    public:
        explicit TsSaxParser(TsJson& root): json_sax_dom_parser<TsJson>(root, false){}

        template<class Exception>
        bool parse_error(std::size_t position, const std::string& lastToken, const Exception& ex){
            failed = true;
            errorPosition = position;
            message = ex.what();
            return json_sax_dom_parser<TsJson>::parse_error(position, lastToken, ex);
        }

        bool failed = false;
        std::size_t errorPosition = 0;
        std::string message;
    };

    class TsReader{
    public:
        explicit TsReader(std::vector<TilesetProblem>& list): problems(list){}

        // false, so that a caller can `return fail(...)`
        bool fail(const std::string& field, const std::string& message, const std::string& fix = std::string()){
            problems.push_back(TilesetProblem{"parse", field, message, fix});
            return false;
        }

        void warn(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix){
            problems.push_back(TilesetProblem{kind, field, message, fix});
        }

        void unknownKeys(const TsJson& object, const std::string& path, std::initializer_list<const char*> known){
            for (auto it = object.begin(); it != object.end(); ++it){
                bool found = false;
                for (const char* name : known)
                    found = found || it.key() == name;
                if (!found)
                    warn("parse", path.empty() ? it.key() : path + "." + it.key(), "unknown key, skipped", "remove the key");
            }
        }

        bool object(const TsJson& value, const std::string& path){
            return value.is_object() || fail(path, "expected an object");
        }

        bool integer(const TsJson& value, const std::string& path, int64_t minValue, int64_t maxValue, int64_t& out){
            if (!value.is_number_integer())
                return fail(path, "expected an integer");
            const bool isUnsigned = value.is_number_unsigned();
            if (isUnsigned && value.get<uint64_t>() > (uint64_t)maxValue)
                return fail(path, "the integer is out of range (at most " + std::to_string(maxValue) + ")");
            out = isUnsigned ? (int64_t)value.get<uint64_t>() : value.get<int64_t>();
            if (out < minValue || out > maxValue)
                return fail(path, "the integer " + std::to_string(out) + " is out of range (" + std::to_string(minValue) + " to " + std::to_string(maxValue) + ")");
            return true;
        }

        bool intField(const TsJson& parent, const char* key, const std::string& path, int minValue, int maxValue, int& out, bool required = false){
            auto it = parent.find(key);
            if (it == parent.end())
                return !required || fail(field(path, key), "missing required key");
            int64_t value = 0;
            if (!integer(*it, field(path, key), minValue, maxValue, value))
                return false;
            out = (int)value;
            return true;
        }

        bool boolField(const TsJson& parent, const char* key, const std::string& path, bool& out){
            auto it = parent.find(key);
            if (it == parent.end())
                return true;
            if (!it->is_boolean())
                return fail(field(path, key), "expected true or false");
            out = it->get<bool>();
            return true;
        }

        bool stringField(const TsJson& parent, const char* key, const std::string& path, std::string& out, bool required = false){
            auto it = parent.find(key);
            if (it == parent.end())
                return !required || fail(field(path, key), "missing required key");
            if (!it->is_string())
                return fail(field(path, key), "expected a string");
            out = it->get<std::string>();
            return true;
        }

        bool pairField(const TsJson& parent, const char* key, const std::string& path, int minValue, int maxValue, int& a, int& b){
            auto it = parent.find(key);
            if (it == parent.end())
                return true;
            const std::string where = field(path, key);
            if (!it->is_array() || it->size() != 2)
                return fail(where, "expected an array of two integers");
            int64_t first = 0, second = 0;
            if (!integer((*it)[0], where + "[0]", minValue, maxValue, first) || !integer((*it)[1], where + "[1]", minValue, maxValue, second))
                return false;
            a = (int)first;
            b = (int)second;
            return true;
        }

        bool assetRef(const TsJson& parent, const char* key, const std::string& path, AssetRef& out){
            auto it = parent.find(key);
            const std::string where = field(path, key);
            if (it == parent.end())
                return fail(where, "missing required key");
            if (it->is_string()){
                const std::string text = it->get<std::string>();
                if (text.rfind("guid:", 0) == 0){
                    const Guid guid = Guid::parse(std::string_view(text).substr(5));
                    if (guid.isNull())
                        return fail(where, "\"" + text + "\" is not guid: and 32 hex digits");
                    out.set(std::string(), guid);
                }else{
                    out.set(text, Guid());
                }
                return true;
            }
            if (!object(*it, where))
                return false;
            unknownKeys(*it, where, {"path", "guid"});
            std::string assetPath, guidText;
            if (!stringField(*it, "path", where, assetPath) || !stringField(*it, "guid", where, guidText))
                return false;
            Guid guid;
            if (!guidText.empty()){
                guid = Guid::parse(guidText);
                if (guid.isNull())
                    return fail(where + ".guid", "\"" + guidText + "\" is not 32 hex digits");
            }
            out.set(assetPath, guid);
            return true;
        }

        static std::string field(const std::string& path, const char* key){
            return path.empty() ? std::string(key) : path + "." + key;
        }

    private:
        std::vector<TilesetProblem>& problems;
    };

    int tsHexDigit(char c){
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool tsParseColor(const std::string& text, Vector4& out){
        if ((text.size() != 7 && text.size() != 9) || text[0] != '#')
            return false;
        int channels[4] = {0, 0, 0, 255};
        for (size_t i = 0; i < (text.size() - 1) / 2; i++){
            const int hi = tsHexDigit(text[1 + i * 2]), lo = tsHexDigit(text[2 + i * 2]);
            if (hi < 0 || lo < 0)
                return false;
            channels[i] = hi * 16 + lo;
        }
        out = Vector4((float)channels[0] / 255.0f, (float)channels[1] / 255.0f, (float)channels[2] / 255.0f, (float)channels[3] / 255.0f);
        return true;
    }

    bool tsPropertyFromJson(const TsJson& value, TilePropertyType type, TilePropertyValue& out){
        out.type = type;
        switch (type){
            case TilePropertyType::Bool:
                if (!value.is_boolean()) return false;
                out.b = value.get<bool>();
                return true;
            case TilePropertyType::Int:
                if (!value.is_number_integer()) return false;
                out.i = value.is_number_unsigned() ? (int64_t)value.get<uint64_t>() : value.get<int64_t>();
                return true;
            case TilePropertyType::Float:
                if (!value.is_number()) return false;
                out.f = value.get<double>();
                return true;
            case TilePropertyType::String:
                if (!value.is_string()) return false;
                out.s = value.get<std::string>();
                return true;
            case TilePropertyType::Color:
                return value.is_string() && tsParseColor(value.get<std::string>(), out.color);
        }
        return false;
    }

    // an undeclared property has no schema to say its type, so its JSON type stands in until
    // validate() refuses it
    TilePropertyType tsInferPropertyType(const TsJson& value){
        if (value.is_boolean()) return TilePropertyType::Bool;
        if (value.is_number_integer()) return TilePropertyType::Int;
        if (value.is_number()) return TilePropertyType::Float;
        return TilePropertyType::String;
    }

    bool tsParseSource(TsReader& reader, const TsJson& j, const std::string& path, TilesetSource& source){
        if (!reader.object(j, path))
            return false;
        reader.unknownKeys(j, path, {"id", "kind", "texture", "sheet", "file", "tile_size", "margin", "spacing", "skip_empty", "frames", "tileset"});
        std::string kindText;
        if (!reader.intField(j, "id", path, 0, 1 << 24, source.id, true) || !reader.stringField(j, "kind", path, kindText, true))
            return false;
        if (!tileset::parseSourceKind(kindText, source.kind))
            return reader.fail(TsReader::field(path, "kind"), "unknown source kind \"" + kindText + "\"", "use texture, sheet or aseprite");
        switch (source.kind){
            case TilesetSourceKind::Texture:
                return reader.assetRef(j, "texture", path, source.asset) && reader.pairField(j, "tile_size", path, 0, 1 << 16, source.tileWidth, source.tileHeight) &&
                       reader.intField(j, "margin", path, 0, 1 << 16, source.margin) && reader.intField(j, "spacing", path, 0, 1 << 16, source.spacing) &&
                       reader.boolField(j, "skip_empty", path, source.skipEmpty);
            case TilesetSourceKind::Sheet:{
                if (!reader.assetRef(j, "sheet", path, source.asset))
                    return false;
                auto frames = j.find("frames");
                if (frames == j.end())
                    return true;
                if (!frames->is_array())
                    return reader.fail(TsReader::field(path, "frames"), "expected an array of frame names");
                for (size_t i = 0; i < frames->size(); i++){
                    if (!(*frames)[i].is_string())
                        return reader.fail(TsReader::field(path, "frames") + "[" + std::to_string(i) + "]", "expected a frame name");
                    source.frames.push_back((*frames)[i].get<std::string>());
                }
                return true;
            }
            case TilesetSourceKind::Aseprite:
                return reader.assetRef(j, "file", path, source.asset) && reader.intField(j, "tileset", path, 0, 1 << 16, source.asepriteTileset);
        }
        return false;
    }

    bool tsParseCollision(TsReader& reader, const TsJson& j, const std::string& path, TileCollision& collision){
        if (!reader.object(j, path))
            return false;
        reader.unknownKeys(j, path, {"mode", "threshold", "kind", "direction", "polygons"});
        std::string mode, kind = "solid", direction = "up";
        if (!reader.stringField(j, "mode", path, mode, true) || !reader.stringField(j, "kind", path, kind) || !reader.stringField(j, "direction", path, direction) ||
            !reader.intField(j, "threshold", path, 0, 256, collision.threshold))
            return false;
        bool found = false;
        for (TileCollisionMode candidate : {TileCollisionMode::None, TileCollisionMode::Full, TileCollisionMode::Alpha, TileCollisionMode::Custom}){
            if (mode == tileset::collisionModeName(candidate)){
                collision.mode = candidate;
                found = true;
            }
        }
        if (!found)
            return reader.fail(TsReader::field(path, "mode"), "unknown collision mode \"" + mode + "\"", "use none, full, alpha or custom");
        found = false;
        for (TileCollisionKind candidate : {TileCollisionKind::Solid, TileCollisionKind::OneWay, TileCollisionKind::Sensor}){
            if (kind == tileset::collisionKindName(candidate)){
                collision.kind = candidate;
                found = true;
            }
        }
        if (!found)
            return reader.fail(TsReader::field(path, "kind"), "unknown collision kind \"" + kind + "\"", "use solid, one_way or sensor");
        found = false;
        for (TileOneWayDirection candidate : {TileOneWayDirection::Up, TileOneWayDirection::Down, TileOneWayDirection::Left, TileOneWayDirection::Right}){
            if (direction == tsOneWayName(candidate)){
                collision.oneWay = candidate;
                found = true;
            }
        }
        if (!found)
            return reader.fail(TsReader::field(path, "direction"), "unknown one-way direction \"" + direction + "\"", "use up, down, left or right");
        auto polygons = j.find("polygons");
        if (polygons == j.end())
            return true;
        const std::string where = TsReader::field(path, "polygons");
        if (!polygons->is_array())
            return reader.fail(where, "expected an array of polygons");
        for (size_t p = 0; p < polygons->size(); p++){
            const TsJson& points = (*polygons)[p];
            const std::string polygonPath = where + "[" + std::to_string(p) + "]";
            if (!points.is_array())
                return reader.fail(polygonPath, "expected an array of [x, y] points");
            TilePolygon polygon;
            for (size_t i = 0; i < points.size(); i++){
                const std::string pointPath = polygonPath + "[" + std::to_string(i) + "]";
                if (!points[i].is_array() || points[i].size() != 2)
                    return reader.fail(pointPath, "expected [x, y]");
                int64_t x = 0, y = 0;
                if (!reader.integer(points[i][0], pointPath + "[0]", INT32_MIN, INT32_MAX, x) || !reader.integer(points[i][1], pointPath + "[1]", INT32_MIN, INT32_MAX, y))
                    return false;
                polygon.points.push_back({(int32_t)x, (int32_t)y});
            }
            collision.polygons.push_back(std::move(polygon));
        }
        return true;
    }

    bool tsParseTerrain(TsReader& reader, const TilesetData& data, const TsJson& j, const std::string& path, TileTerrain& terrain){
        if (!reader.object(j, path))
            return false;
        reader.unknownKeys(j, path, {"set", "center", "peering"});
        if (!reader.intField(j, "set", path, 0, 1 << 16, terrain.set, true) || !reader.intField(j, "center", path, -1, 1 << 16, terrain.center))
            return false;
        auto peering = j.find("peering");
        if (peering == j.end())
            return true;
        const std::string where = TsReader::field(path, "peering");
        if (!reader.object(*peering, where))
            return false;
        for (auto it = peering->begin(); it != peering->end(); ++it){
            const std::string slotPath = where + "." + it.key();
            const int slot = tileset::peeringSlotOfKey(data.shape, it.key());
            if (slot < 0){
                reader.warn("invalid_peering", slotPath, "not a peering direction of this tile shape, skipped", "remove the key");
                continue;
            }
            if (it->is_string()){
                const std::string text = it->get<std::string>();
                if (text == "any")
                    terrain.peering[(size_t)slot] = PEERING_ANY;
                else if (text == "none")
                    terrain.peering[(size_t)slot] = PEERING_NONE;
                else
                    return reader.fail(slotPath, "expected any, none or a terrain index");
            }else{
                int64_t value = 0;
                if (!reader.integer(*it, slotPath, 0, INT16_MAX, value))
                    return false;
                terrain.peering[(size_t)slot] = (int16_t)value;
            }
        }
        return true;
    }

    bool tsParseTile(TsReader& reader, TilesetData& data, const TsJson& j, const std::string& path, TileDef& tile){
        if (!reader.object(j, path))
            return false;
        reader.unknownKeys(j, path, {"id", "source", "at", "frame", "name", "offset", "probability", "properties", "collision", "animation", "random_start", "terrain", "nav_area"});
        if (!reader.intField(j, "id", path, 0, MAX_TILE_ID, tile.id, true) || !reader.intField(j, "source", path, 0, 1 << 24, tile.source, true) ||
            !reader.pairField(j, "at", path, 0, 1 << 24, tile.atX, tile.atY) || !reader.stringField(j, "frame", path, tile.frame) ||
            !reader.stringField(j, "name", path, tile.name) || !reader.pairField(j, "offset", path, -(1 << 24), 1 << 24, tile.offsetX, tile.offsetY) ||
            !reader.boolField(j, "random_start", path, tile.randomStart) || !reader.intField(j, "nav_area", path, -1, 1 << 16, tile.navArea))
            return false;
        auto probability = j.find("probability");
        if (probability != j.end()){
            if (!probability->is_number() || !std::isfinite(probability->get<double>()) || probability->get<double>() < 0.0)
                return reader.fail(TsReader::field(path, "probability"), "expected a number >= 0");
            tile.probability = (float)probability->get<double>();
        }
        auto collision = j.find("collision");
        if (collision != j.end() && !tsParseCollision(reader, *collision, TsReader::field(path, "collision"), tile.collision))
            return false;
        auto terrain = j.find("terrain");
        if (terrain != j.end() && !tsParseTerrain(reader, data, *terrain, TsReader::field(path, "terrain"), tile.terrain))
            return false;
        auto animation = j.find("animation");
        if (animation != j.end()){
            const std::string where = TsReader::field(path, "animation");
            if (!animation->is_array())
                return reader.fail(where, "expected an array of frames");
            for (size_t i = 0; i < animation->size(); i++){
                const std::string framePath = where + "[" + std::to_string(i) + "]";
                if (!reader.object((*animation)[i], framePath))
                    return false;
                reader.unknownKeys((*animation)[i], framePath, {"tile", "ms"});
                TileAnimationFrame frame;
                if (!reader.intField((*animation)[i], "tile", framePath, 0, MAX_TILE_ID, frame.tile, true) || !reader.intField((*animation)[i], "ms", framePath, 1, INT32_MAX, frame.ms))
                    return false;
                tile.animation.push_back(frame);
            }
        }
        auto properties = j.find("properties");
        if (properties != j.end()){
            const std::string where = TsReader::field(path, "properties");
            if (!reader.object(*properties, where))
                return false;
            for (auto it = properties->begin(); it != properties->end(); ++it){
                TilePropertyValue value;
                const PropertySchemaEntry* entry = data.findProperty(it.key());
                const TilePropertyType type = entry ? entry->type : tsInferPropertyType(*it);
                if (!tsPropertyFromJson(*it, type, value)){
                    reader.warn("parse", where + "." + it.key(), std::string("not a ") + tileset::propertyTypeName(type) + " value, skipped", "give a value of the declared type");
                    continue;
                }
                tile.properties[it.key()] = std::move(value);
            }
        }
        return true;
    }

    bool tsParseStamp(TsReader& reader, const TsJson& j, const std::string& path, TileStamp& stamp){
        if (!reader.object(j, path))
            return false;
        reader.unknownKeys(j, path, {"name", "size", "cells"});
        if (!reader.stringField(j, "name", path, stamp.name) || !reader.pairField(j, "size", path, 1, 1 << 16, stamp.width, stamp.height))
            return false;
        auto cells = j.find("cells");
        if (cells == j.end())
            return true;
        const std::string where = TsReader::field(path, "cells");
        if (!cells->is_array())
            return reader.fail(where, "expected an array of [x, y, tile] cells");
        for (size_t i = 0; i < cells->size(); i++){
            const TsJson& entry = (*cells)[i];
            const std::string cellPath = where + "[" + std::to_string(i) + "]";
            if (!entry.is_array() || entry.size() < 3 || entry.size() > 4)
                return reader.fail(cellPath, "expected [x, y, tile] or [x, y, tile, flags]");
            int64_t x = 0, y = 0, tile = 0;
            if (!reader.integer(entry[0], cellPath + "[0]", INT32_MIN, INT32_MAX, x) || !reader.integer(entry[1], cellPath + "[1]", INT32_MIN, INT32_MAX, y) ||
                !reader.integer(entry[2], cellPath + "[2]", 0, MAX_TILE_ID, tile))
                return false;
            TileStampCell cell{(int32_t)x, (int32_t)y, (int32_t)tile, 0};
            if (entry.size() == 4){
                if (!entry[3].is_string())
                    return reader.fail(cellPath + "[3]", "expected flip letters (x, y, t)");
                for (char letter : entry[3].get<std::string>()){
                    if (letter == 'x') cell.flags |= TILE_FLIP_X;
                    else if (letter == 'y') cell.flags |= TILE_FLIP_Y;
                    else if (letter == 't') cell.flags |= TILE_TRANSPOSE;
                    else return reader.fail(cellPath + "[3]", std::string("unknown flip letter '") + letter + "'", "use x, y and t");
                }
            }
            stamp.cells.push_back(cell);
        }
        return true;
    }

    bool tsParseTerrainSet(TsReader& reader, const TsJson& j, const std::string& path, TerrainSetDef& set){
        if (!reader.object(j, path))
            return false;
        reader.unknownKeys(j, path, {"name", "mode", "two_surface", "terrains"});
        std::string mode;
        if (!reader.stringField(j, "name", path, set.name) || !reader.stringField(j, "mode", path, mode, true) || !reader.boolField(j, "two_surface", path, set.twoSurface))
            return false;
        if (!tileset::parseTerrainMode(mode, set.mode))
            return reader.fail(TsReader::field(path, "mode"), "unknown terrain mode \"" + mode + "\"", "use blob, sides or dual_grid");
        auto terrains = j.find("terrains");
        if (terrains == j.end())
            return true;
        const std::string where = TsReader::field(path, "terrains");
        if (!terrains->is_array())
            return reader.fail(where, "expected an array of terrains");
        for (size_t i = 0; i < terrains->size(); i++){
            const std::string terrainPath = where + "[" + std::to_string(i) + "]";
            if (!reader.object((*terrains)[i], terrainPath))
                return false;
            reader.unknownKeys((*terrains)[i], terrainPath, {"name", "color"});
            TerrainDef terrain;
            if (!reader.stringField((*terrains)[i], "name", terrainPath, terrain.name, true) || !reader.stringField((*terrains)[i], "color", terrainPath, terrain.color))
                return false;
            set.terrains.push_back(std::move(terrain));
        }
        return true;
    }

    bool tsParseSchema(TsReader& reader, const TsJson& j, const std::string& path, PropertySchemaEntry& entry){
        if (!reader.object(j, path))
            return false;
        reader.unknownKeys(j, path, {"name", "type", "default"});
        std::string type;
        if (!reader.stringField(j, "name", path, entry.name, true) || !reader.stringField(j, "type", path, type, true))
            return false;
        if (!tileset::parsePropertyType(type, entry.type))
            return reader.fail(TsReader::field(path, "type"), "unknown property type \"" + type + "\"", "use bool, int, float, string or color");
        entry.defaultValue.type = entry.type;
        auto defaultValue = j.find("default");
        if (defaultValue != j.end() && !tsPropertyFromJson(*defaultValue, entry.type, entry.defaultValue))
            return reader.fail(TsReader::field(path, "default"), std::string("not a ") + tileset::propertyTypeName(entry.type) + " value");
        return true;
    }

    bool tsSourcesEqual(const TilesetSource& a, const TilesetSource& b){
        return a.id == b.id && a.kind == b.kind && a.asset == b.asset && a.tileWidth == b.tileWidth && a.tileHeight == b.tileHeight && a.margin == b.margin &&
               a.spacing == b.spacing && a.skipEmpty == b.skipEmpty && a.frames == b.frames && a.asepriteTileset == b.asepriteTileset;
    }

    constexpr uint32_t TS_HASH_C1 = 0xcc9e2d51u;
    constexpr uint32_t TS_HASH_C2 = 0x1b873593u;

    // MurmurHash3's block step (Austin Appleby, public domain): the bytes of std::hash differ
    // between standard libraries, and a map must paint the same tiles everywhere (R28)
    uint32_t tsHashMix(uint32_t h, uint32_t k){
        k *= TS_HASH_C1;
        k = (k << 15) | (k >> 17);
        k *= TS_HASH_C2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        return h * 5u + 0xe6546b64u;
    }

}

bool TilePropertyValue::operator==(const TilePropertyValue& o) const{
    if (type != o.type)
        return false;
    switch (type){
        case TilePropertyType::Bool: return b == o.b;
        case TilePropertyType::Int: return i == o.i;
        case TilePropertyType::Float: return f == o.f;
        case TilePropertyType::String: return s == o.s;
        case TilePropertyType::Color: return color.x == o.color.x && color.y == o.color.y && color.z == o.color.z && color.w == o.color.w;
    }
    return false;
}

bool TileDef::operator==(const TileDef& o) const{
    return id == o.id && source == o.source && atX == o.atX && atY == o.atY && frame == o.frame && name == o.name && offsetX == o.offsetX && offsetY == o.offsetY &&
           probability == o.probability && collision == o.collision && animation == o.animation && randomStart == o.randomStart && terrain == o.terrain &&
           properties == o.properties && navArea == o.navArea;
}

const TileDef* TilesetData::findTile(int32_t id) const{
    auto it = std::lower_bound(tiles.begin(), tiles.end(), id, [](const TileDef& tile, int32_t value){ return tile.id < value; });
    return it != tiles.end() && it->id == id ? &*it : nullptr;
}

TileDef* TilesetData::findTile(int32_t id){
    return const_cast<TileDef*>(static_cast<const TilesetData*>(this)->findTile(id));
}

int32_t TilesetData::findTileByName(std::string_view wanted) const{
    std::unordered_map<int, int> indices;
    for (const TileDef& tile : tiles){
        const int index = indices[tile.source]++;
        if (!tile.name.empty()){
            if (tile.name == wanted)
                return tile.id;
        }else if (std::to_string(tile.source) + "_" + std::to_string(index) == wanted){
            return tile.id;
        }
    }
    return NO_TILE;
}

std::string TilesetData::tileName(const TileDef& tile) const{
    if (!tile.name.empty())
        return tile.name;
    int index = 0;
    for (const TileDef& other : tiles){
        if (other.id == tile.id)
            break;
        if (other.source == tile.source)
            index++;
    }
    return std::to_string(tile.source) + "_" + std::to_string(index);
}

const TilesetSource* TilesetData::findSource(int id) const{
    for (const TilesetSource& source : sources){
        if (source.id == id)
            return &source;
    }
    return nullptr;
}

TileDef& TilesetData::addTile(int source, int atX, int atY){
    TileDef tile;
    tile.id = nextId++;
    tile.source = source;
    tile.atX = atX;
    tile.atY = atY;
    tiles.push_back(std::move(tile));
    return tiles.back();
}

bool TilesetData::removeTile(int32_t id){
    auto it = std::lower_bound(tiles.begin(), tiles.end(), id, [](const TileDef& tile, int32_t value){ return tile.id < value; });
    if (it == tiles.end() || it->id != id)
        return false;
    tiles.erase(it);
    return true;
}

int TilesetData::terrainCount() const{
    int count = 0;
    for (const TerrainSetDef& set : terrainSets)
        count += (int)set.terrains.size();
    return count;
}

int TilesetData::terrainIndex(int set, int local) const{
    if (set < 0 || (size_t)set >= terrainSets.size() || local < 0 || (size_t)local >= terrainSets[(size_t)set].terrains.size())
        return NO_TERRAIN;
    int index = local;
    for (int i = 0; i < set; i++)
        index += (int)terrainSets[(size_t)i].terrains.size();
    return index;
}

bool TilesetData::terrainOf(int global, int& set, int& local) const{
    if (global < 0)
        return false;
    int remaining = global;
    for (size_t i = 0; i < terrainSets.size(); i++){
        const int count = (int)terrainSets[i].terrains.size();
        if (remaining < count){
            set = (int)i;
            local = remaining;
            return true;
        }
        remaining -= count;
    }
    return false;
}

const TerrainDef* TilesetData::terrain(int global) const{
    int set = 0, local = 0;
    return terrainOf(global, set, local) ? &terrainSets[(size_t)set].terrains[(size_t)local] : nullptr;
}

int TilesetData::findTerrain(std::string_view wanted) const{
    int index = 0;
    for (const TerrainSetDef& set : terrainSets){
        for (const TerrainDef& terrain : set.terrains){
            if (terrain.name == wanted)
                return index;
            index++;
        }
    }
    return NO_TERRAIN;
}

const PropertySchemaEntry* TilesetData::findProperty(std::string_view wanted) const{
    for (const PropertySchemaEntry& entry : propertySchema){
        if (entry.name == wanted)
            return &entry;
    }
    return nullptr;
}

bool TilesetData::operator==(const TilesetData& o) const{
    return version == o.version && name == o.name && tileWidth == o.tileWidth && tileHeight == o.tileHeight && shape == o.shape && nextId == o.nextId &&
           propertySchema == o.propertySchema && terrainSets == o.terrainSets && tiles == o.tiles && stamps == o.stamps &&
           std::equal(sources.begin(), sources.end(), o.sources.begin(), o.sources.end(), tsSourcesEqual);
}

const char* tileset::sourceKindName(TilesetSourceKind kind){
    switch (kind){
        case TilesetSourceKind::Texture: return "texture";
        case TilesetSourceKind::Sheet: return "sheet";
        case TilesetSourceKind::Aseprite: return "aseprite";
    }
    return "texture";
}

const char* tileset::shapeName(TileShape shape){
    switch (shape){
        case TileShape::Square: return "square";
        case TileShape::Isometric: return "isometric";
        case TileShape::HexPointy: return "hex_pointy";
        case TileShape::HexFlat: return "hex_flat";
    }
    return "square";
}

const char* tileset::terrainModeName(TerrainMode mode){
    switch (mode){
        case TerrainMode::Blob: return "blob";
        case TerrainMode::Sides: return "sides";
        case TerrainMode::DualGrid: return "dual_grid";
    }
    return "blob";
}

const char* tileset::propertyTypeName(TilePropertyType type){
    switch (type){
        case TilePropertyType::Bool: return "bool";
        case TilePropertyType::Int: return "int";
        case TilePropertyType::Float: return "float";
        case TilePropertyType::String: return "string";
        case TilePropertyType::Color: return "color";
    }
    return "int";
}

const char* tileset::collisionModeName(TileCollisionMode mode){
    switch (mode){
        case TileCollisionMode::None: return "none";
        case TileCollisionMode::Full: return "full";
        case TileCollisionMode::Alpha: return "alpha";
        case TileCollisionMode::Custom: return "custom";
    }
    return "none";
}

const char* tileset::collisionKindName(TileCollisionKind kind){
    switch (kind){
        case TileCollisionKind::Solid: return "solid";
        case TileCollisionKind::OneWay: return "one_way";
        case TileCollisionKind::Sensor: return "sensor";
    }
    return "solid";
}

bool tileset::parseSourceKind(std::string_view text, TilesetSourceKind& out){
    for (TilesetSourceKind kind : {TilesetSourceKind::Texture, TilesetSourceKind::Sheet, TilesetSourceKind::Aseprite}){
        if (text == sourceKindName(kind)){
            out = kind;
            return true;
        }
    }
    return false;
}

bool tileset::parseShape(std::string_view text, TileShape& out){
    for (TileShape shape : {TileShape::Square, TileShape::Isometric, TileShape::HexPointy, TileShape::HexFlat}){
        if (text == shapeName(shape)){
            out = shape;
            return true;
        }
    }
    return false;
}

bool tileset::parseTerrainMode(std::string_view text, TerrainMode& out){
    for (TerrainMode mode : {TerrainMode::Blob, TerrainMode::Sides, TerrainMode::DualGrid}){
        if (text == terrainModeName(mode)){
            out = mode;
            return true;
        }
    }
    return false;
}

bool tileset::parsePropertyType(std::string_view text, TilePropertyType& out){
    for (TilePropertyType type : {TilePropertyType::Bool, TilePropertyType::Int, TilePropertyType::Float, TilePropertyType::String, TilePropertyType::Color}){
        if (text == propertyTypeName(type)){
            out = type;
            return true;
        }
    }
    return false;
}

std::vector<int> tileset::peeringSlots(TileShape shape, TerrainMode mode){
    if (tsIsHex(shape)){
        if (mode == TerrainMode::DualGrid)
            return {};
        std::vector<int> slots;
        for (int slot = 0; slot < (mode == TerrainMode::Blob ? 12 : 6); slot++)
            slots.push_back(slot);
        return slots;
    }
    switch (mode){
        case TerrainMode::Blob: return {0, 1, 2, 3, 4, 5, 6, 7};
        case TerrainMode::Sides: return {0, 2, 4, 6};
        case TerrainMode::DualGrid: return {1, 3, 5, 7};
    }
    return {};
}

const char* tileset::peeringKey(TileShape shape, int slot){
    if (slot < 0 || slot >= tsKeyCount(shape))
        return "";
    if (shape == TileShape::HexPointy)
        return TS_POINTY_KEYS[slot];
    if (shape == TileShape::HexFlat)
        return TS_FLAT_KEYS[slot];
    return TS_SQUARE_KEYS[slot];
}

int tileset::peeringSlotOfKey(TileShape shape, std::string_view key){
    for (int slot = 0; slot < tsKeyCount(shape); slot++){
        if (key == peeringKey(shape, slot))
            return slot;
    }
    return -1;
}

bool tileset::parse(std::string_view text, TilesetData& out, std::vector<TilesetProblem>& problems){
    TsReader reader(problems);
    TsJson root;
    TsSaxParser sax(root);
    TsJson::sax_parse(text.begin(), text.end(), &sax);
    if (sax.failed){
        const size_t end = std::min(sax.errorPosition > 0 ? sax.errorPosition - 1 : 0, text.size());
        const int64_t line = 1 + std::count(text.begin(), text.begin() + (std::ptrdiff_t)end, '\n');
        return reader.fail("line " + std::to_string(line), "not valid JSON: " + sax.message, "fix the JSON syntax");
    }
    if (!reader.object(root, ""))
        return false;
    reader.unknownKeys(root, "", {"version", "name", "tile_size", "tile_shape", "next_id", "sources", "property_schema", "terrain_sets", "tiles", "stamps"});

    TilesetData data;
    int version = 0;
    if (!reader.intField(root, "version", "", 0, INT32_MAX, version, true))
        return false;
    if (version != 1)
        return reader.fail("version", "unsupported version " + std::to_string(version), "this engine reads version 1");
    data.version = version;
    std::string shape = "square";
    if (!reader.stringField(root, "name", "", data.name) || !reader.pairField(root, "tile_size", "", 1, 1 << 16, data.tileWidth, data.tileHeight) ||
        !reader.stringField(root, "tile_shape", "", shape) || !reader.intField(root, "next_id", "", 0, MAX_TILE_ID + 1, data.nextId))
        return false;
    if (!tileset::parseShape(shape, data.shape))
        return reader.fail("tile_shape", "unknown tile shape \"" + shape + "\"", "use square, isometric, hex_pointy or hex_flat");

    const auto forEachEntry = [&](const char* key, auto&& parseEntry){
        auto it = root.find(key);
        if (it == root.end())
            return true;
        if (!it->is_array())
            return reader.fail(key, "expected an array");
        for (size_t i = 0; i < it->size(); i++){
            if (!parseEntry((*it)[i], std::string(key) + "[" + std::to_string(i) + "]"))
                return false;
        }
        return true;
    };

    bool ok = forEachEntry("sources", [&](const TsJson& j, const std::string& path){
        TilesetSource source;
        if (!tsParseSource(reader, j, path, source))
            return false;
        data.sources.push_back(std::move(source));
        return true;
    });
    // the schema first: it decides the type of each tile's property values
    ok = ok && forEachEntry("property_schema", [&](const TsJson& j, const std::string& path){
        PropertySchemaEntry entry;
        if (!tsParseSchema(reader, j, path, entry))
            return false;
        data.propertySchema.push_back(std::move(entry));
        return true;
    });
    ok = ok && forEachEntry("terrain_sets", [&](const TsJson& j, const std::string& path){
        TerrainSetDef set;
        if (!tsParseTerrainSet(reader, j, path, set))
            return false;
        data.terrainSets.push_back(std::move(set));
        return true;
    });
    ok = ok && forEachEntry("tiles", [&](const TsJson& j, const std::string& path){
        TileDef tile;
        if (!tsParseTile(reader, data, j, path, tile))
            return false;
        data.tiles.push_back(std::move(tile));
        return true;
    });
    ok = ok && forEachEntry("stamps", [&](const TsJson& j, const std::string& path){
        TileStamp stamp;
        if (!tsParseStamp(reader, j, path, stamp))
            return false;
        data.stamps.push_back(std::move(stamp));
        return true;
    });
    if (!ok)
        return false;

    std::unordered_set<int32_t> ids;
    int32_t maxId = -1;
    for (const TileDef& tile : data.tiles){
        ids.insert(tile.id);
        maxId = std::max(maxId, tile.id);
    }
    for (size_t t = 0; t < data.tiles.size(); t++){
        std::vector<TileAnimationFrame>& frames = data.tiles[t].animation;
        for (size_t f = frames.size(); f-- > 0;){
            if (ids.count(frames[f].tile) == 0){
                reader.warn("unknown_tile", "tiles[" + std::to_string(t) + "].animation[" + std::to_string(f) + "]",
                            "the frame names tile " + std::to_string(frames[f].tile) + ", which is not in this tileset; skipped", "name an existing tile id");
                frames.erase(frames.begin() + (std::ptrdiff_t)f);
            }
        }
    }
    // a file edited by hand may list a tile above next_id: the id must still never be reissued
    if (data.nextId <= maxId){
        if (root.contains("next_id"))
            reader.warn("parse", "next_id", "below the largest tile id, raised to " + std::to_string(maxId + 1), "set next_id above every tile id");
        data.nextId = maxId + 1;
    }
    std::stable_sort(data.tiles.begin(), data.tiles.end(), [](const TileDef& a, const TileDef& b){ return a.id < b.id; });
    out = std::move(data);
    return true;
}

std::string tileset::write(const TilesetData& data){
    std::string out = "{\n";
    out += "  \"version\": " + std::to_string(data.version) + ",\n";
    out += "  \"name\": " + tsQuoted(data.name) + ",\n";
    out += "  \"tile_size\": " + tsPairText(data.tileWidth, data.tileHeight) + ",\n";
    out += "  \"tile_shape\": " + tsQuoted(shapeName(data.shape)) + ",\n";
    out += "  \"next_id\": " + std::to_string(data.nextId) + ",\n";
    tsWriteArray(out, "sources", data.sources, tsSourceText, false);
    tsWriteArray(out, "property_schema", data.propertySchema, [](const PropertySchemaEntry& entry){
        std::string text;
        TsObject object(text);
        object.raw("name", tsQuoted(entry.name));
        object.raw("type", tsQuoted(propertyTypeName(entry.type)));
        object.raw("default", tsPropertyText(entry.defaultValue));
        object.end();
        return text;
    }, false);
    tsWriteArray(out, "terrain_sets", data.terrainSets, tsTerrainSetText, false);
    tsWriteArray(out, "tiles", data.tiles, [&](const TileDef& tile){ return tsTileText(data, tile); }, false);
    tsWriteArray(out, "stamps", data.stamps, tsStampText, true);
    out += "}\n";
    return out;
}

void tileset::validate(const TilesetData& data, std::vector<TilesetProblem>& problems){
    if (data.terrainCount() > MAX_TILESET_TERRAINS){
        problems.push_back(TilesetProblem{"too_many_terrains", "terrain_sets", std::to_string(data.terrainCount()) + " terrains in this tileset, at most " + std::to_string(MAX_TILESET_TERRAINS),
                                          "remove terrains so that all the sets hold at most " + std::to_string(MAX_TILESET_TERRAINS)});
    }
    std::string declared;
    for (const PropertySchemaEntry& entry : data.propertySchema)
        declared += (declared.empty() ? "" : ", ") + entry.name;
    const std::string propertyFix = declared.empty() ? "declare the property in property_schema first" : "use a declared property: " + declared + ", or declare it in property_schema";

    std::unordered_set<int32_t> ids;
    std::unordered_set<std::string> names;
    for (const TileDef& tile : data.tiles)
        ids.insert(tile.id);
    std::unordered_set<int32_t> seen;
    int32_t maxId = -1;
    for (size_t t = 0; t < data.tiles.size(); t++){
        const TileDef& tile = data.tiles[t];
        const std::string path = "tiles[" + std::to_string(t) + "]";
        maxId = std::max(maxId, tile.id);
        if (!seen.insert(tile.id).second)
            problems.push_back(TilesetProblem{"duplicate_id", path + ".id", "tile id " + std::to_string(tile.id) + " is used twice", "give each tile its own id"});
        if (!tile.name.empty() && !names.insert(tile.name).second)
            problems.push_back(TilesetProblem{"duplicate_name", path + ".name", "the tile name \"" + tile.name + "\" is used twice", "give each tile its own name"});
        if (!data.findSource(tile.source))
            problems.push_back(TilesetProblem{"unknown_source", path + ".source", "tile " + std::to_string(tile.id) + " names source " + std::to_string(tile.source) + ", which does not exist", "use the id of a source of this tileset"});
        for (const auto& [name, value] : tile.properties){
            const PropertySchemaEntry* entry = data.findProperty(name);
            if (!entry){
                problems.push_back(TilesetProblem{"unknown_property", path + ".properties." + name, "the property \"" + name + "\" is not declared", propertyFix});
            }else if (entry->type != value.type){
                problems.push_back(TilesetProblem{"invalid_property", path + ".properties." + name, std::string("the property \"") + name + "\" is declared " + propertyTypeName(entry->type) + " but holds a " + propertyTypeName(value.type),
                                                  std::string("give a ") + propertyTypeName(entry->type) + " value"});
            }
        }
        for (size_t f = 0; f < tile.animation.size(); f++){
            if (ids.count(tile.animation[f].tile) == 0)
                problems.push_back(TilesetProblem{"unknown_tile", path + ".animation[" + std::to_string(f) + "]", "the frame names tile " + std::to_string(tile.animation[f].tile) + ", which is not in this tileset", "name an existing tile id"});
        }
        if (tile.terrain.set < 0)
            continue;
        const std::string terrainPath = path + ".terrain";
        if ((size_t)tile.terrain.set >= data.terrainSets.size()){
            problems.push_back(TilesetProblem{"invalid_peering", terrainPath + ".set", "there is no terrain set " + std::to_string(tile.terrain.set), "use a terrain set of this tileset"});
            continue;
        }
        const TerrainSetDef& set = data.terrainSets[(size_t)tile.terrain.set];
        const std::vector<int> slots = peeringSlots(data.shape, set.mode);
        const int count = (int)set.terrains.size();
        if (tile.terrain.center != -1 && (tile.terrain.center < 0 || tile.terrain.center >= count))
            problems.push_back(TilesetProblem{"invalid_peering", terrainPath + ".center", "the set has no terrain " + std::to_string(tile.terrain.center), "use a terrain index of the set"});
        for (int slot = 0; slot < tsKeyCount(data.shape); slot++){
            const int16_t value = tile.terrain.peering[(size_t)slot];
            if (value == PEERING_ANY)
                continue;
            const std::string slotPath = terrainPath + ".peering." + peeringKey(data.shape, slot);
            if (std::find(slots.begin(), slots.end(), slot) == slots.end()){
                problems.push_back(TilesetProblem{"invalid_peering", slotPath, std::string("the ") + terrainModeName(set.mode) + " mode has no such peering direction on this tile shape", "remove it"});
            }else if (value != PEERING_NONE && (value < 0 || value >= count)){
                problems.push_back(TilesetProblem{"invalid_peering", slotPath, "the set has no terrain " + std::to_string(value), "use any, none or a terrain index of the set"});
            }
        }
    }
    if (data.nextId <= maxId)
        problems.push_back(TilesetProblem{"invalid_next_id", "next_id", "next_id " + std::to_string(data.nextId) + " is not above the largest tile id " + std::to_string(maxId), "set next_id above every tile id"});
    std::unordered_set<int> sourceIds;
    for (size_t s = 0; s < data.sources.size(); s++){
        if (!sourceIds.insert(data.sources[s].id).second)
            problems.push_back(TilesetProblem{"duplicate_id", "sources[" + std::to_string(s) + "].id", "source id " + std::to_string(data.sources[s].id) + " is used twice", "give each source its own id"});
    }
}

uint32_t tileset::cellHash(uint32_t seed, uint32_t stroke, int32_t x, int32_t y){
    uint32_t h = seed;
    h = tsHashMix(h, stroke);
    h = tsHashMix(h, (uint32_t)x);
    h = tsHashMix(h, (uint32_t)y);
    h ^= 16u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

int tileset::weightedPick(const float* weights, size_t count, uint32_t hash){
    // weights become integers once, so the walk adds no float rounding of its own
    uint64_t total = 0;
    std::vector<uint64_t> scaled(count, 0);
    for (size_t i = 0; i < count; i++){
        if (!(weights[i] > 0.0f))
            continue;
        const float value = std::min(weights[i], 65535.0f) * 65536.0f + 0.5f;
        scaled[i] = std::max<uint64_t>((uint64_t)value, 1);
        total += scaled[i];
    }
    if (total == 0)
        return -1;
    uint64_t target = (uint64_t)hash % total;
    for (size_t i = 0; i < count; i++){
        if (target < scaled[i])
            return (int)i;
        target -= scaled[i];
    }
    return -1;
}
