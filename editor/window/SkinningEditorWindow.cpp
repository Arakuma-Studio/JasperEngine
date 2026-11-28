// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SkinningEditorWindow.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "util/UIProbe.h"

#include "component/SpriteComponent.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "sprite/SpriteSheetPool.h"
#include "texture/TextureData.h"
#include "util/Angle.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct SkinningOpenRequest {
        bool pending = false;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
    };

    SkinningOpenRequest& skinningOpenRequest() {
        static SkinningOpenRequest request;
        return request;
    }

    // a bone's colour: hues around the wheel, the same for a bone index everywhere
    ImU32 skinningBoneColor(int bone, int alpha = 255) {
        const float hue = std::fmod(static_cast<float>(bone) * 0.61803398875f, 1.0f);
        float r = 0, g = 0, b = 0;
        ImGui::ColorConvertHSVtoRGB(hue, 0.75f, 1.0f, r, g, b);
        return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), alpha);
    }

    bool skinningUpload(Texture& texture, const editor::pixelart::PixelImage& image, const std::string& id) {
        if (image.empty()) return false;
        const size_t bytes = image.pixels().size();
        void* pixels = std::malloc(bytes);
        if (!pixels) return false;
        std::memcpy(pixels, image.pixels().data(), bytes);
        // TextureData frees what it is given
        TextureData data(image.width(), image.height(), static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
        texture.setData(id, data);
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        TextureRender* render = texture.getRender();
        texture.releaseData();
        if (!render || !render->isCreated()) {
            texture.destroy();
            return false;
        }
        return true;
    }

    // the bone's bind frame in the frame's pixels (y down): at its head, x along head -> tail
    Matrix4 skinningBind(const SpriteBone& bone) {
        const Vector2 d = bone.tail - bone.head;
        const float angle = (d.x == 0.0f && d.y == 0.0f) ? 0.0f : std::atan2(d.y, d.x);
        return Matrix4::composeTRS(Vector3(bone.head.x, bone.head.y, 0.0f), Quaternion(0.0f, 0.0f, Angle::radToDefault(angle)), Vector3(1.0f, 1.0f, 1.0f));
    }

}

SkinningEditorWindow::SkinningEditorWindow(Project* project): project(project) {
}

void SkinningEditorWindow::requestOpen(uint32_t sceneId, Entity entity) {
    SkinningOpenRequest& request = skinningOpenRequest();
    request.pending = true;
    request.sceneId = sceneId;
    request.entity = entity;
}

void SkinningEditorWindow::open() {
    setOpen(true);
    focusRequested = true;
}

void SkinningEditorWindow::setOpen(bool open) {
    windowOpen = open;
    if (!open) {
        stroking = false;
        draggingBone = false;
        turningBone = -1;
    }
}

void SkinningEditorWindow::resetForProjectSwitch() {
    hasTarget = false;
    entity = NULL_ENTITY;
    frameKey.clear();
    pageKey.clear();
    skin = SpriteSkinGeometry();
    if (!pageTexture.empty()) retired.push_back(pageTexture);
    pageTexture = Texture();
}

void SkinningEditorWindow::setStatus(const std::string& message, bool error) {
    statusMessage = message;
    statusIsError = error;
}

void SkinningEditorWindow::followSelection() {
    if (locked) return;
    const uint32_t selectedScene = project->getSelectedSceneId();
    SceneProject* scene = project->getScene(selectedScene);
    if (!scene || !scene->scene) return;
    for (Entity selected : project->getSelectedEntities(selectedScene)) {
        if (scene->scene->isEntityCreated(selected) && scene->scene->findComponent<SpriteComponent>(selected)) {
            if (selected != entity || selectedScene != sceneId) {
                sceneId = selectedScene;
                entity = selected;
                frameKey.clear();
                selectedBone = -1;
                previewAngles.clear();
            }
            return;
        }
    }
}

void SkinningEditorWindow::refreshTarget() {
    hasTarget = entity != NULL_ENTITY && sprite::resolveSkinTarget(project, sceneId, entity, target, targetError);
    if (!hasTarget) return;
    // the stored skin again when the frame or its meta changed (an undo, an agent's edit)
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(fs::path(target.absoluteSource.string() + ".meta"), ec);
    const std::string key = target.source + "|" + target.frameData().name;
    if (!stroking && (key != frameKey || (!ec && time != metaTime))) {
        frameKey = key;
        metaTime = ec ? fs::file_time_type() : time;
        skin = sprite::storedSkin(target);
        if (selectedBone >= static_cast<int>(skin.bones.size())) selectedBone = -1;
    }
    previewAngles.resize(skin.bones.size(), 0.0f);
    uploadPage();
}

void SkinningEditorWindow::uploadPage() {
    const SpriteFrame& frame = target.frameData();
    const std::string key = frame.page < target.sheet->pages.size() ? target.sheet->pages[frame.page].texture : std::string();
    if (key == pageKey) return;
    pageKey = key;
    if (!pageTexture.empty()) retired.push_back(pageTexture);
    pageTexture = Texture();
    pageWidth = pageHeight = 0;
    std::string error;
    std::optional<editor::pixelart::PixelImage> page = sprite::loadFramePage(project, *target.sheet, frame, &error);
    if (page && skinningUpload(pageTexture, *page, "editor:skinning-editor:" + key)) {
        pageWidth = page->width();
        pageHeight = page->height();
    } else {
        setStatus("The frame's page does not load: " + error, true);
    }
}

void SkinningEditorWindow::commit(const SpriteSkinGeometry& edited, const std::vector<std::string>& removed, const std::string& what) {
    std::string error;
    bool changed = false;
    if (!sprite::applySkinEdit(target, edited, removed, true, error, nullptr, &changed)) {
        setStatus(what + ": " + error, true);
        Out::warning("Skinning Editor: %s: %s", what.c_str(), error.c_str());
        // the stored skin stays the truth
        frameKey.clear();
        return;
    }
    skin = edited;
    std::error_code ec;
    metaTime = fs::last_write_time(fs::path(target.absoluteSource.string() + ".meta"), ec);
    setStatus(changed ? what : what + ": nothing changed", false);
}

void SkinningEditorWindow::posedBone(int bone, Vector2& head, Vector2& tail) const {
    head = skin.bones[static_cast<size_t>(bone)].head;
    tail = skin.bones[static_cast<size_t>(bone)].tail;
    // the bone's pose: its parent's pose times its rest offset from the parent, turned by its angle
    std::vector<Matrix4> pose(skin.bones.size());
    for (size_t b = 0; b <= static_cast<size_t>(bone); b++) {
        const SpriteBone& sb = skin.bones[b];
        const Matrix4 bind = skinningBind(sb);
        const Matrix4 parent = sb.parent >= 0 ? pose[static_cast<size_t>(sb.parent)] * skinningBind(skin.bones[static_cast<size_t>(sb.parent)]).inverse()
                                              : Matrix4();
        const float angle = b < previewAngles.size() ? previewAngles[b] : 0.0f;
        pose[b] = parent * bind * Matrix4::composeTRS(Vector3(0.0f, 0.0f, 0.0f), Quaternion(0.0f, 0.0f, Angle::degToDefault(angle)), Vector3(1.0f, 1.0f, 1.0f));
    }
    const Matrix4 skinning = pose[static_cast<size_t>(bone)] * skinningBind(skin.bones[static_cast<size_t>(bone)]).inverse();
    const Vector3 h = skinning * Vector3(head.x, head.y, 0.0f);
    const Vector3 t = skinning * Vector3(tail.x, tail.y, 0.0f);
    head = Vector2(h.x, h.y);
    tail = Vector2(t.x, t.y);
}

std::vector<Vector2> SkinningEditorWindow::posedVertices() const {
    std::vector<Vector2> out = skin.vertices;
    if (skin.bones.empty() || skin.weights.size() != skin.vertices.size()) return out;
    bool posed = false;
    for (float a : previewAngles) posed = posed || a != 0.0f;
    if (!posed) return out;
    std::vector<Matrix4> pose(skin.bones.size()), skinning(skin.bones.size());
    for (size_t b = 0; b < skin.bones.size(); b++) {
        const SpriteBone& sb = skin.bones[b];
        const Matrix4 bind = skinningBind(sb);
        const Matrix4 parent = sb.parent >= 0 ? pose[static_cast<size_t>(sb.parent)] * skinningBind(skin.bones[static_cast<size_t>(sb.parent)]).inverse()
                                              : Matrix4();
        pose[b] = parent * bind * Matrix4::composeTRS(Vector3(0.0f, 0.0f, 0.0f), Quaternion(0.0f, 0.0f, Angle::degToDefault(b < previewAngles.size() ? previewAngles[b] : 0.0f)), Vector3(1.0f, 1.0f, 1.0f));
        skinning[b] = pose[b] * bind.inverse();
    }
    for (size_t v = 0; v < skin.vertices.size(); v++) {
        Vector3 sum(0.0f, 0.0f, 0.0f);
        const SpriteVertexWeights& w = skin.weights[v];
        for (int k = 0; k < SPRITE_SKIN_MAX_INFLUENCES; k++) {
            if (w.weights[k] <= 0.0f || w.bones[k] >= skin.bones.size()) continue;
            sum = sum + (skinning[w.bones[k]] * Vector3(skin.vertices[v].x, skin.vertices[v].y, 0.0f)) * w.weights[k];
        }
        out[v] = Vector2(sum.x, sum.y);
    }
    return out;
}

void SkinningEditorWindow::show() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
    SkinningOpenRequest& request = skinningOpenRequest();
    if (request.pending) {
        request.pending = false;
        sceneId = request.sceneId;
        entity = request.entity;
        frameKey.clear();
        locked = true;
        fitRequested = true;
        open();
    }
    if (!windowOpen) return;

    followSelection();
    refreshTarget();

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 56.0f, ImGui::GetFontSize() * 34.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    const bool visible = ImGui::Begin(WINDOW_NAME, &windowOpen);
    if (!visible) {
        ImGui::End();
        if (!windowOpen) setOpen(false);
        return;
    }
    if (!hasTarget) {
        ImGui::TextDisabled("%s", entity == NULL_ENTITY ? "Select a Sprite to edit its skin." : targetError.c_str());
        ImGui::End();
        return;
    }
    drawToolbar();
    ImGui::Separator();
    if (ImGui::BeginTable("##skinningLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("panel", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 15.0f);
        ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        drawSidePanel();
        ImGui::TableSetColumnIndex(1);
        drawCanvas();
        ImGui::EndTable();
    }
    ImGui::End();
    if (!windowOpen) setOpen(false);
}

void SkinningEditorWindow::drawToolbar() {
    auto toolButton = [this](Tool which, const char* label, const char* tip, const char* probe) {
        const bool active = tool == which;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) {
            tool = which;
            stroking = false;
            draggingBone = false;
        }
        if (active) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
        UIProbe::item(probe);
        ImGui::SameLine();
    };
    toolButton(Tool::CreateBone, ICON_FA_BONE " Create Bone", "Drag from the head to the tail; the selected bone is its parent", "skinning.create_bone");
    toolButton(Tool::Geometry, ICON_FA_DRAW_POLYGON " Geometry", "Auto Geometry: a triangle grid over the opaque pixels", "skinning.geometry");
    toolButton(Tool::WeightBrush, ICON_FA_PAINTBRUSH " Weight Brush", "Paint the selected bone's weights", "skinning.weight_brush");
    toolButton(Tool::PreviewPose, ICON_FA_PERSON_RUNNING " Preview Pose", "Drag a bone to turn it here (nothing is written)", "skinning.preview_pose");
    ImGui::Checkbox("Lock", &locked);
    ImGui::SetItemTooltip("Keep this Sprite when the selection changes");
    ImGui::SameLine();
    ImGui::TextDisabled("%s / %s", target.source.c_str(), target.frameData().name.c_str());
    if (!statusMessage.empty()) {
        ImGui::TextColored(statusIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "%s", statusMessage.c_str());
    }
}

void SkinningEditorWindow::drawSidePanel() {
    ImGui::SeparatorText("Bones");
    for (size_t b = 0; b < skin.bones.size(); b++) {
        const SpriteBone& bone = skin.bones[b];
        ImGui::PushID(static_cast<int>(b));
        ImGui::ColorButton("##color", ImGui::ColorConvertU32ToFloat4(skinningBoneColor(static_cast<int>(b))),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
        std::string label = bone.name;
        if (bone.parent >= 0) label += "  (" + skin.bones[static_cast<size_t>(bone.parent)].name + ")";
        if (ImGui::Selectable(label.c_str(), selectedBone == static_cast<int>(b))) selectedBone = selectedBone == static_cast<int>(b) ? -1 : static_cast<int>(b);
        ImGui::PopID();
    }
    if (skin.bones.empty()) ImGui::TextDisabled("No bones: Create Bone, then drag on the frame.");
    ImGui::BeginDisabled(selectedBone < 0);
    if (ImGui::Button(ICON_FA_TRASH " Delete Bone")) {
        SpriteSkinGeometry edited = skin;
        std::string error;
        const std::string name = skin.bones[static_cast<size_t>(selectedBone)].name;
        if (sprite::removeBone(edited, name, error)) {
            selectedBone = -1;
            commit(edited, {name}, "Delete Bone " + name);
        } else {
            setStatus(error, true);
        }
    }
    UIProbe::item("skinning.delete_bone");
    if (ImGui::Button(ICON_FA_LINK " Bind to Bone (100%)")) {
        SpriteSkinGeometry edited = skin;
        std::string error;
        if (sprite::setWeights(edited, {}, selectedBone, 1.0f, error)) commit(edited, {}, "Bind to " + skin.bones[static_cast<size_t>(selectedBone)].name);
        else setStatus(error, true);
    }
    ImGui::SetItemTooltip("Every vertex follows the selected bone only (a rigid part)");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Geometry");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##cell", &geometry.cellSize, 1, 64, "Cell Size %d px");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##alpha", &geometry.alphaTolerance, 0, 254, "Alpha Tolerance %d");
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Auto Geometry")) {
        SpriteSkinGeometry edited = skin;
        std::string error;
        std::optional<editor::pixelart::PixelImage> page = sprite::loadFramePage(project, *target.sheet, target.frameData(), &error);
        if (page && sprite::autoGeometry(*page, target.frameData(), geometry, edited, error)) {
            // a weighted skin keeps its bones' say: weights again from the bones
            if (!edited.bones.empty()) sprite::autoWeights(edited, error);
            commit(edited, {}, "Auto Geometry");
        } else {
            setStatus("Auto Geometry: " + error, true);
        }
    }
    UIProbe::item("skinning.auto_geometry");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        SpriteSkinGeometry edited = skin;
        edited.vertices.clear();
        edited.indices.clear();
        edited.weights.clear();
        commit(edited, {}, "Clear Geometry");
    }
    ImGui::TextDisabled("%zu vertices, %zu triangles", skin.vertices.size(), skin.indices.size() / 3);

    ImGui::SeparatorText("Weights");
    ImGui::BeginDisabled(skin.bones.empty() || skin.vertices.empty());
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Auto Weights")) {
        SpriteSkinGeometry edited = skin;
        std::string error;
        if (sprite::autoWeights(edited, error)) commit(edited, {}, "Auto Weights");
        else setStatus(error, true);
    }
    UIProbe::item("skinning.auto_weights");
    ImGui::EndDisabled();
    int mode = static_cast<int>(brushMode);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##mode", &mode, "Add\0Subtract\0Smooth\0")) brushMode = static_cast<sprite::WeightBrushMode>(mode);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##size", &brushSize, 1.0f, 128.0f, "Size %.0f px");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##strength", &brushStrength, 0.01f, 1.0f, "Strength %.2f");
    ImGui::Checkbox("Show Weights", &showWeights);
    ImGui::Checkbox("Show Mesh", &showMesh);

    ImGui::SeparatorText("Pose");
    if (ImGui::Button("Reset Pose")) std::fill(previewAngles.begin(), previewAngles.end(), 0.0f);
    ImGui::SameLine();
    if (ImGui::Button("Bind GameObjects")) commit(skin, {}, "Bind Sprite Skin");
    ImGui::SetItemTooltip("Bone GameObjects under the Sprite at the rest pose and its Sprite Skin bound to them");
}

void SkinningEditorWindow::drawCanvas() {
    const SpriteFrame& frame = target.frameData();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 64.0f), std::max(avail.y, 64.0f));
    const float w = std::max(1.0f, frame.sourceSize.x), h = std::max(1.0f, frame.sourceSize.y);
    if (fitRequested) {
        zoom = std::max(1.0f, std::floor(std::min(size.x / w, size.y / h) * 0.9f));
        fitRequested = false;
    }
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##skinningCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    UIProbe::item("skinning.canvas");
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(corner, ImVec2(corner.x + size.x, corner.y + size.y), true);
    draw->AddRectFilled(corner, ImVec2(corner.x + size.x, corner.y + size.y), IM_COL32(40, 40, 46, 255));
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) zoom = std::clamp(zoom * (ImGui::GetIO().MouseWheel > 0 ? 1.25f : 0.8f), 0.25f, 64.0f);
    const ImVec2 origin(std::floor(corner.x + (size.x - w * zoom) / 2.0f), std::floor(corner.y + (size.y - h * zoom) / 2.0f));
    auto toScreen = [&](Vector2 p) { return ImVec2(origin.x + p.x * zoom, origin.y + p.y * zoom); };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Vector2 mousePixel((mouse.x - origin.x) / zoom, (mouse.y - origin.y) / zoom);
    draw->AddRect(toScreen(Vector2(0, 0)), toScreen(Vector2(w, h)), IM_COL32(120, 120, 130, 255));

    // the frame's pixels: its quad, or its mesh as posed
    const ImTextureID texture = pageTexture.empty() ? ImTextureID() : Backend::getImGuiTexture(pageTexture.getRender());
    const float pw = static_cast<float>(std::max(1, pageWidth)), ph = static_cast<float>(std::max(1, pageHeight));
    auto uvOf = [&](Vector2 p) {
        return ImVec2((frame.rect.getX() + std::clamp(p.x - frame.offset.x, 0.0f, frame.rect.getWidth())) / pw,
                      (frame.rect.getY() + std::clamp(p.y - frame.offset.y, 0.0f, frame.rect.getHeight())) / ph);
    };
    const std::vector<Vector2> posed = posedVertices();
    const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    if (texture) {
        if (platform.DrawCallback_SetSamplerNearest) draw->AddCallback(platform.DrawCallback_SetSamplerNearest, nullptr);
        if (skin.drawable()) {
            draw->PushTexture(ImTextureRef(texture));
            draw->PrimReserve(static_cast<int>(skin.indices.size()), static_cast<int>(skin.indices.size()));
            for (uint16_t index : skin.indices) {
                draw->PrimVtx(toScreen(posed[index]), uvOf(skin.vertices[index]), IM_COL32_WHITE);
            }
            draw->PopTexture();
        } else {
            const Vector2 a(frame.offset.x, frame.offset.y), b(frame.offset.x + frame.rect.getWidth(), frame.offset.y + frame.rect.getHeight());
            draw->AddImage(ImTextureRef(texture), toScreen(a), toScreen(b), uvOf(a), uvOf(b));
        }
        if (platform.DrawCallback_SetSamplerLinear) draw->AddCallback(platform.DrawCallback_SetSamplerLinear, nullptr);
    }
    if (showMesh) {
        for (size_t t = 0; t + 2 < skin.indices.size(); t += 3) {
            const ImVec2 a = toScreen(posed[skin.indices[t]]), b = toScreen(posed[skin.indices[t + 1]]), c = toScreen(posed[skin.indices[t + 2]]);
            draw->AddTriangle(a, b, c, IM_COL32(255, 255, 255, 60));
        }
    }
    if (showWeights && skin.weights.size() == skin.vertices.size()) {
        for (size_t v = 0; v < skin.vertices.size(); v++) {
            float r = 0, g = 0, b = 0;
            const SpriteVertexWeights& weights = skin.weights[v];
            for (int k = 0; k < SPRITE_SKIN_MAX_INFLUENCES; k++) {
                if (weights.weights[k] <= 0.0f) continue;
                const ImVec4 c = ImGui::ColorConvertU32ToFloat4(skinningBoneColor(weights.bones[k]));
                r += c.x * weights.weights[k];
                g += c.y * weights.weights[k];
                b += c.z * weights.weights[k];
            }
            draw->AddCircleFilled(toScreen(posed[v]), 3.0f, ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f)));
        }
    }
    for (size_t bi = 0; bi < skin.bones.size(); bi++) {
        Vector2 head, tail;
        posedBone(static_cast<int>(bi), head, tail);
        const ImU32 color = skinningBoneColor(static_cast<int>(bi), static_cast<int>(bi) == selectedBone ? 255 : 200);
        draw->AddLine(toScreen(head), toScreen(tail), color, static_cast<int>(bi) == selectedBone ? 4.0f : 2.5f);
        draw->AddCircleFilled(toScreen(head), 5.0f, color);
        draw->AddCircle(toScreen(tail), 3.5f, color);
    }

    // a bone under the mouse: its segment within a few screen pixels
    auto boneAt = [&](Vector2 p) {
        int best = -1;
        float bestDistance = 6.0f / zoom;
        for (size_t bi = 0; bi < skin.bones.size(); bi++) {
            Vector2 head, tail;
            posedBone(static_cast<int>(bi), head, tail);
            const Vector2 ab = tail - head;
            const float length2 = ab.x * ab.x + ab.y * ab.y;
            const float t = length2 > 0.0f ? std::clamp(((p.x - head.x) * ab.x + (p.y - head.y) * ab.y) / length2, 0.0f, 1.0f) : 0.0f;
            const float dx = p.x - (head.x + ab.x * t), dy = p.y - (head.y + ab.y * t);
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < bestDistance) {
                bestDistance = d;
                best = static_cast<int>(bi);
            }
        }
        return best;
    };

    const bool pressed = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (tool == Tool::CreateBone) {
        if (pressed) {
            draggingBone = true;
            dragStart = Vector2(std::round(mousePixel.x), std::round(mousePixel.y));
        }
        if (draggingBone) {
            const Vector2 end(std::round(mousePixel.x), std::round(mousePixel.y));
            draw->AddLine(toScreen(dragStart), toScreen(end), IM_COL32(255, 255, 255, 220), 2.0f);
            if (!down) {
                draggingBone = false;
                if (end.x != dragStart.x || end.y != dragStart.y) {
                    SpriteSkinGeometry edited = skin;
                    SpriteBone bone;
                    for (int n = static_cast<int>(skin.bones.size()) + 1;; n++) {
                        bone.name = "bone_" + std::to_string(n);
                        if (sprite::findBone(skin, bone.name) < 0) break;
                    }
                    bone.parent = selectedBone;
                    bone.head = dragStart;
                    bone.tail = end;
                    std::string error;
                    if (sprite::addBone(edited, bone, error)) {
                        // weights for the new bone where there is geometry
                        if (!edited.vertices.empty()) sprite::autoWeights(edited, error);
                        commit(edited, {}, "Create Bone " + bone.name);
                        selectedBone = static_cast<int>(skin.bones.size()) - 1;
                    } else {
                        setStatus(error, true);
                    }
                }
            }
        } else if (pressed) {
            selectedBone = boneAt(mousePixel);
        }
    } else if (tool == Tool::WeightBrush) {
        if (hovered) draw->AddCircle(mouse, brushSize * zoom, IM_COL32(255, 255, 255, 160), 48);
        if (pressed && (selectedBone >= 0 || brushMode == sprite::WeightBrushMode::Smooth) && !skin.vertices.empty()) stroking = true;
        if (stroking) {
            if (down) {
                sprite::brushWeights(skin, mousePixel, brushSize, selectedBone, brushStrength * ImGui::GetIO().DeltaTime * 10.0f, brushMode);
            } else {
                stroking = false;
                commit(skin, {}, "Weight Brush");
            }
        } else if (pressed && selectedBone < 0) {
            setStatus("Select the bone to paint in the Bones list", true);
        }
    } else if (tool == Tool::PreviewPose) {
        if (pressed) turningBone = boneAt(mousePixel);
        if (turningBone >= 0) {
            if (!down) {
                turningBone = -1;
            } else {
                Vector2 head, tail;
                posedBone(turningBone, head, tail);
                const float previous = std::atan2(tail.y - head.y, tail.x - head.x);
                const float now = std::atan2(mousePixel.y - head.y, mousePixel.x - head.x);
                previewAngles[static_cast<size_t>(turningBone)] += Angle::radToDeg(now - previous);
            }
        }
    } else if (tool == Tool::Geometry && pressed) {
        selectedBone = boneAt(mousePixel);
    }
    draw->PopClipRect();
}
