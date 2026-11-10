// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's components of its 27d562a6b batch (the second origin/main merge of SYNC#4) on S08's
// reflection tables (docs/specs/S08-reflection.md): the Camera Shaker and Camera Follow (camera
// shake), the Text Effect (ui-scaling-text), the Video Player (video-mp4) and the Safe Area
// (safe-area). Each keeps upstream's codec (Stream.cpp: the keys its files have) and its exported
// code (Factory.cpp); the tables give the properties, schema, Inspector metadata and the cooked
// scenes' fields. The Text Effect's layers (a fixed array of layer structs) stay its codec's: the
// table lists its flat fields, and its extra properties the layers' "layers[N].<field>" (upstream's
// Catalog paths, which set_properties and get_entity use).

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/CameraFollowComponent.h"
#include "component/CameraShakerComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/TextEffectComponent.h"
#include "component/VideoPlayerComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    template<typename E>
    constexpr int v(E value) { return static_cast<int>(value); }

    // ---------------------------------------------------------------- enums

    constexpr EnumItem kCameraShakeModeItems[] = {
        {v(CameraShakeMode::Auto), "auto", "Auto", "CameraShakeMode::Auto"},
        {v(CameraShakeMode::Mode3D), "3d", "3D", "CameraShakeMode::Mode3D"},
        {v(CameraShakeMode::Mode2D), "2d", "2D", "CameraShakeMode::Mode2D"},
    };
    const EnumDesc kCameraShakeMode{"CameraShakeMode", kCameraShakeModeItems, v(CameraShakeMode::Auto)};

    constexpr EnumItem kFollowOffsetSpaceItems[] = {
        {v(FollowOffsetSpace::World), "world", "World", "FollowOffsetSpace::World"},
        {v(FollowOffsetSpace::Target), "target", "Target", "FollowOffsetSpace::Target"},
        {v(FollowOffsetSpace::TargetYaw), "target_yaw", "Target Yaw", "FollowOffsetSpace::TargetYaw"},
    };
    const EnumDesc kFollowOffsetSpace{"FollowOffsetSpace", kFollowOffsetSpaceItems, v(FollowOffsetSpace::World)};

    constexpr EnumItem kVideoPlayerSourceItems[] = {
        {v(VideoPlayerSource::CLIP), "clip", "Video Clip", "VideoPlayerSource::CLIP"},
        {v(VideoPlayerSource::URL), "url", "URL", "VideoPlayerSource::URL"},
    };
    const EnumDesc kVideoPlayerSource{"VideoPlayerSource", kVideoPlayerSourceItems, v(VideoPlayerSource::CLIP)};

    constexpr EnumItem kVideoTimeUpdateModeItems[] = {
        {v(VideoTimeUpdateMode::GAME_TIME), "game_time", "Game Time", "VideoTimeUpdateMode::GAME_TIME"},
        {v(VideoTimeUpdateMode::UNSCALED_GAME_TIME), "unscaled_game_time", "Unscaled Game Time", "VideoTimeUpdateMode::UNSCALED_GAME_TIME"},
    };
    const EnumDesc kVideoTimeUpdateMode{"VideoTimeUpdateMode", kVideoTimeUpdateModeItems, v(VideoTimeUpdateMode::GAME_TIME)};

    constexpr EnumItem kVideoRenderModeItems[] = {
        {v(VideoRenderMode::CAMERA_FAR_PLANE), "camera_far_plane", "Camera Far Plane", "VideoRenderMode::CAMERA_FAR_PLANE"},
        {v(VideoRenderMode::CAMERA_NEAR_PLANE), "camera_near_plane", "Camera Near Plane", "VideoRenderMode::CAMERA_NEAR_PLANE"},
        {v(VideoRenderMode::RENDER_TEXTURE), "render_texture", "Render Texture", "VideoRenderMode::RENDER_TEXTURE"},
        {v(VideoRenderMode::MATERIAL_OVERRIDE), "material_override", "Material Override", "VideoRenderMode::MATERIAL_OVERRIDE"},
        {v(VideoRenderMode::API_ONLY), "api_only", "API Only", "VideoRenderMode::API_ONLY"},
    };
    const EnumDesc kVideoRenderMode{"VideoRenderMode", kVideoRenderModeItems, v(VideoRenderMode::RENDER_TEXTURE)};

    constexpr EnumItem kVideoAspectRatioItems[] = {
        {v(VideoAspectRatio::NO_SCALING), "no_scaling", "No Scaling", "VideoAspectRatio::NO_SCALING"},
        {v(VideoAspectRatio::FIT_VERTICALLY), "fit_vertically", "Fit Vertically", "VideoAspectRatio::FIT_VERTICALLY"},
        {v(VideoAspectRatio::FIT_HORIZONTALLY), "fit_horizontally", "Fit Horizontally", "VideoAspectRatio::FIT_HORIZONTALLY"},
        {v(VideoAspectRatio::FIT_INSIDE), "fit_inside", "Fit Inside", "VideoAspectRatio::FIT_INSIDE"},
        {v(VideoAspectRatio::FIT_OUTSIDE), "fit_outside", "Fit Outside", "VideoAspectRatio::FIT_OUTSIDE"},
        {v(VideoAspectRatio::STRETCH), "stretch", "Stretch", "VideoAspectRatio::STRETCH"},
    };
    const EnumDesc kVideoAspectRatio{"VideoAspectRatio", kVideoAspectRatioItems, v(VideoAspectRatio::FIT_INSIDE)};

    constexpr EnumItem kVideoAudioOutputModeItems[] = {
        {v(VideoAudioOutputMode::NONE), "none", "None", "VideoAudioOutputMode::NONE"},
        {v(VideoAudioOutputMode::AUDIO_SOURCE), "audio_source", "Audio Source", "VideoAudioOutputMode::AUDIO_SOURCE"},
        {v(VideoAudioOutputMode::DIRECT), "direct", "Direct", "VideoAudioOutputMode::DIRECT"},
    };
    const EnumDesc kVideoAudioOutputMode{"VideoAudioOutputMode", kVideoAudioOutputModeItems, v(VideoAudioOutputMode::DIRECT)};

    // ---------------------------------------------------------------- Camera Shaker

    constexpr FieldDesc kCameraShakerFields[] = {
        JE_FIELD(CameraShakerComponent, enabled),
        JE_FIELD(CameraShakerComponent, global, .tooltip = "Shaken by the global CameraShake calls."),
        JE_FIELD(CameraShakerComponent, unscaledTime),
        JE_FIELD(CameraShakerComponent, mode, .enumDesc = &kCameraShakeMode),
        JE_FIELD(CameraShakerComponent, positionScale, .min = 0.0f),
        JE_FIELD(CameraShakerComponent, rotationScale, .min = 0.0f),
        JE_FIELD(CameraShakerComponent, startPreset, .tooltip = "A built-in preset name or a .shake file, played when enabled."),
        JE_FIELD(CameraShakerComponent, traumaDecay, .min = 0.0f),
        JE_FIELD(CameraShakerComponent, traumaPower, .min = 1.0f, .max = 4.0f),
        JE_FIELD(CameraShakerComponent, traumaMaxOffset),
        JE_FIELD(CameraShakerComponent, traumaMaxAngle),
        JE_FIELD(CameraShakerComponent, traumaFrequency, .min = 0.0f),
        JE_FIELD(CameraShakerComponent, pixelSnap, .flags = OmitIfDefault | CodegenIfChanged, .label = "Pixel Snap",
                 .tooltip = "Rounds the shake to whole art pixels (the Pixel Perfect Camera's grid, else screen pixels) and drops the roll. 2D only."),
        JE_FIELD(CameraShakerComponent, tickRate, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Hertz, .min = 0.0f, .max = 240.0f, .label = "Tick Rate",
                 .tooltip = "The shake moves only on ticks of 1 / Tick Rate seconds (0: every frame). 2D only."),
    };
    const ComponentCodec kCameraShakerCodec = JE_STREAM_CODEC(CameraShakerComponent, encodeCameraShakerComponent, decodeCameraShakerComponent);
    constexpr const char* kCameraShakerKnownKeys[] = {"enabled", "global", "mode", "positionScale", "rotationScale", "startPreset",
        "traumaDecay", "traumaFrequency", "traumaMaxAngle", "traumaMaxOffset", "traumaPower", "unscaledTime", "pixelSnap", "tickRate"};

    // ---------------------------------------------------------------- Camera Follow

    constexpr FieldDesc kCameraFollowFields[] = {
        JE_FIELD(CameraFollowComponent, enabled),
        JE_FIELD(CameraFollowComponent, target, .ops = &ops::Entity),
        JE_FIELD(CameraFollowComponent, offset),
        JE_FIELD(CameraFollowComponent, offsetSpace, .enumDesc = &kFollowOffsetSpace),
        JE_FIELD(CameraFollowComponent, damping),
        JE_FIELD(CameraFollowComponent, maxSpeed, .min = 0.0f),
        JE_FIELD(CameraFollowComponent, lookAt),
        JE_FIELD(CameraFollowComponent, lookAtOffset),
        JE_FIELD(CameraFollowComponent, rotationDamping, .min = 0.0f),
        JE_FIELD(CameraFollowComponent, deadZone),
        JE_FIELD(CameraFollowComponent, useBounds),
        JE_FIELD(CameraFollowComponent, boundsMin),
        JE_FIELD(CameraFollowComponent, boundsMax),
        JE_FIELD(CameraFollowComponent, unscaledTime),
    };
    const ComponentCodec kCameraFollowCodec = JE_STREAM_CODEC(CameraFollowComponent, encodeCameraFollowComponent, decodeCameraFollowComponent);
    constexpr const char* kCameraFollowKnownKeys[] = {"boundsMax", "boundsMin", "damping", "deadZone", "enabled", "lookAt", "lookAtOffset",
        "maxSpeed", "offset", "offsetSpace", "rotationDamping", "target", "unscaledTime", "useBounds"};

    // ---------------------------------------------------------------- Text Effect

    // the update flags upstream's Catalog gave them: a range or layer edit re-applies the effect
    constexpr uint64_t kTextEffectApply = UpdateFlags_TextEffect;

    constexpr FieldDesc kTextEffectFields[] = {
        JE_FIELD(TextEffectComponent, enabled, .updateFlags = kTextEffectApply),
        JE_FIELD(TextEffectComponent, autoPlay),
        JE_FIELD(TextEffectComponent, loop),
        JE_FIELD(TextEffectComponent, useUnscaledTime),
        JE_FIELD(TextEffectComponent, startIndex, .updateFlags = kTextEffectApply, .min = 0.0f),
        JE_FIELD(TextEffectComponent, length, .updateFlags = kTextEffectApply, .min = -1.0f),
        JE_FIELD(TextEffectComponent, forWords, .updateFlags = kTextEffectApply),
    };
    const ComponentCodec kTextEffectCodec = JE_STREAM_CODEC(TextEffectComponent, encodeTextEffectComponent, decodeTextEffectComponent);
    constexpr const char* kTextEffectKnownKeys[] = {"autoPlay", "enabled", "forWords", "layers", "length", "loop", "startIndex", "useUnscaledTime"};

    // The layers (a fixed std::array of layer structs, saved by the codec): "layers[N].<field>" as
    // upstream's Catalog resolved and listed them (getTextEffectLayerPropertyFast); the enum names
    // of type and mix are McpValues' ("layers[].type", "layers[].mix")
    PropertyData textEffectLayerProperty(TextEffectLayer& layer, const std::string& field) {
        static TextEffectLayer d;
        const uint64_t flags = kTextEffectApply;
        if (field == "type") return {PropertyType::Enum, flags, &d.type, &layer.type};
        if (field == "enabled") return {PropertyType::Bool, flags, &d.enabled, &layer.enabled};
        if (field == "amplitude") return {PropertyType::Float, flags, &d.amplitude, &layer.amplitude};
        if (field == "duration") return {PropertyType::Float, flags, &d.duration, &layer.duration};
        if (field == "spread") return {PropertyType::Float, flags, &d.spread, &layer.spread};
        if (field == "phaseStep") return {PropertyType::Float, flags, &d.phaseStep, &layer.phaseStep};
        if (field == "frequency") return {PropertyType::Float, flags, &d.frequency, &layer.frequency};
        if (field == "charactersPerSecond") return {PropertyType::Float, flags, &d.charactersPerSecond, &layer.charactersPerSecond};
        if (field == "fadeTime") return {PropertyType::Float, flags, &d.fadeTime, &layer.fadeTime};
        if (field == "gradient") return {PropertyType::Custom, flags, &d.gradient, &layer.gradient};
        if (field == "mix") return {PropertyType::Enum, flags, &d.mix, &layer.mix};
        if (field == "saturation") return {PropertyType::Float, flags, &d.saturation, &layer.saturation};
        if (field == "lightness") return {PropertyType::Float, flags, &d.lightness, &layer.lightness};
        return PropertyData();
    }

    constexpr const char* kTextEffectLayerFields[] = {"type", "enabled", "amplitude", "duration", "spread", "phaseStep", "frequency",
                                                      "charactersPerSecond", "fadeTime", "gradient", "mix", "saturation", "lightness"};

    PropertyData textEffectExtraProperty(void* comp, const std::string& path) {
        TextEffectComponent& effect = *static_cast<TextEffectComponent*>(comp);
        constexpr size_t prefix = 7;  // "layers["
        if (path.compare(0, prefix, "layers[") != 0) return PropertyData();
        size_t pos = prefix;
        size_t index = 0;
        if (pos >= path.size() || path[pos] < '0' || path[pos] > '9') return PropertyData();
        while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') index = index * 10 + static_cast<size_t>(path[pos++] - '0');
        if (pos + 1 >= path.size() || path[pos] != ']' || path[pos + 1] != '.' || index >= effect.layers.size()) return PropertyData();
        return textEffectLayerProperty(effect.layers[index], path.substr(pos + 2));
    }

    void textEffectExtraProperties(void* comp, std::map<std::string, PropertyData>& out) {
        static TextEffectLayer defaultLayer;
        TextEffectComponent* effect = static_cast<TextEffectComponent*>(comp);
        for (size_t i = 0; i < static_cast<size_t>(MAX_TEXT_EFFECT_LAYERS); i++) {
            const std::string prefix = "layers[" + std::to_string(i) + "].";
            TextEffectLayer& layer = effect ? effect->layers[i] : defaultLayer;
            for (const char* field : kTextEffectLayerFields) {
                PropertyData property = textEffectLayerProperty(layer, field);
                if (!effect) property.ref = nullptr;
                out[prefix + field] = property;
            }
        }
    }

    // ---------------------------------------------------------------- Video Player

    constexpr FieldDesc kVideoPlayerFields[] = {
        JE_FIELD(VideoPlayerComponent, enabled),
        JE_FIELD(VideoPlayerComponent, source, .enumDesc = &kVideoPlayerSource),
        JE_FIELD(VideoPlayerComponent, clip),
        JE_FIELD(VideoPlayerComponent, url),
        JE_FIELD(VideoPlayerComponent, playOnAwake),
        JE_FIELD(VideoPlayerComponent, waitForFirstFrame),
        JE_FIELD(VideoPlayerComponent, isLooping),
        JE_FIELD(VideoPlayerComponent, skipOnDrop),
        JE_FIELD(VideoPlayerComponent, playbackSpeed, .min = 0.0625f, .max = 16.0f),
        JE_FIELD(VideoPlayerComponent, timeUpdateMode, .enumDesc = &kVideoTimeUpdateMode),
        JE_FIELD(VideoPlayerComponent, renderMode, .enumDesc = &kVideoRenderMode),
        JE_FIELD(VideoPlayerComponent, targetCamera, .ops = &ops::Entity),
        JE_FIELD(VideoPlayerComponent, targetCameraAlpha, .min = 0.0f, .max = 1.0f),
        JE_FIELD(VideoPlayerComponent, aspectRatio, .enumDesc = &kVideoAspectRatio),
        JE_FIELD(VideoPlayerComponent, targetMaterialRenderer, .ops = &ops::Entity),
        JE_FIELD(VideoPlayerComponent, targetMaterialProperty),
        JE_FIELD(VideoPlayerComponent, targetSubmesh, .min = -1.0f),
        JE_FIELD(VideoPlayerComponent, renderTextureWidth),
        JE_FIELD(VideoPlayerComponent, renderTextureHeight),
        JE_FIELD(VideoPlayerComponent, audioOutputMode, .enumDesc = &kVideoAudioOutputMode),
        JE_FIELD(VideoPlayerComponent, directVolume, .min = 0.0f, .max = 1.0f),
        JE_FIELD(VideoPlayerComponent, directMute),
        JE_FIELD(VideoPlayerComponent, targetAudioSource, .ops = &ops::Entity),
    };
    const ComponentCodec kVideoPlayerCodec = JE_STREAM_CODEC(VideoPlayerComponent, encodeVideoPlayerComponent, decodeVideoPlayerComponent);
    constexpr const char* kVideoPlayerKnownKeys[] = {"aspectRatio", "audioOutputMode", "clip", "directMute", "directVolume", "enabled",
        "isLooping", "playOnAwake", "playbackSpeed", "renderMode", "renderTextureHeight", "renderTextureWidth", "skipOnDrop", "source",
        "targetAudioSource", "targetCamera", "targetCameraAlpha", "targetMaterialProperty", "targetMaterialRenderer", "targetSubmesh",
        "timeUpdateMode", "url", "waitForFirstFrame"};

    // ---------------------------------------------------------------- Safe Area

    constexpr FieldDesc kSafeAreaFields[] = {
        JE_FIELD(SafeAreaComponent, enabled),
        JE_FIELD(SafeAreaComponent, left),
        JE_FIELD(SafeAreaComponent, top),
        JE_FIELD(SafeAreaComponent, right),
        JE_FIELD(SafeAreaComponent, bottom),
        JE_FIELD(SafeAreaComponent, includeExtraInsets),
    };
    const ComponentCodec kSafeAreaCodec = JE_STREAM_CODEC(SafeAreaComponent, encodeSafeAreaComponent, decodeSafeAreaComponent);
    constexpr const char* kSafeAreaKnownKeys[] = {"bottom", "enabled", "includeExtraInsets", "left", "right", "top"};

}

const ComponentDesc kCameraShakerDesc{
    .type = ComponentType::CameraShakerComponent, .cppName = "CameraShakerComponent", .key = "camerashaker", .displayName = "Camera Shaker",
    .icon = ICON_FA_BURST,
    .body = JE_STRUCT_DESC(CameraShakerComponent, "CameraShakerComponent", kCameraShakerFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(CameraShakerComponent),
    .codec = &kCameraShakerCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createCameraShakerComponent),
    .extraKeys = kCameraShakerKnownKeys,
};

const ComponentDesc kCameraFollowDesc{
    .type = ComponentType::CameraFollowComponent, .cppName = "CameraFollowComponent", .key = "camerafollow", .displayName = "Camera Follow",
    .icon = ICON_FA_LOCATION_CROSSHAIRS,
    .body = JE_STRUCT_DESC(CameraFollowComponent, "CameraFollowComponent", kCameraFollowFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(CameraFollowComponent),
    .codec = &kCameraFollowCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createCameraFollowComponent),
    .extraKeys = kCameraFollowKnownKeys,
};

const ComponentDesc kTextEffectDesc{
    .type = ComponentType::TextEffectComponent, .cppName = "TextEffectComponent", .key = "texteffect", .displayName = "Text Effect",
    .icon = ICON_FA_WAND_MAGIC_SPARKLES,
    .body = JE_STRUCT_DESC(TextEffectComponent, "TextEffectComponent", kTextEffectFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TextEffectComponent),
    .codec = &kTextEffectCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createTextEffectComponent),
    .extraKeys = kTextEffectKnownKeys,
    .extraProperty = &textEffectExtraProperty,
    .extraProperties = &textEffectExtraProperties,
};

const ComponentDesc kVideoPlayerDesc{
    .type = ComponentType::VideoPlayerComponent, .cppName = "VideoPlayerComponent", .key = "videoplayer", .displayName = "Video Player",
    .icon = ICON_FA_FILM,
    .body = JE_STRUCT_DESC(VideoPlayerComponent, "VideoPlayerComponent", kVideoPlayerFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(VideoPlayerComponent),
    .codec = &kVideoPlayerCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createVideoPlayerComponent),
    .extraKeys = kVideoPlayerKnownKeys,
};

const ComponentDesc kSafeAreaDesc{
    .type = ComponentType::SafeAreaComponent, .cppName = "SafeAreaComponent", .key = "safearea", .displayName = "Safe Area",
    .icon = ICON_FA_MOBILE_SCREEN_BUTTON,
    .body = JE_STRUCT_DESC(SafeAreaComponent, "SafeAreaComponent", kSafeAreaFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SafeAreaComponent),
    .codec = &kSafeAreaCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createSafeAreaComponent),
    .extraKeys = kSafeAreaKnownKeys,
};

}
