// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Video Player's Inspector (Unity's order) and the asset view of a video file
// (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.9): source, playback, render mode
// and audio rows with undo, a Codec support row (the probe of the clip and where it plays, with
// the "Convert to WebM (AV1)" fix), the playback bar in Play (scrubber, play/pause/stop, state,
// backend), and for a selected .mp4/.webm its probe, platform matrix, fast start ("Optimize for
// streaming" rewrites the moov box to the front) and a preview player.

#include "InspectorWindow.h"

#include "Backend.h"
#include "Catalog.h"
#include "EditorHost.h"
#include "Out.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/UIComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/VideoPlayerComponent.h"
#include "external/IconsFontAwesome6.h"
#include "htmlui/VideoImporter.h"
#include "media/IVideoSource.h"
#include "media/Mp4Probe.h"
#include "media/VideoColorConvert.h"
#include "media/VideoFormats.h"
#include "media/VideoSourceFactory.h"
#include "media/VideoTextureConverter.h"
#include "render/TextureRender.h"
#include "subsystem/VideoPlayerSystem.h"
#include "texture/Framebuffer.h"
#include "util/FileDialogs.h"
#include "util/Util.h"
#include "App.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>

using namespace jasperengine;
namespace fs = std::filesystem;
using jasperengine::editor::Out;

namespace {

    std::vector<editor::EnumEntry> entriesVideoSource = {
        {(int)VideoPlayerSource::CLIP, "Video Clip"},
        {(int)VideoPlayerSource::URL, "URL"},
    };
    std::vector<editor::EnumEntry> entriesVideoTimeUpdate = {
        {(int)VideoTimeUpdateMode::GAME_TIME, "Game Time"},
        {(int)VideoTimeUpdateMode::UNSCALED_GAME_TIME, "Unscaled Game Time"},
    };
    std::vector<editor::EnumEntry> entriesVideoRenderMode = {
        {(int)VideoRenderMode::CAMERA_FAR_PLANE, "Camera Far Plane"},
        {(int)VideoRenderMode::CAMERA_NEAR_PLANE, "Camera Near Plane"},
        {(int)VideoRenderMode::RENDER_TEXTURE, "Render Texture"},
        {(int)VideoRenderMode::MATERIAL_OVERRIDE, "Material Override"},
        {(int)VideoRenderMode::API_ONLY, "API Only"},
    };
    std::vector<editor::EnumEntry> entriesVideoAspect = {
        {(int)VideoAspectRatio::NO_SCALING, "No Scaling"},
        {(int)VideoAspectRatio::FIT_VERTICALLY, "Fit Vertically"},
        {(int)VideoAspectRatio::FIT_HORIZONTALLY, "Fit Horizontally"},
        {(int)VideoAspectRatio::FIT_INSIDE, "Fit Inside"},
        {(int)VideoAspectRatio::FIT_OUTSIDE, "Fit Outside"},
        {(int)VideoAspectRatio::STRETCH, "Stretch"},
    };
    std::vector<editor::EnumEntry> entriesVideoAudio = {
        {(int)VideoAudioOutputMode::NONE, "None"},
        {(int)VideoAudioOutputMode::AUDIO_SOURCE, "Audio Source"},
        {(int)VideoAudioOutputMode::DIRECT, "Direct"},
    };

    // probes by file and its write time: the Inspector asks every frame
    struct CachedProbe {
        fs::file_time_type time;
        uintmax_t size = 0;
        VideoProbe probe;
    };

    const VideoProbe* cachedProbe(const fs::path& file){
        static std::map<std::string, CachedProbe> cache;
        std::error_code ec;
        const fs::file_time_type time = fs::last_write_time(file, ec);
        if (ec)
            return nullptr;
        const uintmax_t size = fs::file_size(file, ec);
        CachedProbe& entry = cache[file.string()];
        if (entry.size != size || entry.time != time || entry.probe.container.empty()){
            entry.time = time;
            entry.size = size;
            entry.probe = VideoFormats::probeFile(file.string());
        }
        return &entry.probe;
    }

    const char* platformGlyph(const char* cell){
        const std::string value = cell;
        if (value == "yes") return ICON_FA_CHECK;
        if (value == "no") return ICON_FA_XMARK;
        return ICON_FA_TRIANGLE_EXCLAMATION;
    }

    std::string platformCellHelp(const char* cell){
        const std::string value = cell;
        if (value == "yes") return "plays with the platform's own decoders";
        if (value == "needs_ffmpeg") return "plays when the player's Linux has the FFmpeg libraries (libavcodec, libavformat, libswresample)";
        if (value == "needs_extension") return "needs a Microsoft Store codec extension (HEVC, VP9 or AV1 Video Extensions; N editions: Media Feature Pack)";
        if (value == "device_dependent") return "plays on devices whose hardware or browser has the decoder";
        return "does not play there: convert it to WebM (AV1)";
    }

    // the per-platform row: Linux*, Windows, macOS, iOS, Android, Web
    bool drawPlatformMatrix(const VideoProbe& probe){
        bool missing = false;
        for (int p = 0; p < VideoSourceFactory::PLATFORM_COUNT; p++){
            const auto platform = (VideoSourceFactory::Platform)p;
            const char* cell = VideoSourceFactory::playableOn(platform, probe);
            const std::string value = cell;
            if (value == "no")
                missing = true;
            if (p > 0)
                ImGui::SameLine();
            const ImVec4 colour = value == "yes" ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f) : value == "no" ? ImVec4(0.9f, 0.4f, 0.4f, 1.0f) : ImVec4(0.9f, 0.75f, 0.35f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::Text("%s %s%s", platformGlyph(cell), VideoSourceFactory::platformLabel(platform), value == "needs_ffmpeg" ? "*" : "");
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s: %s", VideoSourceFactory::platformLabel(platform), platformCellHelp(cell).c_str());
        }
        return missing;
    }

    // "Convert to WebM (AV1)": FFmpeg on a worker (as the Project window's import), the original stays
    void convertToWebm(const fs::path& file){
        Out::info("Converting %s to AV1 + Opus WebM with FFmpeg...", file.filename().u8string().c_str());
        std::thread([file](){
            const editor::htmlui::VideoImportResult result = editor::htmlui::VideoImporter::importFile(file, file.parent_path(), false, {}, true);
            if (result.ok)
                Out::info("%s", result.message.c_str());
            else
                Out::error("Convert to WebM: %s", result.message.c_str());
            for (const auto& d : result.diagnostics)
                Out::warning("%s (fix: %s)", d.message.c_str(), d.suggestion.c_str());
        }).detach();
    }

    // "Optimize for streaming": the moov box to the front, in place (a temporary file renamed over it)
    void optimizeForStreaming(const fs::path& file){
        const fs::path temp = file.parent_path() / (file.filename().u8string() + ".faststart.tmp");
        std::string error;
        if (!Mp4Probe::rewriteFastStart(file.string(), temp.string(), error)){
            std::error_code ec;
            fs::remove(temp, ec);
            Out::error("Optimize for streaming: %s", error.c_str());
            return;
        }
        std::error_code ec;
        fs::rename(temp, file, ec);
        if (ec){
            fs::remove(temp, ec);
            Out::error("Optimize for streaming: %s could not be replaced (%s)", file.filename().u8string().c_str(), ec.message().c_str());
            return;
        }
        Out::info("%s: the moov box is at the front now: it plays while it downloads", file.filename().u8string().c_str());
    }

    // ---- the asset preview: frames converted on the CPU into a small dynamic texture

    struct VideoPreview {
        std::string path;
        std::unique_ptr<IVideoSource> source;
        TextureRender texture;
        int width = 0, height = 0;
        bool playing = false;
        double position = 0.0;
        int iteration = 0;
        int64_t shown = -1;
        std::chrono::steady_clock::time_point last;

        void close(){
            source.reset();
            if (texture.isCreated())
                texture.destroyTexture();
            width = height = 0;
            playing = false;
            position = 0.0;
            iteration = 0;
            shown = -1;
            path.clear();
        }
    };

    VideoPreview& preview(){
        static VideoPreview value;
        return value;
    }

    // at most 480 px wide (nearest sampling of the converted frame)
    void showFrame(VideoPreview& p, const VideoFrame& frame){
        std::vector<uint8_t> rgba;
        VideoColorConvert::toRgba(frame, rgba);
        int w = frame.width, h = frame.height;
        std::vector<uint8_t> small;
        if (w > 480){
            const int sw = 480, sh = std::max(1, h * 480 / w);
            small.resize((size_t)sw * sh * 4);
            for (int y = 0; y < sh; y++){
                const int sy = y * h / sh;
                for (int x = 0; x < sw; x++){
                    const int sx = x * w / sw;
                    std::memcpy(&small[((size_t)y * sw + x) * 4], &rgba[((size_t)sy * w + sx) * 4], 4);
                }
            }
            rgba.swap(small);
            w = sw;
            h = sh;
        }
        if (!p.texture.isCreated() || p.width != w || p.height != h){
            if (p.texture.isCreated())
                p.texture.destroyTexture();
            if (!p.texture.createDynamicTexture("video-preview", w, h))
                return;
            p.width = w;
            p.height = h;
        }
        p.texture.updateTexture(rgba.data(), rgba.size());
    }

}

// ---------------------------------------------------------------- the component

void editor::InspectorWindow::drawVideoPlayerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities, bool readOnly){
    Scene* scene = sceneProject->scene;
    entities.erase(std::remove_if(entities.begin(), entities.end(), [&](Entity entity) {
        return !scene->findComponent<VideoPlayerComponent>(entity);
    }), entities.end());
    if (entities.empty())
        return;
    VideoPlayerComponent& first = scene->getComponent<VideoPlayerComponent>(entities[0]);
    auto same = [&](auto getter) {
        for (Entity entity : entities){
            if (getter(scene->getComponent<VideoPlayerComponent>(entity)) != getter(first))
                return false;
        }
        return true;
    };

    RowSettings settings;
    settings.secondColSize = 9 * ImGui::GetFontSize();
    RowSettings enumSettings = settings;

    ImGui::BeginDisabled(readOnly);
    beginTable(cpType, getLabelSize("Target Material Property"));

    enumSettings.enumEntries = &entriesVideoSource;
    enumSettings.help = "Video Clip: a video asset of the project. URL: http(s)://, data:// (System.getUserDataPath()), an absolute or asset path";
    propertyRow(RowPropertyType::Enum, cpType, "source", "Source", sceneProject, entities, enumSettings);

    const bool urlSource = same([](const VideoPlayerComponent& p){ return p.source; }) && first.source == VideoPlayerSource::URL;
    const bool clipSource = same([](const VideoPlayerComponent& p){ return p.source; }) && first.source == VideoPlayerSource::CLIP;
    if (clipSource){
        RowSettings clipSettings = settings;
        clipSettings.help = "A video asset: .mp4/.m4v/.mov (H.264, HEVC: the platforms' decoders) or .webm (AV1 + Opus everywhere)";
        propertyRow(RowPropertyType::String, cpType, "clip", "Video Clip", sceneProject, entities, clipSettings);
        if (entities.size() == 1 && !readOnly){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(ICON_FA_FOLDER_OPEN " Choose...##videoclip")){
                const std::string chosen = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_VIDEO);
                if (!chosen.empty()){
                    fs::path relative = project->isInsideAssetsPath(chosen) ? project->normalizeToAssetsRelative(chosen) : fs::path();
                    if (relative.empty())
                        Out::error("The video must be inside the project's assets folder");
                    else
                        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                            new PropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType, "clip", relative.generic_string()));
                }
            }
            // a video dragged from the Project window
            if (ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                    std::vector<std::string> received = Util::getStringsFromPayload(payload);
                    if (!received.empty() && VideoFormats::isPlayableExtension(received[0]) && project->isInsideAssetsPath(project->resolveAssetPath(received[0]))){
                        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                            new PropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType, "clip",
                                                         project->normalizeToAssetsRelative(project->resolveAssetPath(received[0])).generic_string()));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
    }else if (urlSource){
        RowSettings urlSettings = settings;
        urlSettings.help = "https://... (downloaded into the media cache, played while it downloads when the MP4 is fast-start), "
                           "data://ads/ad1.mp4 (a file the game downloaded: System.getUserDataPath()), an absolute or asset path";
        propertyRow(RowPropertyType::String, cpType, "url", "URL", sceneProject, entities, urlSettings);
        if (entities.size() == 1){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            const std::string url = first.url;
            std::string hint;
            if (url.empty())
                hint = "no URL";
            else if (VideoSourceFactory::isUrl(url))
                hint = std::string(ICON_FA_GLOBE) + " downloaded into the media cache" + (url.rfind("http://", 0) == 0 ? " (plain http: allowed for localhost only unless Project Settings allow it)" : "");
            else if (url.rfind("data://", 0) == 0)
                hint = std::string(ICON_FA_USER) + " the game's user data (" + project->getUserDataPath().generic_string() + " in Play)";
            else if (fs::path(url).is_absolute())
                hint = fs::exists(url) ? "a file of this computer (not on players' devices)" : std::string(ICON_FA_TRIANGLE_EXCLAMATION) + " no such file";
            else
                hint = fs::exists(project->getAssetsPath() / url) ? "an asset" : std::string(ICON_FA_TRIANGLE_EXCLAMATION) + " no such asset";
            ImGui::TextDisabled("%s", hint.c_str());
            if (!readOnly && ImGui::Button(ICON_FA_FOLDER_OPEN " Browse user data...##videourl")){
                std::error_code ec;
                fs::create_directories(project->getUserDataPath(), ec);
                const std::string chosen = FileDialogs::openFileDialog(project->getUserDataPath().string(), FILE_DIALOG_VIDEO);
                if (!chosen.empty()){
                    const fs::path relative = fs::path(chosen).lexically_relative(project->getUserDataPath());
                    const std::string value = (!relative.empty() && *relative.begin() != "..") ? "data://" + relative.generic_string() : chosen;
                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                        new PropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType, "url", value));
                }
            }
        }
    }

    RowSettings boolSettings = settings;
    boolSettings.help = "Prepares and plays when the scene starts";
    propertyRow(RowPropertyType::Bool, cpType, "playOnAwake", "Play On Awake", sceneProject, entities, boolSettings);
    boolSettings.help = "The clock starts once the first frame shows";
    propertyRow(RowPropertyType::Bool, cpType, "waitForFirstFrame", "Wait For First Frame", sceneProject, entities, boolSettings);
    boolSettings.help = "Starts again at the end (onLoopPointReached at each loop)";
    propertyRow(RowPropertyType::Bool, cpType, "isLooping", "Loop", sceneProject, entities, boolSettings);
    boolSettings.help = "Skips frames the decoder is late with (off: the video waits for them)";
    propertyRow(RowPropertyType::Bool, cpType, "skipOnDrop", "Skip On Drop", sceneProject, entities, boolSettings);
    RowSettings speedSettings = settings;
    speedSettings.floatMin = 0.0625f;
    speedSettings.floatMax = 16.0f;
    speedSettings.logarithmic = true;
    speedSettings.help = "Playback speed (0.0625 to 16; the sound is muted outside 0.25 to 4)";
    propertyRow(RowPropertyType::Float, cpType, "playbackSpeed", "Playback Speed", sceneProject, entities, speedSettings);
    enumSettings.enumEntries = &entriesVideoTimeUpdate;
    enumSettings.help = "Game Time: timeScale slows and pauses it. Unscaled Game Time: plays on at timeScale 0 (ads, menus)";
    propertyRow(RowPropertyType::Enum, cpType, "timeUpdateMode", "Time Update Mode", sceneProject, entities, enumSettings);

    enumSettings.enumEntries = &entriesVideoRenderMode;
    enumSettings.help = "Render Texture: the player's texture (UI Images and materials: video|<entity>). Camera planes: behind or over a camera's scene. "
                        "Material Override: a renderer's material texture. API Only: decoded, drawn nowhere";
    propertyRow(RowPropertyType::Enum, cpType, "renderMode", "Render Mode", sceneProject, entities, enumSettings);
    const bool sameMode = same([](const VideoPlayerComponent& p){ return p.renderMode; });
    const VideoRenderMode mode = first.renderMode;
    if (sameMode && (mode == VideoRenderMode::CAMERA_FAR_PLANE || mode == VideoRenderMode::CAMERA_NEAR_PLANE)){
        RowSettings cameraSettings = settings;
        cameraSettings.indent = true;
        cameraSettings.entityFilter.set(scene->getComponentId<CameraComponent>());
        cameraSettings.help = "Empty: the main camera";
        propertyRow(RowPropertyType::LocalEntity, cpType, "targetCamera", "Camera", sceneProject, entities, cameraSettings);
        RowSettings alphaSettings = settings;
        alphaSettings.indent = true;
        alphaSettings.floatMin = 0.0f;
        alphaSettings.floatMax = 1.0f;
        propertyRow(RowPropertyType::Float_0_1, cpType, "targetCameraAlpha", "Alpha", sceneProject, entities, alphaSettings);
    }
    if (sameMode && mode == VideoRenderMode::MATERIAL_OVERRIDE){
        RowSettings rendererSettings = settings;
        rendererSettings.indent = true;
        rendererSettings.help = "A Mesh, Sprite or Image; empty: this GameObject";
        propertyRow(RowPropertyType::LocalEntity, cpType, "targetMaterialRenderer", "Renderer", sceneProject, entities, rendererSettings);
        // the texture slots of the renderer's material
        if (entities.size() == 1){
            const Entity target = first.targetMaterialRenderer == NULL_ENTITY ? entities[0] : first.targetMaterialRenderer;
            std::vector<std::string> slots;
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(target)){
                slots = {"baseColorTexture", "emissiveTexture", "metallicRoughnessTexture", "occlusionTexture", "normalTexture"};
                for (const auto& [name, texture] : mesh->submeshes[0].material.shaderTextures)
                    slots.push_back(name);
            }else if (UIComponent* ui = scene->findComponent<UIComponent>(target)){
                slots = {"texture"};
                for (const auto& [name, texture] : ui->shaderTextures)
                    slots.push_back(name);
            }
            prefabRowProperty = "targetMaterialProperty";
            if (propertyHeader("Material Property", -1, first.targetMaterialProperty != "baseColorTexture", false, true)){
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType, "targetMaterialProperty", std::string("baseColorTexture")));
            }
            if (ImGui::BeginCombo("##videoproperty", first.targetMaterialProperty.c_str())){
                for (const std::string& slot : slots){
                    if (ImGui::Selectable(slot.c_str(), slot == first.targetMaterialProperty) && slot != first.targetMaterialProperty)
                        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                            new PropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType, "targetMaterialProperty", slot));
                }
                if (slots.empty())
                    ImGui::TextDisabled("the renderer has no material texture slots");
                ImGui::EndCombo();
            }
        }else{
            RowSettings propertySettings = settings;
            propertySettings.indent = true;
            propertyRow(RowPropertyType::String, cpType, "targetMaterialProperty", "Material Property", sceneProject, entities, propertySettings);
        }
        RowSettings submeshSettings = settings;
        submeshSettings.indent = true;
        submeshSettings.intMin = -1;
        submeshSettings.intMax = 63;
        submeshSettings.help = "-1: every submesh";
        propertyRow(RowPropertyType::Int, cpType, "targetSubmesh", "Submesh", sceneProject, entities, submeshSettings);
    }
    if (sameMode && mode != VideoRenderMode::MATERIAL_OVERRIDE && mode != VideoRenderMode::API_ONLY){
        enumSettings.enumEntries = &entriesVideoAspect;
        enumSettings.help = "How the picture fits a camera plane or a fixed-size render texture (letterbox bars are transparent)";
        enumSettings.indent = true;
        propertyRow(RowPropertyType::Enum, cpType, "aspectRatio", "Aspect Ratio", sceneProject, entities, enumSettings);
        enumSettings.indent = false;
    }
    if (sameMode && mode == VideoRenderMode::RENDER_TEXTURE){
        RowSettings sizeSettings = settings;
        sizeSettings.indent = true;
        sizeSettings.help = "0: the video's own size (Native)";
        propertyRow(RowPropertyType::UInt, cpType, "renderTextureWidth", "Texture Width", sceneProject, entities, sizeSettings);
        propertyRow(RowPropertyType::UInt, cpType, "renderTextureHeight", "Texture Height", sceneProject, entities, sizeSettings);
        if (!readOnly && entities.size() == 1 && (first.renderTextureWidth != 0 || first.renderTextureHeight != 0)){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            if (ImGui::SmallButton("Native##videosize")){
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType, "renderTextureWidth", 0u));
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType, "renderTextureHeight", 0u));
            }
        }
    }

    enumSettings.enumEntries = &entriesVideoAudio;
    enumSettings.help = "Direct: straight to the output at its volume. Audio Source: through that AudioSource's volume and 3D settings. None: no audio decoded";
    propertyRow(RowPropertyType::Enum, cpType, "audioOutputMode", "Audio Output Mode", sceneProject, entities, enumSettings);
    if (same([](const VideoPlayerComponent& p){ return p.audioOutputMode; })){
        if (first.audioOutputMode == VideoAudioOutputMode::DIRECT){
            RowSettings volumeSettings = settings;
            volumeSettings.indent = true;
            volumeSettings.floatMin = 0.0f;
            volumeSettings.floatMax = 1.0f;
            propertyRow(RowPropertyType::Float_0_1, cpType, "directVolume", "Volume", sceneProject, entities, volumeSettings);
            RowSettings muteSettings = settings;
            muteSettings.indent = true;
            propertyRow(RowPropertyType::Bool, cpType, "directMute", "Mute", sceneProject, entities, muteSettings);
        }else if (first.audioOutputMode == VideoAudioOutputMode::AUDIO_SOURCE){
            RowSettings sourceSettings = settings;
            sourceSettings.indent = true;
            sourceSettings.entityFilter.set(scene->getComponentId<AudioSourceComponent>());
            sourceSettings.help = "Empty: this GameObject's AudioSource";
            propertyRow(RowPropertyType::LocalEntity, cpType, "targetAudioSource", "Audio Source", sceneProject, entities, sourceSettings);
        }
    }
    endTable();
    ImGui::EndDisabled();

    if (entities.size() != 1)
        return;
    const Entity entity = entities[0];

    // ---- codec support: the clip's probe and where it plays
    fs::path file;
    if (first.source == VideoPlayerSource::CLIP && !first.clip.empty())
        file = project->getAssetsPath() / first.clip;
    else if (first.source == VideoPlayerSource::URL && !first.url.empty() && !VideoSourceFactory::isUrl(first.url)){
        if (first.url.rfind("data://", 0) == 0)
            file = project->getUserDataPath() / first.url.substr(7);
        else if (fs::path(first.url).is_absolute())
            file = first.url;
        else
            file = project->getAssetsPath() / first.url;
    }
    std::error_code ec;
    if (!file.empty() && fs::is_regular_file(file, ec)){
        if (const VideoProbe* probe = cachedProbe(file)){
            ImGui::Separator();
            ImGui::TextDisabled("Codec support");
            ImGui::SameLine();
            ImGui::Text("%s %s, %s%s", probe->container.c_str(), VideoFormats::codecName(probe->videoCodec).c_str(),
                        probe->audioCodec.empty() ? "no sound" : VideoFormats::codecName(probe->audioCodec).c_str(),
                        probe->width > 0 ? (", " + std::to_string(probe->width) + "x" + std::to_string(probe->height)).c_str() : "");
            const bool missing = drawPlatformMatrix(*probe);
            if (missing || probe->encrypted){
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s Some platforms cannot play this video. WebM (AV1 + Opus) plays everywhere.", ICON_FA_TRIANGLE_EXCLAMATION);
                ImGui::PopStyleColor();
                if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Convert to WebM (AV1)##videoconvert"))
                    convertToWebm(file);
            }
            if (probe->width > 1920 || probe->height > 1920)
                ImGui::TextDisabled("%s Larger than 1080p: heavy for mobile devices", ICON_FA_TRIANGLE_EXCLAMATION);
        }
    }
    if (scene->getComponentArray<VideoPlayerComponent>()->size() > 2)
        ImGui::TextDisabled("%s More than 2 Video Players in the scene: mobile decoders may run out", ICON_FA_TRIANGLE_EXCLAMATION);

    // ---- in Play: the playback
    const bool playing = sceneProject->playState == ScenePlayState::PLAYING || sceneProject->playState == ScenePlayState::PAUSED;
    std::shared_ptr<VideoPlayerSystem> system = scene->findSystem<VideoPlayerSystem>();
    if (!playing || !system)
        return;
    const VideoPlayerSystem::Status status = system->getStatus(entity);
    ImGui::Separator();
    if (ImGui::Button(status.playing ? ICON_FA_PAUSE "##videopause" : ICON_FA_PLAY "##videoplay")){
        if (status.playing) system->pause(entity);
        else system->play(entity);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_STOP "##videostop"))
        system->stop(entity);
    ImGui::SameLine();
    ImGui::BeginDisabled(!status.prepared);
    if (ImGui::Button(ICON_FA_FORWARD_STEP "##videostep"))
        system->stepForward(entity);
    ImGui::SameLine();
    float time = (float)status.time;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::SliderFloat("##videotime", &time, 0.0f, (float)std::max(status.length, 0.001), "%.2f s"))
        system->setTime(entity, time);
    ImGui::EndDisabled();
    std::string state = status.error.empty() ? (status.prepared ? (status.playing ? "playing" : status.paused ? "paused" : "prepared")
                                                                  : status.preparing ? "preparing" : "stopped")
                                             : "error";
    ImGui::Text("%s  frame %lld / %lld  %s", state.c_str(), (long long)status.frame, (long long)status.frameCount, status.backend.c_str());
    if (status.preparing)
        ImGui::ProgressBar((float)status.prepareProgress, ImVec2(-1.0f, 0.0f), "preparing");
    else if (status.prepared && status.bufferedSeconds < status.length - 1e-3)
        ImGui::ProgressBar((float)(status.bufferedSeconds / std::max(status.length, 1e-3)), ImVec2(-1.0f, 0.0f), "buffered");
    if (!status.error.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", status.error.c_str());
        ImGui::PopStyleColor();
    }
    // the render texture
    if (first.output && first.output->hasPicture()){
        TextureRender* texture = &first.output->getFramebuffer()->getRender().getColorTexture();
        const float width = std::min(ImGui::GetContentRegionAvail().x, Theme::dpi(320.0f));
        const float height = width * (float)first.output->getHeight() / (float)std::max(first.output->getWidth(), 1);
        ImGui::Image(Backend::getImGuiTexture(texture), ImVec2(width, height));
    }
}

// ---------------------------------------------------------------- the asset view

void editor::InspectorWindow::drawVideoAsset(const fs::path& asset){
    drawAssetHeader(ICON_FA_FILM, asset, "Video");
    const VideoProbe* probe = cachedProbe(asset);
    if (!probe)
        return;
    ImGui::Separator();
    if (!probe->readable || probe->container.empty()){
        ImGui::TextWrapped("%s %s", ICON_FA_TRIANGLE_EXCLAMATION, probe->error.message.empty() ? "not a video the engine reads" : probe->error.message.c_str());
        return;
    }
    ImGui::Text("Container: %s", probe->container.c_str());
    if (!probe->videoCodec.empty())
        ImGui::Text("Video: %s (%s), %d x %d%s", VideoFormats::codecName(probe->videoCodec).c_str(),
                    (probe->videoCodecString.empty() ? probe->videoCodec : probe->videoCodecString).c_str(), probe->width, probe->height,
                    probe->frameRate > 0.0 ? (", " + std::to_string((int)std::lround(probe->frameRate)) + " fps").c_str() : "");
    if (!probe->audioCodec.empty())
        ImGui::Text("Audio: %s, %d channel%s", VideoFormats::codecName(probe->audioCodec).c_str(), probe->audioChannels,
                    probe->audioChannels == 1 ? "" : "s");
    if (probe->duration > 0.0)
        ImGui::Text("Length: %.2f s", probe->duration);
    if (probe->rotation != 0)
        ImGui::Text("Rotation: %d degrees (shown turned)", probe->rotation);
    if (probe->encrypted)
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s Encrypted (DRM): not playable", ICON_FA_LOCK);

    ImGui::Separator();
    ImGui::TextDisabled("Plays on");
    const bool missing = drawPlatformMatrix(*probe);
    ImGui::TextDisabled("* Linux: with the system's FFmpeg libraries");
    const bool mp4 = probe->container == "mp4" || probe->container == "mov" || probe->container == "3gp";
    if (missing && ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Convert to WebM (AV1)##assetconvert"))
        convertToWebm(asset);
    if (mp4){
        ImGui::Text("Fast start: %s", probe->fastStart ? "yes (plays while it downloads from a URL)" : "no");
        if (!probe->fastStart && !probe->fragmented){
            ImGui::SameLine();
            if (ImGui::SmallButton("Optimize for streaming"))
                optimizeForStreaming(asset);
            ImGui::SetItemTooltip("Moves the moov box before the media data (no re-encoding): a URL plays while it downloads");
        }
    }

    // ---- the preview player
    ImGui::Separator();
    VideoPreview& p = preview();
    const std::string path = asset.string();
    if (p.path != path)
        p.close();
    if (!p.source){
        if (ImGui::Button(ICON_FA_PLAY " Preview")){
            VideoSourceFactory::Options options;
            options.audio = false;
            p.source = VideoSourceFactory::create(path, options);
            p.path = path;
            p.playing = true;
            p.last = std::chrono::steady_clock::now();
        }
        return;
    }
    if (ImGui::Button(p.playing ? ICON_FA_PAUSE "##previewpause" : ICON_FA_PLAY "##previewplay")){
        p.playing = !p.playing;
        p.last = std::chrono::steady_clock::now();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_STOP "##previewstop")){
        p.close();
        return;
    }
    const IVideoSource::State state = p.source->getState();
    if (state == IVideoSource::State::FAILED){
        ImGui::SameLine();
        ImGui::TextWrapped("%s", p.source->getError().message.c_str());
        return;
    }
    if (state != IVideoSource::State::READY){
        ImGui::SameLine();
        ImGui::TextDisabled("opening...");
        return;
    }
    const VideoInfo& info = p.source->getInfo();
    ImGui::SameLine();
    ImGui::Text("%.2f / %.2f s  %s", p.position, info.duration, p.source->backendName());
    const auto now = std::chrono::steady_clock::now();
    if (p.playing && info.duration > 0.0){
        p.position += std::chrono::duration<double>(now - p.last).count();
        if (p.position >= info.duration){
            p.position = std::fmod(p.position, info.duration);
            p.iteration++;
            p.source->setLoop(true);
        }
    }
    p.last = now;
    VideoFrame frame;
    const IVideoSource::FrameResult result = p.source->acquireFrame(p.iteration, p.position, false, p.shown, frame);
    if ((result == IVideoSource::FrameResult::NEW || result == IVideoSource::FrameResult::STALE) && !frame.y.empty()){
        p.shown = frame.sequence;
        showFrame(p, frame);
    }
    if (p.texture.isCreated() && p.width > 0){
        const float width = std::min(ImGui::GetContentRegionAvail().x, Theme::dpi(320.0f));
        ImGui::Image(Backend::getImGuiTexture(&p.texture), ImVec2(width, width * p.height / p.width));
    }
    // the Inspector redraws while it plays
    if (p.playing)
        Backend::getApp().requestRedraw();
}
