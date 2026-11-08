// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SUBASSETKEYS_H
#define SUBASSETKEYS_H

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine {

    // The keys of the sub-assets inside a model file (meshes, materials, animations, skins and
    // embedded images of a glTF/GLB/OBJ). A sub-asset's first GUID is
    // Guid::derive(containerGuid, key), so two machines agree on it without the meta file.
    // The key formats are a persistence contract and frozen:
    //   "<kind>:<name>"   the name is non-empty and unique among that kind in the file
    //   "<kind>#<index>"  otherwise, the index among that kind in the file
    // A reference to a sub-asset uses the path hint "<container path>#<key>".
    // Frame, Slice, Tileset and Palette are an .aseprite file's (S44: its frames, slices,
    // tilesets and palette; Image its sheet pages, Animation its tag clips); PaletteRamp a
    // .palette's ramps ("ramp:<name>", an N x 1 strip, S43); appended, frozen
    enum class SubAssetKind { Mesh, Material, Animation, Skin, Image,
                              Frame, Slice, Tileset, Palette, PaletteRamp };

    inline const char* subAssetKindName(SubAssetKind kind) {
        switch (kind) {
            case SubAssetKind::Mesh: return "mesh";
            case SubAssetKind::Material: return "material";
            case SubAssetKind::Animation: return "animation";
            case SubAssetKind::Skin: return "skin";
            case SubAssetKind::Image: return "image";
            case SubAssetKind::Frame: return "frame";
            case SubAssetKind::Slice: return "slice";
            case SubAssetKind::Tileset: return "tileset";
            case SubAssetKind::Palette: return "palette";
            case SubAssetKind::PaletteRamp: return "ramp";
        }
        return "mesh";
    }

    inline bool parseSubAssetKind(std::string_view text, SubAssetKind& kind) {
        static const SubAssetKind kinds[] = {SubAssetKind::Mesh, SubAssetKind::Material, SubAssetKind::Animation,
                                             SubAssetKind::Skin, SubAssetKind::Image, SubAssetKind::Frame,
                                             SubAssetKind::Slice, SubAssetKind::Tileset, SubAssetKind::Palette,
                                             SubAssetKind::PaletteRamp};
        for (SubAssetKind candidate : kinds) {
            if (text == subAssetKindName(candidate)) {
                kind = candidate;
                return true;
            }
        }
        return false;
    }

    inline std::string subAssetNamedKey(SubAssetKind kind, std::string_view name) {
        return std::string(subAssetKindName(kind)) + ":" + std::string(name);
    }

    inline std::string subAssetIndexKey(SubAssetKind kind, size_t index) {
        return std::string(subAssetKindName(kind)) + "#" + std::to_string(index);
    }

    // The keys of one kind's sub-assets, given their names in file order
    inline std::vector<std::string> subAssetKeys(SubAssetKind kind, const std::vector<std::string>& names) {
        std::unordered_map<std::string, size_t> counts;
        for (const std::string& name : names) {
            if (!name.empty()) {
                counts[name]++;
            }
        }
        std::vector<std::string> keys;
        keys.reserve(names.size());
        for (size_t i = 0; i < names.size(); i++) {
            const std::string& name = names[i];
            keys.push_back(!name.empty() && counts[name] == 1 ? subAssetNamedKey(kind, name) : subAssetIndexKey(kind, i));
        }
        return keys;
    }

    // Splits a key; index is the "#<index>" form's index, else -1
    inline bool parseSubAssetKey(std::string_view key, SubAssetKind& kind, std::string& name, long& index) {
        const size_t separator = key.find_first_of(":#");
        if (separator == std::string_view::npos || !parseSubAssetKind(key.substr(0, separator), kind)) {
            return false;
        }
        const std::string_view rest = key.substr(separator + 1);
        name.clear();
        index = -1;
        if (key[separator] == ':') {
            name = std::string(rest);
            return !rest.empty();
        }
        if (rest.empty()) {
            return false;
        }
        long value = 0;
        for (char c : rest) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        index = value;
        return true;
    }

    // True for the file extensions that hold sub-assets (models: .gltf, .glb, .obj; Aseprite
    // files: .aseprite, .ase; palettes: .palette; any case)
    inline bool isSubAssetContainerPath(std::string_view path) {
        auto endsWith = [&](std::string_view ext) {
            if (path.size() < ext.size()) return false;
            for (size_t i = 0; i < ext.size(); i++) {
                char c = path[path.size() - ext.size() + i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != ext[i]) return false;
            }
            return true;
        };
        return endsWith(".gltf") || endsWith(".glb") || endsWith(".obj") || endsWith(".aseprite") || endsWith(".ase") ||
               endsWith(".palette");
    }

    // "<container>#<key>" -> container and key; false for a plain path. The container is a
    // model file, and the first '#' after one that starts a valid key splits the path (names
    // inside keys may contain '#' and '/')
    inline bool splitSubAssetPath(std::string_view path, std::string& container, std::string& key) {
        for (size_t hash = path.find('#'); hash != std::string_view::npos; hash = path.find('#', hash + 1)) {
            SubAssetKind kind;
            std::string name;
            long index;
            if (isSubAssetContainerPath(path.substr(0, hash)) &&
                parseSubAssetKey(path.substr(hash + 1), kind, name, index)) {
                container = std::string(path.substr(0, hash));
                key = std::string(path.substr(hash + 1));
                return true;
            }
        }
        return false;
    }

}

#endif // SUBASSETKEYS_H
