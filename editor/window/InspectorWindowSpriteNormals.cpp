// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's sprite normals entries (sprite normals Phase 4 spec 5, 6): the image asset view
// of a generated normal map (or colour atlas, or height map) and of the source image a generated
// map is paired with, clicked in the Project window: the image, its layout, what made it, whether
// its source changed, Regenerate / Open in Generator / Assign to Selection and the project's
// auto-regenerate opt-in; and the Normal Texture rows' wand (SpriteNormalsEntry.h). Any other
// image is not an asset view: the Inspector keeps showing the scene selection.

#include "InspectorWindow.h"

#include "App.h"
#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PolygonComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "external/IconsFontAwesome6.h"
#include "mcp/McpValues.h"
#include "spritenormals/SpriteNormalsApply.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "texsynth/SynthImage.h"
#include "util/UIProbe.h"
#include "window/ProjectWindow.h"
#include "window/SpriteNormalsEntry.h"
#include "window/SpriteNormalsWindow.h"
#include "window/Widgets.h"

#include "imgui.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace sn = jasperengine::editor::spritenormals;
namespace sne = jasperengine::editor::spritenormalsentry;

namespace {

    constexpr double PROBE_SECONDS = 1.0;

    // A nearest-filtered copy of a PNG for the view (destroyed when another one replaces it)
    struct Thumbnail {
        fs::path path;
        fs::file_time_type time{};
        Texture texture;
        int width = 0;
        int height = 0;
        bool valid() const { return width > 0 && !texture.empty(); }
    };

    struct AssetState {
        fs::path asset;
        fs::file_time_type time{};
        uintmax_t size = 0;
        double probedAt = -1.0;
        sne::RecipeHead head;              // the asset's own recipe
        fs::path output;                   // the generated map shown (the asset, or its paired map)
        nlohmann::json recipe;             // the output's recipe
        int width = 0, height = 0;
        std::string status;
        bool statusError = false;
        Thumbnail thumbnail;
        Thumbnail pairedThumbnail;
        std::vector<Texture> retired;
    };

    AssetState& state() {
        static AssetState s;
        return s;
    }

    void warningLine(const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    // the image, nearest-filtered, at most 512 px per side (larger ones scaled down)
    void loadThumbnail(AssetState& s, Thumbnail& thumb, const fs::path& path) {
        std::error_code ec;
        const fs::file_time_type time = fs::last_write_time(path, ec);
        if (thumb.path == path && thumb.time == time) return;
        if (!thumb.texture.empty()) s.retired.push_back(thumb.texture);
        thumb = Thumbnail();
        thumb.path = path;
        thumb.time = time;
        // no GPU objects without the editor's renderer
        if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendPlatformName) return;
        texsynth::Image image;
        std::string error;
        if (!texsynth::loadImage(path, image, error, nullptr, 4096)) return;
        texsynth::Image rgba = texsynth::toRgba8(image);
        if (rgba.width > 512 || rgba.height > 512) {
            const double scale = 512.0 / std::max(rgba.width, rgba.height);
            rgba = texsynth::resizeRgba8(rgba, std::max(1, static_cast<int>(rgba.width * scale)), std::max(1, static_cast<int>(rgba.height * scale)));
        }
        static uint64_t counter = 0;
        const size_t bytes = rgba.data.size();
        void* pixels = std::malloc(bytes);
        if (!pixels) return;
        std::memcpy(pixels, rgba.data.data(), bytes);
        TextureData data(rgba.width, rgba.height, static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
        thumb.texture.setData("editor:sprite-normals-asset:" + std::to_string(++counter), data);
        thumb.texture.setMinFilter(TextureFilter::NEAREST);
        thumb.texture.setMagFilter(TextureFilter::NEAREST);
        TextureRender* render = thumb.texture.getRender();
        thumb.texture.releaseData();
        if (!render || !render->isCreated()) {
            s.retired.push_back(thumb.texture);
            thumb.texture = Texture();
            return;
        }
        thumb.width = rgba.width;
        thumb.height = rgba.height;
    }

    // the thumbnail on a checkerboard, at most `maxSide` pixels; true when clicked
    bool drawThumbnail(const Thumbnail& thumb, float maxSide, const char* id) {
        if (!thumb.valid()) return false;
        const float scale = std::min(maxSide / thumb.width, maxSide / thumb.height);
        const float zoom = scale >= 1.0f ? std::floor(scale) : scale;
        const ImVec2 size(thumb.width * zoom, thumb.height * zoom);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float checker = Theme::dpi(6.0f);
        draw->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), IM_COL32(105, 105, 105, 255));
        for (float y = 0; y < size.y; y += checker) {
            for (float x = 0; x < size.x; x += checker) {
                if ((static_cast<int>(x / checker) + static_cast<int>(y / checker)) % 2) continue;
                draw->AddRectFilled(ImVec2(a.x + x, a.y + y), ImVec2(a.x + std::min(x + checker, size.x), a.y + std::min(y + checker, size.y)),
                                    IM_COL32(135, 135, 135, 255));
            }
        }
        draw->AddImage(Backend::getImGuiTexture(const_cast<Texture&>(thumb.texture).getRender()), a, ImVec2(a.x + size.x, a.y + size.y));
        draw->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(a.x + size.x + 1, a.y + size.y + 1), IM_COL32(80, 80, 80, 255));
        return clicked;
    }

    std::string number(double v, int decimals = 2) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, v);
        std::string text = buffer;
        if (text.find('.') != std::string::npos) {
            while (!text.empty() && text.back() == '0') text.pop_back();
            if (!text.empty() && text.back() == '.') text.pop_back();
        }
        return text;
    }

    // "Preset Default, strength 1.5, detail 3, smoothing 1, volume 1.15, radius auto (4.8 px), OpenGL"
    std::string recipeSummary(const nlohmann::json& recipe) {
        if (!recipe.is_object() || !recipe.contains("settings")) return std::string();
        const nlohmann::json& s = recipe["settings"];
        auto num = [&](const char* key) { return s.contains(key) && s[key].is_number() ? number(s[key].get<double>()) : std::string("?"); };
        std::string preset = s.contains("preset") && s["preset"].is_string() ? s["preset"].get<std::string>() : std::string("default");
        if (!preset.empty()) preset[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(preset[0])));
        std::string radius = "auto";
        if (s.contains("radius") && s["radius"].is_number()) radius = number(s["radius"].get<double>()) + " px";
        if (recipe.contains("radius_resolved")) {
            const nlohmann::json& r = recipe["radius_resolved"];
            if (r.is_number() && radius == "auto") radius += " (" + number(r.get<double>(), 1) + " px)";
            else if (r.is_string() && radius == "auto") radius += " (per island)";
        }
        const bool directx = s.contains("directx") && s["directx"].is_boolean() && s["directx"].get<bool>();
        return "Preset " + preset + ", strength " + num("strength") + ", detail " + num("detail") + ", smoothing " + num("smoothing") + ", volume " +
               num("volume") + ", radius " + radius + ", " + (directx ? "DirectX" : "OpenGL");
    }

    // "4 frames 32 x 32, 4 x 1" from the recipe's frames
    std::string framesLine(const nlohmann::json& recipe) {
        if (!recipe.is_object() || !recipe.contains("frames") || !recipe["frames"].is_object()) return std::string();
        const nlohmann::json& f = recipe["frames"];
        if (!f.contains("cells") || !f["cells"].is_array() || f["cells"].size() < 2) return "One image (no frames)";
        const nlohmann::json& c0 = f["cells"][0];
        std::string text = std::to_string(f["cells"].size()) + " frames " + std::to_string(c0[2].get<int>()) + " x " + std::to_string(c0[3].get<int>());
        int columns = f.contains("columns") && f["columns"].is_number() ? f["columns"].get<int>() : 0;
        int rows = f.contains("rows") && f["rows"].is_number() ? f["rows"].get<int>() : 0;
        if (columns <= 0 || rows <= 0) {
            std::vector<int> xs, ys;
            for (const nlohmann::json& c : f["cells"]) {
                if (std::find(xs.begin(), xs.end(), c[0].get<int>()) == xs.end()) xs.push_back(c[0].get<int>());
                if (std::find(ys.begin(), ys.end(), c[1].get<int>()) == ys.end()) ys.push_back(c[1].get<int>());
            }
            columns = static_cast<int>(xs.size());
            rows = static_cast<int>(ys.size());
        }
        return text + ", " + std::to_string(columns) + " x " + std::to_string(rows);
    }

    void reveal(const fs::path& path) {
        if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) projectWindow->revealAsset(path);
    }

}

editor::RowSettings editor::InspectorWindow::normalTextureRowSettings(const RowSettings& base, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                                     unsigned int submesh) {
    RowSettings settings = base;
    if (!sceneProject || entities.empty() || !sne::baseIsImage(project, sceneProject->scene, entities[0], submesh)) return settings;
    // only 2D meshes (a Sprite, Tilemap, Polygon or Polygon Mesh, like check --normals): a Lit 3D mesh's
    // normal map is not a sprite normal map
    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    if (!scene || !(scene->findComponent<SpriteComponent>(entity) || scene->findComponent<TilemapComponent>(entity) ||
                    scene->findComponent<PolygonComponent>(entity) || scene->findComponent<MeshPolygonComponent>(entity))) {
        return settings;
    }
    Project* p = project;
    settings.extraButton = [p, sceneProject, entities, submesh](const ImVec2& size) { sne::wandButton(p, sceneProject, entities, submesh, size); };
    return settings;
}

bool editor::InspectorWindow::drawSpriteNormalsAsset(const fs::path& asset) {
    AssetState& s = state();
    for (Texture& t : s.retired) t.destroy();
    s.retired.clear();
    const fs::path root = project->getProjectPath();
    const double now = ImGui::GetTime();
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(asset, ec);
    if (ec) return false;
    const uintmax_t size = fs::file_size(asset, ec);
    // the probe: what the file is (a generated output, or the source of one), at most once a second
    if (s.asset != asset || s.time != time || s.size != size || s.probedAt < 0.0 || now - s.probedAt >= PROBE_SECONDS || now < s.probedAt) {
        if (s.asset != asset) s.status.clear();
        s.asset = asset;
        s.time = time;
        s.size = size;
        s.probedAt = now;
        s.head = sne::recipeHead(root, asset);
        const fs::path output = s.head.generated ? asset : sne::pairedOutput(root, asset);
        s.output = output;
        s.recipe = nlohmann::json();
        if (!output.empty()) {
            if (std::optional<nlohmann::json> recipe = sn::readRecipeHead(output)) s.recipe = *recipe;
        }
        s.width = s.height = 0;
        texsynth::probeImage(asset, s.width, s.height);
    }
    if (s.output.empty()) return false;
    const bool isOutput = s.head.generated;
    const char* kind = !isOutput ? "Image" : s.head.role == "atlas" ? "Colour Atlas" : s.head.role == "height" ? "Height Map" : "Normal Map";
    drawAssetHeader(ICON_FA_IMAGE, asset, kind);

    // the image and its layout
    loadThumbnail(s, s.thumbnail, asset);
    drawThumbnail(s.thumbnail, ImGui::GetFontSize() * 8.0f, "##snAssetThumb");
    UIProbe::item("snAsset.thumbnail");
    ImGui::TextUnformatted((std::to_string(s.width) + " x " + std::to_string(s.height)).c_str());
    const std::string frames = framesLine(s.recipe);
    if (!frames.empty()) ImGui::TextDisabled("%s", frames.c_str());

    // what made it, or what was made from it
    const sne::RecipeHead outputHead = sne::recipeHead(root, s.output);
    const std::string sourceName = sne::fileName(outputHead.source);
    if (isOutput) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Source:");
        ImGui::SameLine();
        if (ImGui::SmallButton((sourceName + "##snAssetSource").c_str())) reveal(outputHead.sourceAbsolute);
        UIProbe::item("snAsset.source");
        ImGui::SetItemTooltip("Select %s in the Project window", outputHead.source.c_str());
        const std::string summary = recipeSummary(s.recipe);
        if (!summary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
            ImGui::TextWrapped("%s", summary.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Normal map:");
        ImGui::SameLine();
        if (ImGui::SmallButton((s.output.filename().u8string() + "##snAssetPaired").c_str())) reveal(s.output);
        UIProbe::item("snAsset.paired");
        ImGui::SetItemTooltip("Select it in the Project window");
        loadThumbnail(s, s.pairedThumbnail, s.output);
        if (drawThumbnail(s.pairedThumbnail, ImGui::GetFontSize() * 5.0f, "##snAssetPairedThumb")) reveal(s.output);
        ImGui::SetItemTooltip("Select it in the Project window");
    }

    // staleness: the watcher's (it hashes sources off the main thread); a missing source is a stat
    SpriteNormalsWatcher* watcher = SpriteNormalsWatcher::active();
    const bool missing = (watcher && watcher->sourceMissing(s.output)) || (!outputHead.sourceAbsolute.empty() && !fs::exists(outputHead.sourceAbsolute, ec));
    if (!missing && watcher && watcher->isStale(s.output)) {
        warningLine(sourceName + " changed since this normal map was generated.");
        UIProbe::item("snAsset.stale");
    } else if (missing) {
        warningLine("The source " + sourceName + " is missing.");
        UIProbe::item("snAsset.missing");
    }

    // actions
    const bool playing = project->isAnyScenePlaying();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(missing || playing);
    if (ImGui::Button(ICON_FA_ROTATE " Regenerate##snAssetRegenerate", ImVec2(half, 0))) {
        // on the worker: the Sprite Normal Map window's tick writes it (one Project undo step)
        std::string error;
        if (Backend::getApp().getSpriteNormalsWindow()->startRegenerate(s.output, error)) {
            s.status = "Regenerating " + s.output.filename().u8string() + "...";
            s.statusError = false;
            s.probedAt = -1.0;
        } else {
            s.status = "Can't regenerate " + s.output.filename().u8string() + ": " + error;
            s.statusError = true;
            getEditorHost().registerAlert("Sprite Normal Map", s.status);
        }
    }
    ImGui::EndDisabled();
    UIProbe::item("snAsset.regenerate");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", missing ? "The source is missing"
                                : playing ? "Stop the scene first: regenerating reloads the texture the game draws"
                                          : "Generate it again from its source with the same settings");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Open in Generator##snAssetOpen", ImVec2(half, 0))) {
        getEditorHost().openSpriteNormals({s.output}, true);
    }
    UIProbe::item("snAsset.open");
    ImGui::SetItemTooltip("Reopen the settings this normal map was generated with in the Sprite Normal Map window");

    // Assign to Selection: the selected Sprites drawing the source (or its atlas)
    const sne::RecipeHead normalHead = outputHead;
    const fs::path normal = s.output;
    std::string atlas;
    if (s.recipe.is_object() && s.recipe.contains("outputs") && s.recipe["outputs"].is_object() && s.recipe["outputs"].contains("atlas") &&
        s.recipe["outputs"]["atlas"].is_string()) {
        atlas = fs::u8path(s.recipe["outputs"]["atlas"].get<std::string>()).lexically_normal().generic_u8string();
    }
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    std::vector<sn::ApplyRequest> requests;
    if (sceneProject && sceneProject->scene && normalHead.role == "normal") {
        const std::string source = fs::u8path(normalHead.source).lexically_normal().generic_u8string();
        const std::string normalRel = sn::projectRelative(root, normal);
        for (Entity e : project->getSelectedEntities(sceneId)) {
            MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(e);
            if (!mesh || !sceneProject->scene->findComponent<SpriteComponent>(e)) continue;
            const std::string base = fs::u8path(mcp::McpValues::assetPathToJson(project, mesh->submeshes[0].material.baseColorTexture.getPath()))
                                         .lexically_normal()
                                         .generic_u8string();
            if (base.empty() || (base != source && base != atlas)) continue;
            sn::ApplyRequest request;
            request.sceneId = sceneId;
            request.entity = e;
            request.normalPath = normalRel;
            request.colorSourcePath = base;
            requests.push_back(request);
        }
    }
    if (!requests.empty()) {
        ImGui::BeginDisabled(playing);
        const std::string label = std::string(ICON_FA_ARROW_POINTER " Assign to Selection") + (requests.size() > 1 ? " (" + std::to_string(requests.size()) + ")" : "") +
                                  "##snAssetAssign";
        if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0))) {
            const sn::ApplyResult result = sn::applyToSprites(project, requests);
            s.status = result.ok ? "Assigned to " + std::to_string(requests.size()) + " sprite" + (requests.size() > 1 ? "s" : "") + " (Scene undo)."
                                 : "Can't assign: " + result.error.message;
            s.statusError = !result.ok;
        }
        ImGui::EndDisabled();
        UIProbe::item("snAsset.assign");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", playing ? "Stop the scene first" : "Set it as the Normal Texture of the selected sprites");
    }

    bool autoRegenerate = project->getSpriteNormalsSettings().autoRegenerate;
    if (ImGui::Checkbox("Auto-regenerate when sources change##snAssetAuto", &autoRegenerate)) {
        project->getSpriteNormalsSettings().autoRegenerate = autoRegenerate;
    }
    UIProbe::item("snAsset.autoRegenerate");
    ImGui::SetItemTooltip("Regenerate this project's normal maps when their source images change (not undoable, logged in the Console)");

    // a Regenerate started here: what it really came to (written, refused, failed, or waiting for Stop)
    if (s.status.rfind("Regenerating ", 0) == 0) {
        const SpriteNormalsWindow::RegenerateOutcome outcome = Backend::getApp().getSpriteNormalsWindow()->regenerateOutcome(s.output);
        if (outcome.known) {
            s.status = outcome.message;
            s.statusError = outcome.finished && !outcome.ok;
        }
    }
    if (!s.status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, s.statusError ? Theme::Colors::ErrorText : Theme::Colors::SubtleText);
        ImGui::TextWrapped("%s", s.status.c_str());
        ImGui::PopStyleColor();
    }
    return true;
}
