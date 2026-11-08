// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The one Create list (project-window spec 6.6): Assets > Create in the main menu, the Project
// window's toolbar "+" and the Create submenu of each of its right-click menus show these items,
// in this order, with these labels

#include <cstdint>
#include <span>

namespace jasperengine::editor {

    enum class AssetCreateKind : uint8_t {
        Folder,
        LuaScript,
        CppScript,
        CppSource,
        CppHeader,
        HtmlDocument,
        AnimationClip,
        AnimatorController,
        AvatarMask,
        AnimationMontage,
        PoseAnimation,
        AudioMixer,
        SoundEvent,
        PlaceholderSound,
        PhysicsMaterial,
        PhysicsMaterial2D,
        ShakePreset,
        SpriteAtlas,    // S45 (the main menu's Create > 2D)
        Palette         // a .palette (S43)
    };

    struct AssetCreateItem {
        AssetCreateKind kind;
        const char* label;        // "Lua Script" (Unity's names, no "Create")
        const char* icon;
        const char* commandId;    // the catalog command of Assets > Create
        bool separatorBefore;
        // asks for its name first (the name is the class, module or folder) instead of being made
        // at once and renamed in place
        bool nameFirst;
    };

    // In Unity's order
    std::span<const AssetCreateItem> assetCreateItems();
    const AssetCreateItem& assetCreateItem(AssetCreateKind kind);

}
