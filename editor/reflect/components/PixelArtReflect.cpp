// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pixel Perfect Camera (docs/specs/S41-pixel-perfect-camera.md §6.1) and the pixel-art
// family's enums. Field order is the YAML key order.

#include "reflect/Components.h"

#include "component/PixelPerfectCameraComponent.h"

namespace jasperengine::editor::reflect {

namespace enums {

    namespace {
        constexpr int pixelArtValue(PixelUpscaleMode mode) { return static_cast<int>(mode); }
        constexpr int pixelArtValue(PixelRotation rotation) { return static_cast<int>(rotation); }

        constexpr EnumItem kPixelUpscaleModeItems[] = {
            {pixelArtValue(PixelUpscaleMode::INTEGER), "integer", "Integer (Letterbox)", "PixelUpscaleMode::INTEGER"},
            {pixelArtValue(PixelUpscaleMode::INTEGER_EXPAND), "integer_expand", "Integer (Expand)", "PixelUpscaleMode::INTEGER_EXPAND"},
            {pixelArtValue(PixelUpscaleMode::STRETCH_FILL), "stretch_fill", "Stretch Fill", "PixelUpscaleMode::STRETCH_FILL"},
        };

        constexpr EnumItem kPixelRotationItems[] = {
            {pixelArtValue(PixelRotation::CAMERA), "camera", "Camera", "PixelRotation::CAMERA"},
            {pixelArtValue(PixelRotation::STEPS_90), "steps_90", "90 Degree Steps", "PixelRotation::STEPS_90"},
            {pixelArtValue(PixelRotation::ON_GRID), "on_grid", "Rotate On Grid", "PixelRotation::ON_GRID"},
        };
    }

    const EnumDesc kPixelUpscaleMode{"PixelUpscaleMode", kPixelUpscaleModeItems, pixelArtValue(PixelUpscaleMode::INTEGER)};
    const EnumDesc kPixelRotation{"PixelRotation", kPixelRotationItems, pixelArtValue(PixelRotation::CAMERA)};

}

namespace {

    // the camera's policy offers the two that are not "the camera's"
    constexpr uint64_t kCameraRotations = enums::bit((int)PixelRotation::STEPS_90) | enums::bit((int)PixelRotation::ON_GRID);

    // what RenderSystem lays out while the component drives its camera: shown by get_entity and
    // get_runtime_entity, never saved, drawn or written
    constexpr uint32_t kLayoutValue = Transient | ReadOnly | NoMcpWrite | NoDefault | Hidden | NoCodegen;

    constexpr FieldDesc kPixelPerfectCameraFields[] = {
        JE_FIELD(PixelPerfectCameraComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(PixelPerfectCameraComponent, referenceWidth, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Pixels, .max = 16384.0f,
                 .label = "Reference Width", .tooltip = "The base resolution the art is drawn at, in pixels. 0 x 0: the project canvas.",
                 .group = "Reference Resolution"),
        JE_FIELD(PixelPerfectCameraComponent, referenceHeight, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Pixels,
                 .max = 16384.0f, .label = "Reference Height", .group = "Reference Resolution"),
        JE_FIELD(PixelPerfectCameraComponent, upscaleMode, .enumDesc = &enums::kPixelUpscaleMode, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Integer (Letterbox): every base pixel k x k screen pixels, bars around. Integer (Expand): the same k, the "
                            "base grows to fill the screen. Stretch Fill: the integer image stretched to fill, sharp at its seams."),
        JE_FIELD(PixelPerfectCameraComponent, upscaleRenderTexture, .flags = OmitIfDefault | CodegenIfChanged,
                 .label = "Upscale Render Texture",
                 .tooltip = "Draws the base resolution into a texture scaled by a whole number (rotated sprites stay on base pixels). "
                            "Off: draws straight at the screen's resolution on the same grid."),
        JE_FIELD(PixelPerfectCameraComponent, pixelSnapping, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Puts the camera and every sprite, tile, UI element and particle on whole base pixels."),
        JE_FIELD(PixelPerfectCameraComponent, rotationMode, .enumDesc = &enums::kPixelRotation, .flags = OmitIfDefault | CodegenIfChanged,
                 .label = "Rotation",
                 .tooltip = "90 Degree Steps: rotations are drawn rounded to the nearest 90 degrees (the Transform keeps its angle). "
                            "Rotate On Grid: the angle is kept, the pivot snaps.",
                 .enumMask = kCameraRotations),
        JE_FIELD(PixelPerfectCameraComponent, subpixelSmoothing, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Scrolls by whole screen pixels between base pixels: the image moves, the art stays on its grid."),
        JE_FIELD(PixelPerfectCameraComponent, letterboxColor, .flags = OmitIfDefault | CodegenIfChanged | ColorSRGB, .label = "Letterbox Color"),
        JE_PROXY(PixelPerfectCameraComponent, "pixelRatio", layout.scale, .flags = kLayoutValue, .label = "Current Pixel Ratio"),
        JE_PROXY(PixelPerfectCameraComponent, "baseWidth", layout.baseWidth, .flags = kLayoutValue, .unit = Unit::Pixels),
        JE_PROXY(PixelPerfectCameraComponent, "baseHeight", layout.baseHeight, .flags = kLayoutValue, .unit = Unit::Pixels),
        JE_PROXY(PixelPerfectCameraComponent, "targetWidth", layout.targetWidth, .flags = kLayoutValue, .unit = Unit::Pixels),
        JE_PROXY(PixelPerfectCameraComponent, "targetHeight", layout.targetHeight, .flags = kLayoutValue, .unit = Unit::Pixels),
        JE_PROXY(PixelPerfectCameraComponent, "viewport", layout.viewport, .flags = kLayoutValue, .unit = Unit::Pixels),
        JE_PROXY(PixelPerfectCameraComponent, "cropped", layout.cropped, .flags = kLayoutValue),
        JE_FIELD(PixelPerfectCameraComponent, snappedPosition, .flags = kLayoutValue),
        JE_FIELD(PixelPerfectCameraComponent, subpixelOffset, .flags = kLayoutValue, .unit = Unit::Pixels),
    };

    constexpr const char* kPixelPerfectCameraCodegenOrder[] = {
        "referenceWidth", "referenceHeight", "upscaleMode", "upscaleRenderTexture", "pixelSnapping", "rotationMode", "subpixelSmoothing",
        "letterboxColor", "enabled"
    };

    // the spelling of the spec's YAML sketch (S41 §6.1), read as the key
    constexpr const char* kPixelPerfectCameraAliases[] = {"pixelPerfectCamera"};

}

const ComponentDesc kPixelPerfectCameraDesc{
    .type = ComponentType::PixelPerfectCameraComponent, .cppName = "PixelPerfectCameraComponent", .key = "pixelperfectcamera",
    .displayName = "Pixel Perfect Camera", .icon = ICON_FA_TABLE_CELLS,
    .body = JE_STRUCT_DESC(PixelPerfectCameraComponent, "PixelPerfectCameraComponent", kPixelPerfectCameraFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(PixelPerfectCameraComponent),
    .keyAliases = kPixelPerfectCameraAliases,
    .codegenVar = "pixelPerfectCamera", .codegenOrder = kPixelPerfectCameraCodegenOrder,
};

}
