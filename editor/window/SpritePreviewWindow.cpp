// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpritePreviewWindow.h"

#include "AppSettings.h"
#include "command/UndoTimeline.h"
#include "pixelart/draw/SpriteDocCache.h"
#include "pixelart/draw/SpriteEditEvents.h"
#include "widget/ColorField.h"

#include "App.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "Workspace.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/aseprite/AsepriteImport.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "shortcuts/CommandCatalog.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "util/UserFiles.h"
#include "window/ProjectWindow.h"

#include "jobs/JobSystem.h"
#include "sprite/SpriteSheetPool.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace ase = jasperengine::editor::pixelart::aseprite;

static double spritePreviewNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct SpritePreviewWindow::Loaded {
    std::shared_ptr<const ase::Document> document;
    std::string error;                 // why the file could not be read
    // the import's pages (RGBA8), in the sheet's order; null where a PNG did not decode
    std::vector<std::shared_ptr<const pixelart::PixelImage>> pages;
    // the document's tags as the sheet holds them (one playback rule for both views)
    std::vector<SpriteTag> tags;
};

namespace {

    // the zoom steps: integers only, so a sprite pixel is always a whole number of screen pixels
    constexpr int kSpritePreviewZooms[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32};
    constexpr float kSpritePreviewSpeeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    // the composed Source frames kept on the GPU (16 M pixels, 64 MB)
    constexpr size_t kSpritePreviewFrameBudget = 16u * 1024u * 1024u;

    bool spritePreviewLeaf(const ase::Layer& layer) {
        return !layer.isGroup() && !layer.reference() && static_cast<uint16_t>(layer.type) <= static_cast<uint16_t>(ase::LayerType::Tilemap);
    }

    bool spritePreviewUnder(const ase::Document& doc, int layer, int group) {
        const int n = static_cast<int>(doc.layers.size());
        for (int p = layer, guard = 0; p >= 0 && guard <= n; p = doc.layers[static_cast<size_t>(p)].parent, guard++) {
            if (p == group) return true;
        }
        return false;
    }

    ImU32 spritePreviewColor(const Vector4& c, float alpha = 1.0f) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
    }

    // a sprite's pixels as squares at any zoom: the backend's nearest sampler for this image only
    // (ImGui draws with a linear one; the texture's own sampler is the engine's, not ImGui's)
    void spritePreviewImage(ImDrawList* draw, ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1),
                            ImU32 tint = IM_COL32_WHITE) {
        const ImGuiPlatformIO& io = ImGui::GetPlatformIO();
        if (io.DrawCallback_SetSamplerNearest) draw->AddCallback(io.DrawCallback_SetSamplerNearest, nullptr);
        draw->AddImage(texture, a, b, uv0, uv1, tint);
        if (io.DrawCallback_SetSamplerLinear) draw->AddCallback(io.DrawCallback_SetSamplerLinear, nullptr);
    }

    bool spritePreviewUpload(Texture& texture, int& width, int& height, const editor::pixelart::PixelImage& image) {
        if (image.empty()) return false;
        static uint64_t counter = 0;
        // TextureData frees what it is given (stbi_image_free = free)
        const size_t bytes = image.pixels().size();
        void* pixels = std::malloc(bytes);
        if (!pixels) return false;
        std::memcpy(pixels, image.pixels().data(), bytes);
        TextureData data(image.width(), image.height(), static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
        texture.setData("editor:sprite-preview:" + std::to_string(++counter), data);
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        TextureRender* render = texture.getRender();
        texture.releaseData();   // the GPU has it now (frees `pixels`)
        if (!render || !render->isCreated()) {
            texture.destroy();
            return false;
        }
        width = image.width();
        height = image.height();
        return true;
    }

    fs::path spritePreviewChoiceFile(const fs::path& projectPath) {
        return Workspace::getUserDirectory(projectPath) / "sprite-preview.yaml";
    }

}

SpritePreviewWindow::SpritePreviewWindow(Project* project, ProjectWindow* projectWindow)
    : project(project), projectWindow(projectWindow) {
    pixelart::draw::SpriteEditEvents::addListener(this, [this](const pixelart::draw::SpriteEdited& event) { onSpriteEdited(event); },
        [this](const pixelart::draw::SpriteExternalSave& event) {
            const fs::path shown = (this->project->getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal();
            if (!assetPath.empty() && (this->project->getProjectPath() / FileUtils::pathFromUtf8(event.path)).lexically_normal() == shown) externalSaveNotice = event.message;
        });
}

SpritePreviewWindow::~SpritePreviewWindow() {
    pixelart::draw::SpriteEditEvents::removeListener(this);
}

void SpritePreviewWindow::onSpriteEdited(const pixelart::draw::SpriteEdited& event) {
    if (assetPath.empty()) return;
    if ((project->getProjectPath() / FileUtils::pathFromUtf8(event.path)).lexically_normal() != (project->getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal()) return;
    adoptDocumentFromSession();
    AgentEdit edit;
    edit.rects = event.rects;
    edit.by = event.by;
    edit.summary = event.summary;
    edit.revision = event.revision;
    edit.at = spritePreviewNow();
    edit.showLine = event.origin == "agent";
    agentEdit = std::move(edit);
    if (event.origin == "agent") externalSaveNotice.clear();
    Backend::getApp().requestRedraw();
}

// the session already holds the edited document: the Source view shows it without a file read (R41)
void SpritePreviewWindow::adoptDocumentFromSession() {
    pixelart::PixelProblem problem;
    std::shared_ptr<pixelart::draw::SpriteSession> session = pixelart::draw::SpriteDocCache::get().openAbsolute(project, project->getAssetsPath() / FileUtils::pathFromUtf8(assetPath), problem);
    if (!session) return;
    auto next = std::make_shared<Loaded>();
    auto document = std::make_shared<ase::Document>(session->sprite.doc);
    document->provenance.reset();
    next->document = document;
    next->tags = loaded ? loaded->tags : std::vector<SpriteTag>();
    if (loaded) next->pages = loaded->pages;
    loaded = std::move(next);
    retireAll();
    const int frames = frameCount();
    frame = frames > 0 ? std::clamp(frame, 0, frames - 1) : 0;
}

void SpritePreviewWindow::open() {
    setOpen(true);
    focusRequested = true;
}

void SpritePreviewWindow::open(const std::string& path) {
    fs::path given = FileUtils::pathFromUtf8(path);
    if (given.is_absolute()) given = given.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal());
    const std::string normalized = FileUtils::pathToGenericUtf8(given.lexically_normal());
    if (normalized != assetPath) {
        clearFile();
        assetPath = normalized;
    }
    // what the Project window shows now is not a new selection to follow
    if (projectWindow) seenSelection = projectWindow->getSelectedFiles();
    open();
}

void SpritePreviewWindow::setOpen(bool openIt) {
    windowOpen = openIt;
    if (!openIt) {
        focused = false;
        playing = false;
    }
}

bool SpritePreviewWindow::isOpen() const {
    return windowOpen;
}

bool SpritePreviewWindow::isFocused() const {
    return windowOpen && focused;
}

void SpritePreviewWindow::resetForProjectSwitch() {
    clearFile();
    assetPath.clear();
    seenSelection.clear();
    compare = false;
    compareStoredFor.clear();
    compareSheetPath.clear();
    compareFrame = 0;
    compareCandidates.clear();
    compareCandidatesAt = -1.0;
    compareUploadedKey.clear();
    compareSheet.reset();
    retire(compareGpu);
}

void SpritePreviewWindow::bindCommands(CommandCatalog& catalog, std::function<bool()> keysHere) {
    auto bind = [&](const char* id, std::function<void()> run) {
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        def.run = [run](const CommandArgs&) { run(); };
        def.enabled = [this]() { return windowOpen && frameCount() > 0; };
        def.keyGate = [keysHere]() { return !ImGui::GetIO().WantTextInput && (!keysHere || keysHere()); };
        catalog.add(std::move(def));
    };
    bind("spritePreview.playPause", [this]() { togglePlay(); });
    bind("spritePreview.previousFrame", [this]() { stepFrame(-1); });
    bind("spritePreview.nextFrame", [this]() { stepFrame(1); });
    bind("spritePreview.firstFrame", [this]() { firstFrame(); });
    bind("spritePreview.lastFrame", [this]() { lastFrame(); });
    bind("spritePreview.zoomIn", [this]() { zoomBy(1); });
    bind("spritePreview.zoomOut", [this]() { zoomBy(-1); });
}

// ---- upkeep

void SpritePreviewWindow::clearFile() {
    record.reset();
    recordKey.clear();
    recordSeen = false;
    loadTask = jobs::Task<std::shared_ptr<Loaded>>();
    loaded.reset();
    sheet.reset();
    sheetGeneration = 0;
    frame = 0;
    tag = -1;
    playing = false;
    playIndex = 0;
    elapsedMs = 0.0;
    spriteGroup = 0;
    layerToggles.clear();
    togglesRevision++;
    retireAll();
    fitRequested = true;
    pan = ImVec2(0.0f, 0.0f);
    statusMessage.clear();
    agentEdit.reset();
    externalSaveNotice.clear();
    statusIsError = false;
}

void SpritePreviewWindow::retire(GpuImage& gpu) {
    if (!gpu.texture.empty()) retired.push_back(gpu.texture);
    gpu = GpuImage();
}

void SpritePreviewWindow::retireAll() {
    for (GpuImage& gpu : pageGpu) retire(gpu);
    pageGpu.clear();
    for (auto& [index, gpu] : sourceGpu) retire(gpu);
    sourceGpu.clear();
    sourceOrder.clear();
    sourceGpuDocument = nullptr;
}

void SpritePreviewWindow::releaseRetired() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
}

void SpritePreviewWindow::followProjectSelection() {
    if (!projectWindow) return;
    std::vector<fs::path> selection = projectWindow->getSelectedFiles();
    if (selection == seenSelection) return;
    seenSelection = selection;
    if (selection.size() != 1 || !SpriteSheetPool::isAsepritePath(FileUtils::pathToUtf8(selection[0]))) return;
    const fs::path relative = selection[0].lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal());
    const std::string path = FileUtils::pathToGenericUtf8(relative);
    if (path.empty() || path.rfind("..", 0) == 0 || path == assetPath) return;
    clearFile();
    assetPath = path;
}

void SpritePreviewWindow::followImport() {
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();
    std::shared_ptr<const ase::ImportRecord> now = importer.record(assetPath);
    const std::string key = now ? now->key : std::string();
    if (!recordSeen || now.get() != record.get() || key != recordKey) {
        // a first look, or a hot reload swapped a new import in: read the file and its pages again
        recordSeen = true;
        record = now;
        recordKey = key;
        sheet.reset();
        sheetGeneration = 0;
        if (record && record->ok) {
            sheet = SpriteSheetPool::get(assetPath);
            sheetGeneration = SpriteSheetPool::generation(assetPath);
        }
        startLoad();
        return;
    }
    if (record && record->ok && SpriteSheetPool::generation(assetPath) != sheetGeneration) {
        sheet = SpriteSheetPool::get(assetPath);
        sheetGeneration = SpriteSheetPool::generation(assetPath);
    }
}

void SpritePreviewWindow::startLoad() {
    const fs::path file = project->getAssetsPath() / FileUtils::pathFromUtf8(assetPath);
    std::vector<fs::path> pageFiles;
    if (record && record->ok) {
        for (size_t p = 0; p < record->pages.size(); p++) pageFiles.push_back(record->pageFile(p));
    }
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "sprite-preview-load";
    // a load still running for an older import finishes unseen: its handle is dropped here
    loadTask = jobs::submit([file, pageFiles]() {
        struct WakeEditor {
            ~WakeEditor() { getEditorHost().enqueueMainThreadTask([]() {}); }
        } wake;
        auto out = std::make_shared<Loaded>();
        ase::ReadOptions read;
        read.provenance = false;
        ase::ReadResult result = ase::readFile(file, read);
        if (result.ok()) {
            out->document = std::make_shared<const ase::Document>(std::move(*result.document));
            const int frames = out->document->frameCount();
            for (const ase::Tag& t : out->document->tags) {
                SpriteTag tag;
                tag.name = t.name;
                tag.from = std::clamp(t.from, 0, std::max(0, frames - 1));
                tag.to = std::clamp(t.to, tag.from, std::max(0, frames - 1));
                tag.direction = static_cast<SpriteTagDirection>(t.direction);
                tag.repeat = t.repeat;
                const pixelart::PixelRgba c = t.color();
                tag.color = Vector4(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
                out->tags.push_back(std::move(tag));
            }
        } else {
            out->error = result.error.message;
        }
        for (const fs::path& page : pageFiles) {
            std::optional<pixelart::PixelImage> image = pixelart::PixelImage::loadPng(page, nullptr);
            out->pages.push_back(image ? std::make_shared<const pixelart::PixelImage>(std::move(*image)) : nullptr);
        }
        return out;
    }, options);
}

void SpritePreviewWindow::adoptLoad() {
    if (!loadTask.isValid() || !loadTask.isReady()) return;
    if (loadTask.status() == jobs::TaskStatus::Completed) {
        try {
            loaded = loadTask.get();
        } catch (...) {
            loaded.reset();
        }
    }
    loadTask = jobs::Task<std::shared_ptr<Loaded>>();
    retireAll();
    const int frames = frameCount();
    frame = frames > 0 ? std::clamp(frame, 0, frames - 1) : 0;
    if (tag >= static_cast<int>(tags().size())) tag = -1;
    spriteGroup = std::clamp(spriteGroup, 0, std::max(0, spriteGroups() - 1));
}

// ---- model

int SpritePreviewWindow::frameCount() const {
    if (loaded && loaded->document) return loaded->document->frameCount();
    return sheet ? sheet->frameCount() : 0;
}

int SpritePreviewWindow::canvasWidth() const {
    if (loaded && loaded->document) return loaded->document->width;
    return sheet && !sheet->frames.empty() ? static_cast<int>(sheet->frames[0].sourceSize.x) : 0;
}

int SpritePreviewWindow::canvasHeight() const {
    if (loaded && loaded->document) return loaded->document->height;
    return sheet && !sheet->frames.empty() ? static_cast<int>(sheet->frames[0].sourceSize.y) : 0;
}

uint32_t SpritePreviewWindow::frameDuration(int index) const {
    if (loaded && loaded->document) {
        const std::vector<uint16_t>& durations = loaded->document->frameDurations;
        return index >= 0 && index < static_cast<int>(durations.size()) ? durations[static_cast<size_t>(index)] : 100u;
    }
    return sheet && index >= 0 && index < sheet->frameCount() ? sheet->frames[static_cast<size_t>(index)].durationMs : 100u;
}

const std::vector<SpriteTag>& SpritePreviewWindow::tags() const {
    static const std::vector<SpriteTag> none;
    if (loaded && loaded->document) return loaded->tags;
    return sheet ? sheet->tags : none;
}

int SpritePreviewWindow::spriteGroups() const {
    // layers_as_sprites: one run of the file's frames per top-level layer or group
    const int frames = frameCount();
    if (!sheet || frames <= 0 || sheet->frameCount() <= frames || sheet->frameCount() % frames != 0) return 1;
    return sheet->frameCount() / frames;
}

std::string SpritePreviewWindow::spriteGroupName(int group) const {
    const int index = group * frameCount();
    if (!sheet || index < 0 || index >= sheet->frameCount()) return std::to_string(group);
    const std::string& name = sheet->frames[static_cast<size_t>(index)].name;
    const size_t slash = name.rfind('/');
    return slash == std::string::npos ? name : name.substr(0, slash);
}

const SpriteFrame* SpritePreviewWindow::sheetFrame(int fileFrame) const {
    if (!sheet) return nullptr;
    const int index = spriteGroup * frameCount() + fileFrame;
    if (index < 0 || index >= sheet->frameCount()) return nullptr;
    return &sheet->frames[static_cast<size_t>(index)];
}

void SpritePreviewWindow::rangeOf(int& from, int& to) const {
    const int frames = frameCount();
    from = 0;
    to = std::max(0, frames - 1);
    const std::vector<SpriteTag>& list = tags();
    if (tag >= 0 && tag < static_cast<int>(list.size())) {
        const SpriteTag& t = list[static_cast<size_t>(tag)];
        from = std::clamp(std::min(t.from, t.to), 0, to);
        to = std::clamp(std::max(t.from, t.to), from, to);
    }
}

std::vector<int> SpritePreviewWindow::playSequence() const {
    const std::vector<SpriteTag>& list = tags();
    if (tag >= 0 && tag < static_cast<int>(list.size())) {
        // looping plays the tag's pass over and over; once, its whole playback (repeat unrolled)
        const SpriteTag& t = list[static_cast<size_t>(tag)];
        return loop ? t.sequence() : t.playback();
    }
    std::vector<int> all;
    for (int f = 0; f < frameCount(); f++) all.push_back(f);
    return all;
}

bool SpritePreviewWindow::layerOn(int layer) const {
    if (!loaded || !loaded->document) return false;
    const ase::Document& doc = *loaded->document;
    auto it = layerToggles.find(doc.layerPath(layer));
    if (it != layerToggles.end()) return it->second;
    return doc.visibleHierarchy(layer);
}

void SpritePreviewWindow::setLayerOn(int layer, bool on) {
    if (!loaded || !loaded->document || layerOn(layer) == on) return;
    layerToggles[loaded->document->layerPath(layer)] = on;
    togglesRevision++;
}

std::vector<int> SpritePreviewWindow::sourceLayers() const {
    std::vector<int> out;
    if (!loaded || !loaded->document) return out;
    const ase::Document& doc = *loaded->document;
    for (int l = 0; l < static_cast<int>(doc.layers.size()); l++) {
        if (spritePreviewLeaf(doc.layers[static_cast<size_t>(l)]) && layerOn(l)) out.push_back(l);
    }
    return out;
}

// ---- actions

void SpritePreviewWindow::togglePlay() {
    if (playing) {
        playing = false;
        return;
    }
    const std::vector<int> sequence = playSequence();
    if (sequence.empty()) return;
    auto at = std::find(sequence.begin(), sequence.end(), frame);
    playIndex = at == sequence.end() ? 0 : static_cast<int>(at - sequence.begin());
    // a finished one-shot starts over
    if (!loop && playIndex == static_cast<int>(sequence.size()) - 1) playIndex = 0;
    frame = sequence[static_cast<size_t>(playIndex)];
    elapsedMs = 0.0;
    playing = true;
}

void SpritePreviewWindow::stepFrame(int delta) {
    if (frameCount() <= 0) return;
    playing = false;
    int from = 0, to = 0;
    rangeOf(from, to);
    const int length = to - from + 1;
    const int inRange = std::clamp(frame, from, to) - from;
    frame = from + ((inRange + delta) % length + length) % length;
    elapsedMs = 0.0;
}

void SpritePreviewWindow::firstFrame() {
    int from = 0, to = 0;
    rangeOf(from, to);
    playing = false;
    frame = from;
}

void SpritePreviewWindow::lastFrame() {
    int from = 0, to = 0;
    rangeOf(from, to);
    playing = false;
    frame = to;
}

void SpritePreviewWindow::zoomBy(int steps) {
    constexpr int count = static_cast<int>(sizeof(kSpritePreviewZooms) / sizeof(kSpritePreviewZooms[0]));
    int index = 0;
    for (int i = 0; i < count; i++) {
        if (kSpritePreviewZooms[i] <= zoom) index = i;
    }
    zoom = kSpritePreviewZooms[std::clamp(index + steps, 0, count - 1)];
    fitRequested = false;
}

void SpritePreviewWindow::selectTag(int index) {
    tag = index;
    int from = 0, to = 0;
    rangeOf(from, to);
    frame = from;
    playIndex = 0;
    elapsedMs = 0.0;
}

void SpritePreviewWindow::tickPlayback() {
    if (!playing) return;
    const std::vector<int> sequence = playSequence();
    if (sequence.empty()) {
        playing = false;
        return;
    }
    playIndex = std::clamp(playIndex, 0, static_cast<int>(sequence.size()) - 1);
    elapsedMs += static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0 * speed;
    // a long stall skips at most one lap instead of spinning
    for (size_t guard = 0; guard <= sequence.size(); guard++) {
        const double duration = std::max<uint32_t>(1u, frameDuration(sequence[static_cast<size_t>(playIndex)]));
        if (elapsedMs < duration) break;
        elapsedMs -= duration;
        if (playIndex + 1 < static_cast<int>(sequence.size())) {
            playIndex++;
        } else if (loop) {
            playIndex = 0;
        } else {
            playing = false;
            elapsedMs = 0.0;
            break;
        }
    }
    frame = sequence[static_cast<size_t>(playIndex)];
}

void SpritePreviewWindow::setStatus(const std::string& message, bool error) {
    statusMessage = message;
    statusIsError = error;
}

void SpritePreviewWindow::excludeFromImport() {
    if (!loaded || !loaded->document) return;
    const ase::Document& doc = *loaded->document;
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();
    ase::AsepriteImportSettings settings = importer.settingsOf(assetPath);
    if (settings.readOnly()) {
        setStatus("The import settings are version " + std::to_string(settings.version) + ", written by a newer editor (read only here).", true);
        return;
    }
    const int n = static_cast<int>(doc.layers.size());
    std::vector<bool> leaf(static_cast<size_t>(n)), off(static_cast<size_t>(n));
    for (int l = 0; l < n; l++) {
        leaf[static_cast<size_t>(l)] = spritePreviewLeaf(doc.layers[static_cast<size_t>(l)]);
        off[static_cast<size_t>(l)] = leaf[static_cast<size_t>(l)] && !layerOn(l);
    }

    // include: the unticked layers leave it
    std::vector<std::string> include;
    for (const std::string& entry : settings.include) {
        const int l = doc.findLayer(entry);
        if (l >= 0 && off[static_cast<size_t>(l)]) continue;
        include.push_back(entry);
    }
    settings.include = include;

    // exclude: an entry stays while it names only unticked layers; a group with a ticked layer
    // under it gives way to its unticked ones
    std::vector<std::string> exclude;
    auto addExclude = [&exclude](const std::string& path) {
        if (std::find(exclude.begin(), exclude.end(), path) == exclude.end()) exclude.push_back(path);
    };
    for (const std::string& entry : settings.exclude) {
        const int l = doc.findLayer(entry);
        if (l < 0) {
            addExclude(entry);
            continue;
        }
        if (leaf[static_cast<size_t>(l)]) {
            if (off[static_cast<size_t>(l)]) addExclude(entry);
            continue;
        }
        bool tickedUnder = false;
        for (int c = 0; c < n; c++) {
            if (leaf[static_cast<size_t>(c)] && !off[static_cast<size_t>(c)] && spritePreviewUnder(doc, c, l)) tickedUnder = true;
        }
        if (!tickedUnder) {
            addExclude(entry);
            continue;
        }
        for (int c = 0; c < n; c++) {
            if (off[static_cast<size_t>(c)] && spritePreviewUnder(doc, c, l)) addExclude(doc.layerPath(c));
        }
    }
    settings.exclude = exclude;

    // what the import would still take that is unticked, and what is ticked that it would not
    // (hidden or prefixed layers)
    std::vector<pixelart::PixelProblem> warnings;
    std::vector<int> selected = ase::selectLayers(doc, settings, warnings);
    for (int l = 0; l < n; l++) {
        if (off[static_cast<size_t>(l)] && std::find(selected.begin(), selected.end(), l) != selected.end()) addExclude(doc.layerPath(l));
    }
    settings.exclude = exclude;
    selected = ase::selectLayers(doc, settings, warnings);
    for (int l = 0; l < n; l++) {
        if (leaf[static_cast<size_t>(l)] && !off[static_cast<size_t>(l)] && std::find(selected.begin(), selected.end(), l) == selected.end()) {
            settings.include.push_back(doc.layerPath(l));
        }
    }

    const fs::path absolute = project->getAssetsPath() / FileUtils::pathFromUtf8(assetPath);
    Command* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(absolute), ase::AsepriteImporter::kSettingsKey,
                                           settings.toYaml());
    if (command->isNoOp()) {
        delete command;
        setStatus("The import already uses the ticked layers.", false);
        return;
    }
    CommandHistory* history = project->getProjectCommandHistory();
    InteractiveEdit edit = history->beginInteractive("Exclude Layers from Import");
    if (!history->addCommand(command)) {
        edit.cancel();
        setStatus("Couldn't write the import settings of " + assetPath + ": check the .meta file is writable.", true);
        return;
    }
    edit.commit();
    setStatus("The import now uses the ticked layers (reimporting).", false);
}

// ---- GPU

SpritePreviewWindow::GpuImage* SpritePreviewWindow::sourceFrameGpu(int fileFrame) {
    if (!loaded || !loaded->document) return nullptr;
    const ase::Document& doc = *loaded->document;
    if (fileFrame < 0 || fileFrame >= doc.frameCount()) return nullptr;
    if (sourceGpuDocument != &doc || sourceGpuRevision != togglesRevision) {
        for (auto& [index, gpu] : sourceGpu) retire(gpu);
        sourceGpu.clear();
        sourceOrder.clear();
        sourceGpuDocument = &doc;
        sourceGpuRevision = togglesRevision;
    }
    auto it = sourceGpu.find(fileFrame);
    if (it != sourceGpu.end()) {
        sourceOrder.erase(std::remove(sourceOrder.begin(), sourceOrder.end(), fileFrame), sourceOrder.end());
        sourceOrder.push_back(fileFrame);
        return it->second.valid() ? &it->second : nullptr;
    }
    ase::RenderOptions options;
    options.layers = sourceLayers();
    // the toggles decide visibility; none ticked draws nothing (an empty list would mean "visible ones")
    if (options.layers.empty()) options.layers = {-1};
    options.includeHidden = true;
    const pixelart::PixelImage image = ase::renderFrame(doc, fileFrame, options);
    GpuImage& gpu = sourceGpu[fileFrame];
    spritePreviewUpload(gpu.texture, gpu.width, gpu.height, image);
    sourceOrder.push_back(fileFrame);
    size_t pixels = 0;
    for (const auto& [index, cached] : sourceGpu) pixels += static_cast<size_t>(cached.width) * static_cast<size_t>(cached.height);
    while (pixels > kSpritePreviewFrameBudget && sourceOrder.size() > 1) {
        const int oldest = sourceOrder.front();
        sourceOrder.erase(sourceOrder.begin());
        GpuImage& old = sourceGpu[oldest];
        pixels -= static_cast<size_t>(old.width) * static_cast<size_t>(old.height);
        retire(old);
        sourceGpu.erase(oldest);
    }
    GpuImage& shown = sourceGpu[fileFrame];
    return shown.valid() ? &shown : nullptr;
}

SpritePreviewWindow::GpuImage* SpritePreviewWindow::pageTexture(size_t page) {
    if (!loaded || page >= loaded->pages.size() || !loaded->pages[page]) return nullptr;
    if (pageGpu.size() != loaded->pages.size()) {
        for (GpuImage& gpu : pageGpu) retire(gpu);
        pageGpu.assign(loaded->pages.size(), GpuImage());
    }
    GpuImage& gpu = pageGpu[page];
    if (!gpu.valid()) spritePreviewUpload(gpu.texture, gpu.width, gpu.height, *loaded->pages[page]);
    return gpu.valid() ? &gpu : nullptr;
}

void SpritePreviewWindow::updateCompareImage() {
    if (!compare || compareSheetPath.empty()) return;
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();
    std::shared_ptr<const ase::ImportRecord> reference = importer.record(compareSheetPath);
    const std::string key = compareSheetPath + "|" + (reference ? reference->key : std::string()) + "|" + std::to_string(compareFrame);
    if (key == compareUploadedKey) return;
    compareUploadedKey = key;
    retire(compareGpu);
    compareSheet.reset();
    if (!reference || !reference->ok) return;
    compareSheet = SpriteSheetPool::get(compareSheetPath);
    if (!compareSheet || compareSheet->frames.empty()) return;
    const SpriteFrame& f = compareSheet->frames[static_cast<size_t>(std::clamp(compareFrame, 0, compareSheet->frameCount() - 1))];
    std::optional<pixelart::PixelImage> page = pixelart::PixelImage::loadPng(reference->pageFile(f.page), nullptr);
    if (!page) return;
    // the untrimmed frame, as the game places it
    pixelart::PixelImage untrimmed(static_cast<int>(f.sourceSize.x), static_cast<int>(f.sourceSize.y));
    const pixelart::IntRect rect{static_cast<int>(f.rect.getX()), static_cast<int>(f.rect.getY()), static_cast<int>(f.rect.getWidth()),
                                 static_cast<int>(f.rect.getHeight())};
    untrimmed.blit(page->crop(rect), static_cast<int>(f.offset.x), static_cast<int>(f.offset.y));
    spritePreviewUpload(compareGpu.texture, compareGpu.width, compareGpu.height, untrimmed);
}

// ---- compare reference

void SpritePreviewWindow::loadCompareChoice() {
    const fs::path root = project->getProjectPath();
    if (root == compareStoredFor) return;
    compareStoredFor = root;
    compareSheetPath.clear();
    compareFrame = 0;
    compareUploadedKey.clear();
    compareSheet.reset();
    retire(compareGpu);
    if (root.empty()) return;
    std::string text;
    if (!userfiles::read(spritePreviewChoiceFile(root), text)) return;
    try {
        const YAML::Node node = YAML::Load(text);
        const YAML::Node choice = node["compare"];
        if (choice && choice.IsMap()) {
            compareSheetPath = choice["sheet"] ? choice["sheet"].as<std::string>("") : std::string();
            compareFrame = std::max(0, choice["frame"] ? choice["frame"].as<int>(0) : 0);
        }
    } catch (const YAML::Exception&) {
        // a damaged file only loses the choice: the next one written replaces it
        compareSheetPath.clear();
        compareFrame = 0;
    }
}

void SpritePreviewWindow::saveCompareChoice() {
    const fs::path root = project->getProjectPath();
    if (root.empty()) return;
    YAML::Node node(YAML::NodeType::Map);
    node["compare"]["sheet"] = compareSheetPath;
    node["compare"]["frame"] = compareFrame;
    YAML::Emitter out;
    out << node;
    std::string error;
    if (!userfiles::write(spritePreviewChoiceFile(root), std::string(out.c_str()) + "\n", &error)) {
        Out::warning("Sprite Preview: couldn't remember the compare reference: %s", error.c_str());
    }
}

void SpritePreviewWindow::scanCompareCandidates(bool force) {
    const double now = ImGui::GetTime();
    if (!force && compareCandidatesAt >= 0.0 && now - compareCandidatesAt < 2.0) return;
    compareCandidatesAt = now;
    compareCandidates.clear();
    const fs::path root = project->getProjectPath();
    const fs::path assets = project->getAssetsPath().lexically_normal();
    project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (info.type != AssetType::AsepriteSprite) return;
        const fs::path relative = (root / FileUtils::pathFromUtf8(info.path)).lexically_normal().lexically_relative(assets);
        const std::string path = FileUtils::pathToGenericUtf8(relative);
        if (!path.empty() && path.rfind("..", 0) != 0) compareCandidates.push_back(path);
    });
    std::sort(compareCandidates.begin(), compareCandidates.end());
}

// ---- UI

void SpritePreviewWindow::show() {
    releaseRetired();
    if (!windowOpen) return;

    loadCompareChoice();
    followProjectSelection();
    if (!assetPath.empty()) {
        followImport();
        adoptLoad();
        tickPlayback();
        updateCompareImage();
    }

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 60.0f, ImGui::GetFontSize() * 36.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    const bool visible = ImGui::Begin(WINDOW_NAME, &windowOpen);
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible) {
        ImGui::End();
        if (!windowOpen) setOpen(false);
        return;
    }

    if (assetPath.empty()) {
        ImGui::TextDisabled("Select an .aseprite file in the Project window, or use Open Sprite Preview in its Inspector.");
    } else {
        drawToolbar();
        drawAgentEditLine();
        ImGui::Separator();
        if (ImGui::BeginTable("##spritePreviewLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("panel", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 15.0f);
            ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            drawSidePanel();
            UIProbe::item("spritePreview.panel");
            ImGui::TableSetColumnIndex(1);
            const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
            const float stripHeight = lineHeight * static_cast<float>(stripLanes + 2) + ImGui::GetStyle().ScrollbarSize + ImGui::GetStyle().WindowPadding.y * 2.0f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            drawCanvas(ImVec2(avail.x, std::max(avail.y - stripHeight - ImGui::GetStyle().ItemSpacing.y, lineHeight * 4.0f)));
            UIProbe::item("spritePreview.canvas");
            drawFrameStrip();
            UIProbe::item("spritePreview.frameStrip");
            ImGui::EndTable();
        }
    }
    ImGui::End();

    if (playing || loadTask.isValid() || (agentEdit && spritePreviewNow() - agentEdit->at < 3.0)) Backend::getApp().requestRedraw();
    if (!windowOpen) setOpen(false);
}

void SpritePreviewWindow::drawAgentEditLine() {
    if (!externalSaveNotice.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", externalSaveNotice.c_str());
        UIProbe::item("spritePreview.externalSave");
    }
    if (!agentEdit || !agentEdit->showLine) return;
    const uint32_t sceneId = project->getSelectedScene() ? project->getSelectedSceneId() : NULL_PROJECT_SCENE;
    UndoTimeline::EntryView top = UndoTimeline::nextUndo(sceneId);
    if (agentEdit->undoId == 0 && top && std::string(top.entry->cmd->typeName()) == "SpriteEditCmd") agentEdit->undoId = top.entry->info.id;
    const int seconds = static_cast<int>(spritePreviewNow() - agentEdit->at);
    ImGui::TextWrapped("Edited by %s: %s \xE2\x80\x94 %d s ago", agentEdit->by.c_str(), agentEdit->summary.c_str(), seconds);
    UIProbe::item("spritePreview.agentEdit.line");
    const bool onTop = top && agentEdit->undoId != 0 && top.entry->info.id == agentEdit->undoId;
    ImGui::SameLine();
    ImGui::BeginDisabled(!onTop);
    if (ImGui::SmallButton("Undo##agentEdit")) {
        std::string why;
        if (!UndoTimeline::undo(sceneId, &why)) setStatus(why, true);
    }
    ImGui::EndDisabled();
    UIProbe::item("spritePreview.agentEdit.undo");
    if (!onTop && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A newer step is on top of the undo history; undo that one first");
    ImGui::TextDisabled("Edited by %s \xE2\x80\x94 reopen it in Aseprite before editing there", agentEdit->by.c_str());
    UIProbe::item("spritePreview.agentEdit.reopen." + agentEdit->revision);
}

void SpritePreviewWindow::drawToolbar() {
    const CommandCatalog& catalog = CommandCatalog::editor();
    const int frames = frameCount();
    const bool importedAvailable = sheet && !sheet->frames.empty();
    if (view == View::Imported && !importedAvailable && loaded && loaded->document) view = View::Source;

    ImGui::TextUnformatted(assetPath.c_str());
    ImGui::SameLine();
    if (ImGui::RadioButton("Source", view == View::Source)) view = View::Source;
    UIProbe::item("spritePreview.view.Source");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The file as Aseprite composes it (the layers ticked under Layers)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!importedAvailable);
    if (ImGui::RadioButton("Imported", view == View::Imported)) view = View::Imported;
    ImGui::EndDisabled();
    UIProbe::item("spritePreview.view.Imported");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("What the game draws: the import's sprite sheet");

    if (view == View::Imported && spriteGroups() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::BeginCombo("Sprite", spriteGroupName(spriteGroup).c_str())) {
            for (int g = 0; g < spriteGroups(); g++) {
                if (ImGui::Selectable(spriteGroupName(g).c_str(), g == spriteGroup)) spriteGroup = g;
            }
            ImGui::EndCombo();
        }
    }

    // ---- playback
    const std::vector<SpriteTag>& list = tags();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    const char* tagPreview = tag >= 0 && tag < static_cast<int>(list.size()) ? list[static_cast<size_t>(tag)].name.c_str() : "All Frames";
    if (ImGui::BeginCombo("##tag", tagPreview)) {
        if (ImGui::Selectable("All Frames", tag < 0)) selectTag(-1);
        UIProbe::item("spritePreview.tagAll");
        for (size_t i = 0; i < list.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::ColorButton("##color", ImVec4(list[i].color.x, list[i].color.y, list[i].color.z, 1.0f), ImGuiColorEditFlags_NoTooltip,
                               ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            const std::string label = list[i].name + "  (" + std::to_string(list[i].from) + "-" + std::to_string(list[i].to) + ", " +
                                      spriteTagDirectionName(list[i].direction) + ")";
            if (ImGui::Selectable(label.c_str(), tag == static_cast<int>(i))) selectTag(static_cast<int>(i));
            UIProbe::item("spritePreview.tagItem." + list[i].name);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // the combo again once its popup ends (End restores the last item)
    UIProbe::item("spritePreview.tagDropdown");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tag");
    ImGui::SameLine();
    ImGui::BeginDisabled(frames <= 0);
    if (ImGui::Button(playing ? ICON_FA_PAUSE : ICON_FA_PLAY)) togglePlay();
    UIProbe::item(playing ? "spritePreview.pause" : "spritePreview.play");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", catalog.tooltip("spritePreview.playPause", playing ? "Pause" : "Play").c_str());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_BACKWARD_STEP)) stepFrame(-1);
    UIProbe::item("spritePreview.previousFrame");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", catalog.tooltip("spritePreview.previousFrame", "Previous Frame").c_str());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FORWARD_STEP)) stepFrame(1);
    UIProbe::item("spritePreview.nextFrame");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", catalog.tooltip("spritePreview.nextFrame", "Next Frame").c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
    char speedText[16];
    std::snprintf(speedText, sizeof(speedText), "%gx", static_cast<double>(speed));
    if (ImGui::BeginCombo("Speed", speedText)) {
        for (float s : kSpritePreviewSpeeds) {
            char label[16];
            std::snprintf(label, sizeof(label), "%gx", static_cast<double>(s));
            if (ImGui::Selectable(label, s == speed)) speed = s;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (frames > 0) ImGui::Text("Frame %d / %d  %u ms", frame, frames - 1, frameDuration(frame));
    else if (loadTask.isValid()) ImGui::TextDisabled("Reading...");

    // ---- view options
    ImGui::BeginDisabled(frames <= 0);
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS_MINUS)) zoomBy(-1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", catalog.tooltip("spritePreview.zoomOut", "Zoom Out").c_str());
    ImGui::SameLine();
    ImGui::Text("%dx", zoom);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS_PLUS)) zoomBy(1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", catalog.tooltip("spritePreview.zoomIn", "Zoom In").c_str());
    ImGui::SameLine();
    if (ImGui::Button("Fit")) fitRequested = true;
    UIProbe::item("spritePreview.fit");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The largest whole zoom that fits the view");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Onion Skin", &onionSkin);
    UIProbe::item("spritePreview.onionSkin");
    if (onionSkin) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
        ImGui::SliderInt("Previous", &onionPrevious, 1, 3);
        UIProbe::item("spritePreview.onionPrevious");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
        ImGui::SliderInt("Next", &onionNext, 1, 3);
        UIProbe::item("spritePreview.onionNext");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
        ImGui::SliderFloat("Opacity", &onionOpacity, 0.05f, 1.0f, "%.2f");
        UIProbe::item("spritePreview.onionOpacity");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Slices", &showSlices);
    UIProbe::item("spritePreview.slices");
    ImGui::SameLine();
    ImGui::Checkbox("Pivot", &showPivot);
    UIProbe::item("spritePreview.pivot");
    ImGui::SameLine();
    ImGui::Checkbox("Trim", &showTrim);
    UIProbe::item("spritePreview.trim");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The rectangle the import keeps of each frame");
    ImGui::SameLine();
    ImGui::Checkbox("Pixel Grid", &showGrid);
    UIProbe::item("spritePreview.pixelGrid");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    const char* backgrounds[] = {"Checkerboard", "Color", "Scene"};
    int backgroundIndex = static_cast<int>(background);
    if (ImGui::Combo("Background", &backgroundIndex, backgrounds, IM_ARRAYSIZE(backgrounds))) background = static_cast<Background>(backgroundIndex);
    UIProbe::item("spritePreview.background");
    if (background == Background::Scene) {
        ImGui::SameLine();
        ImVec4 sceneColor;
        std::string sceneName;
        if (sceneBackgroundColor(sceneColor, &sceneName)) {
            // read-only here: the colour is the scene's, edited in its Inspector
            ImGui::ColorButton("##sceneBackground", sceneColor, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                               ImVec2(ImGui::GetFontSize() * 3.0f, ImGui::GetFrameHeight()));
            UIProbe::item("spritePreview.sceneBackground");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The Background of %s (edit it in the scene's Inspector)", sceneName.c_str());
        } else {
            ImGui::TextDisabled("No scene open");
        }
    }
    if (background == Background::Color) {
        ImGui::SameLine();
        // the preview's own colour (sRGB as shown), no undo: nothing of the project changes
        Vector3 shown(backgroundColor[0], backgroundColor[1], backgroundColor[2]);
        ui::ColorFieldOptions options;
        options.linearStorage = false;
        options.label = "Background";
        options.width = ImGui::GetFontSize() * 3.0f;
        if (ui::colorField("##backgroundColor", shown, options) != ui::EditPhase::None) {
            backgroundColor[0] = shown.x;
            backgroundColor[1] = shown.y;
            backgroundColor[2] = shown.z;
        }
    }
}

void SpritePreviewWindow::drawSidePanel() {
    ImGui::BeginChild("##spritePreviewPanel", ImVec2(0, 0), ImGuiChildFlags_None);
    if (loaded && !loaded->error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("Couldn't read the file: %s", loaded->error.c_str());
        ImGui::PopStyleColor();
    }
    if (record) {
        if (record->ok) {
            ImGui::TextWrapped("%s", record->summary.c_str());
            UIProbe::item("spritePreview.summary");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            ImGui::TextWrapped("Import failed: %s", record->error.message.c_str());
            ImGui::PopStyleColor();
        }
    } else if (ase::AsepriteImporter::get().isPending(assetPath)) {
        ImGui::TextDisabled("Importing...");
    } else {
        ImGui::TextDisabled("Not imported yet");
    }
    ImGui::Spacing();
    drawLayers();
    ImGui::Spacing();
    drawCompareSection();
    // a test wheels this panel to reach what a short window leaves below
    UIProbe::scroll("spritePreview.panel");
    ImGui::EndChild();
}

void SpritePreviewWindow::drawLayers() {
    if (!ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!loaded || !loaded->document) {
        ImGui::TextDisabled("No layers");
        return;
    }
    const ase::Document& doc = *loaded->document;
    const int n = static_cast<int>(doc.layers.size());
    ImGui::BeginDisabled(view != View::Source);
    // Aseprite's order: the top layer first, a group above its children
    std::function<void(int, int)> rows = [&](int parent, int depth) {
        std::vector<int> children = doc.children(parent);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            const int l = *it;
            const ase::Layer& layer = doc.layers[static_cast<size_t>(l)];
            if (layer.reference()) continue;
            ImGui::PushID(l);
            ImGui::Indent(static_cast<float>(depth) * ImGui::GetFontSize());
            if (layer.isGroup()) {
                bool anyOn = false;
                for (int c = 0; c < n; c++) {
                    if (spritePreviewLeaf(doc.layers[static_cast<size_t>(c)]) && spritePreviewUnder(doc, c, l) && layerOn(c)) anyOn = true;
                }
                bool value = anyOn;
                if (ImGui::Checkbox((layer.name + "/").c_str(), &value)) {
                    for (int c = 0; c < n; c++) {
                        if (spritePreviewLeaf(doc.layers[static_cast<size_t>(c)]) && spritePreviewUnder(doc, c, l)) setLayerOn(c, value);
                    }
                }
                UIProbe::item("spritePreview.layer." + doc.layerPath(l));
            } else if (spritePreviewLeaf(layer)) {
                bool value = layerOn(l);
                if (ImGui::Checkbox(layer.name.c_str(), &value)) setLayerOn(l, value);
                UIProbe::item("spritePreview.layer." + doc.layerPath(l));
                if (value) UIProbe::item("spritePreview.layerOn." + doc.layerPath(l));
                if (!doc.visibleHierarchy(l)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(hidden)");
                }
            }
            ImGui::Unindent(static_cast<float>(depth) * ImGui::GetFontSize());
            ImGui::PopID();
            if (layer.isGroup()) rows(l, depth + 1);
        }
    };
    rows(-1, 0);
    ImGui::EndDisabled();
    if (view != View::Source) ImGui::TextDisabled("Layer toggles show in the Source view.");

    if (ImGui::Button("Exclude from Import")) excludeFromImport();
    UIProbe::item("spritePreview.excludeFromImport");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The import takes exactly the ticked layers (one undo step)");
    if (!statusMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, statusIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", statusMessage.c_str());
        UIProbe::item(statusIsError ? "spritePreview.statusError" : "spritePreview.status");
        ImGui::PopStyleColor();
    }
}

void SpritePreviewWindow::drawCompareSection() {
    if (!ImGui::CollapsingHeader("Compare at 1\xc3\x97", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Checkbox("Compare", &compare);
    UIProbe::item("spritePreview.compare");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("This frame next to a reference frame of another sheet, at the same whole scale");
    if (!compare) return;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##compareSheet", compareSheetPath.empty() ? "Reference sheet..." : compareSheetPath.c_str())) {
        scanCompareCandidates(false);
        for (const std::string& candidate : compareCandidates) {
            if (ImGui::Selectable(candidate.c_str(), candidate == compareSheetPath) && candidate != compareSheetPath) {
                compareSheetPath = candidate;
                compareFrame = 0;
                saveCompareChoice();
            }
            UIProbe::item("spritePreview.compareCandidate." + candidate);
        }
        if (compareCandidates.empty()) ImGui::TextDisabled("No .aseprite files in the project");
        ImGui::EndCombo();
    }
    UIProbe::item("spritePreview.compareSheet");
    if (compareSheet && !compareSheet->frames.empty()) {
        const int last = compareSheet->frameCount() - 1;
        ImGui::SetNextItemWidth(-1.0f);
        const std::string preview = compareSheet->frames[static_cast<size_t>(std::clamp(compareFrame, 0, last))].name;
        if (ImGui::BeginCombo("##compareFrame", preview.c_str())) {
            for (int f = 0; f <= last; f++) {
                if (ImGui::Selectable(compareSheet->frames[static_cast<size_t>(f)].name.c_str(), f == compareFrame) && f != compareFrame) {
                    compareFrame = f;
                    saveCompareChoice();
                }
            }
            ImGui::EndCombo();
        }
        UIProbe::item("spritePreview.compareFrame");
        if (sheet && compareSheet->pixelsPerUnit != sheet->pixelsPerUnit) {
            char warning[256];
            std::snprintf(warning, sizeof(warning), "%s %g PPU, %s %g PPU: mixed pixel sizes",
                          FileUtils::pathToUtf8(FileUtils::pathFromUtf8(assetPath).stem()).c_str(), static_cast<double>(sheet->pixelsPerUnit),
                          FileUtils::pathToUtf8(FileUtils::pathFromUtf8(compareSheetPath).stem()).c_str(),
                          static_cast<double>(compareSheet->pixelsPerUnit));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s %s", ICON_FA_TRIANGLE_EXCLAMATION, warning);
            UIProbe::item("spritePreview.compareWarning");
            ImGui::PopStyleColor();
        }
    } else if (!compareSheetPath.empty()) {
        ImGui::TextDisabled("The reference is not imported yet");
    }
}

void SpritePreviewWindow::drawCanvas(const ImVec2& size) {
    ImGui::BeginChild("##spritePreviewCanvas", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvasInput", ImVec2(std::max(canvasSize.x, 1.0f), std::max(canvasSize.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const int w = canvasWidth(), h = canvasHeight();
    if (w <= 0 || h <= 0) {
        const char* text = loadTask.isValid() ? "Reading..." : "Nothing to show";
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(canvasMin.x + (canvasSize.x - textSize.x) * 0.5f, canvasMin.y + (canvasSize.y - textSize.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        ImGui::EndChild();
        return;
    }

    const bool comparing = compare && compareGpu.valid();
    const float gap = ImGui::GetFontSize() * 2.0f;
    const int contentW = w + (comparing ? compareGpu.width : 0);
    const int contentH = std::max(h, comparing ? compareGpu.height : 0);
    if (fitRequested && canvasSize.x > 0.0f && canvasSize.y > 0.0f) {
        const int byWidth = static_cast<int>((canvasSize.x - (comparing ? gap : 0.0f)) / static_cast<float>(contentW));
        const int byHeight = static_cast<int>((canvasSize.y - ImGui::GetTextLineHeightWithSpacing()) / static_cast<float>(contentH));
        zoom = std::clamp(std::min(byWidth, byHeight), 1, 32);
        pan = ImVec2(0.0f, 0.0f);
        // Fit holds until a zoom or a pan: the canvas changes size after its first frames (the toolbar
        // wraps, the dock settles) and when the window is resized
    }
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f) zoomBy(1);
        else if (wheel < 0.0f) zoomBy(-1);
    }
    if (dragging) {
        fitRequested = false;
        pan.x += ImGui::GetIO().MouseDelta.x;
        pan.y += ImGui::GetIO().MouseDelta.y;
    }

    const float z = static_cast<float>(zoom);
    const float totalW = static_cast<float>(contentW) * z + (comparing ? gap : 0.0f);
    // whole screen pixels, so every sprite pixel is the same size
    const ImVec2 origin(std::floor(canvasMin.x + (canvasSize.x - totalW) * 0.5f + pan.x),
                        std::floor(canvasMin.y + (canvasSize.y - static_cast<float>(contentH) * z) * 0.5f + pan.y));

    ImVec4 sceneColor;
    const bool sceneFill = background == Background::Scene && sceneBackgroundColor(sceneColor);
    auto backgroundAt = [&](ImVec2 min, int pw, int ph) {
        const ImVec2 max(min.x + static_cast<float>(pw) * z, min.y + static_cast<float>(ph) * z);
        if (background == Background::Color) {
            draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(ImVec4(backgroundColor[0], backgroundColor[1], backgroundColor[2], 1.0f)));
            return;
        }
        if (sceneFill) {
            draw->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(sceneColor));
            return;
        }
        // 8-pixel squares of the sprite, only where the view shows them
        const float cell = 8.0f * z;
        const ImVec2 clipMin = draw->GetClipRectMin(), clipMax = draw->GetClipRectMax();
        const int x0 = std::max(0, static_cast<int>(std::floor((clipMin.x - min.x) / cell)));
        const int y0 = std::max(0, static_cast<int>(std::floor((clipMin.y - min.y) / cell)));
        const int x1 = std::min(static_cast<int>(std::ceil(static_cast<float>(pw) / 8.0f)), static_cast<int>(std::ceil((clipMax.x - min.x) / cell)));
        const int y1 = std::min(static_cast<int>(std::ceil(static_cast<float>(ph) / 8.0f)), static_cast<int>(std::ceil((clipMax.y - min.y) / cell)));
        draw->AddRectFilled(min, max, IM_COL32(204, 204, 204, 255));
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                if (((x + y) & 1) == 0) continue;
                const ImVec2 a(min.x + static_cast<float>(x) * cell, min.y + static_cast<float>(y) * cell);
                draw->AddRectFilled(a, ImVec2(std::min(a.x + cell, max.x), std::min(a.y + cell, max.y)), IM_COL32(153, 153, 153, 255));
            }
        }
    };

    backgroundAt(origin, w, h);
    if (onionSkin) {
        // Aseprite's red/blue tint: the frames before in red, after in blue, fading with distance
        int from = 0, to = 0;
        rangeOf(from, to);
        for (int d = onionPrevious; d >= 1; d--) {
            const int f = frame - d;
            if (f < from) continue;
            const float alpha = onionOpacity * static_cast<float>(onionPrevious - d + 1) / static_cast<float>(onionPrevious);
            if (drawFrameImage(draw, origin, f, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.25f, 0.25f, alpha)))) {
                UIProbe::rect("spritePreview.onionDrawn.previous." + std::to_string(d), origin.x, origin.y, origin.x + static_cast<float>(w) * z,
                              origin.y + static_cast<float>(h) * z);
            }
        }
        for (int d = onionNext; d >= 1; d--) {
            const int f = frame + d;
            if (f > to) continue;
            const float alpha = onionOpacity * static_cast<float>(onionNext - d + 1) / static_cast<float>(onionNext);
            if (drawFrameImage(draw, origin, f, ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.25f, 1.0f, alpha)))) {
                UIProbe::rect("spritePreview.onionDrawn.next." + std::to_string(d), origin.x, origin.y, origin.x + static_cast<float>(w) * z,
                              origin.y + static_cast<float>(h) * z);
            }
        }
    }
    if (drawFrameImage(draw, origin, frame, IM_COL32_WHITE)) {
        UIProbe::rect("spritePreview.frameImage", origin.x, origin.y, origin.x + static_cast<float>(w) * z, origin.y + static_cast<float>(h) * z);
    } else if (view == View::Imported) {
        draw->AddText(origin, ImGui::GetColorU32(ImGuiCol_TextDisabled), "The import has no pixels for this frame");
    }
    drawOverlays(draw, origin);

    if (comparing) {
        const ImVec2 refOrigin(origin.x + static_cast<float>(w) * z + gap, origin.y);
        backgroundAt(refOrigin, compareGpu.width, compareGpu.height);
        spritePreviewImage(draw, Backend::getImGuiTexture(compareGpu.texture.getRender()), refOrigin,
                           ImVec2(refOrigin.x + static_cast<float>(compareGpu.width) * z, refOrigin.y + static_cast<float>(compareGpu.height) * z));
        UIProbe::rect("spritePreview.compareImage", refOrigin.x, refOrigin.y, refOrigin.x + static_cast<float>(compareGpu.width) * z,
                      refOrigin.y + static_cast<float>(compareGpu.height) * z);
        const float labelY = origin.y + static_cast<float>(contentH) * z + 2.0f;
        draw->AddText(ImVec2(origin.x, labelY), ImGui::GetColorU32(ImGuiCol_Text),
                      FileUtils::pathToUtf8(FileUtils::pathFromUtf8(assetPath).stem()).c_str());
        draw->AddText(ImVec2(refOrigin.x, labelY), ImGui::GetColorU32(ImGuiCol_Text),
                      FileUtils::pathToUtf8(FileUtils::pathFromUtf8(compareSheetPath).stem()).c_str());
    }
    ImGui::EndChild();
}

bool SpritePreviewWindow::drawFrameImage(ImDrawList* draw, ImVec2 origin, int fileFrame, ImU32 tint) {
    const float z = static_cast<float>(zoom);
    if (view == View::Source) {
        GpuImage* gpu = sourceFrameGpu(fileFrame);
        if (!gpu) return false;
        spritePreviewImage(draw, Backend::getImGuiTexture(gpu->texture.getRender()), origin,
                           ImVec2(origin.x + static_cast<float>(gpu->width) * z, origin.y + static_cast<float>(gpu->height) * z), ImVec2(0, 0),
                           ImVec2(1, 1), tint);
        return true;
    }
    const SpriteFrame* f = sheetFrame(fileFrame);
    if (!f) return false;
    GpuImage* page = pageTexture(f->page);
    if (!page) return false;
    const float pw = static_cast<float>(page->width), ph = static_cast<float>(page->height);
    const ImVec2 uv0(f->rect.getX() / pw, f->rect.getY() / ph);
    const ImVec2 uv1((f->rect.getX() + f->rect.getWidth()) / pw, (f->rect.getY() + f->rect.getHeight()) / ph);
    const ImVec2 a(origin.x + f->offset.x * z, origin.y + f->offset.y * z);
    const ImVec2 b(a.x + f->rect.getWidth() * z, a.y + f->rect.getHeight() * z);
    spritePreviewImage(draw, Backend::getImGuiTexture(page->texture.getRender()), a, b, uv0, uv1, tint);
    return true;
}

bool SpritePreviewWindow::sceneBackgroundColor(ImVec4& color, std::string* sceneName) const {
    const SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene) return false;
    // stored as shown (sRGB, the Inspector's Background field), so ImGui draws it as is; opaque,
    // as nothing is behind a cleared game view to show through
    const Vector4 c = sceneProject->scene->getBackgroundColor();
    color = ImVec4(c.x, c.y, c.z, 1.0f);
    if (sceneName) *sceneName = sceneProject->name;
    return true;
}

std::vector<SpritePreviewWindow::SliceOverlay> SpritePreviewWindow::slicesAt(int fileFrame) const {
    std::vector<SliceOverlay> out;
    if (view == View::Source) {
        if (!loaded || !loaded->document) return out;
        for (const ase::Slice& slice : loaded->document->slices) {
            const ase::SliceKey* key = slice.keyAt(fileFrame);
            if (!key) continue;
            SliceOverlay o;
            o.name = slice.name;
            o.bounds = key->bounds;
            if (key->center) {
                o.hasCenter = true;
                o.center = *key->center;
            }
            if (key->pivot) {
                o.hasPivot = true;
                o.pivot = *key->pivot;
            }
            // Aseprite's slice colour (its default is blue)
            const pixelart::PixelRgba c = slice.userData.color.value_or(pixelart::PixelRgba{0, 0, 255, 255});
            o.color = IM_COL32(c[0], c[1], c[2], 255);
            out.push_back(std::move(o));
        }
        return out;
    }
    if (!sheet) return out;
    for (const SpriteSlice& slice : sheet->slices) {
        const SpriteSliceKey* key = slice.keyAt(fileFrame);
        if (!key) continue;
        SliceOverlay o;
        o.name = slice.name;
        o.bounds = {static_cast<int>(key->bounds.getX()), static_cast<int>(key->bounds.getY()), static_cast<int>(key->bounds.getWidth()),
                    static_cast<int>(key->bounds.getHeight())};
        o.hasCenter = key->hasCenter;
        o.center = {static_cast<int>(key->center.getX()), static_cast<int>(key->center.getY()), static_cast<int>(key->center.getWidth()),
                    static_cast<int>(key->center.getHeight())};
        o.hasPivot = key->hasPivot;
        o.pivot = {static_cast<int>(key->pivot.x), static_cast<int>(key->pivot.y)};
        o.color = IM_COL32(0, 0, 255, 255);
        out.push_back(std::move(o));
    }
    return out;
}

void SpritePreviewWindow::drawOverlays(ImDrawList* draw, ImVec2 origin) {
    const float z = static_cast<float>(zoom);
    const int w = canvasWidth(), h = canvasHeight();
    auto at = [&](float x, float y) { return ImVec2(origin.x + x * z, origin.y + y * z); };
    auto cross = [&](ImVec2 p, ImU32 color) {
        const float r = std::max(4.0f, z * 0.75f);
        draw->AddLine(ImVec2(p.x - r, p.y), ImVec2(p.x + r + 1.0f, p.y), color, 1.0f);
        draw->AddLine(ImVec2(p.x, p.y - r), ImVec2(p.x, p.y + r + 1.0f), color, 1.0f);
    };

    // R41: what the agent changed, fading over 3 s
    if (agentEdit && view == View::Source && AppSettings::getPixelArtPreferences().agentEditHighlight) {
        const double age = spritePreviewNow() - agentEdit->at;
        if (age < 3.0) {
            const float alpha = static_cast<float>(1.0 - age / 3.0);
            const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.85f, 1.0f, alpha));
            for (const pixelart::draw::DirtyRect& d : agentEdit->rects) {
                if (d.frame != frame + 1) continue;
                const ImVec2 a = at(static_cast<float>(d.rect.x), static_cast<float>(d.rect.y));
                const ImVec2 b = at(static_cast<float>(d.rect.x + d.rect.w), static_cast<float>(d.rect.y + d.rect.h));
                draw->AddRect(a, b, color, 0.0f, 0, 2.0f);
                const ImVec2 restore = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(a);
                ImGui::Dummy(ImVec2(b.x - a.x, b.y - a.y));
                UIProbe::item("spritePreview.agentEdit.rect");
                ImGui::SetCursorScreenPos(restore);
            }
        }
    }

    if (showGrid && zoom >= 4) {
        const ImU32 grid = IM_COL32(128, 128, 128, 70);
        const ImVec2 clipMin = draw->GetClipRectMin(), clipMax = draw->GetClipRectMax();
        const int x0 = std::max(1, static_cast<int>(std::floor((clipMin.x - origin.x) / z)));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil((clipMax.x - origin.x) / z)));
        const int y0 = std::max(1, static_cast<int>(std::floor((clipMin.y - origin.y) / z)));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil((clipMax.y - origin.y) / z)));
        for (int x = x0; x <= x1; x++) draw->AddLine(at(static_cast<float>(x), 0.0f), at(static_cast<float>(x), static_cast<float>(h)), grid);
        for (int y = y0; y <= y1; y++) draw->AddLine(at(0.0f, static_cast<float>(y)), at(static_cast<float>(w), static_cast<float>(y)), grid);
    }

    const SpriteFrame* f = sheetFrame(frame);
    if (showTrim && f) {
        const ImU32 trim = IM_COL32(255, 210, 0, 255);
        draw->AddRect(at(f->offset.x, f->offset.y), at(f->offset.x + f->rect.getWidth(), f->offset.y + f->rect.getHeight()), trim);
        draw->AddText(ImVec2(at(f->offset.x, f->offset.y).x + 2.0f, at(f->offset.x, f->offset.y + f->rect.getHeight()).y + 1.0f), trim, "trim");
    }

    if (showSlices) {
        for (const SliceOverlay& slice : slicesAt(frame)) {
            const ImVec2 a = at(static_cast<float>(slice.bounds.x), static_cast<float>(slice.bounds.y));
            const ImVec2 b = at(static_cast<float>(slice.bounds.x + slice.bounds.w), static_cast<float>(slice.bounds.y + slice.bounds.h));
            draw->AddRect(a, b, slice.color, 0.0f, 0, 1.5f);
            if (slice.hasCenter) {
                const ImU32 inner = (slice.color & 0x00FFFFFFu) | (static_cast<ImU32>(150) << IM_COL32_A_SHIFT);
                draw->AddRect(at(static_cast<float>(slice.bounds.x + slice.center.x), static_cast<float>(slice.bounds.y + slice.center.y)),
                              at(static_cast<float>(slice.bounds.x + slice.center.x + slice.center.w),
                                 static_cast<float>(slice.bounds.y + slice.center.y + slice.center.h)),
                              inner);
            }
            if (slice.hasPivot) {
                cross(at(static_cast<float>(slice.bounds.x + slice.pivot.x), static_cast<float>(slice.bounds.y + slice.pivot.y)), slice.color);
            }
            const ImVec2 textSize = ImGui::CalcTextSize(slice.name.c_str());
            const ImVec2 label(a.x, a.y - textSize.y - 1.0f);
            draw->AddRectFilled(label, ImVec2(label.x + textSize.x + 4.0f, label.y + textSize.y), (slice.color & 0x00FFFFFFu) | (static_cast<ImU32>(200) << IM_COL32_A_SHIFT));
            draw->AddText(ImVec2(label.x + 2.0f, label.y), IM_COL32_WHITE, slice.name.c_str());
        }
    }

    if (showPivot && f) {
        const ImU32 pivot = IM_COL32(255, 0, 200, 255);
        const ImVec2 p = at(f->pivot.x, f->pivot.y);
        cross(p, pivot);
        draw->AddCircle(p, 3.0f, pivot);
        draw->AddText(ImVec2(p.x + 5.0f, p.y + 2.0f), pivot, "pivot");
    }
}

void SpritePreviewWindow::drawFrameStrip() {
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##spritePreviewStrip", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const int frames = frameCount();
    const std::vector<SpriteTag>& list = tags();
    const float cellW = std::max(ImGui::CalcTextSize("0000 ms").x + 8.0f, ImGui::GetFontSize() * 2.5f);

    // tag lanes: overlapping tags stack
    std::vector<int> order(list.size());
    for (size_t i = 0; i < list.size(); i++) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&list](int a, int b) { return list[static_cast<size_t>(a)].from < list[static_cast<size_t>(b)].from; });
    std::vector<int> laneEnds;
    std::vector<int> laneOf(list.size(), 0);
    for (int i : order) {
        const SpriteTag& t = list[static_cast<size_t>(i)];
        const int from = std::min(t.from, t.to), to = std::max(t.from, t.to);
        auto slot = std::find_if(laneEnds.begin(), laneEnds.end(), [from](int end) { return end < from; });
        if (slot == laneEnds.end()) {
            laneOf[static_cast<size_t>(i)] = static_cast<int>(laneEnds.size());
            laneEnds.push_back(to);
        } else {
            laneOf[static_cast<size_t>(i)] = static_cast<int>(slot - laneEnds.begin());
            *slot = to;
        }
    }
    // the next frame's layout sizes the strip by it
    stripLanes = static_cast<int>(laneEnds.size());
    const float tagsHeight = lineHeight * static_cast<float>(laneEnds.size());

    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < list.size(); i++) {
        const SpriteTag& t = list[i];
        const int from = std::clamp(std::min(t.from, t.to), 0, std::max(0, frames - 1));
        const int to = std::clamp(std::max(t.from, t.to), from, std::max(0, frames - 1));
        const ImVec2 a(start.x + static_cast<float>(from) * cellW, start.y + static_cast<float>(laneOf[i]) * lineHeight);
        const ImVec2 b(start.x + static_cast<float>(to + 1) * cellW - 1.0f, a.y + lineHeight - 2.0f);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##tagBand", ImVec2(std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)))) selectTag(static_cast<int>(i));
        ImGui::PopID();
        const bool selected = tag == static_cast<int>(i);
        UIProbe::item((selected ? "spritePreview.tagBandSelected." : "spritePreview.tagBand.") + t.name);
        draw->AddRectFilled(a, b, spritePreviewColor(t.color, selected ? 1.0f : 0.75f), 2.0f);
        if (selected) draw->AddRect(a, b, IM_COL32_WHITE, 2.0f);
        const ImVec4 c(t.color.x, t.color.y, t.color.z, 1.0f);
        const float luminance = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
        draw->PushClipRect(a, b, true);
        draw->AddText(ImVec2(a.x + 3.0f, a.y), luminance > 0.55f ? IM_COL32_BLACK : IM_COL32_WHITE, t.name.c_str());
        draw->PopClipRect();
    }

    const float rowY = start.y + tagsHeight;
    for (int f = 0; f < frames; f++) {
        const ImVec2 a(start.x + static_cast<float>(f) * cellW, rowY);
        const ImVec2 b(a.x + cellW - 1.0f, a.y + lineHeight * 2.0f - 2.0f);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(f);
        if (ImGui::InvisibleButton("##frameCell", ImVec2(cellW - 1.0f, lineHeight * 2.0f - 2.0f))) {
            playing = false;
            frame = f;
            elapsedMs = 0.0;
        }
        const bool cellHovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool current = f == frame;
        UIProbe::item((current ? "spritePreview.frameCurrent." : "spritePreview.frame.") + std::to_string(f));
        draw->AddRectFilled(a, b, ImGui::GetColorU32(current ? ImGuiCol_HeaderActive : (cellHovered ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg)), 2.0f);
        char index[16];
        std::snprintf(index, sizeof(index), "%d", f);
        char duration[24];
        std::snprintf(duration, sizeof(duration), "%u ms", frameDuration(f));
        draw->AddText(ImVec2(a.x + 4.0f, a.y + 1.0f), ImGui::GetColorU32(ImGuiCol_Text), index);
        draw->AddText(ImVec2(a.x + 4.0f, a.y + lineHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled), duration);
        // playing keeps the shown frame in view
        if (current && playing) {
            const float x = a.x - start.x;
            const float visibleW = ImGui::GetWindowWidth();
            if (x < ImGui::GetScrollX() || x + cellW > ImGui::GetScrollX() + visibleW) ImGui::SetScrollX(std::max(0.0f, x - visibleW * 0.5f));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, rowY + lineHeight * 2.0f));
    ImGui::Dummy(ImVec2(static_cast<float>(frames) * cellW, 1.0f));
    ImGui::EndChild();
}
