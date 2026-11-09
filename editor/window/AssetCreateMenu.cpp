// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetCreateMenu.h"

#include "external/IconsFontAwesome6.h"

#include <array>

using namespace jasperengine::editor;

namespace {

    constexpr std::array<AssetCreateItem, 21> ITEMS = {{
        {AssetCreateKind::Folder, "Folder", ICON_FA_FOLDER_PLUS, "assets.createFolder", false, true},
        {AssetCreateKind::LuaScript, "Lua Script", ICON_FA_FILE_CODE, "assets.createLuaScript", true, true},
        {AssetCreateKind::CppScript, "C++ Script", ICON_FA_FILE_CODE, "assets.createCppScript", false, true},
        {AssetCreateKind::CppSource, "C++ Source File", ICON_FA_FILE_CODE, "assets.createCppSource", false, true},
        {AssetCreateKind::CppHeader, "C++ Header File", ICON_FA_FILE_CODE, "assets.createCppHeader", false, true},
        {AssetCreateKind::HtmlDocument, "HTML Document", ICON_FA_CODE, "assets.createHtmlDocument", true, true},
        {AssetCreateKind::AnimationClip, "Animation Clip", ICON_FA_FILM, "assets.createAnimationClip", true, false},
        {AssetCreateKind::AnimatorController, "Animator Controller", ICON_FA_DIAGRAM_PROJECT, "assets.createAnimatorController", false, false},
        {AssetCreateKind::AvatarMask, "Avatar Mask", ICON_FA_PERSON, "assets.createAvatarMask", false, false},
        {AssetCreateKind::AnimationMontage, "Animation Montage", ICON_FA_FILM, "assets.createAnimationMontage", false, false},
        {AssetCreateKind::PoseAnimation, "Pose Animation", ICON_FA_PERSON, "assets.createPoseAnimation", false, false},
        {AssetCreateKind::AudioMixer, "Audio Mixer", ICON_FA_SLIDERS, "assets.createAudioMixer", true, false},
        {AssetCreateKind::SoundEvent, "Sound Event", ICON_FA_MUSIC, "assets.createSoundEvent", false, false},
        {AssetCreateKind::PlaceholderSound, "Placeholder Sound...", ICON_FA_VOLUME_HIGH, "assets.createPlaceholderSound", false, false},
        {AssetCreateKind::PhysicsMaterial, "Physics Material", ICON_FA_CIRCLE_HALF_STROKE, "assets.createPhysicsMaterial", true, false},
        {AssetCreateKind::PhysicsMaterial2D, "Physics Material 2D", ICON_FA_CIRCLE_HALF_STROKE, "assets.createPhysicsMaterial2D", false, false},
        {AssetCreateKind::ShakePreset, "Shake Preset", ICON_FA_BURST, "assets.createShakePreset", true, false},
        {AssetCreateKind::TitleArt, "Title Art", ICON_FA_FONT, "assets.createTitleArt", false, false},
        {AssetCreateKind::ProceduralMaterial, "Procedural Material", ICON_FA_DIAGRAM_PROJECT, "assets.createProceduralMaterial", true, false},
        {AssetCreateKind::SpriteAtlas, "Sprite Atlas", ICON_FA_TABLE_CELLS, "assets.createSpriteAtlas", true, false},
        {AssetCreateKind::Palette, "Palette", ICON_FA_PALETTE, "assets.createPalette", false, false},
    }};

}

std::span<const AssetCreateItem> jasperengine::editor::assetCreateItems() {
    return ITEMS;
}

const AssetCreateItem& jasperengine::editor::assetCreateItem(AssetCreateKind kind) {
    for (const AssetCreateItem& item : ITEMS) {
        if (item.kind == kind) return item;
    }
    return ITEMS[0];
}
