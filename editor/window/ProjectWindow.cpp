// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "animation/AnimatorAssetDocument.h"
#include "window/PoseAnimationWindow.h"
#include "EditorModal.h"
#include "animation/AnimatorEditorHooks.h"
#include "ProjectWindow.h"
#include "window/vcs/VcsUi.h"
#include "texture/TextureCookQueue.h"
#include "model/ModelCookQueue.h"
#include "util/EditorScripts.h"
#include "asset/AssetTypes.h"
#include "window/AssetPanels.h"
#include "util/PngWriter.h"
#include "DocumentFile.h"
#include "audio/AudioMixerDocument.h"
#include "audio/SoundEventDocument.h"
#include "window/HumanoidConvertDialog.h"
#include "util/BidiText.h"

#include "ImageViewerWindow.h"

#include "AppSettings.h"
#include "external/IconsFontAwesome6.h"
#include "resources/icons/folder-icon_png.h"
#include "resources/icons/file-icon_png.h"
#include "resources/icons/entity-icon_png.h"
#include "resources/icons/scene-icon_png.h"
#include "resources/icons/source-icon_png.h"
#include "resources/icons/header-icon_png.h"
#include "resources/icons/lua-icon_png.h"
#include "resources/icons/audio-icon_png.h"
#include "resources/icons/font-icon_png.h"
#include "resources/icons/html-icon_png.h"
#include "resources/icons/css-icon_png.h"
#include "resources/icons/lut-icon_png.h"
#include "PostProcessingPresets.h"
#include "util/CubeLut.h"

#include "Out.h"
#include "util/HtmlUITemplates.h"
#include "command/type/CopyFileCmd.h"
#include "asset/AssetOps.h"
#include "util/SafeWrite.h"
#include "util/ScriptTemplates.h"
#include "window/dialog/ScriptCreateDialog.h"
#include "command/CommandHandle.h"
#include "command/type/CreateMaterialFileCmd.h"
#include "camera/ShakePreset.h"
#include "camera/ShakePresets.h"
#include "animation/AnimationClipIO.h"
#include "util/ProjectFileWrites.h"
#include "procmat/ProceduralBake.h"
#include "command/type/RenameFileCmd.h"
#include "command/type/CreateDirCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/CreatePrefabVariantAssetCmd.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "vat/VatAppHooks.h"
#include "vat/VatAssetFiles.h"
#include "vat/VatDocument.h"
#include "util/UIProbe.h"
#include "util/PhysicsMaterialAssets.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseEditSession.h"
#include "pose/PoseImage.h"

#include "Backend.h"
#include "window/HierarchyWindow.h"
#include <cstring>
#include "InspectorWindow.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"
#include "Stream.h"
#include "Factory.h"
#include "Generator.h"
#include "Log.h"
#include "subsystem/MeshSystem.h"
#include "render/SystemRender.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/FbxImporter.h"
#include "util/Util.h"
#include "texsynth/SynthesisRecipe.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "window/SpriteNormalsEntry.h"
#include "window/SpriteNormalsWindow.h"
#include "util/UIUtils.h"
#include "util/SHA1.h"
#include "util/GraphicUtils.h"
#include "thread/ResourceProgress.h"
#include "util/EntityPayload.h"
#include "util/ProjectUtils.h"
#include "htmlui/VideoImporter.h"
#include "EditorHost.h"

#include "imgui_internal.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <unordered_set>
#include "audio/AudioAnalysis.h"
#include "audio/AudioImages.h"
#include "audio/AudioWaveform.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

using namespace jasperengine;

unsigned char editor::ProjectWindow::textureByteAt(TextureData& data, size_t pixel, int channel, int bytesPerChannel) {
    if (bytesPerChannel == 2) {
        const unsigned short* src = static_cast<const unsigned short*>(data.getData());
        return static_cast<unsigned char>(src[pixel * data.getChannels() + channel] >> 8);
    }

    const unsigned char* src = static_cast<const unsigned char*>(data.getData());
    return src[pixel * data.getChannels() + channel];
}

bool editor::ProjectWindow::writeImageThumbnail(const fs::path& sourcePath, const fs::path& thumbnailPath) {
    TextureData sourceData;
    if (!sourceData.loadTextureFromFile(sourcePath.string().c_str()) || !sourceData.getData()) {
        return false;
    }

    const int width = sourceData.getWidth();
    const int height = sourceData.getHeight();
    const int channels = sourceData.getChannels();
    const int bytesPerChannel = TextureData::getBytesPerChannel(sourceData.getColorFormat());
    if (width <= 0 || height <= 0 || channels <= 0) {
        sourceData.releaseImageData();
        return false;
    }

    std::vector<unsigned char> rgbaData(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t srcPixel = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            const size_t dstPixel = srcPixel * 4;

            if (channels == 1) {
                const unsigned char value = textureByteAt(sourceData, srcPixel, 0, bytesPerChannel);
                rgbaData[dstPixel + 0] = value;
                rgbaData[dstPixel + 1] = value;
                rgbaData[dstPixel + 2] = value;
                rgbaData[dstPixel + 3] = 255;
            } else if (channels == 2) {
                const unsigned char value = textureByteAt(sourceData, srcPixel, 0, bytesPerChannel);
                rgbaData[dstPixel + 0] = value;
                rgbaData[dstPixel + 1] = value;
                rgbaData[dstPixel + 2] = value;
                rgbaData[dstPixel + 3] = textureByteAt(sourceData, srcPixel, 1, bytesPerChannel);
            } else {
                rgbaData[dstPixel + 0] = textureByteAt(sourceData, srcPixel, 0, bytesPerChannel);
                rgbaData[dstPixel + 1] = textureByteAt(sourceData, srcPixel, 1, bytesPerChannel);
                rgbaData[dstPixel + 2] = textureByteAt(sourceData, srcPixel, 2, bytesPerChannel);
                rgbaData[dstPixel + 3] = channels >= 4 ? textureByteAt(sourceData, srcPixel, 3, bytesPerChannel) : 255;
            }
        }
    }

    int maxThumbSize = THUMBNAIL_SIZE;
    float scale = (width > height) ?
        static_cast<float>(maxThumbSize) / static_cast<float>(width) :
        static_cast<float>(maxThumbSize) / static_cast<float>(height);

    int newWidth = std::max(1, static_cast<int>(width * scale));
    int newHeight = std::max(1, static_cast<int>(height * scale));

    std::vector<unsigned char> thumbData(static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight) * 4);
    stbir_resize_uint8_linear(
        rgbaData.data(),
        width,
        height,
        0,
        thumbData.data(),
        newWidth,
        newHeight,
        0,
        STBIR_RGBA
    );

    fs::create_directories(thumbnailPath.parent_path());
    const bool written = writePng(thumbnailPath, newWidth, newHeight, 4, thumbData.data(), newWidth * 4, io::Durability::Cache);

    sourceData.releaseImageData();
    return written;
}

// The .anim icon: a film strip whose three frames show a ball rising (drawn with PoseImage,
// like the .poseanim icon)
static void makeAnimationClipIcon(int size, std::vector<uint8_t>& rgba) {
    editor::PoseImage image(size, size);
    const float s = static_cast<float>(size) / 64.0f;
    auto px = [s](float v) { return static_cast<int>(v * s); };
    const editor::PoseColor strip = editor::PoseColor::hex(0x2B2F36);
    const editor::PoseColor hole = editor::PoseColor::hex(0xD9D9D9);
    const editor::PoseColor frame = editor::PoseColor::hex(0xE8873A);
    const editor::PoseColor frameEdge = editor::PoseColor::hex(0xB8652A);
    const editor::PoseColor ball = editor::PoseColor::hex(0xF2F2F2);
    image.fillRect(px(4), px(12), px(56), px(40), strip);
    for (int i = 0; i < 6; i++) {
        image.fillRect(px(8 + i * 9.0f), px(15), px(4), px(4), hole);
        image.fillRect(px(8 + i * 9.0f), px(45), px(4), px(4), hole);
    }
    for (int i = 0; i < 3; i++) {
        const float x = 8 + i * 17.0f;
        image.fillRect(px(x), px(22), px(14), px(20), frameEdge);
        image.fillRect(px(x + 1), px(23), px(12), px(18), frame);
        image.fillCircle((x + 7) * s, (36 - i * 5.0f) * s, 6.4f * s, ball);
    }
    rgba = image.getPixels();
}

// The .physicsmaterial icon: a ball resting on a slope
static void makePhysicsMaterialIcon(int size, std::vector<uint8_t>& rgba) {
    editor::PoseImage image(size, size);
    const float s = static_cast<float>(size) / 64.0f;
    auto px = [s](float v) { return static_cast<int>(v * s); };
    const editor::PoseColor panel = editor::PoseColor::hex(0x263238);
    const editor::PoseColor slope = editor::PoseColor::hex(0x8D6E63);
    const editor::PoseColor ball = editor::PoseColor::hex(0x4FC3F7);
    const editor::PoseColor shine = editor::PoseColor::hex(0xE1F5FE);
    image.fillRect(px(4), px(6), px(56), px(52), panel);
    // a stepped ramp, low on the left
    for (int i = 0; i < 12; i++) {
        const float x = 6 + i * 4.5f;
        const float h = 6 + i * 2.2f;
        image.fillRect(px(x), px(56 - h), px(4.6f), px(h), slope);
    }
    image.fillCircle(30 * s, 28 * s, 20 * s, ball);
    image.fillCircle(26 * s, 24 * s, 6 * s, shine);
    rgba = image.getPixels();
}

// The .vat icon: a crowd of three figures over a texture grid (the baked frames)
static void makeVertexAnimationIcon(int size, std::vector<uint8_t>& rgba) {
    editor::PoseImage image(size, size);
    const float s = static_cast<float>(size) / 64.0f;
    auto px = [s](float v) { return static_cast<int>(v * s); };
    const editor::PoseColor panel = editor::PoseColor::hex(0x22313A);
    const editor::PoseColor grid = editor::PoseColor::hex(0x2F4A57);
    const editor::PoseColor back = editor::PoseColor::hex(0x3FA7A0);
    const editor::PoseColor front = editor::PoseColor::hex(0x7FE0D4);
    image.fillRect(px(4), px(6), px(56), px(52), panel);
    for (int i = 0; i < 6; i++) {
        image.fillRect(px(6 + i * 9.0f), px(8), px(1), px(48), grid);
        image.fillRect(px(6), px(8 + i * 9.0f), px(52), px(1), grid);
    }
    auto figure = [&](float x, float y, float scale, editor::PoseColor color) {
        image.fillCircle(x * s, (y - 13.0f * scale) * s, 4.2f * scale * s, color);
        image.fillRect(px(x - 4.5f * scale), px(y - 8.0f * scale), px(9.0f * scale), px(13.0f * scale), color);
        image.fillRect(px(x - 4.0f * scale), px(y + 5.0f * scale), px(3.0f * scale), px(9.0f * scale), color);
        image.fillRect(px(x + 1.0f * scale), px(y + 5.0f * scale), px(3.0f * scale), px(9.0f * scale), color);
    };
    figure(18.0f, 34.0f, 0.85f, back);
    figure(46.0f, 34.0f, 0.85f, back);
    figure(32.0f, 38.0f, 1.0f, front);
    rgba = image.getPixels();
}

// what a hovered file says: its name, and for a generated mesh what made it
static std::string fileTooltip(const editor::FileEntry& file) {
    if (!file.generatedBy.empty()) {
        // a Sprite Normal Map output
        return file.displayName + "\n" + file.generatedBy;
    }
    if (file.generated) {
        return file.displayName + "\nGenerated by the Vertex Animation baker: do not edit; Rebake regenerates it";
    }
    return file.displayName;
}

// the "generated" badge: a small gear in the corner of the icon
static void drawGeneratedBadge(ImDrawList* drawList, const ImVec2& iconMin, float iconExtent, const std::string& name, bool stale = false) {
    const float size = std::max(ImGui::GetFontSize() * 0.75f, iconExtent * 0.32f);
    const ImVec2 min(iconMin.x, iconMin.y);
    const ImVec2 max(min.x + size, min.y + size);
    drawList->AddRectFilled(min, max, IM_COL32(40, 46, 52, 230), size * 0.25f);
    const float fontSize = size * 0.8f;
    const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ICON_FA_GEARS);
    drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(min.x + (size - textSize.x) * 0.5f, min.y + (size - textSize.y) * 0.5f),
                      stale ? ImGui::ColorConvertFloat4ToU32(editor::Theme::Colors::WarningText) : IM_COL32(127, 224, 212, 255), ICON_FA_GEARS);
    editor::UIProbe::rect("project.generated." + name, min.x, min.y, max.x, max.y);
    if (stale) editor::UIProbe::rect("project.generatedStale." + name, min.x, min.y, max.x, max.y);
}

// The Animator assets' icons: a controller is three states and their arrows, a mask a figure, a
// montage a strip cut into sections
static void makeAnimatorControllerIcon(int size, std::vector<uint8_t>& rgba) {
    editor::PoseImage image(size, size);
    const float s = static_cast<float>(size) / 64.0f;
    auto px = [s](float v) { return static_cast<int>(v * s); };
    const editor::PoseColor arrow = editor::PoseColor::hex(0xD9D9D9);
    const editor::PoseColor entry = editor::PoseColor::hex(0x2E8B4A);
    const editor::PoseColor state = editor::PoseColor::hex(0x5A5F69);
    const editor::PoseColor defaultState = editor::PoseColor::hex(0xD07A26);
    image.drawLine(20 * s, 16 * s, 44 * s, 16 * s, 2.5f * s, arrow);
    image.drawLine(14 * s, 22 * s, 24 * s, 42 * s, 2.5f * s, arrow);
    image.drawLine(48 * s, 22 * s, 40 * s, 42 * s, 2.5f * s, arrow);
    image.fillRect(px(4), px(9), px(18), px(14), entry);
    image.fillRect(px(42), px(9), px(18), px(14), state);
    image.fillRect(px(20), px(40), px(24), px(15), defaultState);
    rgba = image.getPixels();
}

static void makeAvatarMaskIcon(int size, std::vector<uint8_t>& rgba) {
    editor::PoseImage image(size, size);
    const float s = static_cast<float>(size) / 64.0f;
    auto px = [s](float v) { return static_cast<int>(v * s); };
    const editor::PoseColor on = editor::PoseColor::hex(0x46AA50);
    const editor::PoseColor off = editor::PoseColor::hex(0xAA3C3C);
    image.fillCircle(32 * s, 9 * s, 11 * s, on);
    image.fillRect(px(25), px(16), px(14), px(22), on);
    image.fillRect(px(17), px(17), px(6), px(18), off);
    image.fillRect(px(41), px(17), px(6), px(18), on);
    image.fillRect(px(25), px(39), px(6), px(22), on);
    image.fillRect(px(33), px(39), px(6), px(22), off);
    rgba = image.getPixels();
}

static void makeMontageIcon(int size, std::vector<uint8_t>& rgba) {
    editor::PoseImage image(size, size);
    const float s = static_cast<float>(size) / 64.0f;
    auto px = [s](float v) { return static_cast<int>(v * s); };
    const editor::PoseColor strip = editor::PoseColor::hex(0x2B2F36);
    const editor::PoseColor colors[] = {editor::PoseColor::hex(0x466EAA), editor::PoseColor::hex(0x5A8C5A), editor::PoseColor::hex(0x966E3C)};
    const editor::PoseColor marker = editor::PoseColor::hex(0xE6C850);
    image.fillRect(px(4), px(18), px(56), px(28), strip);
    image.fillRect(px(7), px(22), px(16), px(20), colors[0]);
    image.fillRect(px(24), px(22), px(20), px(20), colors[1]);
    image.fillRect(px(45), px(22), px(12), px(20), colors[2]);
    image.drawLine(34 * s, 10 * s, 34 * s, 18 * s, 3.0f * s, marker);
    rgba = image.getPixels();
}

bool editor::ProjectWindow::writeAudioThumbnail(const fs::path& sourcePath, const fs::path& thumbnailPath) {
    AudioWaveform::Data wave;
    std::string error;
    if (!AudioWaveform::ofFile(sourcePath, AudioAnalysis::cacheDirectory(project->getProjectPath()), wave, error) || wave.empty()) {
        return false;
    }
    const int size = THUMBNAIL_SIZE;
    const AudioImages::Palette::Rgb background = AudioImages::Palette::LANE_BACKGROUND;
    const AudioImages::Palette::Rgb colour = AudioImages::Palette::WAVE;
    std::vector<unsigned char> rgba((size_t)size * size * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = background.r;
        rgba[i + 1] = background.g;
        rgba[i + 2] = background.b;
        rgba[i + 3] = 255;
    }
    for (int x = 0; x < size; x++) {
        const int column = x * AudioWaveform::COLUMNS / size;
        int low = 32767, high = -32768;
        for (unsigned int c = 0; c < wave.channels; c++) {
            low = std::min(low, (int)wave.min(c, column));
            high = std::max(high, (int)wave.max(c, column));
        }
        const int top = std::clamp(size / 2 - high * size / 2 / 32767, 0, size - 1);
        const int bottom = std::clamp(size / 2 - low * size / 2 / 32767, top, size - 1);
        for (int y = top; y <= bottom; y++) {
            unsigned char* pixel = rgba.data() + ((size_t)y * size + x) * 4;
            pixel[0] = colour.r;
            pixel[1] = colour.g;
            pixel[2] = colour.b;
        }
    }
    fs::create_directories(thumbnailPath.parent_path());
    return writePng(thumbnailPath, size, size, 4, rgba.data(), size * 4, io::Durability::Cache);
}

editor::FileType editor::ProjectWindow::classifyThumbnailFileType(const fs::path& filePath) {
    const std::string extension = filePath.extension().string();

    if (editor::Util::isImageFile(extension)) {
        return editor::FileType::IMAGE;
    }
    if (editor::Util::isMaterialFile(extension)) {
        return editor::FileType::MATERIAL;
    }
    if (editor::Util::isModelFile(extension)) {
        return editor::FileType::MODEL;
    }
    if (editor::Util::isPrefabFile(extension)) {
        return editor::FileType::PREFAB;
    }
    if (editor::Util::isPoseAnimationFile(extension)) {
        return editor::FileType::POSE_ANIMATION;
    }
    if (editor::Util::isAudioFile(extension)) {
        return editor::FileType::AUDIO;
    }
    // .anim clips keep their icon: no thumbnail render (animation clips spec 9.R53)

    return editor::FileType::NONE;
}

std::string editor::ProjectWindow::thumbnailRequestKey(const fs::path& filePath) {
    return filePath.lexically_normal().string();
}

editor::ProjectWindow::ProjectWindow(Project* project, CodeEditor* codeEditor, ImageViewerWindow* imageViewerWindow) {
    this->project = project;
    this->codeEditor = codeEditor;
    this->imageViewerWindow = imageViewerWindow;
    this->firstOpen = true;
    this->requestSort = true;
    this->iconSize = 32;
    this->isExternalDragHovering = false;
    this->isRenaming = false;
    this->renameSelectPending = false;
    this->windowOpen = true;
    this->focusRequested = false;
    this->windowFocused = false;
    this->showDeleteConfirmation = false;
    this->stopThumbnailThread = false;
    memset(this->nameBuffer, 0, sizeof(this->nameBuffer));
    memset(this->searchBuffer, 0, sizeof(this->searchBuffer));

    // the main menu's Assets > Create > Animation Clip (shown once this is set)
    animationClipAppHooks().createClipAsset = [this](Project*) {
        assetsCreateAnimationClip(createTargetDirectory());
    };
    // Assets > Create > Animator Controller / Avatar Mask through the hooks too: S21's assets
    animationClipAppHooks().createControllerAsset = [this](Project*) {
        assetsCreateAnimatorAsset((int)AnimatorAssetDocument::Kind::Controller, createTargetDirectory());
    };
    animationClipAppHooks().createMaskAsset = [this](Project*) {
        assetsCreateAnimatorAsset((int)AnimatorAssetDocument::Kind::Mask, createTargetDirectory());
    };
}

editor::ProjectWindow::~ProjectWindow() {
    animationClipAppHooks().createClipAsset = nullptr;
    // the steps capture this window
    stopThumbnailWork();
}

void editor::ProjectWindow::stopThumbnailWork(){
    stopThumbnailThread = true;
    thumbnails.cancelPending();
    // A running step can wait in Engine::endAsyncThread for the main thread to execute the
    // batch committed before its own, so the main thread keeps executing the render queue
    // until the step ends; its own batch is executed last. A batch nobody executes would
    // block SokolSystem::shutdown's flush for good.
    while (thumbnails.pendingCount() > 0) {
        if (Engine::isViewLoaded())
            SystemRender::executeQueue();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (Engine::isViewLoaded())
        SystemRender::executeQueue();
    cancelThumbnailWork();
}

bool editor::ProjectWindow::isFocused() const{
    return windowFocused;
}

void editor::ProjectWindow::setOpen(bool open){
    if (open){
        if (!windowOpen){
            focusRequested = true;
        }
        windowOpen = true;
        return;
    }

    windowOpen = false;
    focusRequested = false;
    windowFocused = false;
}

bool editor::ProjectWindow::isOpen() const{
    return windowOpen;
}

void editor::ProjectWindow::notifyProjectPathChange(){
    // This is also used by project moves, which may not go through the normal
    // project-open path. Quiesce the old path before scanning the new one.
    cancelThumbnailWork();

    // Clear thumbnail textures when changing projects
    clearThumbnailTextures();
    dirTreeCache.clear();
    // copied paths of the project before are no files of this one
    clipboard.clear();

    resumeThumbnailWork();
    scanDirectory(project->getProjectPath());
}

void editor::ProjectWindow::cancelThumbnailWork(){
    {
        std::unique_lock<std::mutex> lock(thumbnailMutex);
        thumbnailWorkSuspended = true;
        std::queue<ThumbnailRequest> empty;
        std::swap(thumbnailQueue, empty);
        pendingThumbnailRequests.clear();
        abandonedMaterialThumbnails.clear();

        // The worker publishes pending preview state before clearing busy, so
        // after this wait all of its state can be invalidated atomically.
        thumbnailCondition.wait(lock, [this]() {
            return !thumbnailWorkerBusy && activeThumbnailSaves == 0;
        });

        ++thumbnailGeneration;
        hasPendingMaterialRender = false;
        pendingMaterialPath.clear();
        pendingMaterialGeneration = 0;
        hasPendingModelRender = false;
        pendingModelPath.clear();
        pendingModelGeneration = 0;
    }

    // removeScene() also removes one-shot bookkeeping, so the preview objects
    // can now release old-project resources without leaving dangling Scene*s.
    Engine::removeScene(materialRender.getScene());
    {
        std::lock_guard<std::mutex> lock(materialRenderMutex);
        materialRender.applyMaterial(Material());
    }

    Scene* modelScene = modelRender.getScene();
    if (modelScene) {
        Engine::removeScene(modelScene);
    }
    cancelPrefabThumbnails();
    {
        std::lock_guard<std::mutex> lock(modelRenderMutex);
        modelRender.clearScene();
    }

    std::lock_guard<std::mutex> completedLock(completedThumbnailMutex);
    std::queue<ThumbnailRequest> empty;
    std::swap(completedThumbnailQueue, empty);
}

void editor::ProjectWindow::resumeThumbnailWork(){
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        thumbnailWorkSuspended = false;
    }
    notifyThumbnailWork();
}

void editor::ProjectWindow::notifyThumbnailWork(){
    thumbnailCondition.notify_all();
    scheduleThumbnailStep();
}

void editor::ProjectWindow::handleExternalDragEnter() {
    isExternalDragHovering = true;
}

void editor::ProjectWindow::handleExternalDragLeave() {
    isExternalDragHovering = false;
}

std::string editor::ProjectWindow::colorLutTooltip(const fs::path& filePath) {
    // the renderer's cache, by the path the scene stores (assets-relative): parsed once
    const std::string stored = project->normalizeToAssetsRelative(filePath).generic_string();
    const std::shared_ptr<const CubeLut> lut = getCachedColorLut(stored);
    return describeColorLut(lut.get(), getCachedColorLutError(stored)) +
           "\n\nDouble-click (or right-click > Use as Color Grading LUT): the open scene's Color Grading LUT.";
}

void editor::ProjectWindow::assignColorLut(const fs::path& filePath) {
    SceneProject* scene = project->getSelectedScene();
    if (!scene || !scene->scene || scene->sceneType == SceneType::SCENE_UI) {
        Out::warning("Open a 2D or 3D scene to grade it with %s.", editor::FileUtils::pathToUtf8(filePath.filename()).c_str());
        return;
    }
    PostProcessingSettings settings = scene->scene->getPostProcessing();
    settings.lutTexture = project->normalizeToAssetsRelative(filePath).generic_string();
    settings.colorGradingEnabled = true;
    if (settings.lutContribution <= 0.0f)
        settings.lutContribution = 1.0f;
    CommandHandle::get(scene->id)->addCommandNoMerge(makePostProcessingCommand(
        project, scene->id, settings, "Assign Color LUT " + editor::FileUtils::pathToUtf8(filePath.filename())));
    Out::info("%s grades \"%s\" (Color Grading > LUT Texture).", editor::FileUtils::pathToUtf8(filePath.filename()).c_str(),
              scene->name.c_str());
}

void editor::ProjectWindow::notifyResourceFileChanged(const fs::path& filePath) {
    // by extension first: most changed files have no thumbnail (no stat for them)
    FileType type = classifyThumbnailFileType(filePath);

    if (type == FileType::NONE) {
        return;
    }

    std::error_code ec;
    if (!fs::exists(filePath, ec) || ec || fs::is_directory(filePath, ec) || ec) {
        return;
    }

    fs::path thumbnailPath = project->getThumbnailPath(filePath);
    evictThumbnailTexture(thumbnailPath.string());

    if (auto it = fileIndex.find(filePath); it != fileIndex.end()) {
        FileEntry& file = files[it->second];
        file.hasThumbnail = false;
        file.thumbnailPath.clear();
    }

    queueThumbnailGeneration(filePath, type, true);
}

void editor::ProjectWindow::requestThumbnailGeneration(const fs::path& filePath, bool forceRegenerate) {
    FileType type = classifyThumbnailFileType(filePath);
    if (type == FileType::NONE) {
        return;
    }

    if (forceRegenerate) {
        evictThumbnailTexture(project->getThumbnailPath(filePath).string());
    }

    queueThumbnailGeneration(filePath, type, forceRegenerate);
}

bool editor::ProjectWindow::hasPendingThumbnailWork() {
    std::lock_guard<std::mutex> lock(thumbnailMutex);
    // Any pending thumbnail work needs the shared systemDraw pass to keep running.
    return thumbnailWorkerBusy || hasPendingModelRender || hasPendingMaterialRender ||
           !thumbnailQueue.empty() || hasActivePrefabThumbnail || !prefabThumbnailQueue.empty() ||
           !poseThumbnailQueue.empty() || poseThumbnailer.busy();
}

namespace {

    // A load that never ends cannot be had on demand (a texture that fails to load is not pending:
    // it counts as empty): test_material_thumbnail.py holds one with this marker file
    bool materialThumbnailStuckForTest(const fs::path& projectPath) {
        static const char* const marker = std::getenv("JASPER_TEST_MATERIAL_THUMBNAIL_STUCK");
        std::error_code ec;
        return marker && fs::exists(projectPath / marker, ec);
    }

}

void editor::ProjectWindow::processMaterialThumbnails() {
    ThumbnailRequest pending;
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        if (!hasPendingMaterialRender) {
            return;
        }
        pending = {pendingMaterialPath, FileType::MATERIAL, pendingMaterialGeneration};
    }

    if (!Engine::isSceneRunning(materialRender.getScene())) {
        std::lock_guard<std::mutex> lock(materialRenderMutex);

        // a library shader's variant builds off the UI thread (and textures load): draw again
        // until the sphere has them, within a bound (a texture that never loads)
        constexpr int maxMaterialRenderWaits = 600;
        if (materialRenderWaitPath != pending.path) {
            materialRenderWaitPath = pending.path;
            materialRenderWaits = 0;
        }
        const bool stillLoading = materialRender.isPending() || materialThumbnailStuckForTest(project->getProjectPath());
        if (stillLoading && materialRenderWaits < maxMaterialRenderWaits) {
            materialRenderWaits++;
            Engine::executeSceneOnce(materialRender.getScene());
            return;
        }
        materialRenderWaits = 0;
        materialRenderWaitPath.clear();

        if (stillLoading) {
            Log::warn("No thumbnail for material %s: it was still loading after %d frames (asked again when it changes)",
                      pending.path.string().c_str(), maxMaterialRenderWaits);
            std::error_code timeError;
            const fs::file_time_type fileTime = fs::last_write_time(pending.path, timeError);
            {
                std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                if (pending.generation == thumbnailGeneration) {
                    if (!timeError) abandonedMaterialThumbnails[thumbnailRequestKey(pending.path)] = fileTime;
                    pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
                    hasPendingMaterialRender = false;
                    pendingMaterialPath.clear();
                    pendingMaterialGeneration = 0;
                }
            }
            notifyThumbnailWork();
            return;
        }

        fs::path thumbnailPath = project->getThumbnailPath(pending.path);
        fs::create_directories(thumbnailPath.parent_path());

        Framebuffer* framebuffer = materialRender.getFramebuffer();
        if (!framebuffer) {
            Log::warn("Thumbnail render produced no framebuffer for material: %s", pending.path.string().c_str());
            {
                std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                if (pending.generation == thumbnailGeneration) {
                    pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
                    hasPendingMaterialRender = false;
                    pendingMaterialPath.clear();
                    pendingMaterialGeneration = 0;
                }
            }
            notifyThumbnailWork();
            return;
        }

        {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            ++activeThumbnailSaves;
        }
        const bool saveStarted = GraphicUtils::saveFramebufferImage(framebuffer, thumbnailPath, false,
            [this, pending]() {
                bool publish = false;
                {
                    std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                    if (pending.generation == thumbnailGeneration) {
                        pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
                        publish = true;
                    }
                }
                if (publish) {
                    std::lock_guard<std::mutex> completedLock(completedThumbnailMutex);
                    completedThumbnailQueue.push(pending);
                }
                {
                    std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                    --activeThumbnailSaves;
                    // Once the count hits zero cancelThumbnailWork() may return and
                    // the object may be destroyed, so notify while still holding the
                    // lock and touch no members after this block.
                    thumbnailCondition.notify_all();
                }
            });
        if (!saveStarted) {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            --activeThumbnailSaves;
            pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
            thumbnailCondition.notify_all();
        }

        {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            if (pending.generation == thumbnailGeneration &&
                pendingMaterialGeneration == pending.generation) {
                hasPendingMaterialRender = false;
                pendingMaterialPath.clear();
                pendingMaterialGeneration = 0;
            }
        }

        notifyThumbnailWork();
    }
}

void editor::ProjectWindow::processModelThumbnails() {
    ThumbnailRequest pending;
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        if (!hasPendingModelRender) {
            return;
        }
        pending = {pendingModelPath, FileType::MODEL, pendingModelGeneration};
    }

    if (!Engine::isSceneRunning(modelRender.getScene())) {
        std::lock_guard<std::mutex> lock(modelRenderMutex);

        fs::path thumbnailPath = project->getThumbnailPath(pending.path);
        fs::create_directories(thumbnailPath.parent_path());

        Framebuffer* framebuffer = modelRender.getFramebuffer();
        if (!framebuffer) {
            Log::warn("Thumbnail render produced no framebuffer for model: %s", pending.path.string().c_str());
            // Release the scene while hasPendingModelRender still parks the worker;
            // clearing the flags first would let a wakeup start a new load into the
            // scene being torn down here.
            Engine::removeScene(modelRender.getScene());
            modelRender.clearScene();
            {
                std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                if (pending.generation == thumbnailGeneration) {
                    pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
                    hasPendingModelRender = false;
                    pendingModelPath.clear();
                    pendingModelGeneration = 0;
                }
            }
            notifyThumbnailWork();
            return;
        }

        {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            ++activeThumbnailSaves;
        }
        const bool saveStarted = GraphicUtils::saveFramebufferImage(framebuffer, thumbnailPath, false,
            [this, pending, thumbnailPath]() {
                std::error_code ec;
                if (!fs::exists(thumbnailPath, ec) || ec) {
                    Log::warn("Failed to save model thumbnail '%s': %s",
                              thumbnailPath.string().c_str(), ec ? ec.message().c_str() : "output file is missing");
                }
                bool publish = false;
                {
                    std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                    if (pending.generation == thumbnailGeneration) {
                        pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
                        publish = true;
                    }
                }
                if (publish) {
                    std::lock_guard<std::mutex> completedLock(completedThumbnailMutex);
                    completedThumbnailQueue.push(pending);
                }
                {
                    std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                    --activeThumbnailSaves;
                    // Once the count hits zero cancelThumbnailWork() may return and
                    // the object may be destroyed, so notify while still holding the
                    // lock and touch no members after this block.
                    thumbnailCondition.notify_all();
                }
            });
        if (!saveStarted) {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            --activeThumbnailSaves;
            pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
            thumbnailCondition.notify_all();
        }

        // Pixel readback above is synchronous; the detached PNG writer owns its
        // copy. Release this large preview scene now so its GPU buffers don't
        // overlap with the model being loaded into the user's scene.
        Engine::removeScene(modelRender.getScene());
        modelRender.clearScene();

        {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            if (pending.generation == thumbnailGeneration &&
                pendingModelGeneration == pending.generation) {
                hasPendingModelRender = false;
                pendingModelPath.clear();
                pendingModelGeneration = 0;
            }
        }

        notifyThumbnailWork();
    }
}

// ---- Prefab thumbnails (main thread) ----

std::filesystem::path editor::ProjectWindow::prefabStampPath(const fs::path& thumbnailPath) {
    fs::path stampPath = thumbnailPath;
    stampPath += ".deps";
    return stampPath;
}

std::string editor::ProjectWindow::prefabThumbnailStamp(const fs::path& prefabPath) {
    const std::string key = thumbnailRequestKey(prefabPath);
    std::error_code ec;
    const auto writeTime = fs::last_write_time(prefabPath, ec);
    if (ec) {
        return std::string();
    }
    const uintmax_t size = fs::file_size(prefabPath, ec);
    PrefabDependencies& dependencies = prefabDependencies[key];
    if (dependencies.files.empty() || dependencies.writeTime != writeTime || dependencies.size != size) {
        dependencies.writeTime = writeTime;
        dependencies.size = ec ? 0 : size;
        const fs::path relative = prefabPath.lexically_relative(project->getProjectPath());
        dependencies.files = project->getPrefabDependencies(relative);
        if (dependencies.files.empty()) {
            dependencies.files.push_back(relative);
        }
    }
    std::string stamp;
    for (const fs::path& file : dependencies.files) {
        const fs::path full = project->getProjectPath() / file;
        std::error_code fileEc;
        const auto fileTime = fs::last_write_time(full, fileEc);
        const uintmax_t fileSize = fileEc ? 0 : fs::file_size(full, fileEc);
        stamp += file.generic_string() + "|" + (fileEc ? std::string("missing") :
            std::to_string(fileSize) + "|" + std::to_string(static_cast<int64_t>(fileTime.time_since_epoch().count()))) + "\n";
    }
    return stamp;
}

void editor::ProjectWindow::checkPrefabThumbnailStamps() {
    std::vector<fs::path> changed;
    for (const FileEntry& file : files) {
        if (file.type != FileType::PREFAB || file.isDirectory) {
            continue;
        }
        const fs::path path = (currentPath / file.name).lexically_normal();
        auto known = prefabThumbnailStamps.find(thumbnailRequestKey(path));
        if (known != prefabThumbnailStamps.end() && known->second != prefabThumbnailStamp(path)) {
            changed.push_back(path);
        }
    }
    if (changed.empty()) {
        return;
    }
    // (the loaded prefabs read a changed file first: the hot reload applies it to the asset
    // database before it tells this window)
    for (const fs::path& path : changed) {
        notifyResourceFileChanged(path);
    }
}

void editor::ProjectWindow::cancelPrefabThumbnails() {
    std::queue<ThumbnailRequest> empty;
    std::swap(prefabThumbnailQueue, empty);
    std::queue<ThumbnailRequest> emptyPose;
    std::swap(poseThumbnailQueue, emptyPose);
    poseThumbnailer.cancel();
    if (hasActivePrefabThumbnail) {
        if (Scene* scene = prefabRender.getScene()) {
            Engine::removeScene(scene);
        }
        prefabRender.clear();
        hasActivePrefabThumbnail = false;
    }
    prefabThumbnailFrames = 0;
}

void editor::ProjectWindow::processPrefabThumbnails() {
    if (!hasActivePrefabThumbnail) {
        if (!Engine::isViewLoaded()) {
            return;
        }
        ThumbnailRequest next;
        {
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            if (thumbnailWorkSuspended || prefabThumbnailQueue.empty()) {
                return;
            }
            next = prefabThumbnailQueue.front();
            prefabThumbnailQueue.pop();
            if (next.generation != thumbnailGeneration) {
                pendingThumbnailRequests.erase(thumbnailRequestKey(next.path));
                return;
            }
        }
        bool built = false;
        try {
            built = prefabRender.build(project, next.path.lexically_relative(project->getProjectPath()));
        } catch (const std::exception& e) {
            Log::error("Failed to build prefab thumbnail '%s': %s", next.path.string().c_str(), e.what());
        }
        if (!built) {
            prefabRender.clear();
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            pendingThumbnailRequests.erase(thumbnailRequestKey(next.path));
            return;
        }
        prefabThumbnailActive = next;
        hasActivePrefabThumbnail = true;
        prefabThumbnailFrames = 0;
        Engine::executeSceneOnce(prefabRender.getScene());
        return;
    }

    Scene* scene = prefabRender.getScene();
    if (!scene || Engine::isSceneRunning(scene)) {
        return;  // not drawn yet
    }
    prefabThumbnailFrames++;
    if (prefabThumbnailFrames == 1) {
        prefabRender.frame();  // the systems know the world bounds after a frame
    }
    // a few frames, more while textures or models still load
    if (prefabThumbnailFrames < 3 || (ResourceProgress::hasActiveBuilds() && prefabThumbnailFrames < 60)) {
        Engine::executeSceneOnce(scene);
        return;
    }

    const ThumbnailRequest pending = prefabThumbnailActive;
    const fs::path thumbnailPath = project->getThumbnailPath(pending.path);
    std::error_code ec;
    fs::create_directories(thumbnailPath.parent_path(), ec);
    const std::string stamp = prefabThumbnailStamps[thumbnailRequestKey(pending.path)];

    bool saveStarted = false;
    if (Framebuffer* framebuffer = prefabRender.getFramebuffer()) {
        {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            ++activeThumbnailSaves;
        }
        saveStarted = GraphicUtils::saveFramebufferImage(framebuffer, thumbnailPath, false,
            [this, pending, thumbnailPath, stamp]() {
                FileUtils::writeFile(prefabStampPath(thumbnailPath), stamp);
                bool publish = false;
                {
                    std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                    if (pending.generation == thumbnailGeneration) {
                        pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
                        publish = true;
                    }
                }
                if (publish) {
                    std::lock_guard<std::mutex> completedLock(completedThumbnailMutex);
                    completedThumbnailQueue.push(pending);
                }
                {
                    std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
                    --activeThumbnailSaves;
                    // touch no members after this block (see processModelThumbnails)
                    thumbnailCondition.notify_all();
                }
            });
        if (!saveStarted) {
            std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
            --activeThumbnailSaves;
            thumbnailCondition.notify_all();
        }
    } else {
        Log::warn("Thumbnail render produced no framebuffer for prefab: %s", pending.path.string().c_str());
    }
    if (!saveStarted) {
        std::lock_guard<std::mutex> thumbnailLock(thumbnailMutex);
        pendingThumbnailRequests.erase(thumbnailRequestKey(pending.path));
    }

    // The pixels were read already: the preview scene goes
    Engine::removeScene(scene);
    prefabRender.clear();
    hasActivePrefabThumbnail = false;
    prefabThumbnailFrames = 0;
}

void editor::ProjectWindow::refreshProjectFiles(){
    projectFiles.clear();

    for (const SceneProject& sceneProject : project->getScenes()){
        projectFiles.insert(sceneProject.filepath.generic_string());
        for (const PrefabSceneInfo& info : sceneProject.prefabs){
            projectFiles.insert(info.prefabPath.generic_string());
        }
    }

    for (const fs::path& prefabPath : project->getStandalonePrefabs()){
        projectFiles.insert(prefabPath.generic_string());
    }
}

bool editor::ProjectWindow::fileNotInProject(const FileEntry& fe) const{
    if (fe.type != FileType::SCENE && fe.type != FileType::PREFAB)
        return false;

    const std::string relativePath = (currentPath / fe.name).lexically_relative(project->getProjectPath()).generic_string();

    return !projectFiles.count(relativePath);
}

ImU32 editor::ProjectWindow::fileSeparatorColor(const FileEntry& fe) const{
    if (fe.isDirectory)
        return ImGui::GetColorU32(ImVec4(0.60f, 0.60f, 0.60f, 1.0f));

    switch (fe.type) {
        case FileType::IMAGE:
            return ImGui::GetColorU32(ImVec4(0.25f, 0.55f, 1.00f, 1.0f));
        case FileType::MATERIAL:
            return ImGui::GetColorU32(ImVec4(0.20f, 0.80f, 0.70f, 1.0f));
        case FileType::SCENE:
            return ImGui::GetColorU32(ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
        case FileType::PREFAB:
            return ImGui::GetColorU32(ImVec4(0.30f, 0.85f, 0.30f, 1.0f));
        case FileType::MODEL:
            return ImGui::GetColorU32(ImVec4(0.80f, 0.40f, 0.40f, 1.0f));
        case FileType::SOURCE:
            return ImGui::GetColorU32(ImVec4(0.17f, 0.42f, 0.75f, 1.0f));
        case FileType::HEADER:
            return ImGui::GetColorU32(ImVec4(0.49f, 0.25f, 0.75f, 1.0f));
        case FileType::LUA:
            return ImGui::GetColorU32(ImVec4(0.17f, 0.18f, 0.45f, 1.0f));
        case FileType::AUDIO:
            return ImGui::GetColorU32(ImVec4(0.12f, 0.62f, 0.39f, 1.0f));
        case FileType::FONT:
            return ImGui::GetColorU32(ImVec4(0.84f, 0.27f, 0.31f, 1.0f));
        case FileType::HTML:
            return ImGui::GetColorU32(ImVec4(0.89f, 0.30f, 0.15f, 1.0f));
        case FileType::CSS:
            return ImGui::GetColorU32(ImVec4(0.40f, 0.20f, 0.60f, 1.0f));
        case FileType::COLOR_LUT:
            return ImGui::GetColorU32(ImVec4(0.06f, 0.46f, 0.43f, 1.0f));
        case FileType::POSE_ANIMATION:
            return ImGui::GetColorU32(ImVec4(0.23f, 0.43f, 0.65f, 1.0f));
        case FileType::HUMANOID:
            return ImGui::GetColorU32(ImVec4(0.50f, 0.50f, 0.50f, 1.0f));
        case FileType::ANIMATION_CLIP:
            return ImGui::GetColorU32(ImVec4(0.91f, 0.53f, 0.23f, 1.0f));
        case FileType::VERTEX_ANIMATION:
            return ImGui::GetColorU32(ImVec4(0.25f, 0.65f, 0.63f, 1.0f));
        case FileType::ANIMATOR_ASSET:
            return ImGui::GetColorU32(ImVec4(0.82f, 0.48f, 0.15f, 1.0f));
        case FileType::PHYSICS_MATERIAL:
            return ImGui::GetColorU32(ImVec4(0.31f, 0.76f, 0.97f, 1.0f));
        case FileType::NONE:
        default:
            return ImGui::GetColorU32(ImVec4(0.50f, 0.50f, 0.50f, 1.0f));
    }
}

// Flat toolbar button in the Unity style (no frame until hovered).
static bool projectToolbarButton(const char* label, const char* tooltip = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, editor::Theme::Colors::IconButtonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, editor::Theme::Colors::IconButtonActive);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

// Toolbar strip background with Unity's dark bottom edge; returns its height.
static float drawToolbarStrip(float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetCursorScreenPos();
    ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + height);
    dl->AddRectFilled(min, max, ImGui::GetColorU32(editor::Theme::Colors::ToolbarBackground));
    dl->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), ImGui::GetColorU32(editor::Theme::Colors::ToolbarBorder), editor::Theme::dpi(1.0f));
    return height;
}

// The Create items for `directory` (the one list, AssetCreateMenu), then the imports
void editor::ProjectWindow::renderCreateItems(const fs::path& directory) {
    for (const AssetCreateItem& item : assetCreateItems()) {
        if (item.kind == AssetCreateKind::AnimationClip && !animationClipAppHooks().createClipAsset) {
            continue;
        }
        if (item.separatorBefore) ImGui::Separator();
        // a Lua script lives in the Lua directory (create_script refuses one elsewhere)
        const std::string refused = item.kind == AssetCreateKind::LuaScript ? luaScriptRefusal(directory) : std::string();
        const std::string label = std::string(item.icon) + " " + item.label;
        const bool chosen = ImGui::MenuItem(label.c_str(), nullptr, false, refused.empty());
        uiBuilding.contextMenu.push_back({item.label, lastItemRect()});
        if (!refused.empty()) {
            ImGui::SetItemTooltip("%s", refused.c_str());
        }
        if (chosen) {
            createAsset(item.kind, directory);
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import New Asset...")){
        importExternalPaths(editor::FileDialogs::openFileDialogMultiple(), directory);
    }

    if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Import Folder...")){
        importExternalPaths(editor::FileDialogs::pickFolderDialogMultiple(), directory);
    }

    if (ImGui::MenuItem(ICON_FA_VOLUME_HIGH " Import Sound Essentials")){
        assetsImportSoundEssentials();
    }
}

void editor::ProjectWindow::renderHeader() {
    // Unity's Project toolbar: the create menu on the left, search and the
    // panel options on the right.
    const float padX = Theme::dpi(4.0f);
    const float padY = Theme::dpi(2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::dpi(6.0f), Theme::dpi(2.0f)));
    const float frameH = ImGui::GetFrameHeight();
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    const float barH = drawToolbarStrip(frameH + padY * 2.0f);

    ImGui::SetCursorScreenPos(ImVec2(barMin.x + padX, barMin.y + padY));
    if (projectToolbarButton(ICON_FA_PLUS " " ICON_FA_CARET_DOWN, "Create")) {
        ImGui::OpenPopup("##ProjectCreateMenu");
    }
    if (ImGui::BeginPopup("##ProjectCreateMenu")) {
        renderCreateItems(listingFolder());
        ImGui::EndPopup();
    }

    const float gearW = ImGui::CalcTextSize(ICON_FA_ELLIPSIS_VERTICAL).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float searchW = std::min(Theme::dpi(240.0f), std::max(Theme::dpi(100.0f), barW * 0.35f));
    // Search by Type, Search by Label and Save Search sit left of the field (Unity)
    const float toolW = (ImGui::GetFrameHeight() + Theme::dpi(2.0f)) * 3.0f;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(barMin.x + barW - padX - gearW - Theme::dpi(8.0f) - searchW - toolW, barMin.y + padY));
    renderSearchTools();
    ImGui::SameLine(0.0f, Theme::dpi(4.0f));
    ImGui::SetCursorScreenPos(ImVec2(barMin.x + barW - padX - gearW - Theme::dpi(4.0f) - searchW, barMin.y + padY));
    const bool searchEdited = UIUtils::searchInput("##ProjectSearch", "Search  (t:Texture2D l:Label ext: path: re:)", searchBuffer,
                                                   IM_ARRAYSIZE(searchBuffer), false, nullptr, searchW);
    uiBuilding.searchBox = lastItemRect();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Words match names (typos forgiven); \"a phrase\" exactly; t:type l:label ext:png path:part re:regex "
                          "ref:asset missing:; the same key ORs, different keys AND; OR joins whole searches");
    }
    if (searchEdited) {
        std::string trimmed = searchBuffer;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
        if (trimmed != searchText) setSearch(trimmed);
    }
    ImGui::SameLine(0.0f, Theme::dpi(4.0f));
    if (projectToolbarButton(ICON_FA_ELLIPSIS_VERTICAL, "Project panel options")) {
        ImGui::OpenPopup("SettingsPopup");
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(barMin.x, barMin.y + barH));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(300.0f), 0.0f));
    if (ImGui::BeginPopup("SettingsPopup")) {
        ImGui::Text("Project Panel");
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::dpi(2.0f), ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "##ResetSettings")) {
            iconSize = 32;
            currentLayout = LayoutType::AUTO;
            itemViewStyle = ItemViewStyle::CLASSIC;
            leftPanelWidth = 200.0f;
            AppSettings::setResourcesIconSize(iconSize);
            AppSettings::setResourcesLayout(static_cast<int>(currentLayout));
            AppSettings::setResourcesItemViewStyle(static_cast<int>(itemViewStyle));
            AppSettings::setResourcesLeftPanelWidth(leftPanelWidth);
            AppSettings::saveSettings();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reset to defaults");
        }
        ImGui::Separator();

        // Same label | input layout as SceneWindow's scene settings popup, but the
        // value column stretches to fill the remaining popup width
        if (ImGui::BeginTable("resources_settings_table", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(100.0f));
            // Explicit stretch: tables inside auto-resizing popups default to
            // SizingFixedFit, which would shrink this column to its content
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Layout");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            const char* layoutNames[] = { "Auto", "One Column", "Two Column (files only)", "Two Column" };
            int layoutIndex = static_cast<int>(currentLayout);
            if (ImGui::Combo("##Layout", &layoutIndex, layoutNames, IM_ARRAYSIZE(layoutNames))) {
                currentLayout = static_cast<LayoutType>(layoutIndex);
                AppSettings::setResourcesLayout(static_cast<int>(currentLayout));
                AppSettings::saveSettings();
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Item view");
            ImGui::TableSetColumnIndex(1);
            ItemViewStyle prevStyle = itemViewStyle;
            if (ImGui::RadioButton("Icons", itemViewStyle == ItemViewStyle::CLASSIC)) itemViewStyle = ItemViewStyle::CLASSIC;
            ImGui::SameLine();
            if (ImGui::RadioButton("Cards", itemViewStyle == ItemViewStyle::CARD)) itemViewStyle = ItemViewStyle::CARD;
            if (itemViewStyle != prevStyle) {
                AppSettings::setResourcesItemViewStyle(static_cast<int>(itemViewStyle));
                AppSettings::saveSettings();
            }

            ImGui::EndTable();
        }

        ImGui::EndPopup();
    }
}

void editor::ProjectWindow::renderBottomBar() {
    // Unity's bottom strip: the selected asset's path on the left, the icon
    // size slider on the right.
    const float padX = Theme::dpi(6.0f);
    const float padY = Theme::dpi(2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::dpi(4.0f), Theme::dpi(1.0f)));
    const float frameH = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    const float barH = frameH + padY * 2.0f;
    dl->AddRectFilled(barMin, ImVec2(barMin.x + barW, barMin.y + barH), ImGui::GetColorU32(Theme::Colors::ToolbarBackground));
    dl->AddLine(ImVec2(barMin.x, barMin.y + 0.5f), ImVec2(barMin.x + barW, barMin.y + 0.5f), ImGui::GetColorU32(Theme::Colors::ToolbarBorder), Theme::dpi(1.0f));

    const float sliderW = std::min(Theme::dpi(90.0f), barW * 0.3f);
    const float sliderX = barMin.x + barW - padX - sliderW;

    std::string status;
    TextureRender* statusIcon = nullptr;
    if (selectedFiles.size() == 1) {
        const std::string& name = *selectedFiles.begin();
        status = (currentPath / name).lexically_relative(project->getProjectPath()).generic_string();
        for (const FileEntry& f : files) {
            if (f.name == name) { statusIcon = f.icon; break; }
        }
    } else if (selectedFiles.size() > 1) {
        status = std::to_string(selectedFiles.size()) + " items selected";
    }
    // the selection's Asset Labels, left of the size slider (S25 R23)
    const float stripWidth = renderLabelStrip(barMin, barH, sliderX - padX);
    const float statusRight = sliderX - padX - stripWidth - (stripWidth > 0.0f ? padX : 0.0f);
    if (!status.empty()) {
        float x = barMin.x + padX;
        const float lineH = ImGui::GetTextLineHeight();
        const float textY = barMin.y + (barH - lineH) * 0.5f;
        if (ImTextureID tex = Backend::getImGuiTexture(statusIcon); statusIcon && tex != ImTextureID{}) {
            dl->AddImage(tex, ImVec2(x, textY), ImVec2(x + lineH, textY + lineH));
            x += lineH + Theme::dpi(4.0f);
        }
        ImGui::RenderTextEllipsis(dl, ImVec2(x, textY), ImVec2(statusRight, textY + lineH), statusRight, status.c_str(), nullptr, nullptr);
    }

    ImGui::SetCursorScreenPos(ImVec2(sliderX, barMin.y + padY));
    ImGui::SetNextItemWidth(sliderW);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, Theme::dpi(8.0f));
    ImGui::SliderInt("##IconSize", &iconSize, MIN_GRID_ICON_SIZE, THUMBNAIL_SIZE, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Icon size");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        AppSettings::setResourcesIconSize(iconSize);
        AppSettings::saveSettings();
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(barMin.x, barMin.y + barH));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// ImGui hides label text after "##", so breadcrumb items (directory names that
// may contain "##") draw their text manually over a label-less widget; callers
// wrap these in PushID for uniqueness
static bool verbatimTextButton(const std::string& label, bool current) {
    const char* begin = label.c_str();
    const char* end = begin + label.size();
    ImVec2 textSize = ImGui::CalcTextSize(begin, end);
    ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::Button("##BreadcrumbItem", ImVec2(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f));
    ImU32 col = ImGui::GetColorU32(current || ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), col, begin, end);
    return pressed;
}

static bool verbatimTextSelectable(const std::string& label) {
    const char* begin = label.c_str();
    const char* end = begin + label.size();
    ImVec2 textSize = ImGui::CalcTextSize(begin, end);
    bool pressed = ImGui::Selectable("##BreadcrumbHiddenItem", false, 0, ImVec2(textSize.x, 0.0f));
    ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin(), ImGui::GetColorU32(ImGuiCol_Text), begin, end);
    return pressed;
}

// Same ImGui shell hook App uses to open URLs
static void openInFileManager(const fs::path& dir) {
    auto openInShell = ImGui::GetPlatformIO().Platform_OpenInShellFn;
    std::string dirUtf8 = editor::FileUtils::pathToUtf8(dir);
    if (!openInShell || !openInShell(ImGui::GetCurrentContext(), dirUtf8.c_str())) {
        editor::Backend::getApp().registerAlert("Open in File Manager", "Could not open " + dirUtf8);
    }
}

void editor::ProjectWindow::renderPathBreadcrumb(const ImVec2& size) {
    // Unity's path bar over the grid: "project > assets > sprites", each
    // segment clickable and a drop target.
    fs::path rootPath = project->getProjectPath();

    std::vector<std::pair<std::string, fs::path>> segments;
    std::string rootName = rootPath.filename().string();
    if (rootName.empty()) rootName = rootPath.string();
    segments.emplace_back(rootName, rootPath);
    if (!currentPath.empty() && currentPath != rootPath && currentPath.string().find(rootPath.string()) == 0) {
        fs::path accumulated = rootPath;
        for (const auto& part : currentPath.lexically_relative(rootPath)) {
            if (part == "." || part.empty()) continue;
            accumulated /= part;
            segments.emplace_back(part.string(), accumulated);
        }
    }

    // Navigation and drops are deferred to after the child so scanDirectory and
    // handleInternalDragAndDrop don't mutate state mid-layout
    fs::path navigateTo;
    fs::path dropTarget;
    std::vector<fs::path> dropSources;

    ImVec2 barMin = ImGui::GetCursorScreenPos();
    ImGui::BeginChild("##ResourcesPath", size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(barMin.x, barMin.y + size.y - 0.5f), ImVec2(barMin.x + ImGui::GetWindowWidth(), barMin.y + size.y - 0.5f),
                ImGui::GetColorU32(Theme::Colors::ToolbarBorder), Theme::dpi(1.0f));

    const char* separator = ICON_FA_ANGLE_RIGHT;
    const float framePadX = Theme::dpi(3.0f);
    float separatorWidth = ImGui::CalcTextSize(separator).x + framePadX * 2.0f;
    auto segmentWidth = [&](const std::string& name) {
        return ImGui::CalcTextSize(name.c_str()).x + framePadX * 2.0f;
    };

    ImGui::SetCursorPos(ImVec2(Theme::dpi(6.0f), 0.0f));
    ImGui::AlignTextToFramePadding();

    if (!searchText.empty()) {
        ImGui::TextDisabled("Search results for \"%s\" in", searchText.c_str());
        ImGui::SameLine();
    }

    // Fit as many trailing segments as possible; earlier ones collapse into "..."
    size_t firstVisible = segments.size();
    float usedWidth = 0.0f;
    float availableWidth = ImGui::GetContentRegionAvail().x;
    for (size_t i = segments.size(); i-- > 0;) {
        float width = segmentWidth(segments[i].first) + (i > 0 ? separatorWidth : 0.0f);
        float reserved = (i > 0) ? (separatorWidth + segmentWidth("...")) : 0.0f;
        if (firstVisible < segments.size() && (usedWidth + width + reserved) > availableWidth) {
            break;
        }
        usedWidth += width;
        firstVisible = i;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Colors::IconButtonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::IconButtonActive);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(framePadX, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    if (firstVisible > 0) {
        if (ImGui::Button("...##BreadcrumbMore")) {
            ImGui::OpenPopup("##BreadcrumbMorePopup");
        }
        ImGui::SameLine();
    }

    for (size_t i = firstVisible; i < segments.size(); i++) {
        if (i > firstVisible || firstVisible > 0) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(" %s ", separator);
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        bool isCurrent = (segments[i].second == currentPath);
        if (verbatimTextButton(segments[i].first, isCurrent) && (!isCurrent || !searchText.empty())) {
            navigateTo = segments[i].second;
        }
        if (!isCurrent && ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            std::vector<fs::path> sources = peek && peek->IsDataType("resource_files") ? payloadPaths(peek) : std::vector<fs::path>();
            if (acceptsDrop(segments[i].second, sources) && ImGui::AcceptDragDropPayload("resource_files")) {
                dropTarget = segments[i].second;
                dropSources = std::move(sources);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", segments[i].second.string().c_str());
        }
        ImGui::PopID();
        ImGui::SameLine();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);

    if (ImGui::BeginPopup("##BreadcrumbMorePopup")) {
        for (size_t i = 0; i < firstVisible; i++) {
            ImGui::PushID(static_cast<int>(i));
            if (verbatimTextSelectable(segments[i].first)) {
                navigateTo = segments[i].second;
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();

    if (!navigateTo.empty()) {
        // Clicking a path segment leaves search mode, like Unity.
        searchText.clear();
        searchBuffer[0] = '\0';
        scanDirectory(navigateTo);
        selectedFiles.clear();
    }
    if (!dropTarget.empty()) {
        handleInternalDragAndDrop(dropTarget, dropSources);
    }
}

void editor::ProjectWindow::renderFileListing(bool showDirectories){
    // Stored iconSize stays logical; layout/draw use the current window DPI.
    const float uiIconSize = static_cast<float>(iconSize) * Theme::dpiScale();
    const bool useCardView = (itemViewStyle == ItemViewStyle::CARD);
    // Unity turns the grid into a list at the smallest icon size.
    const bool useListView = !useCardView && iconSize <= MIN_GRID_ICON_SIZE;

    // --- Common grid sizing -------------------------------------------------
    // Unity tiles: the icon plus room for a short name under it.
    float columnWidth = useCardView ? uiIconSize * 2.5f : std::max(uiIconSize * 1.3f, Theme::dpi(76.0f));
    float availableWidth = ImGui::GetContentRegionAvail().x;
    int columns = useListView ? 1 : static_cast<int>(availableWidth / columnWidth);
    if (columns < 1) columns = 1;

    // Card view gets a bit more vertical padding than classic.
    ImVec2 cellPadding = Theme::dpi(useCardView ? ImVec2(6.0f, 8.0f) : (useListView ? ImVec2(0.0f, 0.0f) : ImVec2(2.0f, 6.0f)));
    float  totalTableWidth = availableWidth;

    ImVec2 scrollRegionMin = ImGui::GetWindowPos();
    ImVec2 scrollRegionMax = ImVec2(
        scrollRegionMin.x + ImGui::GetWindowSize().x,
        scrollRegionMin.y + ImGui::GetWindowSize().y
    );

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);

    handleTypeAhead();
    handleWheelResize();

    bool clickedInFile = false;
    hoveredFile.clear();

    // --- Update marquee rectangle and auto-scroll ---------------------------
    // (it starts after the items, below: a press arriving with the pointer move must see the file
    // under it, which only this frame's items know; a drag of files ends it)
    if (isDragging && ImGui::GetDragDropPayload()){
        isDragging = false;
    }
    if (isDragging){
        ImVec2 mousePos = ImGui::GetMousePos();
        float scrollMargin = Theme::dpi(20.0f);
        float currentScroll = ImGui::GetScrollY();

        if (mousePos.y > scrollRegionMax.y - scrollMargin){
            float scrollDelta = (mousePos.y - (scrollRegionMax.y - scrollMargin)) * 0.5f;
            ImGui::SetScrollY(currentScroll + scrollDelta);
        }else if (mousePos.y < scrollRegionMin.y + scrollMargin){
            float scrollDelta = (mousePos.y - (scrollRegionMin.y + scrollMargin)) * 0.5f;
            ImGui::SetScrollY(currentScroll + scrollDelta);
        }

        dragEnd = mousePos;
        dragEnd.x -= scrollRegionMin.x;
        dragEnd.y -= scrollRegionMin.y;
        dragEnd.x += ImGui::GetScrollX();
        dragEnd.y += ImGui::GetScrollY();

        if (!ImGui::IsMouseDown(0)){
            isDragging = false;
        }
    }

    // --- Card metrics (constants — only used in Card view) ------------------
    const float pad = Theme::dpi(8.0f);                     // Inner padding
    const float thumbHeight = uiIconSize + Theme::dpi(4.0f);
    const float lineThickness = Theme::dpi(2.0f);
    const int maxTextLines = 2;                         // At most 2 text lines
    const float fontScale = 0.80f;                     // Scale for card text
    const float lineHeight = ImGui::GetTextLineHeight();
    const float textAreaHeight = maxTextLines * (lineHeight * fontScale); // Tight fit for scaled text
    const float cardHeight = pad + thumbHeight
                             + pad * 0.5f + lineThickness
                             + pad * 0.5f + textAreaHeight
                             + pad;
    const float contentYOffset = pad * 0.75f; // Extra vertical offset so thumbnail, line, and text start a bit lower

    if (ImGui::BeginTable("FileTable", columns, ImGuiTableFlags_SizingStretchSame, ImVec2(totalTableWidth, 0))){
        // scanDirectory waits until the loop ended (it rebuilds `files`)
        listingIterating = true;
        for (auto& file : files){
            if (!showDirectories && file.isDirectory) continue;

            bool deferredDirectoryChange = false;
            fs::path dropTarget;
            std::vector<fs::path> dropSources;

            ImGui::TableNextColumn();
            ImGui::PushID(file.name.c_str());

            float cellWidth = ImGui::GetContentRegionAvail().x;

            // --- Choose item size per style --------------------------------
            // Classic tile: icon, then a one-line name in a selection pill.
            const float namePadY = Theme::dpi(1.0f);
            const float nameHeight = lineHeight + namePadY * 2.0f;
            const float iconNameGap = Theme::dpi(3.0f);
            ImVec2 itemSize;
            if (useCardView){
                itemSize = ImVec2(cellWidth, cardHeight);
            }else if (useListView){
                itemSize = ImVec2(cellWidth, lineHeight + Theme::dpi(4.0f));
            }else{
                itemSize = ImVec2(cellWidth, uiIconSize + iconNameGap + nameHeight);
            }

            // --- Marquee overlap before item creation ----------------------
            if (isDragging){
                ImVec2 itemPos = ImGui::GetCursorScreenPos();
                itemPos.x -= scrollRegionMin.x;
                itemPos.y -= scrollRegionMin.y;
                itemPos.x += ImGui::GetScrollX();
                itemPos.y += ImGui::GetScrollY();

                ImRect itemRect(itemPos, ImVec2(itemPos.x + itemSize.x, itemPos.y + itemSize.y));

                ImRect selectionRect(
                    ImVec2(std::min(dragStart.x, dragEnd.x), std::min(dragStart.y, dragEnd.y)),
                    ImVec2(std::max(dragStart.x, dragEnd.x), std::max(dragStart.y, dragEnd.y))
                );

                bool isOverlapping = itemRect.Overlaps(selectionRect);

                if (!ctrlPressed){
                    if (isOverlapping)
                        selectedFiles.insert(file.name);
                    else
                        selectedFiles.erase(file.name);
                }else{
                    if (isOverlapping)
                        selectedFiles.insert(file.name);
                }
            }

            bool isSelected = selectedFiles.find(file.name) != selectedFiles.end();

            // --- Create the hit area ---------------------------------------
            bool itemPressed = false;
            bool hovered = false;

            if (useCardView){
                itemPressed = ImGui::InvisibleButton("##card", itemSize);
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            }else{
                // Classic and list draw their own highlight (see below).
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                itemPressed = ImGui::Selectable("",
                                                isSelected,
                                                ImGuiSelectableFlags_AllowDoubleClick,
                                                itemSize);
                ImGui::PopStyleColor(3);
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            }

            ImVec2 itemMin = ImGui::GetItemRectMin();
            ImVec2 itemMax = ImGui::GetItemRectMax();
            // type-ahead picked it: into view
            if (file.name == scrollToFile) {
                ImGui::SetScrollHereY(0.5f);
                scrollToFile.clear();
            }
            // a ping (Search, Bookmarks): a fading frame around the item, scrolled into view once
            if (file.name == pingName && ImGui::GetTime() < pingUntil) {
                if (pingScroll) {
                    ImGui::SetScrollHereY(0.5f);
                    pingScroll = false;
                }
                const float alpha = static_cast<float>((pingUntil - ImGui::GetTime()) / 1.5);
                ImGui::GetWindowDrawList()->AddRect(itemMin, itemMax, ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.2f, alpha)),
                                                    Theme::dpi(3.0f), 0, Theme::dpi(2.0f));
            }
            if (uiBuilding.entries.size() < 200) {
                uiBuilding.entries.push_back({file.name, lastItemRect()});
            }

            if (hovered) hoveredFile = file.name;

            const bool doubleClickedItem = hovered && ImGui::IsMouseDoubleClicked(0);

            // --- Double-click: open directory or file ----------------------
            if (doubleClickedItem){
                clickedInFile = true;

                selectedFiles.clear();
                selectedFiles.insert(file.name);
                lastSelectedFile = file.name;

                if (file.isDirectory){
                    deferredDirectoryChange = true;
                }else{
                    updateInspectedAsset(file.name);
                    openFile(file, currentPath / file.name);
                }
            }

            // --- Resolve icon/thumbnail common path ------------------------
            ImTextureID fileIconImage = Backend::getImGuiTexture(file.icon);
            float dispW = uiIconSize;
            float dispH = uiIconSize;

            // Only resolve thumbnails for items inside the visible region, so
            // the GPU texture cache stays bounded with large directories
            // (clipped items never draw the image anyway)
            if (file.hasThumbnail && ImGui::IsItemVisible()){
                int tw = 0;
                int th = 0;
                if (ImTextureID thumbTex = getThumbnailTexture(file.thumbnailPath, tw, th)){
                    if (tw > 0 && th > 0){
                        float scale = std::min(uiIconSize / static_cast<float>(tw),
                                               uiIconSize / static_cast<float>(th));
                        dispW = std::max(1.0f, tw * scale);
                        dispH = std::max(1.0f, th * scale);
                    }
                    fileIconImage = thumbTex;
                }
            }

            // --- Drag source (unified) -------------------------------------
            bool itemActive   = ImGui::IsItemActive();
            float dragThreshold = ImGui::GetIO().MouseDragThreshold;
            bool wantDrag = itemActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, dragThreshold);

            if (wantDrag){
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)){
                    // Ensure selection contains this file if dragging an unselected item
                    if (selectedFiles.find(file.name) == selectedFiles.end()){
                        selectedFiles.clear();
                        selectedFiles.insert(file.name);
                        lastSelectedFile = file.name;
                    }

                    // Payload: list of paths separated by '\0'
                    std::vector<char> buffer;
                    buffer.reserve(256);

                    for (const auto& selectedFile : selectedFiles){
                        std::string fullPath = (currentPath / selectedFile).string();
                        buffer.insert(buffer.end(), fullPath.begin(), fullPath.end());
                        buffer.push_back('\0');
                    }

                    ImGui::SetDragDropPayload("resource_files", buffer.data(), buffer.size());

                    ImGui::Text("Moving %zu file(s)", selectedFiles.size());

                    if (selectedFiles.size() == 1){
                        float imageDragSize = Theme::dpi(32.0f);
                        float scale = std::min(imageDragSize / dispW, imageDragSize / dispH);
                        float previewW = dispW * scale;
                        float previewH = dispH * scale;
                        float availWidth = ImGui::GetCurrentWindow()->Size.x;
                        float xPos = (availWidth - previewW) * 0.5f;
                        ImGui::SetCursorPosX(xPos);
                        Widgets::image(fileIconImage, ImVec2(previewW, previewH));
                    }

                    ImGui::EndDragDropSource();
                }
            }

            // --- Directory as drop target ----------------------------------
            // (not one of the dragged folders, nor a folder below one)
            if (file.isDirectory){
                if (ImGui::BeginDragDropTarget()){
                    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
                    std::vector<fs::path> sources = peek && peek->IsDataType("resource_files") ? payloadPaths(peek) : std::vector<fs::path>();
                    if (acceptsDrop(currentPath / file.name, sources) && ImGui::AcceptDragDropPayload("resource_files")){
                        dropTarget = currentPath / file.name;
                        dropSources = std::move(sources);
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            // =================================================================
            // Draw content per style
            // =================================================================
            // outside the project's build, or cut to the clipboard: dimmed
            const bool notInProject = fileNotInProject(file) || clipboard.isCutItem(currentPath / file.name);

            if (useCardView){
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImGuiStyle& style = ImGui::GetStyle();

                ImVec4 header = style.Colors[ImGuiCol_Header];
                ImVec4 headerHovered = style.Colors[ImGuiCol_HeaderHovered];
                ImVec4 headerActive = style.Colors[ImGuiCol_HeaderActive];
                ImVec4 borderV = style.Colors[ImGuiCol_Border];
                ImVec4 bgV = Theme::Colors::FileCardBackground;

                if (hovered) bgV = Theme::Colors::FileCardBackgroundHovered;
                if (isSelected) bgV = header;

                ImU32 bgCol = ImGui::ColorConvertFloat4ToU32(bgV);

                ImVec4 bColV = borderV;
                if (hovered) bColV = headerHovered;
                if (isSelected) bColV = headerActive;
                ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(bColV);

                float rounding = style.FrameRounding;

                // Outer card
                drawList->AddRectFilled(itemMin, itemMax, bgCol, rounding);
                drawList->AddRect(itemMin, itemMax, borderCol, rounding, 0, Theme::dpi(1.0f));

                // Slight inner shadow on top for depth
                ImU32 shadowCol = IM_COL32(0, 0, 0, 40);
                ImVec2 shadowMin(itemMin.x, itemMin.y + Theme::dpi(1.0f));
                ImVec2 shadowMax(itemMax.x, itemMax.y);
                drawList->AddRect(shadowMin, shadowMax, shadowCol, rounding, 0, Theme::dpi(1.0f));

                // Content rect inside padding
                ImVec2 contentMin(itemMin.x + pad, itemMin.y + pad);
                float  contentWidth = itemMax.x - itemMin.x - (pad * 2.0f);

                // --- Name / extension split ---------------------------------
                std::string extensionStr = file.extension;

                // Remove leading '.' for badge text
                std::string extLabel;
                if (!file.isDirectory && !extensionStr.empty()){
                    if (extensionStr[0] == '.')
                        extLabel = extensionStr.substr(1);
                    else
                        extLabel = extensionStr;
                }

                // --- Thumbnail centered -------------------------------------
                float imageX = contentMin.x + (contentWidth - dispW) * 0.5f;
                float imageY = contentMin.y + contentYOffset + (thumbHeight - dispH) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(imageX, imageY));
                if (notInProject) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * 0.4f);
                Widgets::image(fileIconImage, ImVec2(dispW, dispH));
                if (file.type == FileType::PREFAB && isVariantFile(currentPath / file.name)){
                    const float badge = std::max(Theme::dpi(10.0f), std::min(dispW, dispH) * 0.34f);
                    UIUtils::drawPrefabVariantBadge(ImGui::GetWindowDrawList(), ImVec2(imageX + Theme::dpi(3.0f), imageY + dispH - badge - Theme::dpi(2.0f)),
                        ImVec2(imageX + Theme::dpi(3.0f) + badge, imageY + dispH - Theme::dpi(2.0f)), IM_COL32(140, 205, 255, 255));
                }
                drawTextureCookBadge(ImGui::GetWindowDrawList(), file, ImVec2(imageX, imageY), ImVec2(imageX + dispW, imageY + dispH));
                if (file.generated){
                    drawGeneratedBadge(ImGui::GetWindowDrawList(), ImVec2(imageX, imageY + dispH - std::min(dispW, dispH) * 0.32f), std::min(dispW, dispH), file.name, file.generatedStale);
                }

                // --- Extension badge over thumbnail -------------------------
                if (!extLabel.empty()){
                    ImU32 badgeBgCol = fileSeparatorColor(file);
                    ImVec4 badgeBgV = ImGui::ColorConvertU32ToFloat4(badgeBgCol);

                    float luminance = badgeBgV.x * 0.2126f +
                                    badgeBgV.y * 0.7152f +
                                    badgeBgV.z * 0.0722f;

                    ImVec4 txtColV;
                    if (luminance > 0.6f)
                        txtColV = ImVec4(0.10f, 0.10f, 0.10f, 0.98f);   // dark text on bright bg
                    else
                        txtColV = ImVec4(1.00f, 1.00f, 1.00f, 0.98f);   // light text on dark bg

                    ImU32 badgeTextCol = ImGui::ColorConvertFloat4ToU32(txtColV);

                    // Make the extension text smaller (use ImGui::GetFontSize, not font->FontSize)
                    ImFont* font = ImGui::GetFont();
                    float baseSize = ImGui::GetFontSize();
                    float fontScale = 0.80f;                         // 80% of normal
                    float smallSize = baseSize * fontScale;

                    ImVec2 extSize = ImGui::CalcTextSize(extLabel.c_str());
                    extSize.x *= fontScale;
                    extSize.y *= fontScale;

                    float badgePadX = Theme::dpi(4.0f);
                    float badgePadY = Theme::dpi(1.0f);

                    ImVec2 badgeMax(
                        contentMin.x + contentWidth - Theme::dpi(2.0f),
                        contentMin.y + 2.0f + extSize.y + badgePadY * 2.0f
                    );
                    ImVec2 badgeMin(
                        badgeMax.x - extSize.x - badgePadX * 2.0f,
                        badgeMax.y - extSize.y - badgePadY * 2.0f
                    );

                    ImDrawList* drawList   = ImGui::GetWindowDrawList();
                    float badgeRound = ImGui::GetStyle().FrameRounding * 0.5f;

                    drawList->AddRectFilled(badgeMin, badgeMax, badgeBgCol, badgeRound);

                    ImVec2 textPos(badgeMin.x + badgePadX, badgeMin.y + badgePadY);
                    drawList->AddText(font, smallSize, textPos, badgeTextCol, extLabel.c_str());
                }

                // --- Separator line -----------------------------------------
                ImU32 sepCol = fileSeparatorColor(file);
                float lineY  = contentMin.y + contentYOffset + thumbHeight + pad * 0.5f;
                drawList->AddLine(
                    ImVec2(contentMin.x, lineY),
                    ImVec2(contentMin.x + contentWidth, lineY),
                    sepCol,
                    lineThickness
                );

                // --- Name text, clipped to two lines ------------------------
                float textY = lineY + lineThickness + pad * 0.4f;
                ImVec2 textMin(contentMin.x, textY);
                ImVec2 textMax(contentMin.x + contentWidth, textY + textAreaHeight);
                ImGui::PushClipRect(textMin, textMax, true);
                ImGui::SetCursorScreenPos(textMin);
                ImGui::PushTextWrapPos(textMax.x - ImGui::GetWindowPos().x);
                ImGui::SetWindowFontScale(fontScale);
                ImGui::TextUnformatted(file.displayBaseName.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopTextWrapPos();
                ImGui::PopClipRect();
                if (notInProject) ImGui::PopStyleVar();

                if (hovered){
                    if (file.type == FileType::COLOR_LUT)
                        ImGui::SetTooltip("%s\n%s", file.displayName.c_str(), colorLutTooltip(currentPath / file.name).c_str());
                    else
                        ImGui::SetTooltip("%s", fileTooltip(file).c_str());
                }

            }else{
                // ------------------------------------------------------------
                // Unity tile (or list row): icon, name with ellipsis, blue
                // selection behind the icon and the name.
                // ------------------------------------------------------------
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const float alpha = notInProject ? 0.4f : 1.0f;
                const ImU32 selectionCol = ImGui::GetColorU32(windowFocused ? Theme::Colors::Selection : Theme::Colors::SelectionUnfocused);
                const ImU32 hoverCol = ImGui::GetColorU32(Theme::Colors::IconButtonHovered);
                const ImU32 tintCol = IM_COL32(255, 255, 255, (int)(255 * alpha));
                const float rounding = Theme::dpi(3.0f);

                if (useListView){
                    if (isSelected) drawList->AddRectFilled(itemMin, itemMax, selectionCol);
                    else if (hovered) drawList->AddRectFilled(itemMin, itemMax, hoverCol);
                    const float rowH = itemMax.y - itemMin.y;
                    const float iconExtent = std::min(lineHeight, rowH);
                    float w = iconExtent, h = iconExtent;
                    if (dispW > 0.0f && dispH > 0.0f){
                        float scale = std::min(iconExtent / dispW, iconExtent / dispH);
                        w = dispW * scale;
                        h = dispH * scale;
                    }
                    ImVec2 iconMin(itemMin.x + Theme::dpi(4.0f) + (iconExtent - w) * 0.5f, itemMin.y + (rowH - h) * 0.5f);
                    if (fileIconImage != ImTextureID{}){
                        drawList->AddImage(fileIconImage, iconMin, ImVec2(iconMin.x + w, iconMin.y + h), ImVec2(0, 0), ImVec2(1, 1), tintCol);
                    }
                    if (file.type == FileType::PREFAB && isVariantFile(currentPath / file.name)){
                        const float badge = std::max(Theme::dpi(8.0f), iconExtent * 0.55f);
                        UIUtils::drawPrefabVariantBadge(drawList, ImVec2(iconMin.x, iconMin.y + iconExtent - badge), ImVec2(iconMin.x + badge, iconMin.y + iconExtent),
                            IM_COL32(140, 205, 255, (int)(255 * alpha)));
                    }
                    drawTextureCookBadge(drawList, file, iconMin, ImVec2(iconMin.x + iconExtent, iconMin.y + iconExtent));
                    if (file.generated){
                        drawGeneratedBadge(drawList, ImVec2(iconMin.x, iconMin.y + iconExtent * 0.45f), iconExtent, file.name, file.generatedStale);
                    }
                    ImVec2 textMin(itemMin.x + Theme::dpi(8.0f) + iconExtent, itemMin.y + (rowH - lineHeight) * 0.5f);
                    // version control: its state before the name (S29 R27)
                    textMin.x += drawVcsOverlay(file, ImVec2(textMin.x, textMin.y), lineHeight, true);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_Text, alpha));
                    ImGui::RenderTextEllipsis(drawList, textMin, ImVec2(itemMax.x - Theme::dpi(4.0f), itemMax.y), itemMax.x - Theme::dpi(4.0f),
                                              file.displayName.c_str(), nullptr, nullptr);
                    ImGui::PopStyleColor();
                }else{
                    const ImVec2 iconBoxMin(itemMin.x + (cellWidth - uiIconSize) * 0.5f, itemMin.y);
                    const ImVec2 iconBoxMax(iconBoxMin.x + uiIconSize, iconBoxMin.y + uiIconSize);
                    const float pad = Theme::dpi(2.0f);
                    if (isSelected){
                        ImVec4 tint = windowFocused ? Theme::Colors::Selection : Theme::Colors::SelectionUnfocused;
                        tint.w = 0.55f;
                        drawList->AddRectFilled(ImVec2(iconBoxMin.x - pad, iconBoxMin.y - pad), ImVec2(iconBoxMax.x + pad, iconBoxMax.y + pad),
                                                ImGui::GetColorU32(tint), rounding);
                    }else if (hovered){
                        drawList->AddRectFilled(ImVec2(iconBoxMin.x - pad, iconBoxMin.y - pad), ImVec2(iconBoxMax.x + pad, iconBoxMax.y + pad),
                                                hoverCol, rounding);
                    }

                    // Center the scaled thumbnail inside the icon box
                    ImVec2 imgMin(iconBoxMin.x + (uiIconSize - dispW) * 0.5f, iconBoxMin.y + (uiIconSize - dispH) * 0.5f);
                    if (fileIconImage != ImTextureID{}){
                        drawList->AddImage(fileIconImage, imgMin, ImVec2(imgMin.x + dispW, imgMin.y + dispH), ImVec2(0, 0), ImVec2(1, 1), tintCol);
                    }
                    // Unity's Prefab Variant icon: the prefab's with an arrow
                    if (file.type == FileType::PREFAB && isVariantFile(currentPath / file.name)){
                        const float badge = std::max(Theme::dpi(10.0f), uiIconSize * 0.34f);
                        UIUtils::drawPrefabVariantBadge(drawList, ImVec2(iconBoxMin.x + Theme::dpi(3.0f), iconBoxMax.y - badge - Theme::dpi(2.0f)),
                            ImVec2(iconBoxMin.x + Theme::dpi(3.0f) + badge, iconBoxMax.y - Theme::dpi(2.0f)), IM_COL32(140, 205, 255, (int)(255 * alpha)));
                    }
                    drawTextureCookBadge(drawList, file, iconBoxMin, iconBoxMax);
                    // a Vertex Animation's generated mesh
                    if (file.generated){
                        drawGeneratedBadge(drawList, ImVec2(iconBoxMin.x + Theme::dpi(3.0f), iconBoxMax.y - uiIconSize * 0.32f - Theme::dpi(2.0f)), uiIconSize, file.name, file.generatedStale);
                    }
                    // version control: its state at the thumbnail's bottom left, after a variant badge (S29 R27)
                    {
                        const float glyph = std::max(Theme::dpi(10.0f), uiIconSize * 0.28f);
                        const bool variant = file.type == FileType::PREFAB && isVariantFile(currentPath / file.name);
                        const float x = iconBoxMin.x + Theme::dpi(3.0f) + (variant ? std::max(Theme::dpi(10.0f), uiIconSize * 0.34f) + Theme::dpi(2.0f) : 0.0f);
                        drawVcsOverlay(file, ImVec2(x, iconBoxMax.y - glyph - Theme::dpi(2.0f)), glyph, false);
                    }

                    // One line, centred when it fits, cut with an ellipsis when not
                    const char* label = file.displayBaseName.c_str();
                    const ImVec2 labelSize = ImGui::CalcTextSize(label);
                    const float nameTop = iconBoxMax.y + iconNameGap;
                    const float maxTextW = cellWidth - Theme::dpi(8.0f);
                    const bool fits = labelSize.x <= maxTextW;
                    const float textW = fits ? labelSize.x : maxTextW;
                    const float textX = itemMin.x + (cellWidth - textW) * 0.5f;
                    if (isSelected){
                        drawList->AddRectFilled(ImVec2(textX - Theme::dpi(3.0f), nameTop), ImVec2(textX + textW + Theme::dpi(3.0f), nameTop + nameHeight),
                                                selectionCol, rounding);
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_Text, alpha));
                    ImGui::RenderTextEllipsis(drawList, ImVec2(textX, nameTop + namePadY), ImVec2(textX + textW, nameTop + nameHeight),
                                              textX + textW, label, nullptr, &labelSize);
                    ImGui::PopStyleColor();
                }

                if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)){
                    if (file.type == FileType::COLOR_LUT)
                        ImGui::SetTooltip("%s\n%s", file.displayName.c_str(), colorLutTooltip(currentPath / file.name).c_str());
                    else
                        ImGui::SetTooltip("%s", fileTooltip(file).c_str());
                }
            }

            // its asset labels: dots on tiles, chips on list rows (S25 R23)
            drawItemLabels(file, itemMin, itemMax, useListView);
            // UI tests: where each file is (UIProbe; the tree and grid have no other names)
            UIProbe::rect("project.file." + file.name, itemMin.x, itemMin.y, itemMax.x, itemMax.y);

            // --- Selection behavior (unified) ------------------------------
            if (itemPressed && !doubleClickedItem){
                clickedInFile = true;

                if (ctrlPressed){
                    if (isSelected)
                        selectedFiles.erase(file.name);
                    else
                        selectedFiles.insert(file.name);
                    lastSelectedFile = file.name;
                }else if (shiftPressed){
                    if (!lastSelectedFile.empty()){
                        auto itStart = std::find_if(files.begin(), files.end(),
                            [&](const FileEntry& entry) { return entry.name == lastSelectedFile; });
                        auto itEnd = std::find_if(files.begin(), files.end(),
                            [&](const FileEntry& entry) { return entry.name == file.name; });

                        if (itStart != files.end() && itEnd != files.end()){
                            if (itStart > itEnd) std::swap(itStart, itEnd);
                            for (auto it = itStart; it <= itEnd; ++it){
                                if (showDirectories || !it->isDirectory)
                                    selectedFiles.insert(it->name);
                            }
                        }
                    }else{
                        selectedFiles.insert(file.name);
                    }
                }else{
                    selectedFiles.clear();
                    selectedFiles.insert(file.name);
                    lastSelectedFile = file.name;
                }
                updateInspectedAsset(file.name);
            }

            // --- Right-click context menu on the item rect -----------------
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsMouseHoveringRect(itemMin, itemMax, true)){
                clickedInFile = true;
                if (!ctrlPressed && !isSelected){
                    selectedFiles.clear();
                }
                selectedFiles.insert(file.name);
                lastSelectedFile = file.name;
                ImGui::OpenPopup("FileContextMenu");
            }

            if (ImGui::BeginPopup("FileContextMenu")){
                // Unity's right-click menu starts with Create: into a folder, beside a file
                {
                    const fs::path entryPath = currentPath / file.name;
                    const fs::path into = file.isDirectory ? entryPath : entryPath.parent_path();
                    const bool createOpen = ImGui::BeginMenu(ICON_FA_PLUS " Create");
                    uiBuilding.contextMenu.push_back({"Create", lastItemRect()});
                    if (createOpen) {
                        renderCreateItems(into);
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                }
                if (file.type == FileType::MATERIAL || Util::isParticleEffectFile(file.name)){
                    // a double-click shows these in the Inspector: their text stays reachable
                    const bool openText = ImGui::MenuItem(ICON_FA_FILE_CODE " Open as Text");
                    UIProbe::item("project.menu.open_as_text");
                    if (openText) {
                        codeEditor->openFile((currentPath / file.name).string(), true);
                    }
                    ImGui::Separator();
                }
                if (!file.isDirectory && Util::isAudioFile(file.name) && hasAudioSelection()) {
                    if (ImGui::MenuItem(ICON_FA_MUSIC " Create Sound Event from Clips")) {
                        assetsCreateSoundEventFromClips();
                    }
                    // the clip's Inspector shows the grid: inspect it, then detect
                    if (ImGui::MenuItem(ICON_FA_DRUM " Detect Tempo")) {
                        selectedFiles.clear();
                        selectedFiles.insert(file.name);
                        updateInspectedAsset(file.name);
                        const CommandDef* detect = CommandCatalog::editor().find("audioClip.detectTempo");
                        if (detect && detect->run && (!detect->enabled || detect->enabled())) detect->run(CommandArgs{});
                    }
                    ImGui::Separator();
                }
                if (file.type == FileType::SCENE){
                    if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS " Open (Add)")) {
                        project->openScene(currentPath / file.name, false);
                    }
                    ImGui::Separator();
                }
                if (!file.isDirectory && Util::isPrefabFile(file.name)){
                    // Unity's Open Prefab: Prefab Mode in isolation; the YAML stays reachable
                    if (ImGui::MenuItem(ICON_FA_CUBE " Open Prefab")) {
                        editor::getEditorHost().openPrefabAsset(currentPath / file.name);
                    }
                    if (ImGui::MenuItem(ICON_FA_FILE_CODE " Open as Text")) {
                        codeEditor->openFile((currentPath / file.name).string(), true);
                    }
                    // Unity's Create > Prefab Variant on a prefab: a variant of it next to it
                    if (ImGui::BeginMenu(ICON_FA_PLUS " Create")) {
                        if (ImGui::MenuItem(ICON_FA_CLONE " Prefab Variant")) {
                            const fs::path base = project->normalizeToProjectRelative(currentPath / file.name);
                            CreatePrefabVariantAssetCmd* command = new CreatePrefabVariantAssetCmd(project, base, fs::path());
                            const fs::path created = command->getPath();
                            project->getProjectCommandHistory()->addCommandNoMerge(command);
                            scanDirectory(currentPath);
                            selectedFiles.clear();
                            selectedFiles.insert(created.filename().string());
                            lastSelectedFile = created.filename().string();
                        }
                        ImGui::SetItemTooltip("A new Prefab Variant of this Prefab: an instance of it as its root, with overrides of its own");
                        ImGui::EndMenu();
                    }
                    const fs::path base = project->getPrefabFileBase(currentPath / file.name);
                    if (!base.empty()) {
                        if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Select Base")) {
                            revealAsset(project->getProjectPath() / base);
                        }
                        ImGui::SetItemTooltip("This Prefab Variant's base: %s", base.generic_string().c_str());
                    }
                    ImGui::Separator();
                }

                // textures: cook again (S14 R23); the selected ones, or every texture
                if (file.type == FileType::IMAGE && TextureCookQueue::get().getProject() == project) {
                    const bool reimport = ImGui::MenuItem(ICON_FA_ARROWS_ROTATE " Reimport");
                    uiBuilding.contextMenu.push_back({"Reimport", lastItemRect()});
                    ImGui::SetItemTooltip("Cooks the selected textures again for the desktop (the cache answers when nothing changed)");
                    const bool reimportAll = ImGui::MenuItem(ICON_FA_ARROWS_ROTATE " Reimport All Textures");
                    uiBuilding.contextMenu.push_back({"Reimport All Textures", lastItemRect()});
                    if (reimport) {
                        std::vector<fs::path> chosen = getSelectedFiles();
                        if (std::find(chosen.begin(), chosen.end(), currentPath / file.name) == chosen.end()) chosen = {currentPath / file.name};
                        for (const fs::path& path : chosen) {
                            if (!project->isInsideAssetsPath(path)) continue;
                            const std::string assetPath = project->normalizeToAssetsRelative(path).generic_string();
                            TextureCookQueue::get().invalidate(assetPath);
                            TextureCookQueue::get().request(assetPath, TextureFamily::Desktop, TextureCookQueue::Selected);
                        }
                    }
                    if (reimportAll) {
                        TextureCookQueue::get().invalidateAll();
                        TextureCookQueue::get().requestAll({TextureFamily::Desktop});
                    }
                    ImGui::Separator();
                }

                if (!file.isDirectory && Util::isImageFile(file.name) && fs::path(file.name).extension() != ".svg"){
                    // Texture synthesis: this image as the example, or its recipe restored
                    const fs::path imagePath = currentPath / file.name;
                    if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES " Synthesize Texture...")) {
                        editor::getEditorHost().openTextureSynthesis(imagePath, false);
                    }
                    ImGui::SetItemTooltip("Texture Synthesis with this image as the example (its normal, roughness... maps next to it come along)");
                    // read once per popup, not every frame
                    static fs::path recipeProbePath;
                    static fs::file_time_type recipeProbeTime;
                    static bool recipeProbeFound = false;
                    std::error_code probeError;
                    const fs::file_time_type imageTime = fs::last_write_time(imagePath, probeError);
                    if (recipeProbePath != imagePath || recipeProbeTime != imageTime) {
                        recipeProbePath = imagePath;
                        recipeProbeTime = imageTime;
                        std::string extension = fs::path(file.name).extension().string();
                        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        recipeProbeFound = extension == ".png" && texsynth::readRecipeTextHead(imagePath).has_value();
                    }
                    if (recipeProbeFound) {
                        if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES " Edit Synthesis...")) {
                            editor::getEditorHost().openTextureSynthesis(imagePath, true);
                        }
                        ImGui::SetItemTooltip("Reopen the settings this texture was synthesized with (Regenerate reproduces it)");
                    }
                    drawSpriteNormalsMenuItems(file, imagePath);
                    ImGui::Separator();
                }

                // Vertex Animation: bake a model or a clip, import a Houdini / OpenVAT export, open a
                // .vat's recipe (the baker window's hook; hidden until it is set)
                if (!file.isDirectory && vatAppHooks().openBaker){
                    const fs::path filePath = currentPath / file.name;
                    bool shown = false;
                    if (file.type == FileType::VERTEX_ANIMATION){
                        const bool open = ImGui::MenuItem(ICON_FA_GEARS " Open in Vertex Animation Baker");
                        uiBuilding.contextMenu.push_back({"Open in Vertex Animation Baker", lastItemRect()});
                        ImGui::SetItemTooltip("The baker with this animation's recipe (Rebake from there)");
                        if (open) {
                            vatAppHooks().openBaker(project, filePath, VatBakerSource::VatAsset);
                        }
                        shown = true;
                    }
                    if (vat::isVatBakeSource(filePath)){
                        const bool bake = ImGui::MenuItem(ICON_FA_PEOPLE_GROUP " Bake Vertex Animation...");
                        uiBuilding.contextMenu.push_back({"Bake Vertex Animation...", lastItemRect()});
                        ImGui::SetItemTooltip("Bake its animation into a Vertex Animation (.vat): crowds that play on the GPU");
                        if (bake) {
                            vatAppHooks().openBaker(project, filePath, VatBakerSource::Bake);
                        }
                        shown = true;
                    }
                    if (vat::isVatImportSource(filePath)){
                        const bool import = ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import as Vertex Animation...");
                        uiBuilding.contextMenu.push_back({"Import as Vertex Animation...", lastItemRect()});
                        ImGui::SetItemTooltip("Re-pack a Houdini (SideFX Labs VAT 3) or OpenVAT export into a .vat");
                        if (import) {
                            vatAppHooks().openBaker(project, filePath, VatBakerSource::Import);
                        }
                        shown = true;
                    }
                    if (shown) ImGui::Separator();
                }

                // a generic .anim as a humanoid clip any humanoid plays (anim-retarget 9; a model's
                // clips: from its Inspector or a player entry)
                if (!file.isDirectory && file.type == FileType::ANIMATION_CLIP){
                    const bool convert = ImGui::MenuItem(ICON_FA_PERSON " Convert to Humanoid Clip...");
                    uiBuilding.contextMenu.push_back({"Convert to Humanoid Clip...", lastItemRect()});
                    ImGui::SetItemTooltip("A humanoid clip (rotations relative to the neutral pose) that plays on any humanoid character");
                    if (convert) {
                        HumanoidConvertDialog::open(project, FileUtils::pathToGenericUtf8((currentPath / file.name).lexically_normal()));
                    }
                    ImGui::Separator();
                }

                // a colour LUT: what its double-click does, for the open scene (S13)
                if (file.type == FileType::COLOR_LUT) {
                    const bool assign = ImGui::MenuItem(ICON_FA_PALETTE " Use as Color Grading LUT");
                    uiBuilding.contextMenu.push_back({"Use as Color Grading LUT", lastItemRect()});
                    ImGui::SetItemTooltip("The open scene's Post-processing > Color Grading > LUT Texture (one undo step)");
                    if (assign) {
                        assignColorLut(currentPath / file.name);
                    }
                    ImGui::Separator();
                }

                if (!file.isDirectory) {
                    const Guid guid = project->getAssetDatabase().getAssetGuid(currentPath / file.name);
                    const bool copyGuid = ImGui::MenuItem(ICON_FA_FINGERPRINT " Copy GUID", nullptr, false, !guid.isNull());
                    uiBuilding.contextMenu.push_back({"Copy GUID", lastItemRect()});
                    if (copyGuid) {
                        ImGui::SetClipboardText(guid.str().c_str());
                    }
                    const bool findReferences = ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Find References", nullptr, false, !guid.isNull());
                    uiBuilding.contextMenu.push_back({"Find References", lastItemRect()});
                    if (findReferences) {
                        if (AssetReferencesWindow* references = Backend::getApp().getAssetReferencesWindow()) {
                            references->findReferences((currentPath / file.name).string());
                        }
                    }
                    ImGui::Separator();
                }

                if (!file.isDirectory && file.type == FileType::AUDIO){
                    if (ImGui::MenuItem(ICON_FA_ROTATE " Reanalyze")) {
                        std::vector<fs::path> chosen = getSelectedFiles();
                        if (std::find(chosen.begin(), chosen.end(), currentPath / file.name) == chosen.end()) chosen = {currentPath / file.name};
                        assetsReanalyzeAudio(chosen);
                    }
                    ImGui::SetItemTooltip("Drop the cached measurements of the selected clips: they are measured again");
                    ImGui::Separator();
                }

                if (!file.isDirectory && FbxImporter::isFbxFile(file.name)){
                    if (ImGui::MenuItem(ICON_FA_ROTATE " Reimport")) {
                        project->importFbxModel(currentPath / file.name, true);
                        if (ModelCookQueue::get().getProject() == project && project->isInsideAssetsPath(currentPath / file.name)) {
                            const std::string assetPath = project->normalizeToAssetsRelative(currentPath / file.name).generic_string();
                            ModelCookQueue::get().invalidate(assetPath);
                            ModelCookQueue::get().request(assetPath, ModelFamily::Desktop, ModelCookQueue::Selected);
                        }
                    }
                    ImGui::SetItemTooltip("Convert the FBX into its .glb again and cook it (it is reimported by itself when it or its textures change)");
                    ImGui::Separator();
                }else if (!file.isDirectory && isModelSource(file.name) && ModelCookQueue::get().getProject() == project){
                    // models: cook again (S53 R13); the selected ones, or every model
                    const bool reimport = ImGui::MenuItem(ICON_FA_ARROWS_ROTATE " Reimport");
                    uiBuilding.contextMenu.push_back({"Reimport", lastItemRect()});
                    ImGui::SetItemTooltip("Cooks the selected models again for the desktop (the cache answers when nothing changed)");
                    const bool reimportAll = ImGui::MenuItem(ICON_FA_ARROWS_ROTATE " Reimport All Models");
                    uiBuilding.contextMenu.push_back({"Reimport All Models", lastItemRect()});
                    if (reimport) {
                        std::vector<fs::path> chosen = getSelectedFiles();
                        if (std::find(chosen.begin(), chosen.end(), currentPath / file.name) == chosen.end()) chosen = {currentPath / file.name};
                        for (const fs::path& path : chosen) {
                            if (!project->isInsideAssetsPath(path) || !isModelSource(path.filename().string())) continue;
                            const std::string assetPath = project->normalizeToAssetsRelative(path).generic_string();
                            ModelCookQueue::get().invalidate(assetPath);
                            ModelCookQueue::get().request(assetPath, ModelFamily::Desktop, ModelCookQueue::Selected);
                        }
                    }
                    if (reimportAll) {
                        ModelCookQueue::get().invalidateAll();
                        ModelCookQueue::get().requestAll({ModelFamily::Desktop});
                    }
                    ImGui::Separator();
                }

                {
                    auto keys = [](const char* id) { return CommandCatalog::editor().menuShortcutText(id); };
                    const std::string copyKeys = keys("project.copy"), cutKeys = keys("project.cut"),
                                      pasteKeys = keys("project.paste"), duplicateKeys = keys("project.duplicate");
                    const bool copyItem = ImGui::MenuItem(ICON_FA_COPY " Copy", copyKeys.empty() ? nullptr : copyKeys.c_str());
                    uiBuilding.contextMenu.push_back({"Copy", lastItemRect()});
                    const bool cutItem = ImGui::MenuItem(ICON_FA_SCISSORS " Cut", cutKeys.empty() ? nullptr : cutKeys.c_str());
                    uiBuilding.contextMenu.push_back({"Cut", lastItemRect()});
                    const bool pasteItem = ImGui::MenuItem(ICON_FA_PASTE " Paste", pasteKeys.empty() ? nullptr : pasteKeys.c_str(), false, !clipboard.empty());
                    uiBuilding.contextMenu.push_back({"Paste", lastItemRect()});
                    const bool duplicateItem = ImGui::MenuItem(ICON_FA_CLONE " Duplicate", duplicateKeys.empty() ? nullptr : duplicateKeys.c_str());
                    uiBuilding.contextMenu.push_back({"Duplicate", lastItemRect()});
                    if (copyItem) copySelectedFiles(false);
                    if (cutItem) copySelectedFiles(true);
                    if (pasteItem) {
                        // into a folder, beside a file (D1); scanDirectory waits for the loop's end
                        const fs::path clicked = currentPath / file.name;
                        pasteFiles(AssetClipboard::pasteTarget(listingFolder(), &clicked, file.isDirectory));
                    }
                    if (duplicateItem) duplicateSelection();
                }

                ImGui::Separator();

                renderLabelsMenu();
                if (ImGui::MenuItem(ICON_FA_BOOKMARK " Add to Bookmarks")) addSelectionToBookmarks();
                ImGui::Separator();
                const std::string deleteKeys = CommandCatalog::editor().menuShortcutText("project.delete");
                const bool deleteItem = ImGui::MenuItem(ICON_FA_TRASH " Delete", deleteKeys.empty() ? nullptr : deleteKeys.c_str());
                uiBuilding.contextMenu.push_back({"Delete", lastItemRect()});
                if (deleteItem) showDeleteConfirmation = true;
                const std::string renameKeys = CommandCatalog::editor().menuShortcutText("project.rename");
                const bool rename = ImGui::MenuItem(ICON_FA_I_CURSOR " Rename", renameKeys.empty() ? nullptr : renameKeys.c_str());
                uiBuilding.contextMenu.push_back({"Rename", lastItemRect()});
                if (rename){
                    startRename(file.name);
                    ImGui::CloseCurrentPopup();
                }

                ImGui::Separator();

                if (ImGui::MenuItem(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE " Open in File Manager")){
                    openInFileManager(file.isDirectory ? currentPath / file.name : currentPath);
                }

                // S29: Commit..., Revert..., Lock, Unlock, Show History, Diff Against Baseline, Refresh
                {
                    std::vector<fs::path> selection = getSelectedFiles();
                    if (selection.empty()) selection.push_back(currentPath / file.name);
                    Backend::getApp().getVcsUi()->drawContextMenu(selection);
                }

                // the project's editor scripts: Assets/... items (S23)
                {
                    EditorMenuRunContext scriptContext;
                    scriptContext.sceneId = project->getSelectedSceneId();
                    for (const fs::path& selected : getSelectedFiles()) {
                        scriptContext.assets.push_back(selected.lexically_relative(project->getProjectPath()).generic_string());
                    }
                    if (scriptContext.assets.empty()) {
                        scriptContext.assets.push_back((currentPath / file.name).lexically_relative(project->getProjectPath()).generic_string());
                    }
                    EditorScripts::get().drawContextItems("assets", {}, scriptContext);
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();

            if (deferredDirectoryChange){
                listingIterating = false;
                scanDirectory(currentPath / file.name);
                selectedFiles.clear();
                break;
            }

            // Rebuilds files, so the entry this iteration holds is gone from here on
            if (!dropTarget.empty()){
                listingIterating = false;
                handleInternalDragAndDrop(dropTarget, dropSources);
                break;
            }
        }
        listingIterating = false;

        ImGui::EndTable();
    }
    // what an item's menu asked for meanwhile
    if (deferredScan) {
        const fs::path path = *deferredScan;
        deferredScan.reset();
        scanDirectory(path);
    }
    if (afterListing) {
        std::function<void()> action = std::move(afterListing);
        afterListing = nullptr;
        action();
    }
    if (!searchText.empty() && searchMoreResults > 0) {
        ImGui::TextDisabled("%zu more results: narrow the search", searchMoreResults);
    } else if (!searchText.empty() && files.empty() && searchError.empty()) {
        ImGui::TextDisabled("Nothing matches the search.");
    }

    ImGui::PopStyleVar();

    // --- Right-click on empty space ----------------------------------------
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered() && !clickedInFile){
        ImGui::OpenPopup("ResourcesContextMenu");
    }

    if (ImGui::BeginPopup("ResourcesContextMenu")){
        const fs::path folder = listingFolder();
        const bool createOpen = ImGui::BeginMenu(ICON_FA_PLUS " Create");
        uiBuilding.contextMenu.push_back({"Create", lastItemRect()});
        if (createOpen) {
            renderCreateItems(folder);
            ImGui::EndMenu();
        }

        ImGui::Separator();

        const std::string pasteKeys = CommandCatalog::editor().menuShortcutText("project.paste");
        const bool paste = ImGui::MenuItem(ICON_FA_PASTE " Paste", pasteKeys.empty() ? nullptr : pasteKeys.c_str(), false, !clipboard.empty());
        uiBuilding.contextMenu.push_back({"Paste", lastItemRect()});
        if (paste){
            pasteFiles(folder);
        }

        ImGui::Separator();

        if (ImGui::MenuItem(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE " Open in File Manager")){
            openInFileManager(folder);
        }

        ImGui::EndPopup();
    }

    // --- Empty click: the marquee starts, the selection clears -------------
    // After the items, so a press that comes with the pointer move still sees the file under it
    // (Doriax 9050bbaca); Ctrl or Shift held: the marquee adds to the selection. A press on an
    // item leaves the selection to the item (its release selects; a drag takes the selection)
    if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered() && !clickedInFile && !ImGui::IsAnyItemHovered() &&
        !ImGui::GetDragDropPayload()){
        isDragging = true;
        dragStart = ImGui::GetMousePos();
        dragStart.x -= scrollRegionMin.x;
        dragStart.y -= scrollRegionMin.y;
        dragStart.x += ImGui::GetScrollX();
        dragStart.y += ImGui::GetScrollY();
        dragEnd = dragStart;
        if (!ctrlPressed && !shiftPressed){
            selectedFiles.clear();
            updateInspectedAsset("");
        }
    }

    // --- Draw marquee rectangle --------------------------------------------
    if (isDragging){
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImVec2 rectMin(
            scrollRegionMin.x + std::min(dragStart.x, dragEnd.x) - ImGui::GetScrollX(),
            scrollRegionMin.y + std::min(dragStart.y, dragEnd.y) - ImGui::GetScrollY()
        );
        ImVec2 rectMax(
            scrollRegionMin.x + std::max(dragStart.x, dragEnd.x) - ImGui::GetScrollX(),
            scrollRegionMin.y + std::max(dragStart.y, dragEnd.y) - ImGui::GetScrollY()
        );

        rectMin.x = ImClamp(rectMin.x, scrollRegionMin.x, scrollRegionMax.x);
        rectMin.y = ImClamp(rectMin.y, scrollRegionMin.y, scrollRegionMax.y);
        rectMax.x = ImClamp(rectMax.x, scrollRegionMin.x, scrollRegionMax.x);
        rectMax.y = ImClamp(rectMax.y, scrollRegionMin.y, scrollRegionMax.y);

        drawList->AddRect(rectMin, rectMax, IM_COL32(100, 150, 255, 255));
        drawList->AddRectFilled(rectMin, rectMax, IM_COL32(100, 150, 255, 50));
    }

    // --- Create Prefab or Variant? (a Prefab instance dropped in) ----------
    showPrefabOrVariantPopup();

    // --- Delete confirmation modal -----------------------------------------
    if (showDeleteConfirmation){
        ImGui::OpenPopup("Delete Confirmation");
    }

    drawPlaceholderSoundDialog();

    if (EditorModal::begin("Delete Confirmation", NULL, ImGuiWindowFlags_AlwaysAutoResize)){
        // the assets or Lua directory goes only through Project Settings (D10): nothing is deleted
        std::string refused;
        for (const auto& fileName : selectedFiles){
            refused = rootRefusal(currentPath / fileName);
            if (!refused.empty()) break;
        }
        if (!refused.empty()){
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.0f), "%s", refused.c_str());
            EditorModal::message(refused);
            ImGui::Separator();
            if (EditorModal::button("OK", "ok", ImVec2(120.0f, 0))){
                showDeleteConfirmation = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return;
        }
        ImGui::Text("Are you sure you want to delete the following items?");
        ImGui::Separator();
        int fileCount = 0;
        std::string listedNames;
        // a Vertex Animation's .vat and .vat.glb go together: both are listed (one question)
        std::vector<fs::path> listed;
        for (const auto& fileName : selectedFiles){
            listed.push_back(currentPath / fileName);
        }
        const std::vector<fs::path> withPairs = vat::withPairs(listed);
        for (size_t i = 0; i < withPairs.size(); i++){
            if (fileCount < 10){
                const std::string name = withPairs[i].lexically_relative(currentPath).generic_string();
                listedNames += (listedNames.empty() ? "" : ", ") + name;
                if (i < listed.size()){
                    ImGui::BulletText("%s", name.c_str());
                }else{
                    ImGui::BulletText("%s (its Vertex Animation pair)", name.c_str());
                }
            }
            fileCount++;
        }
        EditorModal::message("Are you sure you want to delete the following items? " + listedNames +
                             (fileCount > 10 ? " and " + std::to_string(fileCount - 10) + " more" : std::string()));
        if (fileCount > 10){
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "And %d more items...", fileCount - 10);
        }

        ImGui::Separator();
        const float buttonWidth = 120.0f;
        const float buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float totalWidth = (buttonWidth * 2) + buttonSpacing;
        float windowWidth = ImGui::GetWindowSize().x;
        ImGui::SetCursorPosX((windowWidth - totalWidth) / 2.0f);

        if (EditorModal::button("Yes", "yes", ImVec2(buttonWidth, 0))){
            std::vector<fs::path> pathsToDelete;
            for (const auto& fileName : selectedFiles){
                fs::path filePath = currentPath / fileName;
                pathsToDelete.push_back(filePath);
                codeEditor->closeFile(filePath.string());
                imageViewerWindow->closeFile(filePath);
            }
            pathsToDelete = vat::withPairs(pathsToDelete);

            project->getProjectCommandHistory()->addCommand(new DeleteFileCmd(project, pathsToDelete, project->getProjectPath()));
            selectedFiles.clear();
            scanDirectory(currentPath);
            showDeleteConfirmation = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (EditorModal::button("No", "no", ImVec2(buttonWidth, 0))){
            showDeleteConfirmation = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// Subdirectories the tree can render under path, hidden ones already filtered out.
const std::vector<fs::path>& editor::ProjectWindow::treeSubdirectories(const fs::path& path) {
    const double now = ImGui::GetTime();
    DirTreeEntry& cached = dirTreeCache[path.string()];

    // An invalidation has to take effect at once, so it skips the interval
    const bool invalidated = cached.generation != dirTreeGeneration;
    if (!invalidated && (now - cached.lastCheckTime) < 1.0) {
        return cached.subDirs;
    }
    cached.lastCheckTime = now;

    // Unreadable this tick: keep what we have, the next one picks up the truth
    std::error_code ec;
    const auto writeTime = fs::last_write_time(path, ec);
    if (!invalidated && (ec || writeTime == cached.writeTime)) {
        return cached.subDirs;
    }
    // Marked read even if the listing below fails, so a folder that cannot be opened
    // is retried on the next check instead of on every frame
    cached.generation = dirTreeGeneration;

    std::vector<fs::path> subDirs;
    ec.clear();
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    while (!ec && it != end) {
        const auto entry = *it;
        it.increment(ec);

        // Skip hidden directories (starting with '.')
        if (entry.path().filename().string()[0] == '.') {
            continue;
        }

        std::error_code entryEc;
        if (entry.is_directory(entryEc) && !entryEc) {
            subDirs.push_back(entry.path());
        }
    }

    // A failed or truncated listing keeps the previous children, and leaves writeTime
    // stale so the next check retries instead of the node staying empty for good
    if (!ec) {
        // Alphabetical, ignoring case, like the grid
        std::sort(subDirs.begin(), subDirs.end(), [](const fs::path& a, const fs::path& b) {
            std::string la = a.filename().string(), lb = b.filename().string();
            std::transform(la.begin(), la.end(), la.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(lb.begin(), lb.end(), lb.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return la < lb;
        });
        cached.writeTime = writeTime;
        cached.subDirs = std::move(subDirs);
    }

    return cached.subDirs;
}

void editor::ProjectWindow::renderDirectoryTree(const fs::path& rootPath) {
    std::string rootFullPath = rootPath.string();
    std::string rootDisplayName = rootPath.filename().string();
    if (rootDisplayName.empty()) rootDisplayName = rootPath.string(); // Handle root paths like C:/ or /

    bool isRootSelected = (treeSelectedPath == rootPath);
    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (isRootSelected) rootFlags |= ImGuiTreeNodeFlags_Selected;

    // Check if this directory is in the path to current directory
    bool isInCurrentPath = false;
    if (currentPath.string().find(rootPath.string()) == 0 && rootPath != currentPath) {
        // This directory is a parent of the current directory
        isInCurrentPath = true;
    }

    // For root directory of the project or directories in path to current directory, expand initially
    if (rootPath == project->getProjectPath() || isInCurrentPath) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }

    // Auto-expand the parent of the current directory
    if (rootPath == currentPath.parent_path()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }

    // Use unique ID to prevent ImGui tree node confusion
    ImGui::PushID(rootFullPath.c_str());

    // Same list drives the leaf test and the recursion, so a folder holding only
    // dot-directories no longer gets an expander that opens onto nothing
    const std::vector<fs::path>& subDirs = treeSubdirectories(rootPath);

    // If no subdirectories, mark as leaf node (no arrow)
    if (subDirs.empty()) {
        rootFlags |= ImGuiTreeNodeFlags_Leaf;
    }
    rootFlags |= ImGuiTreeNodeFlags_SpanFullWidth;

    // The label leaves room for the folder icon, drawn after as the same
    // texture the grid uses (Unity shows one folder icon in both panes).
    const float iconSize = ImGui::GetFontSize();
    const float spaceWidth = std::max(1.0f, ImGui::CalcTextSize(" ").x);
    const std::string iconGap(static_cast<size_t>(std::ceil((iconSize + Theme::dpi(6.0f)) / spaceWidth)), ' ');
    const ImVec2 nodePos = ImGui::GetCursorScreenPos();
    bool nodeOpen = ImGui::TreeNodeEx("##node", rootFlags, "%s%s", iconGap.c_str(), rootDisplayName.c_str());
    const bool toggledOpen = ImGui::IsItemToggledOpen();
    if (uiBuilding.tree.size() < 200) {
        UiTreeNode node;
        node.path = project->normalizeToProjectRelative(rootPath).generic_string();
        node.rect = lastItemRect();
        node.arrowX = nodePos.x + ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() * 0.5f;
        node.arrowY = node.rect.y + node.rect.h * 0.5f;
        node.open = nodeOpen;
        uiBuilding.tree.push_back(node);
    }

    ImTextureID folderTex = Backend::getImGuiTexture(folderIcon.getRender());
    if (folderTex != ImTextureID{}) {
        const ImVec2 iconMin(nodePos.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.0f,
                             nodePos.y + (ImGui::GetItemRectSize().y - iconSize) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(folderTex, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize));
    }

    // A click shows the folder in the grid, as in Unity (the arrow only expands)
    if (ImGui::IsItemClicked() && !toggledOpen) {
        treeSelectedPath = rootPath;
        if (rootPath != currentPath || !searchText.empty()) {
            searchText.clear();
            searchBuffer[0] = '\0';
            searchReturnPath.clear();
            scanDirectory(rootPath);
            selectedFiles.clear();
        }
    }

    // Unity's folder menu: Create, Copy, Cut, Paste, Duplicate for this folder
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("FolderTreeContextMenu");
    }
    if (ImGui::BeginPopup("FolderTreeContextMenu")) {
        renderTreeContextMenu(rootPath);
        ImGui::EndPopup();
    }

    // Handle drag and drop target (not a dragged folder, nor below one)
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        std::vector<fs::path> sources = peek && peek->IsDataType("resource_files") ? payloadPaths(peek) : std::vector<fs::path>();
        if (acceptsDrop(rootPath, sources) && ImGui::AcceptDragDropPayload("resource_files")) {
            const fs::path target = rootPath;
            pendingTreeAction = [this, target, sources]() { handleInternalDragAndDrop(target, sources); };
        }
        ImGui::EndDragDropTarget();
    }

    if (nodeOpen) {
        // Recursing inserts into dirTreeCache, which leaves subDirs valid: rehashing
        // an unordered_map does not invalidate references to elements already in it
        for (const fs::path& subDir : subDirs) {
            renderDirectoryTree(subDir);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void editor::ProjectWindow::scanDirectory(const fs::path& path) {
    // the listing loop walks `files`: rebuilt after it (a paste, a create or a navigation from an
    // item's menu)
    if (listingIterating) {
        deferredScan = path;
        return;
    }
    currentPath = path;

    std::error_code ec;
    if (!std::filesystem::is_directory(currentPath, ec) || ec) {
        currentPath = project->getProjectPath();
    }

    // Keep the directory tree highlight in sync with any navigation
    treeSelectedPath = currentPath;

    // Every create, rename, move and delete ends here, so the tree cache only has to
    // be invalidated in this one place
    dirTreeGeneration++;

    refreshProjectFiles();

    requestSort = true;

    files.clear();

    // A search lists matches from this folder and everything below it instead.
    if (!searchText.empty()) {
        collectSearchResults();
        rebuildFileIndex();
        return;
    }

    ec.clear();
    fs::directory_iterator it(currentPath, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    while (!ec && it != end) {
        const auto entry = *it;
        it.increment(ec);

        // Skip hidden files and directories (starting with '.')
        if (entry.path().filename().string()[0] == '.') {
            continue;
        }

        // Skip project.yaml file, and the assets' .meta sidecars (S04: they move with their assets)
        if (entry.path().filename() == "project.yaml" || AssetTypes::isMetaFile(entry.path())) {
            continue;
        }

        files.push_back(makeFileEntry(entry, entry.path().filename().string()));
    }
    rebuildFileIndex();
}

void editor::ProjectWindow::rebuildFileIndex() {
    assert(!listingIterating);
    fileIndex.clear();
    fileIndex.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        fileIndex.emplace(currentPath / files[i].name, i);
    }
}

editor::FileEntry editor::ProjectWindow::makeFileEntry(const fs::directory_entry& entry, const std::string& name) {
    std::error_code entryEc;
    const bool isDirectory = entry.is_directory(entryEc) && !entryEc;
    return makeFileEntry(entry.path(), name, isDirectory);
}

editor::FileEntry editor::ProjectWindow::makeFileEntry(const fs::path& path, const std::string& name, bool isDirectory) {
    FileEntry fileEntry;
    fileEntry.name = name;
    fileEntry.isDirectory = isDirectory;

    const std::string fileName = path.filename().string();
    fileEntry.displayName = BidiText::toVisual(fileName);
    std::string baseName = fileName;
    if (!fileEntry.isDirectory){
        size_t dotPos = baseName.rfind('.');
        if (dotPos != std::string::npos && dotPos > 0)
            baseName = baseName.substr(0, dotPos);
    }
    fileEntry.displayBaseName = BidiText::toVisual(baseName);
    fileEntry.icon = fileEntry.isDirectory ? folderIcon.getRender() : fileIcon.getRender();
    fileEntry.hasThumbnail = false;

    if (!fileEntry.isDirectory) {
        fileEntry.extension = path.extension().string();
        if (Util::isImageFile(fileEntry.extension)){
            fileEntry.type = FileType::IMAGE;
            // a Sprite Normal Map output (its recipe; read again only when the file changed)
            updateSpriteNormalsBadge(fileEntry, path);
            if (!fileEntry.generated) updateProceduralBakeBadge(fileEntry, path);
        }else if (Util::isSceneFile(fileEntry.extension)){
            fileEntry.type = FileType::SCENE;
            fileEntry.icon = sceneIcon.getRender();
        }else if (Util::isMaterialFile(fileEntry.extension)){
            fileEntry.type = FileType::MATERIAL;
        }else if (Util::isPrefabFile(fileEntry.extension)){
            fileEntry.type = FileType::PREFAB;
            fileEntry.icon = entityIcon.getRender();
        }else if (Util::isModelFile(fileEntry.extension)){
            fileEntry.type = FileType::MODEL;
            // a Vertex Animation's mesh (its .vat beside it)
            fileEntry.generated = !vat::existingPairOf(path).empty() && vat::isVatMeshFile(path.generic_string());
        }else if (Util::isVertexAnimationFile(fileEntry.extension)){
            fileEntry.type = FileType::VERTEX_ANIMATION;
            fileEntry.icon = vertexAnimationIcon.getRender();
        }else if (PhysicsMaterialAssets::isPhysicsMaterialFile(fileEntry.extension)){
            fileEntry.type = FileType::PHYSICS_MATERIAL;
            fileEntry.icon = physicsMaterialIcon.getRender();
        }else if (Util::isLuaFile(fileEntry.extension)){
            fileEntry.type = FileType::LUA;
            fileEntry.icon = luaIcon.getRender();
        }else if (Util::isHeaderFile(fileEntry.extension)){
            fileEntry.type = FileType::HEADER;
            fileEntry.icon = headerIcon.getRender();
        }else if (Util::isSourceFile(fileEntry.extension)){
            fileEntry.type = FileType::SOURCE;
            fileEntry.icon = sourceIcon.getRender();
        }else if (Util::isAudioFile(fileEntry.extension)){
            fileEntry.type = FileType::AUDIO;
            fileEntry.icon = audioIcon.getRender();
        }else if (Util::isFontFile(fileEntry.extension)){
            fileEntry.type = FileType::FONT;
            fileEntry.icon = fontIcon.getRender();
        }else if (Util::isHtmlFile(fileEntry.extension)){
            fileEntry.type = FileType::HTML;
            fileEntry.icon = htmlIcon.getRender();
        }else if (Util::isCssFile(fileEntry.extension)){
            fileEntry.type = FileType::CSS;
            fileEntry.icon = cssIcon.getRender();
        }else if (Util::isColorLutFile(fileEntry.extension)){
            fileEntry.type = FileType::COLOR_LUT;
            fileEntry.icon = lutIcon.getRender();
        }else if (Util::isPoseAnimationFile(fileEntry.extension)){
            fileEntry.type = FileType::POSE_ANIMATION;
            fileEntry.icon = poseAnimationIcon.getRender();
        }else if (Util::isAnimationClipFile(fileEntry.extension)){
            fileEntry.type = FileType::ANIMATION_CLIP;
            fileEntry.icon = animationClipIcon.getRender();
        }else if (fileEntry.extension == ".humanoid"){
            fileEntry.type = FileType::HUMANOID;
        }else if (Util::isAnimatorAssetFile(fileEntry.extension)){
            fileEntry.type = FileType::ANIMATOR_ASSET;
            fileEntry.icon = Util::isAnimatorControllerFile(fileEntry.extension) ? animatorControllerIcon.getRender()
                           : Util::isAvatarMaskFile(fileEntry.extension) ? avatarMaskIcon.getRender() : montageIcon.getRender();
        }

        if (fileEntry.type == FileType::IMAGE || fileEntry.type == FileType::MATERIAL || fileEntry.type == FileType::MODEL ||
            fileEntry.type == FileType::PREFAB || fileEntry.type == FileType::POSE_ANIMATION || fileEntry.type == FileType::AUDIO) {
            queueThumbnailGeneration(path, fileEntry.type);
        }
        // its asset labels (S25)
        const Guid guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(path));
        if (!guid.isNull()) fileEntry.labels = project->getAssetLabels().labelsOf(guid);
    } else {
        fileEntry.extension = "";
    }

    return fileEntry;
}

void editor::ProjectWindow::sortWithSortSpecs(ImGuiTableSortSpecs* sortSpecs, std::vector<FileEntry>& files) {
    if (!sortSpecs || sortSpecs->SpecsCount == 0) {
        // Default behavior: directories first, then by name ignoring case (Unity order)
        std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
            if (a.isDirectory != b.isDirectory) {
                return a.isDirectory; // Directories come first
            }
            const size_t n = std::min(a.name.size(), b.name.size());
            for (size_t i = 0; i < n; i++) {
                const int ca = std::tolower(static_cast<unsigned char>(a.name[i]));
                const int cb = std::tolower(static_cast<unsigned char>(b.name[i]));
                if (ca != cb) return ca < cb;
            }
            if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
            return a.name < b.name;
        });
        return;
    }

    auto comparator = [&](const FileEntry& a, const FileEntry& b) -> bool {
        // Always sort directories first
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory; // Directories come first
        }

        for (int i = 0; i < sortSpecs->SpecsCount; i++) {
            const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[i];
            bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

            switch (spec.ColumnIndex) {
                case 0: // Column 0: "Name"
                    if (a.name != b.name) {
                        return ascending ? (a.name < b.name) : (a.name > b.name);
                    }
                    break;

                case 1: // Column 1: "Extension"
                    if (a.extension != b.extension) {
                        return ascending ? (a.extension < b.extension) : (a.extension > b.extension);
                    }
                    break;

                default:
                    return false;
            }
        }

        return false;
    };

    std::sort(files.begin(), files.end(), comparator);
}

void editor::ProjectWindow::highlightDragAndDrop(){
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImVec2 maxPos = ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y);

    ImGui::GetWindowDrawList()->AddRect(
        windowPos,
        maxPos,
        ImGui::GetColorU32(ImGuiCol_DragDropTarget),
        0.0f, 0, 2.0f);
}

std::vector<fs::path> editor::ProjectWindow::payloadPaths(const ImGuiPayload* payload) {
    std::vector<fs::path> paths;
    if (!payload || !payload->Data || payload->DataSize <= 0) return paths;
    const char* data = static_cast<const char*>(payload->Data);
    const char* end = data + payload->DataSize;
    while (data < end) {
        const char* stop = std::find(data, end, '\0');
        if (stop > data) paths.push_back(fs::path(std::string(data, stop)).lexically_normal());
        data = stop + 1;
    }
    return paths;
}

bool editor::ProjectWindow::acceptsDrop(const fs::path& targetDirectory, const std::vector<fs::path>& sources) const {
    for (const fs::path& source : sources) {
        if (!AssetOps::moveRefusal(source, targetDirectory / source.filename()).empty() || !rootRefusal(source).empty()) {
            return false;
        }
    }
    return true;
}

void editor::ProjectWindow::handleInternalDragAndDrop(const fs::path& targetDirectory, const std::vector<fs::path>& sources) {
    if (sources.empty()) return;
    std::vector<std::string> paths;
    for (const fs::path& source : sources) paths.push_back(source.string());
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* command = new CopyFileCmd(project, paths, targetDirectory.string(), false);
    command->reportTo(report);
    if (!project->getProjectCommandHistory()->addCommand(command) && !report->error.empty()) {
        Backend::getApp().registerAlert("Move", report->error);
    }
    selectedFiles.clear();
    scanDirectory(currentPath);
}

void editor::ProjectWindow::startRename(const std::string& fileName){
    // the assets or Lua directory is renamed through Project Settings (D10)
    if (const std::string refused = rootRefusal(currentPath / fileName); !refused.empty()) {
        Backend::getApp().registerAlert("Rename", refused);
        return;
    }
    isRenaming = true;
    renameSelectPending = true;
    fileBeingRenamed = fileName;
    strncpy(nameBuffer, fileName.c_str(), sizeof(nameBuffer) - 1);
    nameBuffer[sizeof(nameBuffer) - 1] = '\0';
}

void editor::ProjectWindow::handleRename(){
    // Handle rename popup
    if (isRenaming) {
        ImGui::OpenPopup("Rename File");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }

    if (EditorModal::begin("Rename File", &isRenaming, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename '%s' to:", fileBeingRenamed.c_str());

        // Auto-focus the input field when the popup opens
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-1);

        // On the first frame, preselect the file name without its extension.
        auto selectNameCallback = [](ImGuiInputTextCallbackData* data) -> int {
            bool& pending = *static_cast<bool*>(data->UserData);
            if (!pending) return 0;

            std::string_view name(data->Buf, data->BufTextLen);
            size_t dotPos = name.find_last_of('.');
            // A leading dot means a hidden file with no extension: select everything.
            int selEnd = (dotPos != std::string_view::npos && dotPos > 0) ? (int)dotPos : data->BufTextLen;

            data->CursorPos = selEnd;
            data->SelectionStart = 0;
            data->SelectionEnd = selEnd;
            pending = false;
            return 0;
        };

        bool enterPressed = EditorModal::inputText("##rename", "name", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways, selectNameCallback, &renameSelectPending);
        EditorModal::message("Rename '" + fileBeingRenamed + "' to: \"" + nameBuffer + "\"");

        // Calculate total width of the buttons
        float buttonWidth = 120.0f;
        float buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
        float totalWidth = (buttonWidth * 2) + buttonSpacing;

        // Center the buttons
        float windowWidth = ImGui::GetWindowSize().x;
        ImGui::SetCursorPosX((windowWidth - totalWidth) * 0.5f);

        bool confirmed = EditorModal::button("OK", "ok", ImVec2(buttonWidth, 0)) || enterPressed;
        if (confirmed) {
            std::string newName = nameBuffer;
            if (!newName.empty() && newName != fileBeingRenamed) {
                try {
                    fs::path oldPath = currentPath / fileBeingRenamed;
                    fs::path newPath = currentPath / newName;

                    // Check if the target file already exists (a Vertex Animation's other file too:
                    // the .vat and its .vat.glb are renamed together)
                    fs::path pairOld, pairNew;
                    const bool pairTaken = vat::renamedPair(oldPath, newPath, pairOld, pairNew) && fs::exists(pairNew);
                    // a Vertex Animation's file keeps its extension (as move_asset refuses)
                    renameErrorText = vat::pairRenameRefusal(oldPath, newPath, fileBeingRenamed);
                    if (!renameErrorText.empty()) {
                        ImGui::OpenPopup("Rename Error");
                    } else if (fs::exists(newPath) || pairTaken) {
                        ImGui::OpenPopup("File Already Exists");
                    } else {
                        const bool renamed = project->getProjectCommandHistory()->addCommand(new RenameFileCmd(project, fileBeingRenamed, newName, currentPath.string()));
                        if (renamed && (Util::isLuaFile(fileBeingRenamed) || Util::isLuaFile(newName))) {
                            Generator::writeLuaScriptStubs(project->getProjectPath(), project->getProjectInternalPath(), {oldPath, newPath});
                        }
                        codeEditor->handleFileRename(oldPath, newPath);
                        imageViewerWindow->handleFileRename(oldPath, newPath);
                        scanDirectory(currentPath);
                        isRenaming = false;
                        ImGui::CloseCurrentPopup();
                    }
                } catch (const fs::filesystem_error& e) {
                    renameErrorText.clear();
                    ImGui::OpenPopup("Rename Error");
                }
            }
            if (newName.empty()) {
                ImGui::OpenPopup("Invalid Name");
            }
        }

        ImGui::SameLine();

        // The active input eats the first Escape to revert its text; close on the same press
        if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            isRenaming = false;
            ImGui::CloseCurrentPopup();
        }

        if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0))) {
            isRenaming = false;
            ImGui::CloseCurrentPopup();
        }

        // Error popup for existing file
        if (EditorModal::begin("File Already Exists", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("A file with this name already exists.");
            EditorModal::message("A file with this name already exists.");
            ImGui::Separator();

            float popupWidth = ImGui::GetWindowSize().x;
            float buttonWidth = 120.0f;
            ImGui::SetCursorPosX((popupWidth - buttonWidth) * 0.5f);

            if (EditorModal::button("OK", "ok", ImVec2(buttonWidth, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Error popup for rename failure
        if (EditorModal::begin("Rename Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(renameErrorText.empty() ? "Failed to rename the file." : renameErrorText.c_str());
            EditorModal::message(renameErrorText.empty() ? "Failed to rename the file." : renameErrorText);
            ImGui::Separator();

            float popupWidth = ImGui::GetWindowSize().x;
            float buttonWidth = 120.0f;
            ImGui::SetCursorPosX((popupWidth - buttonWidth) * 0.5f);

            if (EditorModal::button("OK", "ok", ImVec2(buttonWidth, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Error popup for invalid name
        if (EditorModal::begin("Invalid Name", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Please enter a valid file name.");
            EditorModal::message("Please enter a valid file name.");
            ImGui::Separator();

            float popupWidth = ImGui::GetWindowSize().x;
            float buttonWidth = 120.0f;
            ImGui::SetCursorPosX((popupWidth - buttonWidth) * 0.5f);

            if (EditorModal::button("OK", "ok", ImVec2(buttonWidth, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndPopup();
    }
}

void editor::ProjectWindow::copySelectedFiles(bool cut) {
    std::vector<fs::path> paths;
    for (const auto& fileName : selectedFiles) {
        paths.push_back(currentPath / fileName);
    }
    copyPaths(std::move(paths), cut);
}

void editor::ProjectWindow::copyPaths(std::vector<fs::path> paths, bool cut) {
    if (cut) {
        // the assets or Lua directory moves only through Project Settings (D10)
        for (const fs::path& path : paths) {
            if (const std::string refused = rootRefusal(path); !refused.empty()) {
                Backend::getApp().registerAlert("Cut", refused);
                return;
            }
        }
    }
    clipboard.set(std::move(paths), cut);
}

bool editor::ProjectWindow::pasteFiles(const fs::path& targetDirectory) {
    size_t gone = 0;
    const std::vector<fs::path> sources = clipboard.existing(&gone);
    if (sources.empty()) {
        clipboard.clear();
        Out::warning("Nothing to paste: the copied files were moved or deleted");
        return false;
    }
    if (gone > 0) {
        Out::info("%zu of %zu copied files are gone: pasting the others", gone, gone + sources.size());
    }
    const bool cut = clipboard.isCut();
    // the assets or Lua directory moves only through Project Settings (D10); a folder into itself
    // and the like the command refuses below, and a refused or failed paste keeps a cut
    for (const fs::path& source : sources) {
        if (const std::string refused = cut ? rootRefusal(source) : std::string(); !refused.empty()) {
            Backend::getApp().registerAlert("Paste", refused);
            return false;
        }
    }
    std::vector<std::string> paths;
    for (const fs::path& source : sources) paths.push_back(source.string());
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* command = new CopyFileCmd(project, paths, targetDirectory.string(), !cut);
    command->reportTo(report);
    const bool done = project->getProjectCommandHistory()->addCommand(command);
    if (!done && !report->error.empty()) {
        Backend::getApp().registerAlert("Paste", report->error);
    }
    clipboard.pasted(done && !report->landed.empty());
    scanDirectory(currentPath);
    if (done) {
        selectLanded(report->landed, targetDirectory);
    }
    return done && !report->landed.empty();
}

void editor::ProjectWindow::selectLanded(const std::vector<fs::path>& landed, const fs::path& target) {
    if (landed.empty()) return;
    selectedFiles.clear();
    const fs::path listing = listingFolder().lexically_normal();
    if (target.lexically_normal() == listing || !searchText.empty()) {
        // the copies, as the listing names them (a search: relative paths)
        for (const fs::path& path : landed) {
            const fs::path relative = path.lexically_normal().lexically_relative(currentPath.lexically_normal());
            if (relative.empty() || *relative.begin() == "..") continue;
            selectedFiles.insert(FileUtils::pathToGenericUtf8(relative));
            lastSelectedFile = FileUtils::pathToGenericUtf8(relative);
        }
        if (!lastSelectedFile.empty() && selectedFiles.count(lastSelectedFile)) {
            updateInspectedAsset(lastSelectedFile);
            pingAsset(lastSelectedFile);
        }
    } else if (target.parent_path().lexically_normal() == currentPath.lexically_normal()) {
        // pasted into a folder of the listing: that folder stays selected
        const std::string folder = FileUtils::pathToUtf8(target.lexically_normal().filename());
        selectedFiles.insert(folder);
        lastSelectedFile = folder;
    }
}

std::string editor::ProjectWindow::rootRefusal(const fs::path& path) const {
    const fs::path normal = path.lexically_normal();
    const fs::path projectNormal = project->getProjectPath().lexically_normal();
    if (normal == projectNormal) {
        return "The project folder itself can't be moved, renamed or deleted from the Project window.";
    }
    struct Root {
        fs::path configured;
        const char* what;
        const char* setting;
    };
    const Root roots[] = {{project->getAssetsPath(), "assets directory", "Project Settings > Editor > Assets Directory"},
                          {project->getLuaPath(), "Lua directory", "Project Settings > Scripting > Lua Directory"}};
    for (const Root& root : roots) {
        const fs::path configured = root.configured.lexically_normal();
        if (configured.empty() || configured == projectNormal) continue;
        const fs::path resolved = safewrite::resolvedPath(configured);
        const fs::path entry = safewrite::entryLocation(normal);
        if (!safewrite::isAtOrBelow(configured, normal) && (entry.empty() || resolved.empty() || !safewrite::isAtOrBelow(resolved, entry))) {
            continue;
        }
        const std::string name = "'" + FileUtils::pathToUtf8(normal.filename()) + "'";
        return name + (configured == normal ? " is" : " holds") + " the project's " + root.what + ": change it in " + root.setting +
               ", which moves the referenced files.";
    }
    return std::string();
}

fs::path editor::ProjectWindow::listingFolder() const {
    if (!searchText.empty() && !searchReturnPath.empty()) {
        return searchReturnPath;
    }
    return currentPath.empty() ? project->getProjectPath() : currentPath;
}

fs::path editor::ProjectWindow::createTargetDirectory() const {
    return listingFolder();
}

std::string editor::ProjectWindow::luaScriptRefusal(const fs::path& directory) const {
    const fs::path lua = project->getLuaPath().lexically_normal();
    if (safewrite::isAtOrBelow(directory.lexically_normal(), lua)) {
        return std::string();
    }
    return "Lua scripts live in the Lua directory (" + FileUtils::pathToGenericUtf8(project->getLuaDir()) +
           "): Project Settings > Scripting > Lua Directory";
}

bool editor::ProjectWindow::canDuplicate() const {
    return !selectedFiles.empty() && !isRenaming && !pendingNewItem.open;
}

void editor::ProjectWindow::duplicateSelection() {
    if (selectedFiles.empty()) return;
    std::vector<FileCopyData> entries;
    for (const fs::path& path : getSelectedFiles()) {
        FileCopyData entry;
        entry.filename = path.filename();
        entry.sourceDirectory = path.parent_path();
        entry.targetDirectory = entry.sourceDirectory;
        entries.push_back(entry);
    }
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* command = new CopyFileCmd(project, std::move(entries), true);
    command->setDuplicate(true);
    command->reportTo(report);
    const bool done = project->getProjectCommandHistory()->addCommand(command);
    if (!done && !report->error.empty()) {
        Backend::getApp().registerAlert("Duplicate", report->error);
    }
    scanDirectory(currentPath);
    if (done) {
        selectLanded(report->landed, listingFolder());
    }
}

void editor::ProjectWindow::importExternalPaths(const std::vector<std::string>& sourcePaths, const fs::path& targetDirectory) {
    std::vector<std::string> validPaths;
    std::vector<fs::path> videos;

    for (const auto& sourcePath : sourcePaths) {
        fs::path source = fs::path(sourcePath).lexically_normal();

        std::error_code ec;
        // a video the engine does not play is converted (FFmpeg), not copied (phase 6)
        if (fs::is_regular_file(source, ec) && htmlui::VideoImporter::isVideoFile(source) && !htmlui::VideoImporter::isPlayable(source)) {
            videos.push_back(source);
            continue;
        }
        if (fs::is_directory(source, ec) && !ec) {
            // copying a directory into itself would recurse forever
            fs::path relative = targetDirectory.lexically_normal().lexically_relative(source);
            if (!relative.empty() && *relative.begin() != "..") {
                Log::error("Cannot import '%s': destination is inside it", source.string().c_str());
                continue;
            }
        }

        validPaths.push_back(sourcePath);
    }

    if (!videos.empty()) {
        importVideos(videos, targetDirectory);
    }

    if (validPaths.empty()) {
        return;
    }

    project->getProjectCommandHistory()->addCommand(new CopyFileCmd(project, validPaths, targetDirectory.string(), true));
    scanDirectory(currentPath);
}

// Converting a video takes seconds to minutes: a job of the process channel runs FFmpeg
// (htmlui/VideoImporter, as import_files does) and reports each file in the Console; the
// folder is listed again on the main thread when a file is written. An existing output is not
// replaced. The job captures no window, so its handle is not kept.
void editor::ProjectWindow::importVideos(const std::vector<fs::path>& videos, const fs::path& targetDir) {
    for (const fs::path& video : videos)
        Out::info("Converting %s to AV1 + Opus WebM with FFmpeg...", editor::FileUtils::pathToUtf8(video.filename()).c_str());
    jobs::SubmitOptions options;
    options.name = "ffmpeg-import";
    jobs::system().processChannel().submit([videos, targetDir]() {
        for (const fs::path& video : videos) {
            const htmlui::VideoImportResult result = htmlui::VideoImporter::importFile(video, targetDir, false);
            if (result.ok) {
                Out::info("%s", result.message.c_str());
            } else if (result.diagnostics.empty()) {
                Out::error("Video import: %s", result.message.c_str());
            }
            for (const jasperengine::htmlui::compat::Diagnostic& d : result.diagnostics) {
                const std::string line = d.message + " (fix: " + d.suggestion + ") [" + d.code.id() + "]";
                if (d.severity == jasperengine::htmlui::compat::Severity::Error)
                    Out::error("%s", line.c_str());
                else
                    Out::warning("%s", line.c_str());
            }
            if (!result.output.empty()) {
                editor::getEditorHost().enqueueMainThreadTask([]() {
                    if (ProjectWindow* window = Backend::getApp().getProjectWindow())
                        window->refreshCurrentDirectory();
                });
            }
        }
    }, options);
}

void editor::ProjectWindow::queueThumbnailGeneration(const fs::path& filePath, FileType type, bool forceRegenerate) {
    std::error_code ec;
    if (!fs::exists(filePath, ec) || ec) {
        return;
    }

    fs::path normalizedPath = filePath;
    if (normalizedPath.is_relative() && !project->getProjectPath().empty()) {
        normalizedPath = project->getProjectPath() / normalizedPath;
    }
    normalizedPath = normalizedPath.lexically_normal();

    const std::string requestKey = thumbnailRequestKey(normalizedPath);

    fs::path thumbnailPath = project->getThumbnailPath(normalizedPath);

    ThumbnailRequest thumbFile = {normalizedPath, type, 0};
    ec.clear();
    // A prefab's thumbnail also shows its nested prefabs: their changes count too
    bool stampCurrent = true;
    std::string stamp;
    if (type == FileType::PREFAB) {
        stamp = prefabThumbnailStamp(normalizedPath);
        std::ifstream stampFile(prefabStampPath(thumbnailPath));
        std::string written((std::istreambuf_iterator<char>(stampFile)), std::istreambuf_iterator<char>());
        stampCurrent = !stamp.empty() && written == stamp;
    }
    if (!forceRegenerate && stampCurrent && fs::exists(thumbnailPath, ec) && !ec) {
        std::error_code imageTimeEc;
        std::error_code thumbTimeEc;
        auto imageTime = fs::last_write_time(normalizedPath, imageTimeEc);
        auto thumbTime = fs::last_write_time(thumbnailPath, thumbTimeEc);
        if (!imageTimeEc && !thumbTimeEc && thumbTime >= imageTime) {
            {
                std::lock_guard<std::mutex> lock(thumbnailMutex);
                if (thumbnailWorkSuspended) {
                    return;
                }
                thumbFile.generation = thumbnailGeneration;
            }
            // Thumbnail is up-to-date, queue it for loading. Consumers verify
            // the generation in case a project switch races this push.
            if (type == FileType::PREFAB) {
                prefabThumbnailStamps[requestKey] = stamp;
            }
            std::lock_guard<std::mutex> lock(completedThumbnailMutex);
            completedThumbnailQueue.push(thumbFile);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        if (thumbnailWorkSuspended) {
            return;
        }
        if (auto abandoned = abandonedMaterialThumbnails.find(requestKey); abandoned != abandonedMaterialThumbnails.end()) {
            std::error_code timeError;
            const fs::file_time_type fileTime = fs::last_write_time(normalizedPath, timeError);
            if (!forceRegenerate && !timeError && fileTime == abandoned->second) {
                return;
            }
            abandonedMaterialThumbnails.erase(abandoned);
        }
        if (!pendingThumbnailRequests.insert(requestKey).second) {
            return;
        }
        thumbFile.generation = thumbnailGeneration;
        if (type == FileType::POSE_ANIMATION) {
            // the main thread renders these too (processPoseThumbnails)
            poseThumbnailQueue.push(thumbFile);
            return;
        }
        if (type == FileType::PREFAB) {
            // the main thread renders these (processPrefabThumbnails)
            prefabThumbnailStamps[requestKey] = stamp;
            prefabThumbnailQueue.push(thumbFile);
            return;
        }
        thumbnailQueue.push(thumbFile);
    }
    notifyThumbnailWork();
}

void editor::ProjectWindow::scheduleThumbnailStep() {
    if (!thumbnailStepScheduled.exchange(true))
        thumbnails.push([this]() { thumbnailStep(); });
}

// One step of the thumbnail strand (a Background SerialQueue): one request, then the next
// step is scheduled while work is left. A step never waits: a request that must wait for the
// main thread (a pending material or model render) is picked up by the step the main thread
// schedules when it clears that state.
void editor::ProjectWindow::thumbnailStep() {
    thumbnailStepScheduled.store(false);
    if (stopThumbnailThread) {
        return;
    }

    // warm shared preview resources once: rendering the material preview scene once
    // generates the IBL environment maps, which are pooled by texture id (TexturePool), so
    // the first material preview in Properties opens without lag
    if (!thumbnailWarmupDone && Engine::isViewLoaded()) {
        thumbnailWarmupDone = true;
        bool runWarmup = false;
        {
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            if (!thumbnailWorkSuspended) {
                thumbnailWorkerBusy = true;
                runWarmup = true;
            }
        }
        if (runWarmup) {
            try {
                Engine::AsyncThreadScope asyncThreadScope;
                Engine::executeSceneOnce(materialRender.getScene());
            } catch (const std::exception& e) {
                Log::error("Failed to warm thumbnail preview: %s", e.what());
            } catch (...) {
                Log::error("Failed to warm thumbnail preview: unknown error");
            }
            {
                std::lock_guard<std::mutex> lock(thumbnailMutex);
                thumbnailWorkerBusy = false;
            }
            thumbnailCondition.notify_all();
        }
    }

    {
        ThumbnailRequest thumbFile;
        {
            std::unique_lock<std::mutex> lock(thumbnailMutex);
            if (thumbnailWorkSuspended || thumbnailQueue.empty() || hasPendingMaterialRender || hasPendingModelRender) {
                return;
            }
            thumbFile = thumbnailQueue.front();
            thumbnailQueue.pop();
            thumbnailWorkerBusy = true;
        }

        try {
        // Generate thumbnail
        if (thumbFile.type == FileType::IMAGE || thumbFile.type == FileType::AUDIO) {
            fs::path thumbnailPath = project->getThumbnailPath(thumbFile.path);
            bool thumbnailWritten = thumbFile.type == FileType::IMAGE ? writeImageThumbnail(thumbFile.path, thumbnailPath)
                                                                      : writeAudioThumbnail(thumbFile.path, thumbnailPath);

            if (thumbnailWritten) {
                {
                    std::lock_guard<std::mutex> lock(completedThumbnailMutex);
                    completedThumbnailQueue.push(thumbFile);
                }
            } else {
                std::cerr << "Failed to generate image thumbnail: " << thumbFile.path.string() << std::endl;
            }

            {
                std::lock_guard<std::mutex> lock(thumbnailMutex);
                pendingThumbnailRequests.erase(thumbnailRequestKey(thumbFile.path));
            }
        } else if (thumbFile.type == FileType::MATERIAL) {
            try {
                if (YAML::Node materialNode = DocumentFiles::read(thumbFile.path, DocumentKind::Material).root) {
                    Material material = Stream::decodeMaterial(materialNode);
                    materialRender.applyMaterial(material);

                    {
                        Engine::AsyncThreadScope asyncThreadScope;
                        Engine::executeSceneOnce(materialRender.getScene());
                    }

                    // Set the pending flag before executing the scene
                    {
                        std::lock_guard<std::mutex> lock(thumbnailMutex);
                        if (!thumbnailWorkSuspended && thumbFile.generation == thumbnailGeneration) {
                            pendingMaterialPath = thumbFile.path;
                            pendingMaterialGeneration = thumbFile.generation;
                            hasPendingMaterialRender = true;
                        }
                    }

                    // The processMaterialThumbnails method will handle the rest
                    // and set hasPendingMaterialRender to false when done
                } else {
                    std::lock_guard<std::mutex> lock(thumbnailMutex);
                    pendingThumbnailRequests.erase(thumbnailRequestKey(thumbFile.path));
                }
            } catch (const std::exception& e) {
                // Log the error and continue with other thumbnails
                std::cerr << "Error generating thumbnail for material: " << thumbFile.path.string() << " - " << e.what() << std::endl;

                // Make sure we reset the pending flag if there was an error
                {
                    std::lock_guard<std::mutex> lock(thumbnailMutex);
                    if (pendingMaterialGeneration == thumbFile.generation) {
                        hasPendingMaterialRender = false;
                        pendingMaterialPath.clear();
                        pendingMaterialGeneration = 0;
                    }
                }
                std::lock_guard<std::mutex> lock(thumbnailMutex);
                pendingThumbnailRequests.erase(thumbnailRequestKey(thumbFile.path));
            }

        } else if (thumbFile.type == FileType::MODEL) {
            try {
                // Run the load under the async-thread flag: building the multi-node child-entity
                // hierarchy mutates the ECS, which is only safe on the main thread, so off-thread the
                // loader bakes node transforms into one static preview mesh instead.
                // GPU work is deferred to the main thread; ModelRender frames whatever entities exist.
                // Cap texture decode resolution too: a 128px preview never needs full 4K maps.
                // An FBX previews through its import, created here on first sight
                const bool loadable = !FbxImporter::isFbxFile(thumbFile.path) || project->importFbxModel(thumbFile.path);

                Engine::AsyncThreadScope loadAsyncScope;
                MeshSystem::setImageDecodeMaxDimension(256);
                const bool modelLoaded = loadable && modelRender.loadModel(thumbFile.path.string());
                MeshSystem::setImageDecodeMaxDimension(0);
                if (modelLoaded) {
                    modelRender.fixDarkMaterials();
                    modelRender.positionCameraForModel();

                    {
                        Engine::AsyncThreadScope asyncThreadScope;
                        Engine::executeSceneOnce(modelRender.getScene());
                    }

                    {
                        std::lock_guard<std::mutex> lock(thumbnailMutex);
                        if (!thumbnailWorkSuspended && thumbFile.generation == thumbnailGeneration) {
                            pendingModelPath = thumbFile.path;
                            pendingModelGeneration = thumbFile.generation;
                            hasPendingModelRender = true;
                        }
                    }
                } else {
                    Log::error("Failed to load model for thumbnail: %s", thumbFile.path.string().c_str());
                    std::lock_guard<std::mutex> lock(thumbnailMutex);
                    pendingThumbnailRequests.erase(thumbnailRequestKey(thumbFile.path));
                }
            } catch (const std::exception& e) {
                MeshSystem::setImageDecodeMaxDimension(0);
                std::cerr << "Error generating thumbnail for model: " << thumbFile.path.string() << " - " << e.what() << std::endl;

                {
                    std::lock_guard<std::mutex> lock(thumbnailMutex);
                    if (pendingModelGeneration == thumbFile.generation) {
                        hasPendingModelRender = false;
                        pendingModelPath.clear();
                        pendingModelGeneration = 0;
                    }
                }
                std::lock_guard<std::mutex> lock(thumbnailMutex);
                pendingThumbnailRequests.erase(thumbnailRequestKey(thumbFile.path));
            }
        }
        } catch (const std::exception& e) {
            MeshSystem::setImageDecodeMaxDimension(0);
            Log::error("Unhandled thumbnail error for '%s': %s", thumbFile.path.string().c_str(), e.what());
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            if (thumbFile.generation == thumbnailGeneration) {
                pendingThumbnailRequests.erase(thumbnailRequestKey(thumbFile.path));
            }
        } catch (...) {
            MeshSystem::setImageDecodeMaxDimension(0);
            Log::error("Unhandled thumbnail error for '%s'", thumbFile.path.string().c_str());
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            if (thumbFile.generation == thumbnailGeneration) {
                pendingThumbnailRequests.erase(thumbnailRequestKey(thumbFile.path));
            }
        }

        {
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            thumbnailWorkerBusy = false;
        }
        thumbnailCondition.notify_all();
    }
    scheduleThumbnailStep();
}

ImTextureID editor::ProjectWindow::getThumbnailTexture(const std::string& thumbnailPath, int& outWidth, int& outHeight) {
    auto it = thumbnailTextures.find(thumbnailPath);
    if (it == thumbnailTextures.end()) {
        ThumbnailTexture cached;
        cached.texture = Texture(thumbnailPath, TextureData(thumbnailPath.c_str()));
        cached.failed = !cached.texture.load();
        it = thumbnailTextures.emplace(thumbnailPath, std::move(cached)).first;
    }

    ThumbnailTexture& cached = it->second;
    cached.lastUsedFrame = ImGui::GetFrameCount();

    if (cached.failed) {
        return ImTextureID{};
    }

    TextureRender* render = cached.texture.getRender();
    if (!render || !render->isCreated()) {
        // GPU creation failed (e.g. texture pool exhausted); keep the entry
        // marked as failed so we don't retry every frame. LRU eviction will
        // drop it eventually, allowing a retry on a later visit.
        cached.failed = true;
        return ImTextureID{};
    }

    outWidth = static_cast<int>(cached.texture.getWidth());
    outHeight = static_cast<int>(cached.texture.getHeight());

    return Backend::getImGuiTexture(render);
}

ImTextureID editor::ProjectWindow::getAssetThumbnail(const fs::path& filePath, int& outWidth, int& outHeight) {
    outWidth = outHeight = 0;
    if (filePath.empty() || !Engine::isViewLoaded()) {
        return ImTextureID{};
    }

    const fs::path assetPath = project->resolveAssetPath(filePath);
    std::error_code ec;
    if (!fs::is_regular_file(assetPath, ec)) {
        return ImTextureID{};
    }

    const fs::path thumbnailPath = project->getThumbnailPath(assetPath);
    if (!fs::is_regular_file(thumbnailPath, ec)) {
        requestThumbnailGeneration(assetPath);
        return ImTextureID{};
    }

    ImTextureID texture = getThumbnailTexture(thumbnailPath.string(), outWidth, outHeight);
    enforceThumbnailCacheLimit();
    return texture;
}

void editor::ProjectWindow::evictThumbnailTexture(const std::string& thumbnailKey) {
    auto it = thumbnailTextures.find(thumbnailKey);
    if (it != thumbnailTextures.end()) {
        // destroy() also removes the entries from TexturePool/TextureDataPool,
        // otherwise the GPU texture would outlive the cache (and a later
        // reload would pick up the stale pooled texture)
        it->second.texture.destroy();
        thumbnailTextures.erase(it);
    }
}

void editor::ProjectWindow::enforceThumbnailCacheLimit() {
    const int currentFrame = ImGui::GetFrameCount();

    while (thumbnailTextures.size() > MAX_THUMBNAIL_TEXTURES) {
        auto oldest = thumbnailTextures.end();
        for (auto it = thumbnailTextures.begin(); it != thumbnailTextures.end(); ++it) {
            if (it->second.lastUsedFrame == currentFrame) {
                continue; // used this frame, keep it
            }
            if (oldest == thumbnailTextures.end() || it->second.lastUsedFrame < oldest->second.lastUsedFrame) {
                oldest = it;
            }
        }
        if (oldest == thumbnailTextures.end()) {
            break; // everything in the cache is visible this frame
        }
        oldest->second.texture.destroy();
        thumbnailTextures.erase(oldest);
    }
}

void editor::ProjectWindow::clearThumbnailTextures() {
    for (auto& [key, cached] : thumbnailTextures) {
        cached.texture.destroy();
    }
    thumbnailTextures.clear();
}

fs::path editor::ProjectWindow::uniqueRelativePath(const fs::path& directory, const std::string& baseName, const std::string& extension) {
    std::string sanitized = baseName;
    for (char& c : sanitized) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    std::string fileName = sanitized + extension;
    fs::path targetFile = directory / fileName;
    int counter = 1;
    while (fs::exists(targetFile)) {
        fileName = sanitized + " " + std::to_string(counter) + extension;
        targetFile = directory / fileName;
        counter++;
    }
    return fs::relative(targetFile, project->getProjectPath());
}

void editor::ProjectWindow::saveMaterialFile(const fs::path& directory, const char* materialContent, size_t contentLen, const MaterialPayload* sourceMaterial) {
    project->getProjectCommandHistory()->addCommandNoMerge(new CreateMaterialFileCmd(project, directory, materialContent, contentLen, sourceMaterial));
    scanDirectory(currentPath);
}

// every GameObject of the encoded prefabs (the dropped scene objects): their clip previews restore
// before the prefab is made of them
static std::vector<Entity> prefabEntities(const std::vector<std::pair<fs::path, YAML::Node>>& prefabs) {
    std::vector<Entity> entities;
    for (const auto& prefab : prefabs) {
        editor::ProjectUtils::collectEntities(prefab.second, entities);
    }
    return entities;
}

void editor::ProjectWindow::saveEntityFile(const fs::path& directory, uint32_t sceneId, const std::string& yamlString) {
    if (yamlString.empty() || !project->getScene(sceneId)) {
        return;
    }
    YAML::Node entityNode = YAML::Load(yamlString);

    // A prefab has one root GameObject: several dragged roots make one prefab each (Unity's
    // way), a GameObject without a Transform (an action of one) going with the root it acts
    // on, else with the first
    const std::string transformKey = Catalog::getComponentName(ComponentType::Transform, true);
    const std::string actionKey = Catalog::getComponentName(ComponentType::ActionComponent, true);
    std::vector<YAML::Node> roots;
    std::vector<YAML::Node> loose;
    if (entityNode["members"] && entityNode["members"].IsSequence()) {
        for (YAML::Node member : entityNode["members"]) {
            const YAML::Node components = member["components"];
            (components && components.IsMap() && components[transformKey] ? roots : loose).push_back(member);
        }
    } else {
        roots.push_back(entityNode);
    }
    if (roots.empty() && !loose.empty()) {
        roots.push_back(loose.front());
        loose.erase(loose.begin());
    }
    std::vector<std::vector<YAML::Node>> groups(roots.size());
    for (YAML::Node& node : loose) {
        size_t owner = 0;
        const YAML::Node action = node["components"] ? node["components"][actionKey] : YAML::Node();
        if (action && action["target"]) {
            const Entity target = action["target"].as<Entity>();
            for (size_t i = 0; i < roots.size(); i++) {
                std::vector<Entity> ids;
                ProjectUtils::collectEntities(roots[i], ids);
                if (std::find(ids.begin(), ids.end(), target) != ids.end()) {
                    owner = i;
                    break;
                }
            }
        }
        groups[owner].push_back(node);
    }

    std::vector<std::pair<fs::path, YAML::Node>> prefabs;
    std::set<std::string> usedNames;
    for (size_t i = 0; i < roots.size(); i++) {
        // (a new node: assigning to a yaml-cpp node writes into the one it refers to)
        YAML::Node prefabNode = YAML::Clone(roots[i]);
        if (!groups[i].empty()) {
            YAML::Node group(YAML::NodeType::Map);
            group["type"] = "Prefab";
            YAML::Node members(YAML::NodeType::Sequence);
            members.push_back(YAML::Clone(roots[i]));
            for (const YAML::Node& node : groups[i]) {
                members.push_back(YAML::Clone(node));
            }
            group["members"] = members;
            prefabNode.reset(group);
        }
        std::string baseName = roots[i]["name"] ? roots[i]["name"].as<std::string>() : std::string("Prefab");
        if (baseName.empty()) {
            baseName = "Prefab";
        }
        fs::path relativePath = uniqueRelativePath(directory, baseName, ".prefab");
        for (int counter = 1; usedNames.count(relativePath.generic_string()); counter++) {
            relativePath = uniqueRelativePath(directory, baseName + "_" + std::to_string(counter), ".prefab");
        }
        usedNames.insert(relativePath.generic_string());
        prefabs.emplace_back(relativePath, prefabNode);
    }

    // Unity: a Prefab instance root dragged in asks "Create Prefab or Variant?" (an original
    // prefab from its GameObjects, or a variant of its prefab it becomes an instance of)
    PendingPrefabDrop pending;
    pending.sceneId = sceneId;
    for (size_t i = 0; i < roots.size(); i++) {
        const Entity root = roots[i]["entity"] ? roots[i]["entity"].as<Entity>() : NULL_ENTITY;
        const bool instanceRoot = root != NULL_ENTITY && project->canCreatePrefabVariant(sceneId, root);
        pending.instanceRoots.push_back(instanceRoot);
        fs::path variantPath;
        if (instanceRoot) {
            const std::string rootName = roots[i]["name"] ? roots[i]["name"].as<std::string>() : std::string();
            std::string baseName = (rootName.empty() ? std::string("Prefab") : rootName) + " Variant";
            variantPath = uniqueRelativePath(directory, baseName, ".prefab");
            for (int counter = 1; usedNames.count(variantPath.generic_string()); counter++) {
                variantPath = uniqueRelativePath(directory, baseName + " " + std::to_string(counter), ".prefab");
            }
            usedNames.insert(variantPath.generic_string());
            if (pending.firstInstance.empty()) {
                pending.firstInstance = roots[i]["name"] ? roots[i]["name"].as<std::string>() : std::string("GameObject");
            }
        }
        pending.variantPaths.push_back(variantPath);
    }
    if (!pending.firstInstance.empty()) {
        pending.prefabs = std::move(prefabs);
        pending.open = true;
        pendingPrefabDrop = std::move(pending);
        return;
    }

    // prefabs of a posed character keep its rest bones; clip previews of the GameObjects restore
    PoseEditSession::suspendActive();
    PoseEditSession::suspendActiveFor(sceneId, prefabEntities(prefabs));
    CreatePrefabCmd* createPrefabCmd = new CreatePrefabCmd(project, sceneId, prefabs);
    CommandHandle::get(sceneId)->addCommandNoMerge(createPrefabCmd);

    scanDirectory(currentPath);
}

void editor::ProjectWindow::showPrefabOrVariantPopup() {
    static constexpr const char* POPUP = "Create Prefab or Variant?";
    if (pendingPrefabDrop.open) {
        ImGui::OpenPopup(POPUP);
        pendingPrefabDrop.open = false;
    }
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!EditorModal::begin(POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    // Unity's dialog
    ImGui::TextUnformatted("Would you like to create a new original Prefab or a variant of this Prefab?");
    EditorModal::message("Would you like to create a new original Prefab or a variant of this Prefab?");
    ImGui::TextDisabled("%s", pendingPrefabDrop.prefabs.size() > 1 ?
        "Prefab instance roots of the selection become variants of their Prefabs; the other GameObjects original Prefabs" :
        ("'" + pendingPrefabDrop.firstInstance + "' is a Prefab instance").c_str());
    ImGui::Spacing();
    const float buttonWidth = Theme::dpi(130.0f);
    bool done = false;
    if (EditorModal::button("Original Prefab", "yes", ImVec2(buttonWidth, 0))) {
        PoseEditSession::suspendActive();
        PoseEditSession::suspendActiveFor(pendingPrefabDrop.sceneId, prefabEntities(pendingPrefabDrop.prefabs));
        CommandHandle::get(pendingPrefabDrop.sceneId)->addCommandNoMerge(new CreatePrefabCmd(project, pendingPrefabDrop.sceneId, pendingPrefabDrop.prefabs));
        done = true;
    }
    ImGui::SetItemTooltip("A new Prefab of these GameObjects: the instance is unpacked into it (nested Prefabs stay nested)");
    ImGui::SameLine();
    if (EditorModal::button("Prefab Variant", "no", ImVec2(buttonWidth, 0))) {
        std::vector<std::pair<fs::path, YAML::Node>> prefabs = pendingPrefabDrop.prefabs;
        for (size_t i = 0; i < prefabs.size(); i++) {
            if (i < pendingPrefabDrop.instanceRoots.size() && pendingPrefabDrop.instanceRoots[i]) {
                prefabs[i].first = pendingPrefabDrop.variantPaths[i];
            }
        }
        PoseEditSession::suspendActive();
        PoseEditSession::suspendActiveFor(pendingPrefabDrop.sceneId, prefabEntities(prefabs));
        CreatePrefabCmd* command = new CreatePrefabCmd(project, pendingPrefabDrop.sceneId, prefabs);
        for (size_t i = 0; i < prefabs.size(); i++) {
            command->setVariant(i, i < pendingPrefabDrop.instanceRoots.size() && pendingPrefabDrop.instanceRoots[i]);
        }
        CommandHandle::get(pendingPrefabDrop.sceneId)->addCommandNoMerge(command);
        done = true;
    }
    ImGui::SetItemTooltip("A Prefab Variant of the instance's Prefab: its overrides become the variant's, and it is an instance of the variant");
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        done = true;
    }
    if (done) {
        pendingPrefabDrop = PendingPrefabDrop();
        scanDirectory(currentPath);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool editor::ProjectWindow::isVariantFile(const fs::path& path) {
    return Util::isPrefabFile(path.string()) && !project->getPrefabFileBase(path).empty();
}

// Never throws: it runs at every project open, also after a failed one (a first project whose
// project.yaml does not parse leaves no project path), so a missing project or an unreadable
// folder only skips the cleanup.
void editor::ProjectWindow::cleanupThumbnails() {
    const fs::path projectPath = project->getProjectPath();
    std::error_code ec;
    if (projectPath.empty() || !fs::is_directory(projectPath, ec)) {
        return;
    }
    const fs::directory_options options = fs::directory_options::skip_permission_denied;

    // 1. Collect all valid thumbnail paths of existing files
    std::unordered_set<std::string> validThumbPaths;

    // Recursively scan project directory for files that have thumbnails
    std::error_code walk;
    for (fs::recursive_directory_iterator it(projectPath, options, walk), end; !walk && it != end; it.increment(walk)) {
        std::error_code entry;
        if (!it->is_regular_file(entry))
            continue;

        std::string ext = it->path().extension().string();
        if (Util::isImageFile(ext) || Util::isMaterialFile(ext) || Util::isModelFile(ext) || Util::isPrefabFile(ext)) {
            fs::path thumbnailPath = project->getThumbnailPath(it->path());
            validThumbPaths.insert(thumbnailPath.string());
            // a prefab's thumbnail keeps its dependency stamp beside it
            validThumbPaths.insert(prefabStampPath(thumbnailPath).string());
        }
    }
    // a scan that stopped part way knows only some of the files: removing would drop valid thumbnails
    if (walk) {
        return;
    }

    // 2. Walk .jasperengine/thumbs/ and remove any orphaned thumbnails. Never through a link: a
    // thumbnails folder resolving outside the project is not swept, and the removals go beneath
    // the project (util/SafeWrite.h)
    fs::path thumbsDir = project->getThumbsDir();
    if (!fs::is_directory(thumbsDir, ec)) return;
    if (!safewrite::isInsideResolved(thumbsDir, project->getProjectPath())) return;
    safewrite::Scope removalScope(project->getProjectPath());

    // Recursively iterate through the thumbnails directory to handle subdirectories, then remove
    // what it found (never while iterating)
    std::vector<fs::path> orphans;
    std::vector<fs::path> folders;
    for (fs::recursive_directory_iterator it(thumbsDir, options, walk), end; !walk && it != end; it.increment(walk)) {
        std::error_code entry;
        if (it->is_directory(entry)) {
            folders.push_back(it->path());
        } else if (it->is_regular_file(entry) && validThumbPaths.find(it->path().string()) == validThumbPaths.end()) {
            // This thumbnail file doesn't correspond to any existing image/material file
            orphans.push_back(it->path());
        }
    }
    for (const fs::path& orphan : orphans) {
        FileUtils::removePath(orphan, ec);
        // Also remove from the loaded thumbnails cache
        evictThumbnailTexture(orphan.string());
    }

    // 3. Remove empty directories in the thumbnails folder, the deepest first
    for (auto it = folders.rbegin(); it != folders.rend(); ++it) {
        if (fs::is_empty(*it, ec) && !ec) {
            FileUtils::removePath(*it, ec);
        }
    }
}

void editor::ProjectWindow::refreshCurrentDirectory() {
    scanDirectory(currentPath);
}

void editor::ProjectWindow::onFilesChanged(std::span<const FileChange> changes) {
    const fs::path current = currentPath.lexically_normal();
    auto inCurrent = [&](const fs::path& path) {
        if (path.empty()) return false;
        // a search lists everything under the folder
        if (!searchText.empty()) {
            std::string rel;
            return watch::relativeTo(current, path, rel) && !rel.empty();
        }
        return path.lexically_normal().parent_path() == current;
    };
    bool rescan = false, folders = false, documents = false, prefabs = false;
    std::vector<fs::path> modified;
    for (const FileChange& change : changes) {
        if (change.kind == FileChangeKind::Rescan) {
            rescan = folders = documents = true;
            continue;
        }
        const bool listing = change.kind != FileChangeKind::Modified;
        if (listing && (inCurrent(change.path) || inCurrent(change.oldPath))) rescan = true;
        if (listing && change.directory) folders = true;
        // the open folder itself went (renamed or removed): scanDirectory falls back
        std::string rel;
        if (listing && (watch::relativeTo(change.path, current, rel) || watch::relativeTo(change.oldPath, current, rel))) rescan = true;
        const std::string name = watch::pathToUtf8(change.path);
        if (listing && (Util::isSceneFile(name) || Util::isPrefabFile(name))) documents = true;
        if (change.kind == FileChangeKind::Modified && !change.directory) {
            modified.push_back(change.path);
            if (Util::isPrefabFile(name)) prefabs = true;
        }
    }
    if (rescan) {
        scanDirectory(currentPath);
    } else {
        if (folders) dirTreeGeneration++;
        if (documents) refreshProjectFiles();
    }
    for (const fs::path& path : modified) notifyResourceFileChanged(path);
    // a prefab's thumbnail also shows what it nests
    if (prefabs || !modified.empty()) checkPrefabThumbnailStamps();
}

editor::ProjectWindow::UiRect editor::ProjectWindow::lastItemRect() {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return {min.x, min.y, max.x - min.x, max.y - min.y};
}

void editor::ProjectWindow::updateSpriteNormalsBadge(FileEntry& file, const fs::path& path) {
    const spritenormalsentry::RecipeHead head = spritenormalsentry::recipeHead(project->getProjectPath(), path);
    // re-evaluated from scratch: a recipe gone (the file replaced) leaves no badge behind
    file.generated = false;
    file.generatedBy.clear();
    file.generatedStale = false;
    if (!head.generated) return;
    file.generated = true;
    file.generatedBy = spritenormalsentry::generatedText(head);
    if (SpriteNormalsWatcher* watcher = SpriteNormalsWatcher::active()) {
        // a generated map the watcher has not indexed (copied in, a VCS update) while it rests
        // on a project without one: it walks the project again (once per path)
        watcher->noticeGenerated(path);
        if (watcher->isStale(path)) {
            file.generatedStale = true;
            file.generatedBy += "\n" + spritenormalsentry::fileName(head.source) + " changed since: right-click > Regenerate Normal Map";
        } else if (watcher->sourceMissing(path)) {
            file.generatedStale = true;
            file.generatedBy += "\nThe source " + spritenormalsentry::fileName(head.source) + " is missing";
        }
    }
}

void editor::ProjectWindow::updateProceduralBakeBadge(FileEntry& file, const fs::path& path) {
    const std::optional<procmat::BakeStatus> status = procmat::cachedBakeStatus(project->getProjectPath(), path);
    if (!status) return;
    file.generated = true;
    file.generatedBy = "Baked from " + status->recipe.graphPath + " (" + status->recipe.output + ", " + std::to_string(status->recipe.size) +
                       ", seed " + std::to_string(status->recipe.seed) + ", " + status->recipe.tier + " " + status->recipe.device + ")";
    if (status->state != procmat::BakeState::Fresh) {
        file.generatedStale = true;
        file.generatedBy += "\nStale: " + status->reason + ". Bake it again.";
    }
}

void editor::ProjectWindow::drawSpriteNormalsMenuItems(const FileEntry& file, const fs::path& imagePath) {
    // Sprite Normal Map: one image, or every selected image (batch)
    std::vector<fs::path> images;
    for (const std::string& name : selectedFiles) {
        const fs::path path = currentPath / name;
        if (Util::isImageFile(name) && spritenormalsentry::lowerExtension(path) != ".svg" && !fs::is_directory(path)) images.push_back(path);
    }
    std::sort(images.begin(), images.end());
    if (images.size() <= 1 || std::find(images.begin(), images.end(), imagePath) == images.end()) images = {imagePath};
    if (images.size() > 1) {
        if (ImGui::MenuItem((std::string(ICON_FA_WAND_MAGIC_SPARKLES " Generate Normal Maps (") + std::to_string(images.size()) + ")...").c_str())) {
            editor::getEditorHost().openSpriteNormals(images, false);
        }
        UIProbe::item("project.menu.generateNormalMaps");
        ImGui::SetItemTooltip("One normal map per selected image, same settings, saved as one undo step");
    } else {
        if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES " Generate Normal Map...")) {
            editor::getEditorHost().openSpriteNormals(images, false);
        }
        UIProbe::item("project.menu.generateNormalMap");
        ImGui::SetItemTooltip("Sprite Normal Map with this image: preview it lit, then save %s_normal.png", jasperengine::editor::FileUtils::pathToUtf8(imagePath.stem()).c_str());
    }
    // a generated output (its recipe), or a source with its generated map beside it
    const fs::path root = project->getProjectPath();
    const spritenormalsentry::RecipeHead head = spritenormalsentry::recipeHead(root, imagePath);
    const fs::path output = head.generated ? imagePath : spritenormalsentry::pairedOutput(root, imagePath);
    if (head.generated) {
        if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES " Edit Normal Map...")) {
            editor::getEditorHost().openSpriteNormals({imagePath}, true);
        }
        ImGui::SetItemTooltip("Reopen the settings this normal map was generated with");
    }
    if (!output.empty()) {
        std::error_code ec;
        const spritenormalsentry::RecipeHead outputHead = head.generated ? head : spritenormalsentry::recipeHead(root, output);
        const bool sourceThere = fs::is_regular_file(outputHead.sourceAbsolute, ec);
        const bool playing = project->isAnyScenePlaying();
        if (ImGui::MenuItem(ICON_FA_ROTATE " Regenerate Normal Map", nullptr, false, sourceThere && !playing)) {
            // on the worker; the window's tick writes it (one undo step) and refreshes this window
            std::string error;
            if (!Backend::getApp().getSpriteNormalsWindow()->startRegenerate(output, error)) {
                editor::getEditorHost().registerAlert("Sprite Normal Map", "Can't regenerate " + jasperengine::editor::FileUtils::pathToUtf8(output.filename()) + ": " + error);
            }
        }
        UIProbe::item("project.menu.regenerateNormalMap");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", !sourceThere ? ("Its source " + outputHead.source + " is missing").c_str()
                                    : playing     ? "Stop the scene first: regenerating reloads the texture the game draws"
                                                  : "Generate it again from its source with the same settings (one undo step)");
        }
    }
}

void editor::ProjectWindow::show() {
    uiBuilding = UiState();
    // the watcher's staleness changed: the badges follow (no directory re-read)
    if (SpriteNormalsWatcher* watcher = SpriteNormalsWatcher::active()) {
        if (watcher->revision() != spriteNormalsRevision) {
            spriteNormalsRevision = watcher->revision();
            for (FileEntry& f : files) {
                if (f.type == FileType::IMAGE) updateSpriteNormalsBadge(f, currentPath / f.name);
            }
        }
    }
    if (firstOpen) {
        // Load saved settings (AppSettings is initialized by now)
        iconSize = AppSettings::getResourcesIconSize();
        currentLayout = static_cast<LayoutType>(AppSettings::getResourcesLayout());
        itemViewStyle = static_cast<ItemViewStyle>(AppSettings::getResourcesItemViewStyle());
        leftPanelWidth = AppSettings::getResourcesLeftPanelWidth();

        TextureData data;

        data.loadTextureFromMemory(folder_icon_png, folder_icon_png_len);
        folderIcon.setData("editor:resources:folder_icon", data);
        folderIcon.load();

        data.loadTextureFromMemory(file_icon_png, file_icon_png_len);
        fileIcon.setData("editor:resources:file_icon", data);
        fileIcon.load();

        data.loadTextureFromMemory(scene_icon_png, scene_icon_png_len);
        sceneIcon.setData("editor:resources:scene_icon", data);
        sceneIcon.load();

        data.loadTextureFromMemory(entity_icon_png, entity_icon_png_len);
        entityIcon.setData("editor:resources:entity_icon", data);
        entityIcon.load();

        data.loadTextureFromMemory(source_icon_png, source_icon_png_len);
        sourceIcon.setData("editor:resources:source_icon", data);
        sourceIcon.load();

        data.loadTextureFromMemory(header_icon_png, header_icon_png_len);
        headerIcon.setData("editor:resources:header_icon", data);
        headerIcon.load();

        data.loadTextureFromMemory(lua_icon_png, lua_icon_png_len);
        luaIcon.setData("editor:resources:lua_icon", data);
        luaIcon.load();

        data.loadTextureFromMemory(audio_icon_png, audio_icon_png_len);
        audioIcon.setData("editor:resources:audio_icon", data);
        audioIcon.load();

        data.loadTextureFromMemory(font_icon_png, font_icon_png_len);
        fontIcon.setData("editor:resources:font_icon", data);
        fontIcon.load();

        data.loadTextureFromMemory(html_icon_png, html_icon_png_len);
        htmlIcon.setData("editor:resources:html_icon", data);
        htmlIcon.load();

        data.loadTextureFromMemory(css_icon_png, css_icon_png_len);
        cssIcon.setData("editor:resources:css_icon", data);
        cssIcon.load();

        data.loadTextureFromMemory(lut_icon_png, lut_icon_png_len);
        lutIcon.setData("editor:resources:lut_icon", data);
        lutIcon.load();

        const int poseIconSize = 64;
        makePoseAnimationIcon(poseIconSize, poseAnimationIconPixels);
        poseAnimationIcon.setData("editor:resources:pose_animation_icon",
            TextureData(poseIconSize, poseIconSize, static_cast<unsigned int>(poseAnimationIconPixels.size()), ColorFormat::RGBA, 4,
                        poseAnimationIconPixels.data()));
        poseAnimationIcon.load();

        const int clipIconSize = 64;
        makeAnimationClipIcon(clipIconSize, animationClipIconPixels);
        animationClipIcon.setData("editor:resources:animation_clip_icon",
            TextureData(clipIconSize, clipIconSize, static_cast<unsigned int>(animationClipIconPixels.size()), ColorFormat::RGBA, 4,
                        animationClipIconPixels.data()));
        animationClipIcon.load();

        makeVertexAnimationIcon(clipIconSize, vertexAnimationIconPixels);
        vertexAnimationIcon.setData("editor:resources:vertex_animation_icon",
            TextureData(clipIconSize, clipIconSize, static_cast<unsigned int>(vertexAnimationIconPixels.size()), ColorFormat::RGBA, 4,
                        vertexAnimationIconPixels.data()));
        vertexAnimationIcon.load();
        auto proceduralIcon = [](Texture& icon, std::vector<uint8_t>& pixels, const char* id, void (*make)(int, std::vector<uint8_t>&)) {
            const int iconSize = 64;
            make(iconSize, pixels);
            icon.setData(id, TextureData(iconSize, iconSize, static_cast<unsigned int>(pixels.size()), ColorFormat::RGBA, 4, pixels.data()));
            icon.load();
        };
        proceduralIcon(animatorControllerIcon, animatorControllerIconPixels, "editor:resources:animator_controller_icon", makeAnimatorControllerIcon);
        proceduralIcon(avatarMaskIcon, avatarMaskIconPixels, "editor:resources:avatar_mask_icon", makeAvatarMaskIcon);
        proceduralIcon(montageIcon, montageIconPixels, "editor:resources:montage_icon", makeMontageIcon);

        makePhysicsMaterialIcon(clipIconSize, physicsMaterialIconPixels);
        physicsMaterialIcon.setData("editor:resources:physics_material_icon",
            TextureData(clipIconSize, clipIconSize, static_cast<unsigned int>(physicsMaterialIconPixels.size()), ColorFormat::RGBA, 4,
                        physicsMaterialIconPixels.data()));
        physicsMaterialIcon.load();

        scanDirectory(project->getProjectPath().string());

        firstOpen = false;
    }

    processPoseThumbnails();

    // Process completed thumbnails
    while (true) {
        ThumbnailRequest thumbFile;
        {
            std::lock_guard<std::mutex> lock(completedThumbnailMutex);
            if (completedThumbnailQueue.empty()) {
                break;
            }
            thumbFile = completedThumbnailQueue.front();
            completedThumbnailQueue.pop();
        }
        {
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            if (thumbFile.generation != thumbnailGeneration || thumbnailWorkSuspended) {
                continue;
            }
        }
        // Find and mark the corresponding file entry; the texture itself is
        // created on demand when the item becomes visible
        if (auto it = fileIndex.find(thumbFile.path); it != fileIndex.end()) {
            FileEntry& file = files[it->second];
            fs::path thumbnailPath = project->getThumbnailPath(thumbFile.path);
            std::error_code ec;
            if (fs::exists(thumbnailPath, ec) && !ec) {
                file.hasThumbnail = true;
                file.thumbnailPath = thumbnailPath.string();
            } else {
                file.hasThumbnail = false;
                file.thumbnailPath.clear();
            }
        }
    }

    if (!windowOpen) {
        windowFocused = false;
        return;
    }

    ctrlPressed = ImGui::GetIO().KeyCtrl;
    shiftPressed = ImGui::GetIO().KeyShift;

    // S25: a search follows the asset index, the listing the label store
    refreshSearchIfStale();
    refreshEntryLabels();

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    bool wasOpen = windowOpen;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool windowVisible = ImGui::Begin(ProjectWindow::WINDOW_NAME, &windowOpen);
    ImGui::PopStyleVar();
    if (!windowVisible) {
        windowFocused = false;
        ImGui::End();
        if (wasOpen && !windowOpen) {
            setOpen(false);
        }
        return;
    }

    // Determine effective layout when in AUTO mode: Unity's two-column layout
    // (folder tree + grid with subfolders) unless the panel is too narrow.
    LayoutType effectiveLayout = currentLayout;
    if (currentLayout == LayoutType::AUTO) {
        float windowWidth = ImGui::GetWindowWidth();
        effectiveLayout = (windowWidth < Theme::dpi(layoutAutoThreshold)) ? LayoutType::GRID : LayoutType::SPLIT;
    }
    lastLayout = effectiveLayout;

    // Folders first, then names (the listing has no sortable header)
    if (requestSort) {
        sortWithSortSpecs(nullptr, files);
        rebuildFileIndex();
        requestSort = false;
    }

    renderHeader();

    const ImVec2 innerPadding = Theme::dpi(ImVec2(6.0f, 4.0f));
    const float breadcrumbHeight = ImGui::GetFrameHeight() + Theme::dpi(2.0f);

    // Drops onto the file grid: imported files, materials and GameObjects
    // (saved as Prefabs) land in the folder being shown.
    auto acceptListingDrops = [&]() {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("external_files")) {
                importExternalPaths(Util::getStringsFromPayload(payload), currentPath);
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("material")) {
                const char* materialContent = static_cast<const char*>(payload->Data);
                size_t contentLen = payload->DataSize;

                MaterialPayload materialSource{0, NULL_PROJECT_SCENE, NULL_ENTITY, 0};
                const MaterialPayload* sourcePtr = nullptr;

                if (contentLen > sizeof(MaterialPayload)) {
                    const MaterialPayload* candidate = reinterpret_cast<const MaterialPayload*>(materialContent);
                    const char* yamlData = materialContent + sizeof(MaterialPayload);

                    // New payload format: [MaterialPayload header][YAML string]
                    if (candidate->magic == 0x4D54524C) {
                        materialSource = *candidate;
                        sourcePtr = &materialSource;
                        materialContent = yamlData;
                        contentLen -= sizeof(MaterialPayload);
                    }
                }

                saveMaterialFile(currentPath, materialContent, contentLen, sourcePtr);
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                if (payload->DataSize >= static_cast<int>(sizeof(EntityPayload))) {
                    EntityPayload entity;
                    std::memcpy(&entity, payload->Data, sizeof(EntityPayload));
                    const uint32_t sceneId = payloadSceneId(entity, project->getSelectedSceneId());
                    // a child scene's GameObject is a reference: its prefab is made from its own scene
                    std::string problem = sameSceneDropProblem(*project, entity, project->getSelectedSceneId(), true);
                    if (!problem.empty()) {
                        const SceneProject* source = project->getScene(sceneId);
                        problem += ": open " + (source ? "\"" + source->name + "\"" : std::string("it")) + " to make a prefab of its GameObjects";
                        ImGui::SetTooltip("%s", problem.c_str());
                    } else if (payload->IsDelivery()) {
                        if (HierarchyWindow* hierarchy = Backend::getApp().getHierarchyWindow()) {
                            saveEntityFile(currentPath, sceneId, hierarchy->encodeDraggedSelection(sceneId, entity.entity));
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    };

    // Right pane (or the only pane): path bar, grid, bottom bar.
    auto renderListingPane = [&](bool showDirectories) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        renderPathBreadcrumb(ImVec2(0.0f, breadcrumbHeight));
        ImGui::PopStyleVar();
        renderSearchScopeBar();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::dpi(4.0f), Theme::dpi(1.0f)));
        const float bottomBarHeight = ImGui::GetFrameHeight() + Theme::dpi(4.0f);
        ImGui::PopStyleVar();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, innerPadding);
        // Ctrl+wheel resizes the items instead of scrolling (S25 R25)
        ImGui::BeginChild("FileTableScrollRegion", ImVec2(0, -bottomBarHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGui::GetIO().KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None);
        renderFileListing(showDirectories);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        acceptListingDrops();

        renderBottomBar();
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (effectiveLayout == LayoutType::GRID) {
        renderListingPane(true);
    } else if (effectiveLayout == LayoutType::SPLIT_FILES_ONLY || effectiveLayout == LayoutType::SPLIT) {
        const float splitterWidth = Theme::dpi(4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, innerPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemSpacing);
        ImGui::BeginChild("LeftPanel", ImVec2(Theme::dpi(leftPanelWidth), 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Theme::dpi(2.0f)));
        // Unity's Favorites: saved searches and bookmarks over the folders (S25 R26)
        renderFavorites();
        renderDirectoryTree(project->getProjectPath());
        // a node's menu item, now that the walk (which holds references into its cache) is over
        if (pendingTreeAction) {
            std::function<void()> action = std::move(pendingTreeAction);
            pendingTreeAction = nullptr;
            action();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::SameLine();
        ImGui::InvisibleButton("splitter", ImVec2(splitterWidth, -1));

        // A 1px dark line between the panes; the grab area is wider
        ImVec2 splitterMin = ImGui::GetItemRectMin();
        ImVec2 splitterMax = ImGui::GetItemRectMax();
        const bool splitterHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        const float lineX = std::floor((splitterMin.x + splitterMax.x) * 0.5f);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(lineX - (splitterHot ? Theme::dpi(1.0f) : 0.0f), splitterMin.y),
            ImVec2(lineX + Theme::dpi(1.0f), splitterMax.y),
            ImGui::GetColorU32(splitterHot ? ImGui::GetStyleColorVec4(ImGuiCol_SeparatorActive) : Theme::Colors::ToolbarBorder));

        // Handle splitter interaction
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive()) {
            const float dpi = Theme::dpiScale();
            leftPanelWidth += ImGui::GetIO().MouseDelta.x / dpi;
            const float maxLogical = std::max(100.0f, ImGui::GetWindowWidth() / dpi - 100.0f);
            leftPanelWidth = ImClamp(leftPanelWidth, 100.0f, maxLogical);
        }
        if (ImGui::IsItemDeactivated()) {
            AppSettings::setResourcesLeftPanelWidth(leftPanelWidth);
            AppSettings::saveSettings();
        }
        ImGui::SameLine();
        ImGui::BeginChild("RightPanel", ImVec2(0, 0), ImGuiChildFlags_None);
        renderListingPane(effectiveLayout == LayoutType::SPLIT);
        ImGui::EndChild();
    }
    ImGui::PopStyleVar();


    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
        if (const ImGuiPayload* payload = ImGui::GetDragDropPayload()) {
            if (payload->IsDataType("external_files")) highlightDragAndDrop();
        }
    }

    if (isExternalDragHovering) highlightDragAndDrop();

    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Its keys (Ctrl+C/X/V/A, Delete, F2) are the command catalog's Project context
    // (shortcuts/DefaultCommands.cpp), which calls copySelection, pasteClipboard and the others

    renderSearchPopups();

    // the Inspector shows the importer of the files selected here (S14 R22): after a click or a
    // key in this window, or when the selection changed
    {
        const bool interacted = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsKeyReleased(ImGuiKey_UpArrow) ||
             ImGui::IsKeyReleased(ImGuiKey_DownArrow) || ImGui::IsKeyReleased(ImGuiKey_LeftArrow) ||
             ImGui::IsKeyReleased(ImGuiKey_RightArrow) || ImGui::IsKeyReleased(ImGuiKey_A));
        std::vector<fs::path> selection;
        for (const fs::path& file : getSelectedFiles()) selection.push_back(project->normalizeToProjectRelative(file));
        if (interacted || (selection != inspectedSelection && !project->getInspectedAssets().empty())) {
            project->setInspectedAssets(selection);
        }
        inspectedSelection = std::move(selection);
    }

    uiBuilding.drawn = true;
    uiBuilding.directory = project->normalizeToProjectRelative(currentPath).generic_string();
    uiBuilding.search = searchText;
    uiBuilding.selected = lastSelectedFile;
    for (const fs::path& path : clipboard.paths()) {
        uiBuilding.clipboard.push_back(project->normalizeToProjectRelative(path).generic_string());
    }
    uiBuilding.clipboardCut = clipboard.isCut();
    uiState = std::move(uiBuilding);

    ImGui::End();

    // Evict least-recently-used thumbnail textures (entries used this frame
    // are never evicted)
    enforceThumbnailCacheLimit();

    if (wasOpen && !windowOpen) {
        setOpen(false);
    }

    handleNewItem();
    handleRename();
}

// ---- Assets menu (main menu bar) ------------------------------------------
// Additive block: entry points for App's Assets menu, working on the same
// selection and current folder as this panel's own context menus.

static void focusProjectPanel(editor::ProjectWindow* window) {
    window->setOpen(true);
    ImGui::SetWindowFocus(editor::ProjectWindow::WINDOW_NAME);
}

bool editor::ProjectWindow::hasAssetSelection() const {
    return !selectedFiles.empty();
}

bool editor::ProjectWindow::hasSingleAssetSelection() const {
    return selectedFiles.size() == 1;
}

void editor::ProjectWindow::revealAsset(const fs::path& absolutePath) {
    if (absolutePath.empty() || !fs::exists(absolutePath)) {
        return;
    }
    scanDirectory(absolutePath.parent_path());
    selectedFiles.clear();
    selectedFiles.insert(absolutePath.filename().string());
    lastSelectedFile = absolutePath.filename().string();
    focusProjectPanel(this);
}

void editor::ProjectWindow::assetsCreatePoseAnimation(const fs::path& directory) {
    if (!fs::is_directory(directory)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    focusProjectPanel(this);
    // previews on the Pose Animation window's character, when it has one
    std::string previewModel;
    PoseEditSession& session = Backend::getApp().getPoseAnimationWindow()->getSession();
    if (session.bound() && session.scene()) {
        if (ModelComponent* model = session.scene()->findComponent<ModelComponent>(session.model())) {
            previewModel = model->filename;
        }
    }
    std::string error;
    const std::string created = PoseAnimationWindow::createAsset(project, directory, "New Pose Animation", previewModel, &error);
    if (created.empty()) {
        Backend::getApp().registerAlert("Create Pose Animation", error);
        return;
    }
    selectCreated(directory, FileUtils::pathToUtf8(FileUtils::pathFromUtf8(created).filename()), false);
}

void editor::ProjectWindow::assetsCreateShakePreset(const fs::path& directory) {
    if (!fs::is_directory(directory)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    focusProjectPanel(this);
    camera::ShakePresetFile file;
    file.guid = anim::generateClipGuid();
    file.preset = *camera::findBuiltinShakePreset(camera::defaultNewShakePresetName());
    const fs::path relative = uniqueRelativePath(directory, "New Shake Preset", camera::SHAKE_EXTENSION);
    std::string error;
    if (!ProjectFileWrites::writeUndoable(project, {{project->getProjectPath() / relative, camera::writeShakePreset(file)}}, error)) {
        Backend::getApp().registerAlert("Create Shake Preset", error);
        return;
    }
    selectCreated(directory, relative.filename().string(), true);
}

void editor::ProjectWindow::assetsCreateAnimationClip(const fs::path& directory) {
    if (!fs::is_directory(directory)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    focusProjectPanel(this);
    // Unity's Create > Animation: an empty clip (30 samples, once); the Animation window adds
    // the tracks
    ClipData initial;
    std::string error;
    const fs::path relative = uniqueRelativePath(directory, "New Animation", anim::ANIM_EXTENSION);
    const uint64_t clock = AnimationClipController::clipCommitClock(project);
    if (!AnimationClipDocument::create(project, relative.generic_string(), initial, "Create Animation Clip", &error)) {
        Backend::getApp().registerAlert("Create Animation Clip", error);
        return;
    }
    // a clip file write of the editor UI: Ctrl+Z undoes it from any window (9.R5c)
    AnimationClipController::markClipCommit(clock);
    selectCreated(directory, relative.filename().string(), true);
}

void editor::ProjectWindow::assetsCreateAnimatorAsset(int kindValue, const fs::path& directory) {
    if (!fs::is_directory(directory)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    focusProjectPanel(this);
    const auto kind = static_cast<AnimatorAssetDocument::Kind>(kindValue);
    const char* name = kind == AnimatorAssetDocument::Kind::Controller ? "New Animator Controller"
                     : kind == AnimatorAssetDocument::Kind::Mask ? "New Avatar Mask" : "New Animation Montage";
    const char* extension = kind == AnimatorAssetDocument::Kind::Controller ? ".controller"
                          : kind == AnimatorAssetDocument::Kind::Mask ? ".mask" : ".montage";
    const std::string title = std::string("Create ") + AnimatorAssetDocument::kindName(kind);
    const fs::path relative = uniqueRelativePath(directory, name, extension);
    std::string error;
    if (!AnimatorAssetDocument::create(project, relative.generic_string(), kind, title, &error)) {
        Backend::getApp().registerAlert(title, error);
        return;
    }
    selectCreated(directory, relative.filename().string(), true);
}

void editor::ProjectWindow::assetsCreatePhysicsMaterial(bool preset2D, const fs::path& directory) {
    if (!fs::is_directory(directory)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    focusProjectPanel(this);
    std::string error;
    const fs::path created = PhysicsMaterialAssets::create(project, directory, preset2D ? "New Physics Material 2D" : "New Physics Material",
                                                           preset2D ? PhysicsMaterialData::preset2D() : PhysicsMaterialData::preset3D(), error);
    if (created.empty()) {
        Backend::getApp().registerAlert(preset2D ? "Create Physics Material 2D" : "Create Physics Material", error);
        return;
    }
    selectCreated(directory, created.filename().string(), true);
}

void editor::ProjectWindow::assetsCreateAudioMixer(const fs::path& directory) {
    if (!fs::is_directory(directory)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    focusProjectPanel(this);
    // Unity's Create > Audio Mixer: Master only; the Audio Mixer window adds the groups
    const fs::path relative = uniqueRelativePath(directory, "New Audio Mixer", AudioMixerIO::EXTENSION);
    std::string error;
    if (!AudioMixerDocument::commit(project, project->getProjectPath() / relative, AudioMixerIO::createDefault(), "", &error)) {
        Backend::getApp().registerAlert("Create Audio Mixer", error);
        return;
    }
    selectCreated(directory, relative.filename().string(), true);
}

void editor::ProjectWindow::assetsCreateSoundEvent(const fs::path& directory) {
    if (!fs::is_directory(directory)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    focusProjectPanel(this);
    const fs::path relative = uniqueRelativePath(directory, "New Sound Event", ".soundevent");
    SoundEventDef def;
    def.mode = SoundEventMode::Shuffle;
    std::string error;
    if (!SoundEventDocument::commit(project, project->getProjectPath() / relative, def, "Create Sound Event", "", &error)) {
        Backend::getApp().registerAlert("Create Sound Event", error);
        return;
    }
    selectCreated(directory, relative.filename().string(), true);
}

bool editor::ProjectWindow::hasAudioSelection() const {
    for (const std::string& name : selectedFiles) {
        if (Util::isAudioFile(name)) return true;
    }
    return false;
}

void editor::ProjectWindow::assetsCreateSoundEventFromClips() {
    if (currentPath.empty() || !hasAudioSelection()) {
        return;
    }
    focusProjectPanel(this);
    SoundEventDef def;
    def.mode = SoundEventMode::Shuffle;
    for (const std::string& name : selectedFiles) {
        if (!Util::isAudioFile(name)) continue;
        SoundEventClip clip;
        clip.clip = SoundEventDocument::clipRef(project, (currentPath / name).string(), nullptr);
        def.clips.push_back(std::move(clip));
    }
    const fs::path relative = uniqueRelativePath(currentPath, "New Sound Event", ".soundevent");
    std::string error;
    if (!SoundEventDocument::commit(project, project->getProjectPath() / relative, def, "Create Sound Event from Clips", "", &error)) {
        Backend::getApp().registerAlert("Create Sound Event from Clips", error);
        return;
    }
    scanDirectory(currentPath);
    const std::string fileName = relative.filename().string();
    selectedFiles.clear();
    selectedFiles.insert(fileName);
    lastSelectedFile = fileName;
    updateInspectedAsset(fileName);
    startRename(fileName);
}

void editor::ProjectWindow::processPoseThumbnails() {
    poseThumbnailer.process();
    if (poseThumbnailer.busy() || poseThumbnailQueue.empty() || !Engine::isViewLoaded()) {
        return;
    }
    ThumbnailRequest request = poseThumbnailQueue.front();
    poseThumbnailQueue.pop();
    const fs::path thumbnailPath = project->getThumbnailPath(request.path);
    poseThumbnailer.request(project, request.path, thumbnailPath, [this, request](bool ok) {
        {
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            pendingThumbnailRequests.erase(thumbnailRequestKey(request.path));
            if (request.generation != thumbnailGeneration) return;
        }
        if (!ok) return;   // no preview model: the file keeps its icon
        evictThumbnailTexture(project->getThumbnailPath(request.path).string());
        std::lock_guard<std::mutex> completedLock(completedThumbnailMutex);
        completedThumbnailQueue.push(request);
    });
    poseThumbnailer.process();
}

void editor::ProjectWindow::assetsShowInFileManager() {
    fs::path target = currentPath;
    if (selectedFiles.size() == 1) {
        const fs::path selected = currentPath / *selectedFiles.begin();
        if (fs::is_directory(selected)) {
            target = selected;
        }
    }
    if (!target.empty()) {
        openInFileManager(target);
    }
}

void editor::ProjectWindow::drawTextureCookBadge(ImDrawList* drawList, const FileEntry& file, ImVec2 min, ImVec2 max) {
    if (file.isDirectory) return;
    const fs::path absolute = currentPath / file.name;
    if (!project->isInsideAssetsPath(absolute)) return;
    const char* icon = nullptr;
    ImU32 color = 0;
    std::string tooltip;
    if (file.type == FileType::IMAGE) {
        TextureCookQueue& cooks = TextureCookQueue::get();
        if (cooks.getProject() != project) return;
        switch (cooks.badgeStatus(project->normalizeToAssetsRelative(absolute).generic_string())) {
            case TextureCookStatus::Queued: icon = ICON_FA_HOURGLASS_HALF; color = IM_COL32(200, 200, 200, 230); tooltip = "Texture cook queued"; break;
            case TextureCookStatus::Cooking: icon = ICON_FA_GEAR; color = IM_COL32(120, 200, 255, 240); tooltip = "Cooking the texture"; break;
            case TextureCookStatus::Failed: icon = ICON_FA_TRIANGLE_EXCLAMATION; color = IM_COL32(255, 110, 90, 255); tooltip = "The texture cook failed: the Inspector shows why"; break;
            default: return;
        }
    } else if (isModelSource(file.name)) {
        // models: cooked, cooking or failed (S53 R13, R18)
        ModelCookQueue& cooks = ModelCookQueue::get();
        if (cooks.getProject() != project) return;
        const std::string assetPath = project->normalizeToAssetsRelative(absolute).generic_string();
        switch (cooks.badgeStatus(assetPath)) {
            case ModelCookStatus::Queued: icon = ICON_FA_HOURGLASS_HALF; color = IM_COL32(200, 200, 200, 230); tooltip = "Model cook queued"; break;
            case ModelCookStatus::Cooking: icon = ICON_FA_GEAR; color = IM_COL32(120, 200, 255, 240); tooltip = "Cooking the model"; break;
            case ModelCookStatus::Failed: icon = ICON_FA_TRIANGLE_EXCLAMATION; color = IM_COL32(255, 110, 90, 255); tooltip = "The model cook failed: the Inspector shows why"; break;
            case ModelCookStatus::Cooked: {
                ModelCookState state;
                if (!cooks.getState(assetPath, ModelFamily::Desktop, state) || state.stale) return;
                icon = ICON_FA_CHECK;
                color = IM_COL32(130, 210, 140, 220);
                const ModelCookRecord& record = state.record;
                const double saved = record.sourceBytes > 0 ? 100.0 * (1.0 - (double)record.diskBytes / (double)record.sourceBytes) : 0.0;
                char text[160];
                std::snprintf(text, sizeof(text), "Cooked for desktop (%s): %.0f%% smaller download", meshCompressionLabel(state.resolved.meshCompression), saved);
                tooltip = text;
                // what the loader finds in the source (S53 R5): an error fails export
                const GltfDiagnostic* worst = nullptr;
                for (const GltfDiagnostic& diagnostic : record.diagnostics) {
                    if (diagnostic.cookFixes || diagnostic.severity == GltfDiagnosticSeverity::Info) continue;
                    if (!worst || (diagnostic.severity == GltfDiagnosticSeverity::Error && worst->severity != GltfDiagnosticSeverity::Error)) worst = &diagnostic;
                }
                if (worst) {
                    const bool isError = worst->severity == GltfDiagnosticSeverity::Error;
                    icon = ICON_FA_TRIANGLE_EXCLAMATION;
                    color = isError ? IM_COL32(255, 110, 90, 255) : IM_COL32(240, 190, 80, 255);
                    tooltip = (isError ? "Load error (export fails): " : "Load warning: ") + worst->text();
                }
                break;
            }
            default: {
                // not cooked yet: what its last load found
                const std::vector<GltfDiagnostic> found = ModelLoadDiagnostics::get(absolute.generic_string());
                const auto error = std::find_if(found.begin(), found.end(), [](const GltfDiagnostic& d) {
                    return d.severity == GltfDiagnosticSeverity::Error && !d.cookFixes;
                });
                if (error == found.end()) return;
                icon = ICON_FA_TRIANGLE_EXCLAMATION;
                color = IM_COL32(255, 110, 90, 255);
                tooltip = "Load error (export fails): " + error->text();
                break;
            }
        }
    } else {
        return;
    }
    const float size = std::max(Theme::dpi(9.0f), std::min(max.x - min.x, max.y - min.y) * 0.3f);
    const ImVec2 at(max.x - size - Theme::dpi(1.0f), max.y - size - Theme::dpi(1.0f));
    drawList->AddRectFilled(ImVec2(at.x - Theme::dpi(1.0f), at.y - Theme::dpi(1.0f)), ImVec2(at.x + size + Theme::dpi(1.0f), at.y + size + Theme::dpi(1.0f)),
                            IM_COL32(20, 20, 20, 170), Theme::dpi(2.0f));
    drawList->AddText(ImGui::GetFont(), size, at, color, icon);
    if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + size, at.y + size))) ImGui::SetTooltip("%s", tooltip.c_str());
}

std::vector<fs::path> editor::ProjectWindow::getSelectedFiles() const {
    std::vector<fs::path> files;
    // the names of the current folder's selected entries
    for (const std::string& file : selectedFiles) files.push_back(currentPath / file);
    std::sort(files.begin(), files.end());
    return files;
}

void editor::ProjectWindow::assetsOpenSelected() {
    if (selectedFiles.size() != 1) {
        return;
    }
    const std::string fileName = *selectedFiles.begin();
    auto it = std::find_if(files.begin(), files.end(),
        [&](const FileEntry& entry) { return entry.name == fileName; });
    if (it == files.end()) {
        return;
    }
    const fs::path filePath = currentPath / fileName;
    if (it->isDirectory) {
        scanDirectory(filePath);
        selectedFiles.clear();
        focusProjectPanel(this);
    } else {
        openFile(*it, filePath);
    }
}

void editor::ProjectWindow::openFile(const FileEntry& file, const fs::path& filePath) {
    if (file.type == FileType::IMAGE) {
        imageViewerWindow->openFile(filePath);
    } else if (file.type == FileType::SCENE) {
        project->openScene(filePath);
    } else if (file.type == FileType::POSE_ANIMATION) {
        Backend::getApp().getPoseAnimationWindow()->openFromAsset(filePath.string());
    } else if (file.type == FileType::VERTEX_ANIMATION) {
        // the baker with its recipe once it is there; the Inspector's asset view always
        selectedFiles.clear();
        selectedFiles.insert(file.name);
        lastSelectedFile = file.name;
        project->setInspectedAsset(filePath.lexically_normal());
        if (InspectorWindow* inspector = Backend::getApp().getInspectorWindow()) {
            inspector->setOpen(true);
        }
        if (vatAppHooks().openBaker) {
            vatAppHooks().openBaker(project, filePath, VatBakerSource::VatAsset);
        }
    } else if (file.type == FileType::ANIMATOR_ASSET && Util::isAnimatorControllerFile(filePath.string()) &&
               animatorEditorHooks().openController) {
        // the Animator window on the controller alone (it binds to an Animator once one is selected)
        const std::string controller = project->isInsideAssetsPath(filePath)
            ? project->normalizeToAssetsRelative(filePath).generic_string()
            : filePath.generic_string();
        animatorEditorHooks().openController(project, 0, NULL_ENTITY, controller);
    } else if (file.type == FileType::ANIMATOR_ASSET) {
        // a mask or a montage: the Inspector edits it
        updateInspectedAsset(file.name);
    } else if (file.type == FileType::ANIMATION_CLIP && animationClipInspectorHooks().openClip) {
        // the Animation window, target-less (the clip's preview model), as the component stores it
        const std::string clip = project->isInsideAssetsPath(filePath)
            ? project->normalizeToAssetsRelative(filePath).generic_string()
            : filePath.generic_string();
        animationClipInspectorHooks().openClip(project, 0, NULL_ENTITY, clip);
    } else if (Util::isPrefabFile(filePath.string())) {
        editor::getEditorHost().openPrefabAsset(filePath);  // Unity: Prefab Mode
    } else if (filePath.extension() == ".inputactions") {
        editor::getEditorHost().openInputActions(filePath);
    } else if (file.type == FileType::COLOR_LUT) {
        assignColorLut(filePath);  // the open scene's Color Grading LUT
    } else if (file.type == FileType::MATERIAL || Util::isParticleEffectFile(filePath.string())) {
        // Unity: selected and shown in the Inspector (the context menu has Open as Text)
        selectedFiles.clear();
        selectedFiles.insert(file.name);
        lastSelectedFile = file.name;
        project->setInspectedAsset(filePath.lexically_normal());
        if (InspectorWindow* inspector = Backend::getApp().getInspectorWindow()) {
            inspector->showPickedParticleAgain();
            inspector->requestFocus();
        }
    } else if (filePath.extension() == AudioMixerIO::EXTENSION) {
        editor::getEditorHost().openAudioMixerWindow(filePath);
    } else {
        // text files (an .anim only when the clip hooks are unset: its JSON text)
        codeEditor->openFile(filePath.string(), true);
    }
}

void editor::ProjectWindow::updateInspectedAsset(const std::string& clickedName) {
    fs::path inspected;
    if (!clickedName.empty() && selectedFiles.count(clickedName)) {
        inspected = currentPath / clickedName;
    } else if (selectedFiles.size() == 1) {
        inspected = currentPath / *selectedFiles.begin();
    }
    std::error_code ec;
    if (inspected.empty() || fs::is_directory(inspected, ec)) {
        project->clearInspectedAsset();
    } else {
        project->setInspectedAsset(inspected.lexically_normal());
    }
}

void editor::ProjectWindow::assetsRenameSelected() {
    if (selectedFiles.size() != 1) {
        return;
    }
    focusProjectPanel(this);
    startRename(*selectedFiles.begin());
}

void editor::ProjectWindow::assetsDeleteSelected() {
    if (selectedFiles.empty()) {
        return;
    }
    focusProjectPanel(this);
    showDeleteConfirmation = true;
}

void editor::ProjectWindow::assetsImport() {
    if (currentPath.empty()) {
        return;
    }
    focusProjectPanel(this);
    importExternalPaths(editor::FileDialogs::openFileDialogMultiple(), createTargetDirectory());
}

void editor::ProjectWindow::assetsRefresh() {
    refreshCurrentDirectory();
}

std::vector<fs::path> editor::ProjectWindow::getSelectedPaths() const{
    std::vector<fs::path> paths;
    for (const std::string& name : selectedFiles){
        paths.push_back((currentPath / name).lexically_normal());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

// ---- the Project context's commands (the catalog's project.*) ---------------

void editor::ProjectWindow::copySelection(bool cut) {
    copySelectedFiles(cut);
}

bool editor::ProjectWindow::canPasteClipboard() const {
    return !clipboard.empty();
}

void editor::ProjectWindow::pasteClipboard() {
    if (!clipboard.empty()) pasteFiles(listingFolder());
}

void editor::ProjectWindow::selectAllInListing() {
    // Directories are only selectable when the listing shows them
    const bool listingShowsDirectories = lastLayout != LayoutType::SPLIT_FILES_ONLY;
    selectedFiles.clear();
    for (const auto& file : files) {
        if (listingShowsDirectories || !file.isDirectory) selectedFiles.insert(file.name);
    }
    if (!files.empty()) lastSelectedFile = files.back().name;
}

void editor::ProjectWindow::requestDeleteSelection() {
    // the confirmation is drawn by renderFileListing
    if (!selectedFiles.empty()) showDeleteConfirmation = true;
}

void editor::ProjectWindow::renameSelectedOrHovered() {
    // the single selected file, or the hovered one when nothing is selected
    std::string target = hoveredFile;
    if (selectedFiles.size() == 1) {
        const std::string& selected = *selectedFiles.begin();
        if (std::any_of(files.begin(), files.end(), [&](const FileEntry& f){ return f.name == selected; })) target = selected;
    }
    if (!target.empty()) startRename(target);
}

// ---- Create (project-window spec 6.6) ------------------------------------

void editor::ProjectWindow::createAsset(AssetCreateKind kind, const fs::path& directory) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        Backend::getApp().registerAlert("Create", "The folder " + FileUtils::pathToUtf8(directory) + " is gone.");
        return;
    }
    if (assetCreateItem(kind).nameFirst) {
        startNewItem(kind, directory);
        return;
    }
    switch (kind) {
        case AssetCreateKind::AnimationClip: assetsCreateAnimationClip(directory); break;
        case AssetCreateKind::AnimatorController: assetsCreateAnimatorAsset((int)AnimatorAssetDocument::Kind::Controller, directory); break;
        case AssetCreateKind::AvatarMask: assetsCreateAnimatorAsset((int)AnimatorAssetDocument::Kind::Mask, directory); break;
        case AssetCreateKind::AnimationMontage: assetsCreateAnimatorAsset((int)AnimatorAssetDocument::Kind::Montage, directory); break;
        case AssetCreateKind::PoseAnimation: assetsCreatePoseAnimation(directory); break;
        case AssetCreateKind::AudioMixer: assetsCreateAudioMixer(directory); break;
        case AssetCreateKind::SoundEvent: assetsCreateSoundEvent(directory); break;
        case AssetCreateKind::PlaceholderSound: assetsCreatePlaceholderSound(directory); break;
        case AssetCreateKind::PhysicsMaterial: assetsCreatePhysicsMaterial(false, directory); break;
        case AssetCreateKind::PhysicsMaterial2D: assetsCreatePhysicsMaterial(true, directory); break;
        case AssetCreateKind::ShakePreset: assetsCreateShakePreset(directory); break;
        default: break;
    }
}

namespace {

    // the extensions a named item's files take
    const char* newItemExtension(editor::AssetCreateKind kind) {
        switch (kind) {
            case editor::AssetCreateKind::LuaScript: return ".lua";
            case editor::AssetCreateKind::CppSource: return ".cpp";
            case editor::AssetCreateKind::CppHeader: return ".h";
            case editor::AssetCreateKind::HtmlDocument: return ".html";
            default: return "";
        }
    }

    bool identifierKind(editor::AssetCreateKind kind) {
        return kind == editor::AssetCreateKind::LuaScript || kind == editor::AssetCreateKind::CppScript;
    }

    // the files a named item writes into `directory` (a folder: itself)
    std::vector<fs::path> newItemFiles(editor::AssetCreateKind kind, const fs::path& directory, const std::string& name) {
        switch (kind) {
            case editor::AssetCreateKind::Folder: return {directory / editor::FileUtils::pathFromUtf8(name)};
            case editor::AssetCreateKind::CppScript:
                return {directory / editor::FileUtils::pathFromUtf8(name + ".h"), directory / editor::FileUtils::pathFromUtf8(name + ".cpp")};
            case editor::AssetCreateKind::HtmlDocument:
                return {directory / editor::FileUtils::pathFromUtf8(name + ".html"), directory / editor::FileUtils::pathFromUtf8(name + ".css")};
            default: return {directory / editor::FileUtils::pathFromUtf8(name + newItemExtension(kind))};
        }
    }

    // the name typed without the extension its kind adds ("Goblin.lua" = "Goblin")
    std::string newItemName(editor::AssetCreateKind kind, std::string name) {
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        std::vector<std::string> extensions;
        if (kind == editor::AssetCreateKind::CppScript) extensions = {".h", ".cpp"};
        else if (kind != editor::AssetCreateKind::Folder) extensions = {newItemExtension(kind)};
        if (kind == editor::AssetCreateKind::HtmlDocument) extensions.push_back(".css");
        for (const std::string& extension : extensions) {
            if (!extension.empty() && name.size() > extension.size() &&
                name.compare(name.size() - extension.size(), extension.size(), extension) == 0) {
                name.erase(name.size() - extension.size());
                break;
            }
        }
        return name;
    }

    // why `name` can't be the name of a new item in `directory` ("" when it can)
    std::string newItemProblem(editor::AssetCreateKind kind, const fs::path& directory, const std::string& name) {
        static const std::unordered_set<std::string> luaKeywords = {
            "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
            "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
        if (name.empty()) return "Enter a name.";
        if (identifierKind(kind)) {
            if (editor::Factory::toIdentifier(name) != name) return "A script's name is its class: letters, digits and _, not starting with a digit.";
            if (kind == editor::AssetCreateKind::LuaScript && luaKeywords.count(name)) return "\"" + name + "\" is a Lua keyword.";
        } else {
            if (name == "." || name == ".." || name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
                return "A name can't be . or .. or hold / \\ : * ? \" < > |";
            }
        }
        for (const fs::path& file : newItemFiles(kind, directory, name)) {
            std::error_code ec;
            if (fs::exists(fs::symlink_status(file, ec))) {
                return editor::FileUtils::pathToUtf8(file.filename()) + " already exists here.";
            }
        }
        return std::string();
    }

}

void editor::ProjectWindow::startNewItem(AssetCreateKind kind, const fs::path& directory) {
    focusProjectPanel(this);
    pendingNewItem = PendingNewItem();
    pendingNewItem.kind = kind;
    pendingNewItem.directory = directory;
    pendingNewItem.open = true;
    pendingNewItem.focus = true;
    // a free default, selected for typing over (Unity's "New Folder", "NewBehaviourScript")
    const char* base = kind == AssetCreateKind::Folder ? "New Folder"
                     : kind == AssetCreateKind::LuaScript ? "NewLuaScript"
                     : kind == AssetCreateKind::CppScript ? "NewScript"
                     : kind == AssetCreateKind::CppSource ? "NewSource"
                     : kind == AssetCreateKind::CppHeader ? "NewHeader" : "NewDocument";
    std::string name = base;
    for (int counter = 1; !newItemProblem(kind, directory, name).empty() && counter < 10000; counter++) {
        name = std::string(base) + (kind == AssetCreateKind::Folder ? " " : "") + std::to_string(counter);
    }
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", name.c_str());
}

bool editor::ProjectWindow::createNamedItem(AssetCreateKind kind, const fs::path& directory, const std::string& name, std::string& error) {
    if (kind == AssetCreateKind::Folder) {
        if (!project->getProjectCommandHistory()->addCommand(new CreateDirCmd(name, directory.string()))) {
            error = "The folder could not be created (permissions, or a file of that name).";
            return false;
        }
        return true;
    }
    const std::vector<fs::path> files = newItemFiles(kind, directory, name);
    std::vector<WriteFilesCmd::Item> items;
    std::string step;
    switch (kind) {
        case AssetCreateKind::LuaScript:
            items.push_back({files[0], ScriptTemplates::lua(name)});
            step = "Create Lua Script " + name + ".lua";
            break;
        case AssetCreateKind::CppScript: {
            const ScriptTemplates::Files templates = ScriptTemplates::cppScript(name, name + ".h");
            items.push_back({files[0], templates.header});
            items.push_back({files[1], templates.source});
            step = "Create C++ Script " + name;
            break;
        }
        case AssetCreateKind::CppSource: {
            std::error_code ec;
            const bool header = fs::is_regular_file(directory / FileUtils::pathFromUtf8(name + ".h"), ec);
            items.push_back({files[0], ScriptTemplates::cppSource(name, header)});
            step = "Create File " + name + ".cpp";
            break;
        }
        case AssetCreateKind::CppHeader:
            items.push_back({files[0], ScriptTemplates::cppHeader(name)});
            step = "Create File " + name + ".h";
            break;
        case AssetCreateKind::HtmlDocument:
            items.push_back({files[0], HtmlUITemplates::documentText(name)});
            items.push_back({files[1], HtmlUITemplates::styleSheetText()});
            step = "Create HTML Document " + name;
            break;
        default:
            return false;
    }
    Project* owner = project;
    CodeEditor* editor = codeEditor;
    const bool lua = kind == AssetCreateKind::LuaScript;
    // after the write, its undo and its redo: the editor refreshes, Lua stubs follow, and a tab of a
    // file the undo removed closes when it holds no edits
    auto afterChange = [owner, editor, lua](const std::vector<fs::path>& changed) {
        ProjectFileWrites::filesWritten(owner, changed);
        if (lua) Generator::writeLuaScriptStubs(owner->getProjectPath(), owner->getProjectInternalPath(), changed);
        for (const fs::path& path : changed) {
            std::error_code ec;
            if (!fs::exists(path, ec) && editor && editor->isFileOpen(path.string()) && !editor->isFileModified(path.string())) {
                editor->closeFile(path.string());
            }
        }
    };
    if (!ProjectFileWrites::writeUndoable(project, std::move(items), error, afterChange, nullptr, {}, false, step)) {
        return false;
    }
    if (!project->isInsideAssetsPath(files[0]) && kind == AssetCreateKind::HtmlDocument) {
        Out::warning("[HTML UI] %s is outside the assets directory: a UI Document can only use files under %s",
                     FileUtils::pathToUtf8(files[0].filename()).c_str(), project->getAssetsPath().string().c_str());
    }
    return true;
}

void editor::ProjectWindow::handleNewItem() {
    if (!pendingNewItem.open) return;
    const AssetCreateItem& item = assetCreateItem(pendingNewItem.kind);
    const std::string title = std::string("New ") + item.label + "###ProjectNewItem";
    if (!ImGui::IsPopupOpen("###ProjectNewItem")) {
        ImGui::OpenPopup(title.c_str());
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    if (!EditorModal::begin(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const AssetCreateKind kind = pendingNewItem.kind;
    ImGui::TextUnformatted(kind == AssetCreateKind::Folder ? "Folder name:" : identifierKind(kind) ? "Class name:" : "File name:");
    ImGui::SetNextItemWidth(Theme::dpi(320.0f));
    if (pendingNewItem.focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    if (identifierKind(kind)) flags |= ImGuiInputTextFlags_CallbackCharFilter;
    const bool enter = EditorModal::inputText("##newItemName", "name", nameBuffer, sizeof(nameBuffer), flags,
                                              identifierKind(kind) ? ScriptCreateDialog::classNameCharFilter : nullptr);
    const std::string where = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(pendingNewItem.directory));
    EditorModal::message(std::string("New ") + item.label + " \"" + nameBuffer + "\" in " + where);
    UIProbe::rect("project.newItem.name", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    pendingNewItem.focus = false;
    ImGui::TextDisabled("in %s", where.c_str());
    // a C++ file outside the Script Directories is compiled only when attached as a script
    bool inScriptDirs = false;
    for (const fs::path& scriptDir : project->getScriptDirs()) {
        const fs::path root = scriptDir.is_absolute() ? scriptDir : project->getProjectPath() / scriptDir;
        if (safewrite::isAtOrBelow(pendingNewItem.directory.lexically_normal(), root.lexically_normal())) inScriptDirs = true;
    }
    if ((kind == AssetCreateKind::CppScript || kind == AssetCreateKind::CppSource) && !inScriptDirs) {
        ImGui::TextDisabled("Outside the Script Directories: compiled only when attached as a script.");
    }
    if (!pendingNewItem.error.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Theme::dpi(320.0f));
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.0f), "%s", pendingNewItem.error.c_str());
        ImGui::PopTextWrapPos();
        EditorModal::error(pendingNewItem.error);
    }
    const float buttonWidth = Theme::dpi(120.0f);
    const bool create = EditorModal::button("Create", "yes", ImVec2(buttonWidth, 0)) || enter;
    ImGui::SameLine();
    const bool cancel = EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0)) || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape));
    if (create) {
        const std::string name = newItemName(kind, nameBuffer);
        pendingNewItem.error = newItemProblem(kind, pendingNewItem.directory, name);
        std::string error;
        if (pendingNewItem.error.empty() && !createNamedItem(kind, pendingNewItem.directory, name, error)) {
            pendingNewItem.error = error;
        }
        if (pendingNewItem.error.empty()) {
            const fs::path first = newItemFiles(kind, pendingNewItem.directory, name).front();
            const fs::path directory = pendingNewItem.directory;
            pendingNewItem = PendingNewItem();
            ImGui::CloseCurrentPopup();
            selectCreated(directory, FileUtils::pathToUtf8(first.filename()), false);
        } else {
            pendingNewItem.focus = true;
        }
    } else if (cancel) {
        pendingNewItem = PendingNewItem();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void editor::ProjectWindow::selectCreated(const fs::path& directory, const std::string& fileName, bool rename) {
    auto finish = [this, directory, fileName, rename]() {
        // Unity shows what it made: the folder it is in, the new item selected
        if (searchText.empty() && directory.lexically_normal() != currentPath.lexically_normal()) {
            scanDirectory(directory);
        } else {
            scanDirectory(currentPath);
        }
        const fs::path relative = (directory / FileUtils::pathFromUtf8(fileName)).lexically_normal().lexically_relative(currentPath.lexically_normal());
        const std::string name = FileUtils::pathToGenericUtf8(relative);
        selectedFiles.clear();
        selectedFiles.insert(name);
        lastSelectedFile = name;
        updateInspectedAsset(name);
        pingAsset(name);
        if (rename) {
            startRename(name);
        }
    };
    if (listingIterating) {
        afterListing = std::move(finish);
    } else {
        finish();
    }
}

void editor::ProjectWindow::renderTreeContextMenu(const fs::path& folder) {
    const bool isProjectRoot = folder.lexically_normal() == project->getProjectPath().lexically_normal();
    const bool createOpen = ImGui::BeginMenu(ICON_FA_PLUS " Create");
    uiBuilding.contextMenu.push_back({"Create", lastItemRect()});
    if (createOpen) {
        // the items run at once (they write, or open the name dialog): no listing is iterated here
        renderCreateItems(folder);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    auto keys = [](const char* id) { return CommandCatalog::editor().menuShortcutText(id); };
    const std::string copyKeys = keys("project.copy"), cutKeys = keys("project.cut"), pasteKeys = keys("project.paste"),
                      duplicateKeys = keys("project.duplicate");
    const bool copyItem = ImGui::MenuItem(ICON_FA_COPY " Copy", copyKeys.empty() ? nullptr : copyKeys.c_str(), false, !isProjectRoot);
    uiBuilding.contextMenu.push_back({"Copy", lastItemRect()});
    const bool cutItem = ImGui::MenuItem(ICON_FA_SCISSORS " Cut", cutKeys.empty() ? nullptr : cutKeys.c_str(), false,
                                         !isProjectRoot && rootRefusal(folder).empty());
    uiBuilding.contextMenu.push_back({"Cut", lastItemRect()});
    const bool pasteItem = ImGui::MenuItem(ICON_FA_PASTE " Paste", pasteKeys.empty() ? nullptr : pasteKeys.c_str(), false, !clipboard.empty());
    uiBuilding.contextMenu.push_back({"Paste", lastItemRect()});
    const bool duplicateItem = ImGui::MenuItem(ICON_FA_CLONE " Duplicate", duplicateKeys.empty() ? nullptr : duplicateKeys.c_str(), false, !isProjectRoot);
    uiBuilding.contextMenu.push_back({"Duplicate", lastItemRect()});
    ImGui::Separator();
    const bool showItem = ImGui::MenuItem(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE " Show in File Manager");
    if (copyItem || cutItem) {
        const bool cut = cutItem;
        pendingTreeAction = [this, folder, cut]() { copyPaths({folder}, cut); };
    } else if (pasteItem) {
        pendingTreeAction = [this, folder]() { pasteFiles(folder); };
    } else if (duplicateItem) {
        pendingTreeAction = [this, folder]() {
            FileCopyData entry;
            entry.filename = folder.filename();
            entry.sourceDirectory = folder.parent_path();
            entry.targetDirectory = entry.sourceDirectory;
            auto report = std::make_shared<CopyFileCmd::Report>();
            auto* command = new CopyFileCmd(project, std::vector<FileCopyData>{entry}, true);
            command->setDuplicate(true);
            command->reportTo(report);
            if (!project->getProjectCommandHistory()->addCommand(command) && !report->error.empty()) {
                Backend::getApp().registerAlert("Duplicate", report->error);
            }
            scanDirectory(currentPath);
            selectLanded(report->landed, folder.parent_path());
        };
    } else if (showItem) {
        openInFileManager(folder);
    }
}

void editor::ProjectWindow::pingAsset(const std::string& projectRelative) {
    pingName = fs::path(projectRelative).filename().string();
    pingUntil = ImGui::GetTime() + 1.5;
    pingScroll = true;
}
