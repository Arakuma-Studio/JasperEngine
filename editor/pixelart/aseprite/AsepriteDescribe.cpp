// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The document as JSON (docs/specs/S44-aseprite-import.md R9): the one shape get_asset_info,
// S47's get_sprite and `jasperengine-editor aseprite info --json` share. Coordinates are pixels,
// origin top-left; frames from 0; colours "#rrggbbaa".

#include "AsepriteDocument.h"

#include <cstdio>

namespace jasperengine::editor::pixelart::aseprite {

    namespace {

        std::string hexColor(const PixelRgba& c) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);
            return buffer;
        }

        nlohmann::json rect(const IntRect& r) { return {r.x, r.y, r.w, r.h}; }

        const char* layerTypeName(LayerType type) {
            switch (type) {
                case LayerType::Image: return "image";
                case LayerType::Group: return "group";
                case LayerType::Tilemap: return "tilemap";
            }
            return "unknown";
        }

        nlohmann::json userData(const Document& doc, const UserData& ud) {
            nlohmann::json j = nlohmann::json::object();
            if (ud.text) j["text"] = *ud.text;
            if (ud.color) j["color"] = hexColor(*ud.color);
            for (const auto& [key, map] : ud.properties) {
                if (key == 0) {
                    j["properties"] = toJson(map);
                    continue;
                }
                // an extension's properties, by the extension's name in the external files
                std::string name = "extension_" + std::to_string(key);
                for (const ExternalFile& f : doc.externalFiles) {
                    if (f.id == key) name = f.name;
                }
                j["extensions"][name] = toJson(map);
            }
            return j;
        }

        void putUserData(nlohmann::json& into, const Document& doc, const UserData& ud) {
            if (!ud.empty()) into["user_data"] = userData(doc, ud);
        }

    }

    nlohmann::json describe(const Document& doc, Detail detail) {
        nlohmann::json j;
        j["format"] = "aseprite";
        j["width"] = doc.width;
        j["height"] = doc.height;
        j["color_mode"] = colorModeName(doc.mode);
        j["frame_count"] = doc.frameCount();
        int total = 0;
        nlohmann::json durations = nlohmann::json::array();
        for (uint16_t d : doc.frameDurations) {
            durations.push_back(d);
            total += d;
        }
        j["frame_durations_ms"] = std::move(durations);
        j["duration_ms"] = total;
        j["pixel_ratio"] = {doc.pixelWidth, doc.pixelHeight};
        j["grid"] = rect(doc.grid);

        nlohmann::json palette = {{"size", doc.palette.size()}};
        if (doc.mode == ColorMode::Indexed) palette["transparent_index"] = doc.transparentIndex;
        if (detail != Detail::Summary) {
            nlohmann::json colors = nlohmann::json::array();
            nlohmann::json names = nlohmann::json::object();
            for (size_t i = 0; i < doc.palette.size(); i++) {
                colors.push_back(hexColor(doc.palette[i].color));
                if (doc.palette[i].name) names[std::to_string(i)] = *doc.palette[i].name;
            }
            palette["colors"] = std::move(colors);
            if (!names.empty()) palette["names"] = std::move(names);
            if (!doc.paletteChanges.empty()) {
                nlohmann::json changes = nlohmann::json::array();
                for (const auto& [frame, entries] : doc.paletteChanges) changes.push_back({{"frame", frame}, {"size", entries.size()}});
                palette["changes"] = std::move(changes);
            }
        }
        j["palette"] = std::move(palette);

        nlohmann::json layers = nlohmann::json::array();
        for (int i = 0; i < static_cast<int>(doc.layers.size()); i++) {
            const Layer& l = doc.layers[static_cast<size_t>(i)];
            nlohmann::json lj = {
                {"index", i},
                {"name", l.name},
                {"path", doc.layerPath(i)},
                {"type", layerTypeName(l.type)},
                {"parent", l.parent},
                {"visible", l.visible()},
                {"blend", blendModeName(l.blend)},
                {"opacity", l.opacity},
            };
            if (l.reference()) lj["reference"] = true;
            if (l.background()) lj["background"] = true;
            if (l.flags & kLayerCollapsed) lj["collapsed"] = true;
            if (l.type == LayerType::Tilemap) lj["tileset"] = l.tilesetIndex;
            if (detail != Detail::Summary) {
                lj["flags"] = l.flags;
                if (l.uuid) lj["uuid"] = l.uuid->toString();
            }
            putUserData(lj, doc, l.userData);
            layers.push_back(std::move(lj));
        }
        j["layers"] = std::move(layers);

        nlohmann::json tags = nlohmann::json::array();
        for (const Tag& t : doc.tags) {
            nlohmann::json tj = {
                {"name", t.name},
                {"from", t.from},
                {"to", t.to},
                {"direction", tagDirectionName(t.direction)},
                {"repeat", t.repeat},
                {"color", hexColor(t.color())},
            };
            putUserData(tj, doc, t.userData);
            tags.push_back(std::move(tj));
        }
        j["tags"] = std::move(tags);

        nlohmann::json slices = nlohmann::json::array();
        for (const Slice& s : doc.slices) {
            nlohmann::json keys = nlohmann::json::array();
            bool ninePatch = false, pivot = false;
            for (const SliceKey& k : s.keys) {
                nlohmann::json kj = {{"frame", k.frame}, {"bounds", rect(k.bounds)}};
                if (k.center) {
                    // an empty centre (a hidden key's) is left out, as Aseprite's --data does
                    if (!k.center->empty()) kj["center"] = rect(*k.center);
                    ninePatch = true;
                }
                if (k.pivot) {
                    kj["pivot"] = {k.pivot->x, k.pivot->y};
                    pivot = true;
                }
                keys.push_back(std::move(kj));
            }
            nlohmann::json sj = {{"name", s.name}, {"nine_patch", ninePatch}, {"has_pivot", pivot}, {"keys", std::move(keys)}};
            putUserData(sj, doc, s.userData);
            slices.push_back(std::move(sj));
        }
        j["slices"] = std::move(slices);

        nlohmann::json tilesets = nlohmann::json::array();
        for (const Tileset& t : doc.tilesets) {
            nlohmann::json tj = {
                {"id", t.id},
                {"name", t.name},
                {"tile_size", {t.tileWidth, t.tileHeight}},
                {"count", t.tileCount},
                {"base_index", t.baseIndex},
                {"zero_is_empty", t.zeroIsEmpty()},
            };
            if (t.external) {
                std::string file;
                for (const ExternalFile& f : doc.externalFiles) {
                    if (f.id == t.external->fileId) file = f.name;
                }
                tj["external"] = {{"file", file}, {"tileset", t.external->tilesetId}};
            }
            putUserData(tj, doc, t.userData);
            if (detail != Detail::Summary) {
                nlohmann::json tiles = nlohmann::json::array();
                for (size_t i = 0; i < t.tileUserData.size(); i++) {
                    if (!t.tileUserData[i].empty()) tiles.push_back({{"tile", i}, {"user_data", userData(doc, t.tileUserData[i])}});
                }
                if (!tiles.empty()) tj["tiles"] = std::move(tiles);
            }
            tilesets.push_back(std::move(tj));
        }
        j["tilesets"] = std::move(tilesets);

        if (!doc.spriteUserData.empty()) j["user_data"] = userData(doc, doc.spriteUserData);
        if (doc.colorProfile) {
            static const char* types[] = {"none", "srgb", "icc"};
            j["color_profile"] = doc.colorProfile->type < 3 ? types[doc.colorProfile->type] : "unknown";
        }
        if (detail != Detail::Summary) {
            if (!doc.externalFiles.empty()) {
                nlohmann::json files = nlohmann::json::array();
                for (const ExternalFile& f : doc.externalFiles) files.push_back({{"id", f.id}, {"type", f.type}, {"name", f.name}});
                j["external_files"] = std::move(files);
            }
            if (!doc.opaque.empty()) {
                nlohmann::json opaque = nlohmann::json::array();
                for (const OpaqueChunk& o : doc.opaque) {
                    char type[8];
                    std::snprintf(type, sizeof(type), "0x%04X", o.type);
                    opaque.push_back({{"type", type}, {"frame", o.frame}, {"bytes", o.body.size()}});
                }
                j["kept_chunks"] = std::move(opaque);
            }
        }

        if (detail == Detail::Cels) {
            nlohmann::json cels = nlohmann::json::array();
            for (const Cel& c : doc.cels) {
                nlohmann::json cj = {{"layer", c.layer}, {"frame", c.frame}, {"x", c.x}, {"y", c.y}, {"opacity", c.opacity}, {"z_index", c.zIndex}};
                if (c.linkedFrame >= 0) {
                    cj["type"] = "link";
                    cj["linked_frame"] = c.linkedFrame;
                } else if (c.tilemap) {
                    cj["type"] = "tilemap";
                    cj["columns"] = c.tilemap->columns;
                    cj["rows"] = c.tilemap->rows;
                } else {
                    cj["type"] = "image";
                    cj["w"] = c.image ? c.image->width() : 0;
                    cj["h"] = c.image ? c.image->height() : 0;
                }
                if (c.extra) cj["precise_bounds"] = {c.extra->x / 65536.0, c.extra->y / 65536.0, c.extra->w / 65536.0, c.extra->h / 65536.0};
                putUserData(cj, doc, c.userData);
                cels.push_back(std::move(cj));
            }
            j["cels"] = std::move(cels);
        }
        return j;
    }

}
