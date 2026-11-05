// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Cameras and audio sources (docs/specs/S08-reflection.md P1). Field order is the YAML key order.

#include "reflect/Components.h"

#include "Factory.h"
#include "pool/SoundPool.h"
#include "component/AudioSourceComponent.h"
#include "component/CameraComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    // upstream's audio-mixer load types (SoundPool::loadTypeName's tokens)
    constexpr EnumItem kAudioLoadTypeItems[] = {
        {static_cast<int>(AudioLoadType::Auto), "auto", "Auto", "AudioLoadType::Auto"},
        {static_cast<int>(AudioLoadType::DecompressOnLoad), "decompress_on_load", "Decompress On Load", "AudioLoadType::DecompressOnLoad"},
        {static_cast<int>(AudioLoadType::CompressedInMemory), "compressed_in_memory", "Compressed In Memory", "AudioLoadType::CompressedInMemory"},
        {static_cast<int>(AudioLoadType::Streaming), "streaming", "Streaming", "AudioLoadType::Streaming"},
    };
    const EnumDesc kAudioLoadType{"AudioLoadType", kAudioLoadTypeItems, static_cast<int>(AudioLoadType::Auto)};

    // ---------------------------------------------------------------- Camera

    bool cameraUsesTarget(const void* camera) { return static_cast<const CameraComponent*>(camera)->useTarget; }
    bool cameraPerspective(const void* camera) { return static_cast<const CameraComponent*>(camera)->type == CameraType::CAMERA_PERSPECTIVE; }
    bool cameraOrthographic(const void* camera) {
        const CameraType type = static_cast<const CameraComponent*>(camera)->type;
        return type == CameraType::CAMERA_ORTHO || type == CameraType::CAMERA_UI;
    }
    bool cameraRendersToTexture(const void* camera) { return static_cast<const CameraComponent*>(camera)->renderToTexture; }
    bool cameraOrderIndependent(const void* camera) {
        return static_cast<const CameraComponent*>(camera)->transparencyMode == TransparencyMode::ORDER_INDEPENDENT;
    }
    // its clips and aspect are the canvas's, set when it becomes the scene's camera and each
    // time the canvas changes (RenderSystem::updateCameraSizeForCanvas): in the editor, the size
    // of the view that drew last
    bool cameraSizedByCanvas(const void* camera) {
        const CameraComponent* component = static_cast<const CameraComponent*>(camera);
        return component->autoResize && !component->renderToTexture;
    }

    constexpr uint64_t kCamera = UpdateFlags_Camera;

    constexpr FieldDesc kCameraFields[] = {
        JE_FIELD(CameraComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(CameraComponent, type, .enumDesc = &enums::kCameraType, .updateFlags = kCamera),
        // read by RenderSystem when the camera draws
        JE_FIELD(CameraComponent, cullingMask, .ops = &ops::LayerMask, .uiOrder = 1),
        // written only when finite; the base made finite when absent (Stream::encodeFiniteVector3)
        JE_FIELD(CameraComponent, target, .ops = &ops::FiniteVector3Always, .flags = OmitIfNonFinite, .updateFlags = kCamera,
                 .visibleIf = &cameraUsesTarget, .uiOrder = 3),
        JE_FIELD(CameraComponent, up, .ops = &ops::FiniteVector3Always, .flags = OmitIfNonFinite, .updateFlags = kCamera, .drawer = "direction",
                 .uiOrder = 4),
        JE_FIELD_NAMED(CameraComponent, leftClip, "left", .yamlKey = "leftClip", .ops = &ops::FiniteFloatAlways, .flags = OmitIfNonFinite,
                       .updateFlags = kCamera, .visibleIf = &cameraOrthographic, .uiOrder = 7, .derivedIf = &cameraSizedByCanvas),
        JE_FIELD_NAMED(CameraComponent, rightClip, "right", .yamlKey = "rightClip", .ops = &ops::FiniteFloatAlways, .flags = OmitIfNonFinite,
                       .updateFlags = kCamera, .visibleIf = &cameraOrthographic, .uiOrder = 8, .derivedIf = &cameraSizedByCanvas),
        JE_FIELD_NAMED(CameraComponent, bottomClip, "bottom", .yamlKey = "bottomClip", .ops = &ops::FiniteFloatAlways, .flags = OmitIfNonFinite,
                       .updateFlags = kCamera, .visibleIf = &cameraOrthographic, .uiOrder = 9, .derivedIf = &cameraSizedByCanvas),
        JE_FIELD_NAMED(CameraComponent, topClip, "top", .yamlKey = "topClip", .ops = &ops::FiniteFloatAlways, .flags = OmitIfNonFinite,
                       .updateFlags = kCamera, .visibleIf = &cameraOrthographic, .uiOrder = 10, .derivedIf = &cameraSizedByCanvas),
        JE_FIELD(CameraComponent, yfov, .ops = &ops::PositiveFiniteFloatAlways, .updateFlags = kCamera, .unit = Unit::Radians,
                 .step = 0.01f, .format = "%.3f", .label = "Y FOV", .visibleIf = &cameraPerspective, .uiOrder = 5),
        JE_FIELD(CameraComponent, aspect, .ops = &ops::PositiveFiniteFloatAlways, .updateFlags = kCamera, .visibleIf = &cameraPerspective,
                 .uiOrder = 6, .derivedIf = &cameraSizedByCanvas),
        JE_FIELD_NAMED(CameraComponent, nearClip, "near", .yamlKey = "nearClip", .ops = &ops::FiniteFloatAlways, .flags = OmitIfNonFinite,
                       .updateFlags = kCamera, .unit = Unit::Meters, .uiOrder = 11),
        JE_FIELD_NAMED(CameraComponent, farClip, "far", .yamlKey = "farClip", .ops = &ops::FiniteFloatAlways, .flags = OmitIfNonFinite,
                       .updateFlags = kCamera, .unit = Unit::Meters, .uiOrder = 12),
        JE_FIELD(CameraComponent, renderToTexture, .flags = Hidden, .updateFlags = kCamera),
        JE_FIELD(CameraComponent, transparentSort, .flags = Hidden),
        JE_FIELD(CameraComponent, depthTest, .uiOrder = 13),
        JE_FIELD(CameraComponent, useTarget, .updateFlags = kCamera, .uiOrder = 2),
        JE_FIELD(CameraComponent, autoResize, .updateFlags = kCamera, .uiOrder = 14),
        // applied by RenderSystem::update (no update flag needed)
        JE_FIELD(CameraComponent, framebufferWidth, .step = 1.0f, .label = "Width", .group = "Render Target", .visibleIf = &cameraRendersToTexture,
                 .uiOrder = 15),
        JE_FIELD(CameraComponent, framebufferHeight, .step = 1.0f, .label = "Height", .group = "Render Target",
                 .visibleIf = &cameraRendersToTexture, .uiOrder = 16),
        // render-target framebuffers have no mipmaps
        JE_FIELD(CameraComponent, framebufferFilter, .enumDesc = &enums::kTextureFilter, .label = "Filter", .group = "Render Target",
                 .visibleIf = &cameraRendersToTexture, .uiOrder = 17,
                 .enumMask = enums::bit((int)TextureFilter::NEAREST) | enums::bit((int)TextureFilter::LINEAR)),
        // S57 R4: read by RenderSystem each frame (it reloads the variants itself), written only
        // away from their defaults
        JE_FIELD(CameraComponent, transparencyMode, .enumDesc = &enums::kTransparencyMode, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Sorted draws blended renderers back to front. Order Independent draws alpha-blended materials through "
                            "weighted blended OIT: one extra pass into RGBA16F+R16F targets and a fullscreen resolve; Sorted on "
                            "devices without half-float targets.",
                 .uiOrder = 18),
        JE_FIELD(CameraComponent, oitWeightScale, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0001f, .label = "OIT Weight Scale",
                 .tooltip = "Multiplies the view depths the OIT weight takes: lower it for large scenes, so far layers keep their weight.",
                 .visibleIf = &cameraOrderIndependent, .uiOrder = 19),
    };

    constexpr const char* kCameraCodegenOrder[] = {
        "type", "target", "up", "left", "right", "bottom", "top", "yfov", "aspect", "near", "far", "renderToTexture", "transparentSort",
        "depthTest", "cullingMask", "useTarget", "autoResize", "framebufferWidth", "framebufferHeight", "framebufferFilter", "transparencyMode", "oitWeightScale", "enabled"
    };

    // ---------------------------------------------------------------- Audio Source

    std::string exportedState(const FieldDesc&, const void* value, const CodegenContext&) {
        // a build starts a Playing source stopped, and plays it through startTrigger
        // (audioSourceCodegen); a Paused one stays Paused, as the editor's Play loads it
        const AudioSourceState state = *static_cast<const AudioSourceState*>(value);
        return state == AudioSourceState::Paused ? "AudioSourceState::Paused" : "AudioSourceState::Stopped";
    }

    constexpr TypeOps kAudioStateOps = detail::withLiteral(ops::Enum<AudioSourceState>, &exportedState);

    constexpr uint64_t kAudio = UpdateFlags_AudioSource;

    // what was loaded came from the old path
    void audioFileMoved(void* owner) {
        AudioSourceComponent& audio = *static_cast<AudioSourceComponent*>(owner);
        audio.loaded = false;
        audio.loadFailed = false;
    }

    constexpr FieldDesc kAudioSourceFields[] = {
        JE_FIELD(AudioSourceComponent, enabled, .flags = CodegenIfChanged),
        JE_FIELD(AudioSourceComponent, state, .ops = &kAudioStateOps, .enumDesc = &enums::kAudioSourceState, .updateFlags = kAudio),
        JE_FIELD(AudioSourceComponent, filename, .ops = &ops::AssetRefIfSet, .updateFlags = kAudio, .asset = AssetKind::Audio, .label = "Audio Clip", .assetMoved = &audioFileMoved),
        JE_FIELD(AudioSourceComponent, enableClocked, .updateFlags = kAudio, .label = "Clocked", .group = "Playback", .uiOrder = 7),
        JE_FIELD(AudioSourceComponent, volume, .ops = &ops::FiniteDouble, .flags = OmitIfNonFinite, .updateFlags = kAudio, .min = 0.0f,
                 .step = 0.01f, .format = "%.3f", .group = "Playback", .uiOrder = 1),
        JE_FIELD(AudioSourceComponent, speed, .ops = &ops::FiniteFloat, .flags = OmitIfNonFinite, .updateFlags = kAudio, .min = 0.0f,
                 .step = 0.01f, .format = "%.3f", .group = "Playback", .uiOrder = 2),
        JE_FIELD(AudioSourceComponent, pan, .ops = &ops::FiniteFloat, .flags = OmitIfNonFinite, .updateFlags = kAudio, .step = 0.01f,
                 .format = "%.3f", .tooltip = "-1 left, 0 center, 1 right", .group = "Playback", .uiOrder = 3),
        JE_FIELD(AudioSourceComponent, looping, .updateFlags = kAudio, .group = "Playback", .uiOrder = 4),
        JE_FIELD(AudioSourceComponent, loopingPoint, .ops = &ops::FiniteDouble, .flags = OmitIfNonFinite, .updateFlags = kAudio, .unit = Unit::Seconds,
                 .min = 0.0f, .step = 0.01f, .format = "%.3f", .label = "Loop Point", .group = "Playback", .uiOrder = 5),
        JE_FIELD(AudioSourceComponent, protectVoice, .updateFlags = kAudio, .group = "Playback", .uiOrder = 6),
        JE_FIELD(AudioSourceComponent, inaudibleBehaviorMustTick, .updateFlags = kAudio, .label = "Inaudible Must Tick", .group = "Playback",
                 .uiOrder = 8),
        JE_FIELD(AudioSourceComponent, inaudibleBehaviorKill, .updateFlags = kAudio, .label = "Inaudible Kill", .group = "Playback", .uiOrder = 9),
        JE_FIELD(AudioSourceComponent, minDistance, .ops = &ops::FiniteFloat, .flags = OmitIfNonFinite, .updateFlags = kAudio, .unit = Unit::Meters,
                 .min = 0.0f, .step = 0.01f, .format = "%.3f", .group = "3D Audio Source Settings", .uiOrder = 10),
        JE_FIELD(AudioSourceComponent, maxDistance, .ops = &ops::FiniteFloat, .flags = OmitIfNonFinite, .updateFlags = kAudio, .unit = Unit::Meters,
                 .min = 0.0f, .step = 0.01f, .format = "%.3f", .group = "3D Audio Source Settings", .uiOrder = 11),
        JE_FIELD(AudioSourceComponent, attenuationModel, .enumDesc = &enums::kAudioSourceAttenuation, .updateFlags = kAudio, .label = "Attenuation",
                 .group = "3D Audio Source Settings", .uiOrder = 12),
        JE_FIELD(AudioSourceComponent, attenuationRolloffFactor, .ops = &ops::FiniteFloat, .flags = OmitIfNonFinite, .updateFlags = kAudio,
                 .min = 0.0f, .step = 0.01f, .format = "%.3f", .label = "Rolloff", .group = "3D Audio Source Settings", .uiOrder = 13),
        JE_FIELD(AudioSourceComponent, dopplerFactor, .ops = &ops::FiniteFloat, .flags = OmitIfNonFinite, .updateFlags = kAudio, .min = 0.0f,
                 .step = 0.01f, .format = "%.3f", .label = "Doppler", .group = "3D Audio Source Settings", .uiOrder = 14),
        // the loaded clip's: read only
        // upstream (audio-mixer): written only when not their default; the priority and the voice
        // limit read clamped into their range (upstream's decode)
        // the group: the "outputGroup" property is its name (upstream's Catalog and the Inspector's
        // Output row set it, "" = Master); the update resolves the id (Catalog::updateEntity), saved
        // as upstream's "outputGroup" key and reached as outputGroupId
        JE_FIELD_NAMED(AudioSourceComponent, outputGroup, "outputGroupId", .yamlKey = "outputGroup",
                       .flags = OmitIfDefault | Hidden | NotListed | CodegenIfChanged, .updateFlags = kAudio),
        JE_FIELD_NAMED(AudioSourceComponent, outputGroupName, "outputGroup", .yamlKey = "outputGroupName",
                       .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kAudio, .label = "Output"),
        JE_FIELD(AudioSourceComponent, loadType, .enumDesc = &kAudioLoadType, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kAudio),
        JE_FIELD(AudioSourceComponent, priority, .ops = &ops::IntClampedOnRead, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kAudio,
                 .min = 0.0f, .max = 256.0f),
        JE_FIELD(AudioSourceComponent, maxVoices, .ops = &ops::IntClampedOnRead, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kAudio,
                 .min = 1.0f, .max = 64.0f),
        JE_FIELD(AudioSourceComponent, ignoreListenerPause, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kAudio),
        JE_FIELD(AudioSourceComponent, length, .flags = Transient | NoDefault | ReadOnly | Hidden, .unit = Unit::Seconds),
        JE_FIELD(AudioSourceComponent, playingTime, .flags = Transient | NoDefault | ReadOnly | Hidden, .unit = Unit::Seconds),
    };

    constexpr const char* kAudioSourceCodegenOrder[] = {
        "state", "filename", "enableClocked", "volume", "speed", "pan", "looping", "loopingPoint", "protectVoice", "inaudibleBehaviorMustTick",
        "inaudibleBehaviorKill", "minDistance", "maxDistance", "attenuationModel", "attenuationRolloffFactor", "dopplerFactor",
        // upstream's audio-mixer fields, only when not their default (upstream's Factory)
        "outputGroupId", "outputGroup", "loadType", "priority", "maxVoices", "ignoreListenerPause"
    };

    void audioSourceCodegen(const void* comp, const CodegenContext& ctx, std::string& out) {
        const AudioSourceComponent& audio = *static_cast<const AudioSourceComponent*>(comp);
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        out += ind + ctx.variable + ".needUpdate = true;\n";
        if (audio.state == AudioSourceState::Playing) out += ind + ctx.variable + ".startTrigger = true;\n";
        if (!audio.enabled) out += ind + ctx.variable + ".enabled = false;\n";
    }

    // The loaded sample stays while the file is the same; any other runtime state starts afresh
    void audioSourcePostDecode(void* comp, const DecodeContext& ctx) {
        AudioSourceComponent& audio = *static_cast<AudioSourceComponent*>(comp);
        const AudioSourceComponent* old = static_cast<const AudioSourceComponent*>(ctx.old);
        const std::string oldFilename = old ? old->filename.path : std::string();
        // (upstream's audio-mixer rules) a sample is "<file>#<load type>": another load type is
        // another sample
        const bool keepLoadedSample = old && oldFilename == audio.filename && old->sample && old->loaded &&
                                      old->loadType == audio.loadType;
        if (!keepLoadedSample && old && !old->sampleKey.empty()) {
            audio.sample.reset();
            SoundPool::remove(old->sampleKey);
        }
        if (!keepLoadedSample) {
            audio.sample.reset();
            audio.sampleKey.clear();
        }
        // the voices stay with the component (an Inspector edit or an undo in Play does not leave
        // one playing on its own): the system applies the new values to them, stops them when the
        // state says Stopped, and when the file changed (no sample) before it loads the new one
        audio.groupWarned = false;
        audio.lastVolumeApplied = -1;
        audio.loaded = keepLoadedSample;
        audio.loadFailed = false;
        audio.length = keepLoadedSample ? old->length : 0;
        audio.playingTime = 0;
        audio.lastPosition = Vector3(0, 0, 0);
        audio.positionTracked = false;
        audio.lastVelocity = Vector3(0, 0, 0);
        audio.startTrigger = false;
        audio.pauseTrigger = false;
        audio.stopTrigger = false;
        audio.needUpdate = true;
    }

}

const ComponentDesc kCameraDesc{
    .type = ComponentType::CameraComponent, .cppName = "CameraComponent", .key = "camera", .displayName = "Camera", .icon = ICON_FA_VIDEO,
    .body = JE_STRUCT_DESC(CameraComponent, "CameraComponent", kCameraFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(CameraComponent),
    .codegenVar = "camera", .codegenOrder = kCameraCodegenOrder,
};

const ComponentDesc kAudioSourceDesc{
    .type = ComponentType::AudioSourceComponent, .cppName = "AudioSourceComponent", .key = "audiosource", .displayName = "Audio Source",
    .icon = ICON_FA_VOLUME_HIGH,
    .body = JE_STRUCT_DESC(AudioSourceComponent, "AudioSourceComponent", kAudioSourceFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(AudioSourceComponent),
    .postDecode = &audioSourcePostDecode,
    .codegenVar = "audio", .codegenOrder = kAudioSourceCodegenOrder, .codegenExtra = &audioSourceCodegen,
};

}
