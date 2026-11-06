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
        // The pixel-art family's reserved values 24 to 27 (PIXEL-ART-LEDGER.md, docs/specs/S41-S52):
        // each keeps its number whatever order the specs land in, so a value between two may still be unused
        Palette = 24,               // a .palette palette (S43)
        SoundEvent = 28             // a .soundevent Sound Event (S37; its GUID in its .meta)
    };

    // the highest value in use
    constexpr uint8_t kLastAssetTypeValue = static_cast<uint8_t>(AssetType::SoundEvent);

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
            case AssetType::AnimationMontage: return "animation_montage";
            case AssetType::VertexAnimation: return "vertex_animation";
            case AssetType::Palette: return "palette";
            case AssetType::SoundEvent: return "sound_event";
            case AssetType::Unknown: break;
        }
        return "unknown";
    }

    inline AssetType assetTypeFromName(std::string_view name) {
        for (uint8_t value = 1; value <= kLastAssetTypeValue; value++) {
            const AssetType type = static_cast<AssetType>(value);
            const char* typeName = assetTypeName(type);
            // a reserved value no spec has added yet has no name
            if (std::string_view(typeName) != "unknown" && name == typeName) {
                return type;
            }
        }
        return AssetType::Unknown;
    }

}

#endif // ASSETTYPE_H
