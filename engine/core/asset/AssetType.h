// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ASSETTYPE_H
#define ASSETTYPE_H

#include <cstdint>
#include <string_view>

namespace jasperengine {

    // The kinds of asset that have an identity. The editor's type table
    // (editor/asset/AssetTypes.cpp) maps file extensions to them; the runtime map
    // (AssetMap, assetmap.bin) stores the value, so the numbers are a file format: append only.
    enum class AssetType : uint8_t {
        Unknown = 0,
        Prefab = 1,
        Scene = 2,
        Material = 3,
        Texture = 4,
        Model = 5,
        Audio = 6,
        Font = 7,
        UIDocument = 8,
        Stylesheet = 9,
        Shader = 10,
        Script = 11,
        Data = 12,
        SubAsset = 13,  // a mesh, material, animation, skin or image inside a model
        ColorLut = 14,  // a .cube colour LUT (post-processing's lutTexture, S13)
        ParticleEffect = 15,  // a .particles effect (S20)
        AnimationClip = 16,     // an .anim clip (Animation Player clips; its GUID in its JSON)
        PoseAnimation = 17,     // a .poseanim pose animation (Pose Animator's animation)
        NavMesh = 18,         // a baked .navmesh (S19)
        NavGrid = 19,         // a .navgrid's cells (S19)
        AnimatorController = 20,    // a .controller Animator Controller (S21; its GUID in its JSON)
        AvatarMask = 21,            // a .mask avatar mask (S21)
        AnimationMontage = 22,      // a .montage animation montage (S21)
        VertexAnimation = 23,       // a .vat Vertex Animation (Vertex Animator's animation; its GUID in its JSON)
        // 24 to 27 are the pixel art program's (docs/specs/S41-S52: Palette 24, AsepriteSprite 25,
        // SpriteAtlas 26, Tileset 27), each written explicitly whatever order they land in
        Palette = 24,               // a .palette palette (S43)
        AsepriteSprite = 25,        // an .aseprite / .ase file (S44): its sheet, frames, clips and slices are sub-assets
        SpriteAtlas = 26,           // a .spriteatlas Sprite Atlas (S45; its GUID in its JSON)
        SoundEvent = 28,            // a .soundevent Sound Event (S37; its GUID in its .meta)
        ProceduralMaterial = 29,    // a .procmat node graph baked in the editor (S90; its GUID in its JSON; never ships)
        // The terrain programme's (T2-T6) follow upstream's 29. Stored in assetmap.bin: these never move.
        TreeData = 30,              // a .jetrees file of a Terrain's trees (T5 R22)
        TerrainWorld = 31,          // a Name.terrainworld folder, its GUID in its manifest.yaml (T4 R48)
        VoxelVolume = 32,           // a .voxelvolume (T6 R77, its GUID in a sidecar)
        NoiseGraph = 33,            // a .noisegraph Noise Graph (T2)
        TitleArt = 34               // a .titleart Title Art document (S80; its GUID in its JSON)
    };
    // the highest value present (a sibling's reserved value below it may still be missing)
    inline constexpr uint8_t kLastAssetType = static_cast<uint8_t>(AssetType::TitleArt);

    // The lowercase names used by Lua (AssetRef.type), MCP and the editor ("texture", "model", ...)
    inline const char* assetTypeName(AssetType type) {
        switch (type) {
            case AssetType::Prefab: return "prefab";
            case AssetType::Scene: return "scene";
            case AssetType::Material: return "material";
            case AssetType::Texture: return "texture";
            case AssetType::Model: return "model";
            case AssetType::Audio: return "audio";
            case AssetType::Font: return "font";
            case AssetType::UIDocument: return "ui_document";
            case AssetType::Stylesheet: return "stylesheet";
            case AssetType::Shader: return "shader";
            case AssetType::Script: return "script";
            case AssetType::Data: return "data";
            case AssetType::SubAsset: return "sub_asset";
            case AssetType::ColorLut: return "color_lut";
            case AssetType::ParticleEffect: return "particle_effect";
            case AssetType::AnimationClip: return "animation_clip";
            case AssetType::PoseAnimation: return "pose_animation";
            case AssetType::NavMesh: return "navmesh";
            case AssetType::NavGrid: return "navgrid";
            case AssetType::AnimatorController: return "animator_controller";
            case AssetType::AvatarMask: return "avatar_mask";
            case AssetType::TreeData: return "tree_data";
            case AssetType::AnimationMontage: return "animation_montage";
            case AssetType::VertexAnimation: return "vertex_animation";
            case AssetType::Palette: return "palette";
            case AssetType::AsepriteSprite: return "aseprite_sprite";
            case AssetType::SpriteAtlas: return "sprite_atlas";
            case AssetType::SoundEvent: return "sound_event";
            case AssetType::ProceduralMaterial: return "procedural_material";
            case AssetType::TerrainWorld: return "terrain_world";
            case AssetType::NoiseGraph: return "noise_graph";
            case AssetType::VoxelVolume: return "voxel_volume";
            case AssetType::TitleArt: return "title_art";
            case AssetType::Unknown: break;
        }
        return "unknown";
    }

    inline AssetType assetTypeFromName(std::string_view name) {
        // values without a name (a reserved one not added yet) read "unknown" and never match
        for (uint8_t value = 1; value <= kLastAssetType; value++) {
            const AssetType type = static_cast<AssetType>(value);
            if (name != "unknown" && name == assetTypeName(type)) {
                return type;
            }
        }
        return AssetType::Unknown;
    }

}

#endif // ASSETTYPE_H
