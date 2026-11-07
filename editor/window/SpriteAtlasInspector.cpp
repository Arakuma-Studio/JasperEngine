// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteAtlasInspector.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/aseprite/AsepriteEditorHooks.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "window/ImporterFields.h"
#include "window/ProjectWindow.h"

#include "jobs/JobSystem.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* kPlatformLabels[] = {"Default", "Desktop", "Mobile", "Web", "Android ETC2"};
    const int kMaxSizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};

    bool atlasInspectorUpload(Texture& texture, int& width, int& height, const pixelart::PixelImage& image) {
        if (image.empty()) return false;
        static uint64_t counter = 0;
        const size_t bytes = image.pixels().size();
        void* pixels = std::malloc(bytes);
        if (!pixels) return false;
        std::memcpy(pixels, image.pixels().data(), bytes);
        TextureData data(image.width(), image.height(), static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
        texture.setData("editor:sprite-atlas:" + std::to_string(++counter), data);
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        TextureRender* render = texture.getRender();
        texture.releaseData();
        if (!render || !render->isCreated()) {
            texture.destroy();
            return false;
        }
        width = image.width();
        height = image.height();
        return true;
    }

    void atlasProblemLine(const pixelart::PixelProblem& p) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", p.message.c_str());
        if (!p.fix.empty()) ImGui::TextDisabled("%s", p.fix.c_str());
        ImGui::PopTextWrapPos();
    }

    void checkerboard(ImDrawList* draw, ImVec2 a, ImVec2 b, float cell) {
        draw->AddRectFilled(a, b, IM_COL32(200, 200, 200, 255));
        for (float y = a.y; y < b.y; y += cell) {
            for (float x = a.x + (static_cast<int>((y - a.y) / cell) % 2 ? cell : 0.0f); x < b.x; x += 2.0f * cell) {
                draw->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, b.x), std::min(y + cell, b.y)), IM_COL32(150, 150, 150, 255));
            }
        }
    }

}

// ------------------------------------------------------------------ SheetPreviewTextures

SheetPreviewTextures::~SheetPreviewTextures() {
    clear();
}

void SheetPreviewTextures::clear() {
    for (auto& [path, image] : images) image.texture.destroy();
    images.clear();
}

SheetPreviewTextures::Image* SheetPreviewTextures::get(Project* project, const std::string& path) {
    fs::path file(path);
    if (!file.is_absolute() && project) file = project->getAssetsPath() / file;
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    if (ec) return nullptr;
    auto it = images.find(path);
    if (it != images.end() && it->second.time == time) return it->second.valid() ? &it->second : nullptr;
    if (it != images.end()) {
        it->second.texture.destroy();
        images.erase(it);
    }
    Image& image = images[path];
    image.time = time;
    std::optional<pixelart::PixelImage> pixels = pixelart::PixelImage::loadPng(file, nullptr);
    if (pixels) atlasInspectorUpload(image.texture, image.width, image.height, *pixels);
    return image.valid() ? &image : nullptr;
}

SheetPreviewTextures::Image* SheetPreviewTextures::fromImage(const std::string& key, const pixelart::PixelImage& pixels) {
    auto it = images.find(key);
    if (it != images.end()) return it->second.valid() ? &it->second : nullptr;
    Image& image = images[key];
    atlasInspectorUpload(image.texture, image.width, image.height, pixels);
    return image.valid() ? &image : nullptr;
}

// ------------------------------------------------------------------ inspector

SpriteAtlasInspector::SpriteAtlasInspector(Project* project): project(project) {
    ImporterPrompt::add(this, {[this]() { return dirty; }, [this]() { return fs::path(atlasPath).filename().string(); },
                               [this]() { apply(); }, [this]() { revert(); }});
}

SpriteAtlasInspector::~SpriteAtlasInspector() {
    ImporterPrompt::remove(this);
}

void SpriteAtlasInspector::select(const fs::path& selection) {
    const fs::path file = (project->getProjectPath() / selection).lexically_normal();
    const std::string asset = file.lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    if (asset == atlasPath && time == fileTime && (stored || !readError.empty())) return;
    if (asset != atlasPath && dirty) {
        // the old selection's edits: the prompt asks first
        ImporterPrompt::guard(nullptr);
        return;
    }
    const bool sameAtlas = asset == atlasPath;
    atlasPath = asset;
    absolute = file;
    fileTime = time;
    readError = pixelart::PixelProblem();
    stored = sprite::SpriteAtlasBuilder::readDocument(file, &readError);
    if (stored && !(sameAtlas && dirty)) working = *stored;
    if (!sameAtlas) {
        preview.reset();
        previewInMemory = false;
        page = 0;
        selectedFrame = -1;
        textures.clear();
    }
}

void SpriteAtlasInspector::revert() {
    if (stored) working = *stored;
    dirty = false;
}

void SpriteAtlasInspector::apply() {
    if (!dirty) return;
    std::string error;
    if (!ProjectFileWrites::writeUndoable(project, {{absolute, sprite::writeSpriteAtlas(working)}}, error, nullptr, nullptr, {}, false,
                                          "Edit Sprite Atlas " + absolute.filename().string())) {
        Out::error("Couldn't save %s: %s", absolute.filename().string().c_str(), error.c_str());
        return;
    }
    dirty = false;
    stored = working;
    std::error_code ec;
    fileTime = fs::last_write_time(absolute, ec);
    previewInMemory = false;
    sprite::SpriteAtlasBuilder::get().rebuild(atlasPath);
}

void SpriteAtlasInspector::startPreview() {
    sprite::AtlasBuildInput input;
    const std::string platform = platformTab == 0 ? std::string("desktop") : sprite::kAtlasPlatforms[platformTab - 1];
    sprite::SpriteAtlasBuilder::gather(*project, atlasPath, working, platform, input);
    input.outDir.clear();
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "sprite-atlas-pack-preview";
    packing = jobs::submit([input]() { return sprite::SpriteAtlasBuilder::get().build(input); }, options);
}

void SpriteAtlasInspector::openMember(const std::string& member, bool open) {
    const fs::path file = project->getAssetsPath() / member;
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) projectWindow->revealAsset(file);
    if (open && SpriteSheetPool::isAsepritePath(member)) pixelart::aseprite::openInAsepriteOrFallback(project, {member}, true);
}

bool SpriteAtlasInspector::draw(const std::vector<fs::path>& selection) {
    if (selection.size() != 1 || !SpriteSheetSources::isAtlasPath(selection[0].generic_string())) return false;
    select(selection[0]);
    sprite::SpriteAtlasBuilder& builder = sprite::SpriteAtlasBuilder::get();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ICON_FA_TABLE_CELLS);
    ImGui::SameLine();
    ImGui::TextUnformatted(absolute.filename().string().c_str());
    ImGui::TextDisabled("Sprite Atlas");
    ImGui::Separator();
    if (!stored) {
        atlasProblemLine(readError);
        return true;
    }
    // the state: building, failed, built
    std::shared_ptr<const sprite::AtlasBuildResult> built = builder.result(atlasPath);
    std::shared_ptr<const sprite::AtlasBuildResult> attempt = builder.lastAttempt(atlasPath);
    if (builder.isBuilding(atlasPath)) {
        ImGui::TextDisabled(ICON_FA_SPINNER " %s", builder.statusText().empty() ? "Building..." : builder.statusText().c_str());
    } else if (attempt && !attempt->ok && attempt->error.kind != "cancelled") {
        atlasProblemLine(attempt->error);
    } else if (builder.mode() == sprite::AtlasMode::Disabled) {
        ImGui::TextDisabled("Sprite Atlas mode is Disabled (Project Settings > 2D): nothing is built.");
    }
    if (packing && packing->isReady()) {
        if (packing->status() == jobs::TaskStatus::Completed) {
            preview = packing->get();
            previewInMemory = true;
            textures.clear();
        }
        packing.reset();
    }
    if (!previewInMemory) preview = built;

    if (ImGui::CollapsingHeader("Objects for Packing", ImGuiTreeNodeFlags_DefaultOpen)) drawMembers();
    if (ImGui::CollapsingHeader("Packing", ImGuiTreeNodeFlags_DefaultOpen)) drawSettings();

    ImGui::Separator();
    ImGui::BeginDisabled(packing.has_value());
    if (ImGui::Button(ICON_FA_EYE " Pack Preview")) startPreview();
    UIProbe::item("spriteAtlas.packPreview");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Packs the settings below in memory (nothing written) for the selected platform tab");
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Revert")) revert();
    UIProbe::item("spriteAtlas.revert");
    ImGui::SameLine();
    if (ImGui::Button("Apply")) apply();
    UIProbe::item("spriteAtlas.apply");
    ImGui::EndDisabled();

    drawWarnings(preview.get());
    if (preview && preview->ok) {
        drawStats(*preview);
        if (ImGui::CollapsingHeader("Pages", ImGuiTreeNodeFlags_DefaultOpen)) drawPreview(*preview);
        if (ImGui::CollapsingHeader("Frames", ImGuiTreeNodeFlags_DefaultOpen)) drawFrameList(*preview);
    } else if (!preview) {
        ImGui::TextDisabled("Not built yet.");
    }
    return true;
}

void SpriteAtlasInspector::drawMembers() {
    ImGui::PushID("members");
    if (ImGui::Checkbox("Include in Build", &working.includeInBuild)) dirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: builds never draw from it (a script can still load it by path)");
    int removeIndex = -1;
    if (ImGui::BeginTable("atlas_members", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("member", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        for (size_t i = 0; i < working.members.size(); i++) {
            const sprite::SpriteAtlasMember& m = working.members[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (m.isFolder()) ImGui::Text(ICON_FA_FOLDER " %s/", m.folder.c_str());
            else ImGui::Text("%s %s", SpriteSheetPool::isAsepritePath(m.path) ? ICON_FA_PEN_TO_SQUARE : ICON_FA_IMAGE, m.path.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(ICON_FA_MINUS)) removeIndex = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIndex >= 0) {
        working.members.erase(working.members.begin() + removeIndex);
        dirty = true;
    }
    // drop files and folders from the Project window
    ImGui::Button(ICON_FA_PLUS " Add (drop textures, .aseprite files or folders here)", ImVec2(-FLT_MIN, 0));
    UIProbe::item("spriteAtlas.addMembers");
    auto addPath = [&](const fs::path& dropped) {
        const fs::path file = dropped.is_absolute() ? dropped : project->getProjectPath() / dropped;
        std::string asset = file.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
        if (asset.empty() || asset.rfind("..", 0) == 0) return;
        std::error_code ec;
        sprite::SpriteAtlasMember m;
        if (fs::is_directory(file, ec)) m.folder = asset;
        else if (SpriteSheetSources::isTexturePath(asset) || SpriteSheetPool::isAsepritePath(asset)) {
            m.path = asset;
            m.guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(file));
        } else return;
        for (const sprite::SpriteAtlasMember& existing : working.members) {
            if ((m.isFolder() && existing.folder == m.folder) || (!m.isFolder() && existing.path == m.path)) return;
        }
        working.members.push_back(m);
        dirty = true;
    };
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            for (const std::string& path : Util::getStringsFromPayload(payload)) addPath(FileUtils::pathFromUtf8(path));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked()) {
        // the Project window's selection
        if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
            for (const fs::path& path : projectWindow->getSelectedFiles()) addPath(path);
        }
    }
    // Exclude: path prefixes inside the folder members
    if (ImGui::TreeNode("Exclude")) {
        int removeExclude = -1;
        for (size_t i = 0; i < working.exclude.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(ICON_FA_MINUS)) removeExclude = static_cast<int>(i);
            ImGui::SameLine();
            ImGui::TextUnformatted(working.exclude[i].c_str());
            ImGui::PopID();
        }
        if (removeExclude >= 0) {
            working.exclude.erase(working.exclude.begin() + removeExclude);
            dirty = true;
        }
        static char excludeBuffer[256] = "";
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##exclude", "ui/icons/old", excludeBuffer, sizeof(excludeBuffer));
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_PLUS "##add_exclude") && excludeBuffer[0]) {
            working.exclude.push_back(excludeBuffer);
            excludeBuffer[0] = '\0';
            dirty = true;
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void SpriteAtlasInspector::drawSettings() {
    sprite::SpriteAtlasSettings& s = working.settings;
    ImGui::PushID("settings");
    // Max Texture Size per platform tab
    if (ImGui::BeginTabBar("platforms")) {
        for (int i = 0; i < 5; i++) {
            if (ImGui::BeginTabItem(kPlatformLabels[i])) {
                platformTab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    int* maxSize = &s.maxSize;
    bool overridden = true;
    if (platformTab > 0) {
        sprite::AtlasPlatformOverride& o = s.platforms[sprite::kAtlasPlatforms[platformTab - 1]];
        if (o.maxSize <= 0) o.maxSize = s.maxSize;
        const std::string label = std::string("Override for ") + kPlatformLabels[platformTab];
        if (ImGui::Checkbox(label.c_str(), &o.override)) dirty = true;
        maxSize = &o.maxSize;
        overridden = o.override;
    }
    auto row = [](const char* label) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    if (ImGui::BeginTable("packing", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        row("Max Texture Size");
        ImGui::BeginDisabled(!overridden);
        const std::string current = std::to_string(*maxSize);
        if (ImGui::BeginCombo("##max_size", current.c_str())) {
            for (int size : kMaxSizes) {
                if (ImGui::Selectable(std::to_string(size).c_str(), size == *maxSize)) {
                    *maxSize = size;
                    dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        row("Padding");
        if (ImGui::SliderInt("##padding", &s.padding, 0, 16)) dirty = true;
        row("Extrude Edges");
        if (ImGui::SliderInt("##extrude", &s.extrude, 0, 4)) dirty = true;
        row("Trim");
        if (ImGui::Checkbox("##trim", &s.trim)) dirty = true;
        row("Keep Members Together");
        if (ImGui::Checkbox("##together", &s.keepMembersTogether)) dirty = true;
        row("Power of Two");
        if (ImGui::Checkbox("##pot", &s.powerOfTwo)) dirty = true;
        row("Square");
        if (ImGui::Checkbox("##square", &s.square)) dirty = true;
        row("Compression");
        {
            const char* names[] = {"Auto", "Lossless", "Platform Default"};
            const char* tokens[] = {"auto", "lossless", "platform_default"};
            int index = 0;
            for (int i = 0; i < 3; i++) if (s.compression == tokens[i]) index = i;
            if (ImGui::Combo("##compression", &index, names, 3)) {
                s.compression = tokens[index];
                dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto: lossless (RGBA8) when a member is an .aseprite file, a lossless or sliced texture; else each platform's sprite format");
        }
        row("Filter");
        {
            const char* names[] = {"Auto from members", "Point", "Bilinear"};
            const char* tokens[] = {"auto", "point", "bilinear"};
            int index = 0;
            for (int i = 0; i < 3; i++) if (s.filter == tokens[i]) index = i;
            if (ImGui::Combo("##filter", &index, names, 3)) {
                s.filter = tokens[index];
                dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bilinear needs extruded edges: the build extrudes at least 2 pixels for it");
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void SpriteAtlasInspector::drawWarnings(const sprite::AtlasBuildResult* result) {
    std::vector<pixelart::PixelProblem> problems = sprite::SpriteAtlasBuilder::get().crossWarnings(atlasPath);
    if (result) problems.insert(problems.end(), result->warnings.begin(), result->warnings.end());
    if (result && !result->ok && result->error.kind != "cancelled") problems.insert(problems.begin(), result->error);
    for (size_t i = 0; i < problems.size(); i++) {
        const pixelart::PixelProblem& p = problems[i];
        atlasProblemLine(p);
        ImGui::PushID(static_cast<int>(i));
        // the fixes that exist: remove a double-packed (or missing) member, raise Max Size
        if (p.kind == "double_packed" || p.kind == "missing_member") {
            bool member = false;
            for (const sprite::SpriteAtlasMember& m : working.members) member = member || (!m.isFolder() && m.path == p.field) || (m.isFolder() && m.folder == p.field);
            if (member || p.kind == "double_packed") {
                ImGui::SameLine();
                if (ImGui::SmallButton("Fix: remove it")) {
                    const size_t before = working.members.size();
                    working.members.erase(std::remove_if(working.members.begin(), working.members.end(), [&](const sprite::SpriteAtlasMember& m) {
                        return (m.isFolder() ? m.folder : m.path) == p.field;
                    }), working.members.end());
                    if (working.members.size() == before) working.exclude.push_back(p.field);
                    dirty = true;
                }
            }
        } else if (p.kind == "frame_too_large" && working.settings.maxSize < 8192) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix: raise Max Size")) {
                working.settings.maxSize = std::min(8192, working.settings.maxSize * 2);
                dirty = true;
            }
        }
        ImGui::PopID();
    }
}

void SpriteAtlasInspector::drawStats(const sprite::AtlasBuildResult& r) {
    ImGui::PushTextWrapPos(0.0f);
    std::string line = r.statsLine();
    if (previewInMemory) line = "Preview (" + r.platform + ", not written): " + line;
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopTextWrapPos();
    UIProbe::item("spriteAtlas.stats");
    if (ImGui::TreeNode("Waste")) {
        for (size_t p = 0; p < r.pages.size(); p++) {
            const long long area = static_cast<long long>(r.pages[p].width) * r.pages[p].height;
            ImGui::Text("Page %zu: %d x %d, %d frames, %lld wasted pixels (%d %% used)", p, r.pages[p].width, r.pages[p].height, r.pages[p].frames,
                        area - static_cast<long long>(std::llround(r.pages[p].used * static_cast<double>(area))), static_cast<int>(std::lround(r.pages[p].used * 100.0)));
        }
        ImGui::Separator();
        ImGui::TextDisabled("Most transparent area before trim:");
        for (size_t i = 0; i < r.worstMembers.size() && i < 5; i++) {
            ImGui::Text("%s: %d px", r.worstMembers[i].first.c_str(), r.worstMembers[i].second);
        }
        ImGui::TreePop();
    }
}

SheetPreviewTextures::Image* SpriteAtlasInspector::pageImage(const sprite::AtlasBuildResult& r, int index) {
    if (index < 0 || static_cast<size_t>(index) >= r.pages.size()) return nullptr;
    if (static_cast<size_t>(index) < r.pageImages.size()) {
        return textures.fromImage("preview:" + r.key + ":" + std::to_string(index), r.pageImages[static_cast<size_t>(index)]);
    }
    return textures.get(project, r.pageFile(static_cast<size_t>(index)).string());
}

void SpriteAtlasInspector::drawPreview(const sprite::AtlasBuildResult& r) {
    if (r.pages.empty()) return;
    page = std::clamp(page, 0, static_cast<int>(r.pages.size()) - 1);
    if (ImGui::BeginTabBar("pages")) {
        for (size_t p = 0; p < r.pages.size(); p++) {
            if (ImGui::BeginTabItem(("Page " + std::to_string(p)).c_str())) {
                page = static_cast<int>(p);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    const char* zooms[] = {"Fit", "1x", "2x", "4x", "8x", "16x"};
    const int zoomValues[] = {0, 1, 2, 4, 8, 16};
    int zoomIndex = 0;
    for (int i = 0; i < 6; i++) if (zoomValues[i] == zoom) zoomIndex = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    if (ImGui::Combo("Zoom", &zoomIndex, zooms, 6)) zoom = zoomValues[zoomIndex];
    SheetPreviewTextures::Image* image = pageImage(r, page);
    if (!image) {
        ImGui::TextDisabled("The page does not load.");
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    float scale = zoom > 0 ? static_cast<float>(zoom) : std::max(1.0f, std::floor(avail / static_cast<float>(image->width)));
    if (zoom == 0 && avail < static_cast<float>(image->width)) scale = avail / static_cast<float>(image->width);
    const ImVec2 size(static_cast<float>(image->width) * scale, static_cast<float>(image->height) * scale);
    ImGui::BeginChild("page_view", ImVec2(0, std::min(size.y + 16.0f, ImGui::GetFontSize() * 30.0f)), true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    checkerboard(draw, origin, ImVec2(origin.x + size.x, origin.y + size.y), std::max(4.0f, 8.0f * scale / 2.0f));
    draw->AddImage(Backend::getImGuiTexture(image->texture.getRender()), origin, ImVec2(origin.x + size.x, origin.y + size.y));
    ImGui::InvisibleButton("page_image", size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hoveredFrame = -1;
    const int extrude = std::max(working.settings.extrude, r.filter == "bilinear" ? 2 : 0);
    for (size_t i = 0; i < r.frames.size(); i++) {
        const sprite::AtlasFrameRow& f = r.frames[i];
        if (f.page != page || f.duplicateOf >= 0) continue;
        const ImVec2 a(origin.x + static_cast<float>(f.rect.x) * scale, origin.y + static_cast<float>(f.rect.y) * scale);
        const ImVec2 b(a.x + static_cast<float>(f.rect.w) * scale, a.y + static_cast<float>(f.rect.h) * scale);
        // the extrusion around the trimmed pixels
        if (extrude > 0) {
            const float e = static_cast<float>(extrude) * scale;
            draw->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), IM_COL32(255, 160, 0, 90));
        }
        const bool selected = static_cast<int>(i) == selectedFrame;
        draw->AddRect(a, b, selected ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 200, 255, 160), 0.0f, 0, selected ? 2.0f : 1.0f);
        if (hovered && mouse.x >= a.x && mouse.y >= a.y && mouse.x < b.x && mouse.y < b.y) hoveredFrame = static_cast<int>(i);
    }
    if (hoveredFrame >= 0) {
        const sprite::AtlasFrameRow& f = r.frames[static_cast<size_t>(hoveredFrame)];
        ImGui::BeginTooltip();
        ImGui::Text("%s", f.name.c_str());
        ImGui::TextDisabled("%s", f.member.c_str());
        ImGui::Text("%d x %d, trimmed from %d x %d at (%d, %d)", f.rect.w, f.rect.h, f.sourceSize.w, f.sourceSize.h, f.offset.x, f.offset.y);
        ImGui::Text("Page %d [%d, %d, %d, %d]", f.page, f.rect.x, f.rect.y, f.rect.w, f.rect.h);
        ImGui::EndTooltip();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selectedFrame = hoveredFrame;
            openMember(f.member, false);
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openMember(f.member, true);
    }
    ImGui::EndChild();
}

void SpriteAtlasInspector::drawFrameList(const sprite::AtlasBuildResult& r) {
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    ImGui::InputTextWithHint("##frame_search", "Search: name, member:<path>, page:<n>", search, sizeof(search));
    UIProbe::item("spriteAtlas.frameSearch");
    ImGui::SameLine();
    const char* sorts[] = {"Name", "Size", "Page", "Waste"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##frame_sort", &sortBy, sorts, 4);
    const nlohmann::json rows = r.toJson(search, 0, -1, true)["frames"];
    std::vector<int> order;
    for (const nlohmann::json& row : rows) order.push_back(row["index"].get<int>());
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const sprite::AtlasFrameRow& fa = r.frames[static_cast<size_t>(a)];
        const sprite::AtlasFrameRow& fb = r.frames[static_cast<size_t>(b)];
        switch (sortBy) {
            case 1: return fa.rect.w * fa.rect.h > fb.rect.w * fb.rect.h;
            case 2: return fa.page < fb.page;
            case 3: return fa.waste > fb.waste;
            default: return fa.name < fb.name;
        }
    });
    ImGui::TextDisabled("%zu of %zu frames", order.size(), r.frames.size());
    const float thumb = ImGui::GetFontSize() * 2.0f;
    if (ImGui::BeginTable("atlas_frames", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, ImGui::GetFontSize() * 18.0f))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, thumb);
        ImGui::TableSetupColumn("Frame");
        ImGui::TableSetupColumn("Member");
        ImGui::TableSetupColumn("Page / Size / Waste");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(order.size()), thumb + ImGui::GetStyle().CellPadding.y * 2.0f);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const int index = order[static_cast<size_t>(row)];
                const sprite::AtlasFrameRow& f = r.frames[static_cast<size_t>(index)];
                ImGui::TableNextRow(0, thumb);
                ImGui::TableSetColumnIndex(0);
                if (SheetPreviewTextures::Image* image = pageImage(r, f.page); image && f.rect.w > 0 && f.rect.h > 0) {
                    const float w = static_cast<float>(image->width), h = static_cast<float>(image->height);
                    const float k = thumb / static_cast<float>(std::max(f.rect.w, f.rect.h));
                    ImGui::Image(Backend::getImGuiTexture(image->texture.getRender()), ImVec2(f.rect.w * k, f.rect.h * k),
                                 ImVec2(f.rect.x / w, f.rect.y / h), ImVec2((f.rect.x + f.rect.w) / w, (f.rect.y + f.rect.h) / h));
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(index);
                if (ImGui::Selectable(f.name.c_str(), index == selectedFrame, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedFrame = index;
                    page = f.page;
                    openMember(f.member, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", f.member.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d \xC2\xB7 %dx%d \xC2\xB7 %d", f.page, f.rect.w, f.rect.h, f.waste);
            }
        }
        ImGui::EndTable();
    }
}
