// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexPaintWindow.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "component/VertexColorsComponent.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteUse.h"
#include "shortcuts/CommandCatalog.h"
#include "util/UIProbe.h"
#include "util/UIUtils.h"
#include "vertexcolor/VertexPaintImages.h"
#include "widget/ColorField.h"
#include "widget/CurveField.h"
#include "widget/SwatchLibrary.h"
#include "window/PaletteAssetField.h"
#include "window/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::vertexcolor {

namespace {

    // the Terrain window's two-column property table (beginTerrainProperties / terrainPropertyRow)
    bool vpBeginProperties(const char* id) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) return false;
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, std::min(ImGui::GetFontSize() * 8.0f, ImGui::GetContentRegionAvail().x * 0.4f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }

    void vpRow(const char* label, const char* tooltip = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
    }

    bool vpHeader(const char* label, bool open = true) {
        return ImGui::CollapsingHeader(label, open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    }

    bool vpPercent(const char* id, float* value) {
        float percent = *value * 100.0f;
        if (!UIUtils::sliderFloatInput(id, &percent, 0.0f, 100.0f, "%.0f%%")) return false;
        *value = std::clamp(percent / 100.0f, 0.0f, 1.0f);
        return true;
    }

    bool vpToggle(const char* label, bool on, const char* tooltip) {
        ImGui::PushStyleColor(ImGuiCol_Button, on ? Theme::Colors::ButtonActivated : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        const bool clicked = ImGui::Button(label);
        ImGui::PopStyleColor();
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
        return clicked;
    }

    Vector4 fromRgba8(const jasperengine::pixelart::Rgba8& c) {
        return Vector4(vc::srgbToLinear(c.r / 255.0f), vc::srgbToLinear(c.g / 255.0f), vc::srgbToLinear(c.b / 255.0f), c.a / 255.0f);
    }

    jasperengine::pixelart::Rgba8 toRgba8(const Vector4& linear) {
        auto byte = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        return jasperengine::pixelart::Rgba8{byte(vc::linearToSrgb(linear.x)), byte(vc::linearToSrgb(linear.y)), byte(vc::linearToSrgb(linear.z)), byte(linear.w)};
    }

    const char* const kBlendNames[] = {"Replace", "Add", "Subtract", "Multiply", "Screen", "Overlay", "Min", "Max", "Lerp"};
    const char* const kFalloffNames[] = {"Smooth", "Linear", "Constant", "Sphere", "Custom"};
    const char* const kChannelLetters[] = {"R", "G", "B", "A"};

}

VertexPaintWindow& VertexPaintWindow::get() {
    static VertexPaintWindow window;
    return window;
}

void VertexPaintWindow::installPaintToolHook() {
    paintToolHook() = [](Project* project, uint32_t sceneId, Entity entity) {
        if (!project) return;
        project->replaceSelectedEntities(sceneId, {entity});
        VertexPaintWindow& window = VertexPaintWindow::get();
        window.setProject(project);
        window.setOpen(true);
        VertexPaintTools::get().armIn(sceneId, PaintTool::Paint);
    };
}

void VertexPaintWindow::setProject(Project* project) {
    if (projectPtr == project) return;
    cancelJob();
    projectPtr = project;
    dialog = Dialog::None;
    meshColors = MeshColors();
    VertexPaintTools::get().setProject(project);
}

void VertexPaintWindow::setOpen(bool open) {
    windowOpen = open;
    if (open) focusRequested = true;
}

const char* VertexPaintWindow::dialogTitle(Dialog d) {
    switch (d) {
        case Dialog::None: return "";
        case Dialog::FromTexture: return "From Texture";
        case Dialog::AmbientOcclusion: return "Ambient Occlusion";
        case Dialog::Curvature: return "Curvature";
        case Dialog::Thickness: return "Thickness";
        case Dialog::Gradient: return "Gradient";
        case Dialog::Noise: return "Noise";
        case Dialog::Direction: return "Direction";
        case Dialog::TransferFrom: return "Transfer From";
        case Dialog::Adjust: return "Adjust";
        case Dialog::Channels: return "Channels";
        case Dialog::ExportModel: return "Export as Model";
        case Dialog::BakeTexture: return "Bake to Texture";
    }
    return "";
}

void VertexPaintWindow::openDialog(Dialog d) {
    dialog = d;
    dialogNote.clear();
    if (!projectPtr) return;
    const uint32_t sceneId = projectPtr->getSelectedSceneId();
    const std::vector<TargetRef> targets = VertexPaintTools::get().actingTargets(sceneId);
    SceneProject* sceneProject = projectPtr->getScene(sceneId);
    const std::string name = !targets.empty() && sceneProject && sceneProject->scene ? sceneProject->scene->getEntityName(targets.front().entity) : std::string("Painted");
    if (d == Dialog::ExportModel && fields.exportPath.empty()) fields.exportPath = "models/" + name + "_painted.glb";
    if (d == Dialog::BakeTexture && fields.bakePath.empty()) fields.bakePath = "textures/" + name + "_vertex_colors.png";
}

void VertexPaintWindow::cancelJob() {
    if (job.running) job.cancel = true;
}

// ==================================================================== the dialogs' ops

bool VertexPaintWindow::dialogOps(Dialog d, std::vector<std::vector<vc::Op>>& perTarget, std::vector<TargetRef>& targets, std::string& undoName,
                                  std::string& error) {
    perTarget.clear();
    targets.clear();
    if (!projectPtr) return false;
    VertexPaintTools& tools = VertexPaintTools::get();
    const VertexPaintSettings& settings = tools.settings();
    const uint32_t sceneId = projectPtr->getSelectedSceneId();
    targets = tools.actingTargets(sceneId);
    if (targets.empty()) {
        error = "Select a Model or a Mesh to paint.";
        return false;
    }
    vc::Op op;
    op.channels = settings.channels;
    op.blend = settings.blend;
    op.strength = 1.0f;
    undoName = std::string(dialogTitle(d));
    switch (d) {
        case Dialog::FromTexture: {
            op.type = vc::OpType::FromTexture;
            op.uvSet = std::clamp(fields.uvSet, 0, 1);
            op.samples = std::max(16, fields.samples);
            op.keepSeams = fields.keepSeams;
            undoName = "Colors From Texture";
            if (fields.fromMaterial) {
                // each target samples its own material's map (R41 material_slot)
                for (const TargetRef& target : targets) {
                    vc::Op mine = op;
                    mine.image = materialImage(projectPtr, sceneId, target, (MaterialSlot)fields.materialSlot, &error);
                    if (!mine.image) return false;
                    perTarget.push_back({mine});
                }
                return true;
            }
            op.image = loadPaintImage(projectPtr, fields.texturePath, fields.colorData, &error);
            if (!op.image) return false;
            break;
        }
        case Dialog::AmbientOcclusion:
        case Dialog::Thickness:
            op.type = d == Dialog::AmbientOcclusion ? vc::OpType::BakeAO : vc::OpType::BakeThickness;
            op.rays = std::clamp(fields.rays, 1, 4096);
            op.distance = std::max(1e-4f, fields.distance);
            op.sceneScope = fields.sceneScope;
            op.seed = (uint32_t)std::max(0, fields.seed);
            undoName = d == Dialog::AmbientOcclusion ? "Bake AO" : "Bake Thickness";
            break;
        case Dialog::Curvature:
            op.type = vc::OpType::BakeCurvature;
            op.iterations = std::clamp(fields.curvatureBlur, 0, 64);
            undoName = "Bake Curvature";
            break;
        case Dialog::Gradient: {
            op = gradientOp(settings, vc::Target(), fields.gradientFrom, fields.gradientTo);
            op.filters.clear();
            op.gradientType = (vc::GradientType)std::clamp(fields.gradientType, 0, 3);
            op.axis = fields.axis;
            op.interpolation = (vc::Interpolation)std::clamp(fields.interpolation, 0, 2);
            op.extend = (vc::Extend)std::clamp(fields.extend, 0, 3);
            op.target.element = fields.gradientElement == 1 ? vc::Element::Face : vc::Element::Vertex;
            undoName = "Gradient Vertex Colors";
            break;
        }
        case Dialog::Noise:
            op.type = vc::OpType::Noise;
            op.noiseKind = (vc::NoiseKind)std::clamp(fields.noiseKind, 0, 2);
            op.noiseScale = std::max(1e-4f, fields.noiseScale);
            op.octaves = std::clamp(fields.octaves, 1, 8);
            op.seed = (uint32_t)std::max(0, fields.seed);
            op.noiseWorld = fields.noiseWorld;
            op.value = settings.secondary;
            op.value2 = settings.primary;
            undoName = "Noise Vertex Colors";
            break;
        case Dialog::Direction:
            op.type = vc::OpType::ByDirection;
            op.direction = fields.direction.length() > 0.0f ? fields.direction.normalized() : Vector3(0.0f, 1.0f, 0.0f);
            op.rangeMin = fields.rangeMin;
            op.rangeMax = fields.rangeMax;
            op.value = settings.primary;
            undoName = "Colors by Direction";
            break;
        case Dialog::TransferFrom:
            op.type = vc::OpType::Transfer;
            if (fields.transferSource == NULL_ENTITY) {
                error = "Choose the GameObject to transfer from.";
                return false;
            }
            // its corners too, so its hard edges carry over (R60)
            op.source = transferSource(projectPtr, sceneId, fields.transferSource, nullptr);
            if (!op.source) {
                error = "The source has no paintable mesh loaded.";
                return false;
            }
            op.maxDistance = std::max(1e-5f, fields.maxDistance);
            op.normalAngle = std::clamp(fields.normalAngle, 0.0f, 180.0f);
            op.unmatched = fields.unmatchedNearest ? vc::Unmatched::Nearest : vc::Unmatched::Keep;
            undoName = "Transfer Vertex Colors";
            break;
        case Dialog::Adjust:
            op.type = vc::OpType::Adjust;
            op.adjustKind = (vc::AdjustKind)std::clamp(fields.adjustKind, 0, 4);
            op.inBlack = fields.inBlack;
            op.inWhite = fields.inWhite;
            op.gamma = std::max(0.01f, fields.gamma);
            op.outBlack = fields.outBlack;
            op.outWhite = fields.outWhite;
            op.brightness = fields.brightness;
            op.contrast = fields.contrast;
            op.hueShift = fields.hueShift;
            op.saturationShift = fields.saturationShift;
            op.valueShift = fields.valueShift;
            undoName = "Adjust Vertex Colors";
            break;
        case Dialog::Channels:
            op.type = vc::OpType::Channel;
            op.channels = vc::CHANNELS_RGBA;
            op.channelKind = (vc::ChannelOpKind)std::clamp(fields.channelKind, 0, 3);
            op.sourceChannel = std::clamp(fields.sourceChannel, 0, 3);
            op.destChannel = std::clamp(fields.destChannel, 0, 3);
            for (int c = 0; c < 4; c++) {
                op.packSource[(size_t)c] = (int8_t)std::clamp(fields.packSource[c], -1, 3);
                op.packConstant[(size_t)c] = fields.packConstant[c];
            }
            undoName = "Edit Vertex Color Channels";
            break;
        case Dialog::None:
        case Dialog::ExportModel:
        case Dialog::BakeTexture:
            error = "This dialog runs no op.";
            return false;
    }
    // the selection narrows every generator (R110: on all selected targets)
    for (const TargetRef& target : targets) {
        vc::Op mine = op;
        vc::Target selection;
        if (selectionTarget(sceneId, target.entity, target.key, selection)) {
            selection.element = mine.target.element;
            mine.target = selection;
        }
        perTarget.push_back({mine});
    }
    return true;
}

bool VertexPaintWindow::runDialog() {
    if (!projectPtr || job.running) return false;
    dialogNote.clear();
    const uint32_t sceneId = projectPtr->getSelectedSceneId();
    VertexPaintTools& tools = VertexPaintTools::get();
    std::string error;
    if (dialog == Dialog::ExportModel || dialog == Dialog::BakeTexture) {
        const std::vector<TargetRef> targets = tools.actingTargets(sceneId);
        if (targets.empty()) {
            dialogNote = "Select a Model or a Mesh with vertex colors.";
            return false;
        }
        bool ok = false;
        if (dialog == Dialog::ExportModel) {
            ExportModelOptions options;
            options.path = fields.exportPath;
            options.replaceInScene = fields.replaceInScene;
            options.splitHardEdges = fields.splitHardEdges;
            ok = exportAsModel(projectPtr, sceneId, targets.front().entity, options, &error);
            dialogNote = ok ? "Wrote " + fields.exportPath : error;
        } else {
            const int index = std::clamp(fields.bakeTarget, 0, (int)targets.size() - 1);
            BakeTextureOptions options;
            options.key = targets[(size_t)index].key;
            options.path = fields.bakePath;
            options.size = std::clamp(fields.bakeSize, 16, 8192);
            options.padding = std::clamp(fields.bakePadding, 0, 64);
            options.uvSet = std::clamp(fields.bakeUvSet, 0, 1);
            options.srgb = fields.bakeSrgb;
            ok = bakeToTexture(projectPtr, sceneId, targets[(size_t)index].entity, options, &error);
            dialogNote = ok ? "Wrote " + fields.bakePath : error;
        }
        return ok;
    }
    std::vector<std::vector<vc::Op>> perTarget;
    std::vector<TargetRef> targets;
    std::string undoName;
    if (!dialogOps(dialog, perTarget, targets, undoName, error)) {
        dialogNote = error;
        return false;
    }
    EditRequest request;
    request.targets = targets;
    request.perTargetOps = std::move(perTarget);
    request.undoName = undoName;
    job.cancel = false;
    job.progress = 0.0f;
    request.cancel = &job.cancel;
    request.progress = [this](float progress) { job.progress = progress; };
    // R97: a bake (and a big op) goes to a job; its result comes back here on the main thread
    request.done = [this, sceneId](const EditResult& result) {
        job.running = false;
        if (!result.ok) dialogNote = result.error.empty() ? "Cancelled: nothing was changed." : result.error;
        else if (!result.warnings.empty()) dialogNote = result.warnings.front().message;
        else dialogNote = "Done.";
        if (SceneProject* scene = projectPtr ? projectPtr->getScene(sceneId) : nullptr) scene->needUpdateRender = true;
    };
    job.running = true;
    const EditResult result = applyEdit(projectPtr, sceneId, request);
    if (result.async) return true;
    // ran now: done() may or may not have been called; the result says it all
    job.running = false;
    if (!result.ok) {
        dialogNote = result.error;
        return false;
    }
    dialogNote = result.warnings.empty() ? "Done." : result.warnings.front().message;
    if (SceneProject* scene = projectPtr->getScene(sceneId)) scene->needUpdateRender = true;
    return true;
}

// ==================================================================== commands (Table 3)

void VertexPaintWindow::bindCommands(CommandCatalog& catalog) {
    auto bind = [&catalog](const char* id, std::function<void()> action, std::function<bool()> enabled) {
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        def.run = [action](const CommandArgs&) { action(); };
        def.enabled = std::move(enabled);
        // not while a field of the window is being edited
        def.keyGate = []() { return !ImGui::IsAnyItemActive(); };
        catalog.add(std::move(def));
    };
    auto armed = []() { return VertexPaintTools::get().isArmed(); };
    auto scene = []() { return VertexPaintTools::get().armedScene(); };
    // a tool's command arms it (the Search palette, run_command); its key only works once one is armed (the context)
    for (int i = 0; i < PAINT_TOOL_COUNT; i++) {
        const PaintTool tool = (PaintTool)i;
        bind(paintToolCommand(tool), [tool]() { VertexPaintTools::get().arm(tool); }, nullptr);
    }
    for (int c = 0; c < 4; c++) {
        static const char* const ids[] = {"vertexPaint.channelR", "vertexPaint.channelG", "vertexPaint.channelB", "vertexPaint.channelA"};
        bind(ids[c], [c]() { toggleChannel(VertexPaintTools::get().settings(), c); }, armed);
    }
    bind("vertexPaint.swapColors", []() { swapColors(VertexPaintTools::get().settings()); }, armed);
    bind("vertexPaint.mode.vertex", [scene]() { VertexPaintTools::get().setSelectionMode(scene(), vc::ElementKind::Vertex); }, armed);
    bind("vertexPaint.mode.face", [scene]() { VertexPaintTools::get().setSelectionMode(scene(), vc::ElementKind::Face); }, armed);
    bind("vertexPaint.mode.corner", [scene]() { VertexPaintTools::get().setSelectionMode(scene(), vc::ElementKind::Corner); }, armed);
    bind("vertexPaint.apply", [scene]() { VertexPaintTools::get().applyToSelection(scene(), false); },
         [armed, scene]() { return armed() && VertexPaintTools::get().selectedCount(scene()) > 0; });
    using Cmd = VertexPaintTools::SelectCommand;
    auto select = [&bind, armed, scene](const char* id, Cmd command) {
        bind(id, [scene, command]() { VertexPaintTools::get().selectCommand(scene(), command); }, armed);
    };
    select("vertexPaint.select.all", Cmd::All);
    select("vertexPaint.select.none", Cmd::None);
    select("vertexPaint.select.invert", Cmd::Invert);
    select("vertexPaint.select.grow", Cmd::Grow);
    select("vertexPaint.select.shrink", Cmd::Shrink);
    select("vertexPaint.select.linked", Cmd::Linked);
    bind("vertexPaint.view.rgb", []() { VertexPaintTools::get().setViewChannel(ViewVertexColorChannel::RGB); }, armed);
    bind("vertexPaint.view.cycle", []() { VertexPaintTools::get().cycleViewChannel(); }, armed);
    bind("vertexPaint.xray", []() { VertexPaintSettings& s = VertexPaintTools::get().settings(); s.xray = !s.xray; }, armed);
}

// ==================================================================== the window

void VertexPaintWindow::show() {
    VertexPaintTools& tools = VertexPaintTools::get();
    if (projectPtr) tools.setProject(projectPtr);
    tools.tick(false);
    if (!windowOpen) {
        if (dialog != Dialog::None && !job.running) dialog = Dialog::None;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, ImGui::GetFontSize() * 46.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    SceneProject* sceneProject = projectPtr ? projectPtr->getSelectedScene() : nullptr;
    if (!sceneProject || !sceneProject->scene) {
        ImGui::TextWrapped("Open a scene to paint vertex colors.");
        ImGui::End();
        return;
    }
    const uint32_t sceneId = sceneProject->id;
    if (sceneProject->playState != ScenePlayState::STOPPED) {
        ImGui::TextWrapped("Stop the scene to paint vertex colors.");
        ImGui::End();
        return;
    }
    drawToolStrip(sceneId);
    drawTargetRow(sceneId);
    drawMaterialWarnings(sceneId);
    if (!tools.lastError().empty()) ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", tools.lastError().c_str());
    else if (!tools.lastNotice().empty()) ImGui::TextDisabled("%s", tools.lastNotice().c_str());
    drawColor(sceneId);
    drawBrush();
    drawMasks(sceneId);
    drawSelection(sceneId);
    drawColorsOnMesh(sceneId);
    drawGenerate(sceneId);
    drawView();
    ImGui::End();
    drawDialog(sceneId);
}

void VertexPaintWindow::drawToolStrip(uint32_t sceneId) {
    VertexPaintTools& tools = VertexPaintTools::get();
    static const char* const icons[PAINT_TOOL_COUNT] = {ICON_FA_ARROW_POINTER, ICON_FA_PAINTBRUSH, ICON_FA_DROPLET, ICON_FA_FILL_DRIP, ICON_FA_CIRCLE_HALF_STROKE,
                                                         ICON_FA_EYE_DROPPER};
    const float size = std::round(ImGui::GetFrameHeight() * 1.6f);
    const CommandCatalog& catalog = CommandCatalog::editor();
    for (int i = 0; i < PAINT_TOOL_COUNT; i++) {
        const PaintTool tool = (PaintTool)i;
        if (i > 0) ImGui::SameLine();
        const bool selected = tools.isArmedIn(sceneId) && tools.currentTool() == tool;
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? Theme::Colors::ButtonActivated : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        const bool clicked = Widgets::iconButton((std::string("##vp_tool_") + paintToolName(tool)).c_str(), icons[i], ImVec2(size, size));
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", catalog.tooltip(paintToolCommand(tool), paintToolName(tool)).c_str());
        UIProbe::item(paintToolCommand(tool));
        if (clicked) {
            // the armed tool again puts the brush down
            if (selected) tools.exit();
            else tools.armIn(sceneId, tool);
        }
    }
    ImGui::TextUnformatted(tools.isArmedIn(sceneId) ? paintToolName(tools.currentTool()) : "No tool armed");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", paintLegend(catalog, tools.currentTool()).c_str());
    ImGui::PopStyleColor();
}

void VertexPaintWindow::drawTargetRow(uint32_t sceneId) {
    VertexPaintTools& tools = VertexPaintTools::get();
    SceneProject* sceneProject = projectPtr->getScene(sceneId);
    const std::vector<TargetRef> all = tools.targets(sceneId);
    if (all.empty()) {
        ImGui::TextWrapped("Select a Model or a Mesh in the Hierarchy to paint its vertex colors.");
        for (Entity entity : projectPtr->getSelectedEntities(sceneId)) {
            std::string why;
            if (!canPaint(projectPtr, sceneId, entity, &why) && !why.empty()) ImGui::TextColored(Theme::Colors::WarningText, "%s", why.c_str());
        }
        return;
    }
    if (tools.targetFilter >= (int)all.size()) tools.targetFilter = -1;
    auto nameOf = [&](const TargetRef& ref) {
        std::string name = sceneProject->scene->getEntityName(ref.entity);
        const std::vector<TargetInfo> infos = listTargets(projectPtr, sceneId, ref.entity);
        for (const TargetInfo& info : infos) {
            if (info.key == ref.key && infos.size() > 1) name += " / " + info.name;
        }
        return name;
    };
    char preview[256];
    if (tools.targetFilter < 0) std::snprintf(preview, sizeof(preview), "%s (%zu target%s)", all.size() == 1 ? nameOf(all[0]).c_str() : "All", all.size(), all.size() == 1 ? "" : "s");
    else std::snprintf(preview, sizeof(preview), "%s", nameOf(all[(size_t)tools.targetFilter]).c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Target");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##vp_target", preview)) {
        if (ImGui::Selectable("All selected targets", tools.targetFilter < 0)) tools.targetFilter = -1;
        for (size_t i = 0; i < all.size(); i++) {
            if (ImGui::Selectable((nameOf(all[i]) + "##vp_target" + std::to_string(i)).c_str(), tools.targetFilter == (int)i)) tools.targetFilter = (int)i;
        }
        ImGui::EndCombo();
    }
    UIProbe::item("vertexPaint.target");
}

void VertexPaintWindow::drawMaterialWarnings(uint32_t sceneId) {
    VertexPaintTools& tools = VertexPaintTools::get();
    std::vector<Entity> entities;
    for (const TargetRef& ref : tools.actingTargets(sceneId)) {
        if (std::find(entities.begin(), entities.end(), ref.entity) == entities.end()) entities.push_back(ref.entity);
    }
    int index = 0;
    for (Entity entity : entities) {
        for (const vc::Warning& warning : materialWarnings(projectPtr, sceneId, entity, tools.settings().channels)) {
            ImGui::PushID(index++);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Fix").x - ImGui::GetStyle().FramePadding.x * 4.0f);
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", warning.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix")) {
                std::string error;
                if (!fixMaterialWarning(projectPtr, sceneId, entity, warning, &error)) Out::warning("Vertex Paint: %s", error.c_str());
            }
            UIProbe::item("vertexPaint.materialFix");
            ImGui::PopID();
        }
    }
}

void VertexPaintWindow::drawColor(uint32_t sceneId) {
    if (!vpHeader("Color")) return;
    VertexPaintTools& tools = VertexPaintTools::get();
    VertexPaintSettings& s = tools.settings();
    if (!vpBeginProperties("vp_color")) return;
    auto colorRow = [](const char* label, const char* id, Vector4& value) {
        vpRow(label);
        ui::ColorFieldOptions options;
        options.label = label;
        options.width = ImGui::GetFrameHeight() * 2.5f;
        ui::colorField(id, value, options);
        ImGui::SameLine();
        // R53: hex (sRGB) and numeric RGBA
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%s", vc::toHex(value, value.w < 1.0f).c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::InputText((std::string(id) + "_hex").c_str(), hex, sizeof(hex), ImGuiInputTextFlags_EnterReturnsTrue)) {
            Vector4 parsed;
            if (vc::parseHex(hex, parsed)) value = parsed;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        float rgba[4] = {value.x, value.y, value.z, value.w};
        if (ImGui::DragFloat4((std::string(id) + "_rgba").c_str(), rgba, 0.005f, 0.0f, 1.0f, "%.3f")) value = Vector4(rgba[0], rgba[1], rgba[2], rgba[3]);
    };
    colorRow("Primary", "##vp_primary", s.primary);
    colorRow("Secondary", "##vp_secondary", s.secondary);
    vpRow("", nullptr);
    if (ImGui::Button(ICON_FA_RIGHT_LEFT " Swap")) swapColors(s);
    ImGui::SetItemTooltip("%s", CommandCatalog::editor().tooltip("vertexPaint.swapColors", "Swap primary and secondary").c_str());

    vpRow("Channels", "The channels every tool writes; the others are never touched");
    const std::array<std::string, 4> uses = [&]() {
        const std::vector<TargetRef> targets = tools.actingTargets(sceneId);
        return targets.empty() ? std::array<std::string, 4>() : channelUses(projectPtr, sceneId, targets.front().entity);
    }();
    for (int c = 0; c < 4; c++) {
        if (c > 0) ImGui::SameLine();
        const bool on = (s.channels & (1u << c)) != 0;
        const std::string label = std::string(kChannelLetters[c]) + "##vp_channel" + std::to_string(c);
        const std::string tip = uses[(size_t)c].empty() ? std::string(kChannelLetters[c]) : uses[(size_t)c];
        if (vpToggle(label.c_str(), on, tip.c_str())) toggleChannel(s, c);
    }
    std::string names;
    for (int c = 0; c < 4; c++) names += (c ? " / " : "") + (uses[(size_t)c].empty() ? std::string("-") : uses[(size_t)c]);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", names.c_str());

    vpRow("Blend", "How the color meets the stored one (linear space)");
    int blend = (int)s.blend;
    if (ImGui::Combo("##vp_blend", &blend, kBlendNames, IM_ARRAYSIZE(kBlendNames))) s.blend = (vc::Blend)blend;
    vpRow("Strength", "How far toward the color a stroke or Apply moves (a stroke never passes it, R31)");
    vpPercent("##vp_strength", &s.strength);

    // R55: the S43 palette assets, else the default swatch library
    vpRow("Palette", "Click: the primary color. Shift-click: onto the selection");
    std::string picked;
    if (paletteAssetField("##vp_palette", *projectPtr, paletteAsset, picked, true)) {
        if (picked.empty()) {
            paletteAsset.clear();
        } else {
            editor::pixelart::ResolvedPalette resolved;
            bool wroteCopy = false;
            std::string error;
            if (editor::pixelart::usePalette(*projectPtr, picked, resolved, wroteCopy, error)) paletteAsset = resolved.assetPath;
            else Out::warning("Vertex Paint: %s", error.c_str());
        }
    }
    std::vector<jasperengine::pixelart::Rgba8> colors;
    if (!paletteAsset.empty()) {
        if (std::shared_ptr<const jasperengine::pixelart::Palette> palette = jasperengine::pixelart::PaletteLibrary::get(paletteAsset)) colors = palette->colors;
    }
    if (colors.empty()) {
        static const SwatchLibrary library = swatches::defaultLibrary();
        for (const Swatch& swatch : library.swatches) colors.push_back(toRgba8(swatch.linear));
    }
    if (!colors.empty()) {
        const int shown = std::min((int)colors.size(), 64);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        paletteSwatchStrip("##vp_palette_strip", colors, width, ImGui::GetFontSize(), shown);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const int index = std::clamp((int)((ImGui::GetIO().MousePos.x - origin.x) / width * (float)shown), 0, shown - 1);
            const Vector4 value = fromRgba8(colors[(size_t)index]);
            if (ImGui::GetIO().KeyShift) tools.applyColorToSelection(sceneId, value);
            else s.primary = value;
        }
        UIProbe::item("vertexPaint.palette");
    }
    ImGui::EndTable();
}

void VertexPaintWindow::drawBrush() {
    if (!vpHeader("Brush")) return;
    VertexPaintSettings& s = VertexPaintTools::get().settings();
    if (!vpBeginProperties("vp_brush")) return;
    vpRow("Size", "Radius in world units ([ and ]; Ctrl+Right drag up and down)");
    UIUtils::sliderFloatInput("##vp_size", &s.radius, 0.01f, 10.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
    s.radius = std::clamp(s.radius, BRUSH_MIN_RADIUS, BRUSH_MAX_RADIUS);
    vpRow("Hardness", "The inner part at full strength (Shift+Right drag)");
    vpPercent("##vp_hardness", &s.hardness);
    vpRow("Falloff");
    int falloff = (int)s.falloff;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    if (ImGui::Combo("##vp_falloff", &falloff, kFalloffNames, IM_ARRAYSIZE(kFalloffNames))) s.falloff = (vc::FalloffPreset)falloff;
    if (s.falloff == vc::FalloffPreset::Custom) {
        ImGui::SameLine();
        ui::CurveFieldOptions options;
        options.label = "Falloff";
        ui::curveField("##vp_falloff_curve", s.falloffCurve, options);
    }
    vpRow("Spacing", "Stamps along the stroke, as a share of the radius");
    vpPercent("##vp_spacing", &s.spacing);
    s.spacing = std::max(0.01f, s.spacing);
    vpRow("Build-up", "Off: a stroke moves a vertex at most Strength of the way, however often it passes. On: every stamp adds.");
    ImGui::Checkbox("##vp_buildup", &s.buildup);
    vpRow("Element", "Vertex: smooth, shared values. Face: whole faces, hard edges.");
    int element = (int)s.element;
    if (ImGui::Combo("##vp_element", &element, "Vertex\0Face\0")) s.element = (vc::Element)element;
    vpRow("Front faces only");
    ImGui::Checkbox("##vp_front", &s.frontFacesOnly);
    vpRow("Skip occluded", "Vertices hidden from the view are not painted");
    ImGui::Checkbox("##vp_occluded", &s.skipOccluded);
    vpRow("Pressure", "A pen's pressure scales the strength and/or the size");
    ImGui::Checkbox("Strength##vp_pressure_strength", &s.pressureStrength);
    ImGui::SameLine();
    ImGui::Checkbox("Size##vp_pressure_size", &s.pressureSize);
    vpRow("Stencil", "A grayscale texture, projected in world space (triplanar), multiplying the falloff");
    char path[512];
    std::snprintf(path, sizeof(path), "%s", s.stencilPath.c_str());
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 1.5f);
    if (ImGui::InputTextWithHint("##vp_stencil", "(none)", path, sizeof(path), ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.stencilPath = path;
        std::string error;
        s.stencil = s.stencilPath.empty() ? nullptr : loadPaintImage(projectPtr, s.stencilPath, false, &error);
        if (!s.stencilPath.empty() && !s.stencil) Out::warning("Vertex Paint: %s", error.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_XMARK "##vp_stencil_clear")) {
        s.stencilPath.clear();
        s.stencil.reset();
    }
    if (s.stencil) {
        vpRow("Stencil Scale");
        UIUtils::sliderFloatInput("##vp_stencil_scale", &s.stencilScale, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        vpRow("Stencil Rotation", "Shift+Right drag left and right");
        UIUtils::sliderFloatInput("##vp_stencil_rotation", &s.stencilRotation, 0.0f, 360.0f, "%.0f deg");
    }
    ImGui::EndTable();
}

void VertexPaintWindow::drawMasks(uint32_t sceneId) {
    if (!vpHeader("Masks", false)) return;
    VertexPaintTools& tools = VertexPaintTools::get();
    PaintMasks& m = tools.settings().masks;
    if (!vpBeginProperties("vp_masks")) return;
    vpRow("Slope", "Degrees from up");
    ImGui::Checkbox("##vp_mask_slope", &m.slope);
    ImGui::SameLine();
    ImGui::DragFloatRange2("##vp_mask_slope_range", &m.slopeMin, &m.slopeMax, 0.5f, 0.0f, 180.0f, "%.0f deg");
    vpRow("Height", "World Y");
    ImGui::Checkbox("##vp_mask_height", &m.height);
    ImGui::SameLine();
    ImGui::DragFloatRange2("##vp_mask_height_range", &m.heightMin, &m.heightMax, 0.01f, -1e6f, 1e6f, "%.2f");
    vpRow("Curvature", "-1 concave .. 1 convex");
    ImGui::Checkbox("##vp_mask_curvature", &m.curvature);
    ImGui::SameLine();
    ImGui::DragFloatRange2("##vp_mask_curvature_range", &m.curvatureMin, &m.curvatureMax, 0.01f, -1.0f, 1.0f, "%.2f");
    vpRow("Channel", "Only where a channel's value is in the range");
    ImGui::Checkbox("##vp_mask_channel", &m.channel);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.0f);
    ImGui::Combo("##vp_mask_channel_index", &m.channelIndex, "R\0G\0B\0A\0");
    ImGui::SameLine();
    ImGui::DragFloatRange2("##vp_mask_channel_range", &m.channelMin, &m.channelMax, 0.005f, 0.0f, 1.0f, "%.2f");
    vpRow("Invert");
    ImGui::Checkbox("##vp_mask_invert", &m.invert);
    vpRow("");
    ImGui::BeginDisabled(!m.any());
    // what the masks let through, as the element selection: the view shows it
    if (ImGui::Button(ICON_FA_EYE " Preview Mask")) {
        std::vector<ElementSelection> selection;
        for (const TargetRef& ref : tools.actingTargets(sceneId)) {
            std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, ref.entity, ref.key);
            if (!mesh) continue;
            const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, ref.entity, ref.key);
            const vc::ApplyOptions options = applyOptions(projectPtr, sceneId, ref.entity, ref.key, true);
            vc::Target all;
            all.element = tools.settings().selectionMode == vc::ElementKind::Vertex ? vc::Element::Vertex : vc::Element::Face;
            const std::vector<vc::Filter> filters = maskFilters(tools.settings());
            vc::ElementKind kind = vc::ElementKind::Vertex;
            vc::Selection picked;
            picked.ids = vc::resolveElements(data, *mesh, all, filters, options, kind);
            picked.kind = kind;
            if (!picked.empty()) selection.push_back(ElementSelection{ref.entity, ref.key, picked});
        }
        setElementSelection(projectPtr, sceneId, std::move(selection), "Preview Mask");
        if (!tools.isArmedIn(sceneId)) tools.armIn(sceneId, PaintTool::Select);
    }
    UIProbe::item("vertexPaint.previewMask");
    ImGui::EndDisabled();
    ImGui::EndTable();
}

void VertexPaintWindow::drawSelection(uint32_t sceneId) {
    if (!vpHeader("Selection")) return;
    VertexPaintTools& tools = VertexPaintTools::get();
    VertexPaintSettings& s = tools.settings();
    const CommandCatalog& catalog = CommandCatalog::editor();
    using Cmd = VertexPaintTools::SelectCommand;
    if (!vpBeginProperties("vp_selection")) return;
    vpRow("Mode", "Vertex: shared values (smooth). Face and Corner: hard edges.");
    static const char* const modes[] = {"Vertex", "Face", "Corner"};
    static const char* const modeCommands[] = {"vertexPaint.mode.vertex", "vertexPaint.mode.face", "vertexPaint.mode.corner"};
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine();
        if (vpToggle((std::string(modes[i]) + "##vp_mode").c_str(), (int)s.selectionMode == i, catalog.tooltip(modeCommands[i], modes[i]).c_str())) {
            tools.setSelectionMode(sceneId, (vc::ElementKind)i);
        }
    }
    ImGui::SameLine();
    if (vpToggle("X-Ray##vp_xray", s.xray, catalog.tooltip("vertexPaint.xray", "X-Ray: select through to hidden elements").c_str())) s.xray = !s.xray;
    vpRow("Drag", "What a drag of the Select tool draws");
    int shape = (int)s.selectShape;
    if (ImGui::Combo("##vp_shape", &shape, "Box\0Lasso\0")) s.selectShape = (SelectShape)shape;
    vpRow("Select");
    struct Button { const char* label; const char* command; Cmd cmd; };
    static const Button buttons[] = {
        {"All", "vertexPaint.select.all", Cmd::All}, {"None", "vertexPaint.select.none", Cmd::None}, {"Invert", "vertexPaint.select.invert", Cmd::Invert},
        {"Grow", "vertexPaint.select.grow", Cmd::Grow}, {"Shrink", "vertexPaint.select.shrink", Cmd::Shrink}, {"Linked", "vertexPaint.select.linked", Cmd::Linked},
    };
    for (size_t i = 0; i < IM_ARRAYSIZE(buttons); i++) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(buttons[i].label)) tools.selectCommand(sceneId, buttons[i].cmd);
        ImGui::SetItemTooltip("%s", catalog.tooltip(buttons[i].command, buttons[i].label).c_str());
    }
    if (ImGui::SmallButton("Flat Region")) tools.selectCommand(sceneId, Cmd::FlatRegion);
    ImGui::SetItemTooltip("Adjacent faces within the angle of the clicked face: one side of a low-poly model");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    ImGui::DragFloat("##vp_flat_angle", &s.flatRegionAngle, 0.5f, 0.0f, 180.0f, "%.0f deg");
    ImGui::SameLine();
    if (ImGui::SmallButton("Similar")) tools.selectCommand(sceneId, Cmd::Similar);
    ImGui::SetItemTooltip("Elements of the selection's color, within the tolerance");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    ImGui::DragFloat("##vp_similar_tol", &s.similar.tolerance, 0.002f, 0.0f, 1.0f, "%.3f");
    vpRow("Similar ignores", "HSV parts Similar Color does not compare");
    ImGui::Checkbox("H##vp_ign_h", &s.similar.ignoreH);
    ImGui::SameLine();
    ImGui::Checkbox("S##vp_ign_s", &s.similar.ignoreS);
    ImGui::SameLine();
    ImGui::Checkbox("V##vp_ign_v", &s.similar.ignoreV);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    vpPercent("##vp_coverage", &s.similar.coverage);
    ImGui::SetItemTooltip("Coverage: the share of a face's corners that must match");
    vpRow("By Normal", "Faces within the angle of the clicked face's normal (up when none)");
    if (ImGui::SmallButton("Select##vp_by_normal")) tools.selectCommand(sceneId, Cmd::ByNormal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    ImGui::DragFloat("##vp_normal_angle", &s.byNormalAngle, 0.5f, 0.0f, 180.0f, "%.0f deg");
    vpRow("By Channel Range");
    if (ImGui::SmallButton("Select##vp_by_range")) tools.selectCommand(sceneId, Cmd::ByChannelRange);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.0f);
    ImGui::Combo("##vp_range_channel", &s.rangeChannel, "R\0G\0B\0A\0");
    ImGui::SameLine();
    ImGui::DragFloatRange2("##vp_range", &s.rangeMin, &s.rangeMax, 0.005f, 0.0f, 1.0f, "%.2f");
    vpRow("Keep on exit", "The selection stays when the tool is put down");
    ImGui::Checkbox("##vp_pin", &s.pinSelection);
    ImGui::EndTable();

    const size_t count = tools.selectedCount(sceneId);
    ImGui::BeginDisabled(count == 0);
    if (ImGui::Button((std::string(ICON_FA_FILL " Apply ") + catalog.bindingsText("vertexPaint.apply")).c_str())) tools.applyToSelection(sceneId, false);
    UIProbe::item("vertexPaint.apply");
    ImGui::SetItemTooltip("Face and Corner mode: hard edges. Vertex mode: shared values.");
    ImGui::SameLine();
    if (ImGui::Button("Gradient Across")) tools.gradientAcross(sceneId);
    ImGui::SetItemTooltip("Primary to secondary along the selection's longest side");
    ImGui::SameLine();
    if (ImGui::Button("Copy Color")) {
        std::string hex;
        if (tools.copyColorFromSelection(sceneId, &hex)) ImGui::SetClipboardText(hex.c_str());
    }
    ImGui::SetItemTooltip("The selection's average into the primary color and the clipboard");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", selectionCountText(s.selectionMode, count).c_str());
}

void VertexPaintWindow::drawColorsOnMesh(uint32_t sceneId) {
    if (!vpHeader("Colors on this mesh")) return;
    VertexPaintTools& tools = VertexPaintTools::get();
    const std::vector<TargetRef> targets = tools.actingTargets(sceneId);
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t position = history ? history->position() : 0;
    if (meshColors.sceneId != sceneId || meshColors.historyPosition != position || !(meshColors.targets == targets)) {
        meshColors.sceneId = sceneId;
        meshColors.historyPosition = position;
        meshColors.targets = targets;
        meshColors.entries.clear();
        // R54: the distinct values of every acting target, merged by value
        for (const TargetRef& ref : targets) {
            std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, ref.entity, ref.key);
            if (!mesh) continue;
            const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, ref.entity, ref.key);
            for (const vc::PaletteEntry& entry : vc::palette(data, *mesh, 64)) {
                auto same = std::find_if(meshColors.entries.begin(), meshColors.entries.end(), [&](const vc::PaletteEntry& e) { return e.value == entry.value; });
                if (same != meshColors.entries.end()) same->corners += entry.corners;
                else meshColors.entries.push_back(entry);
            }
        }
    }
    if (meshColors.entries.empty()) {
        ImGui::TextDisabled("No vertex colors yet.");
        return;
    }
    if (!ImGui::BeginTable("vp_mesh_colors", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("Swatch");
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Select");
    for (size_t i = 0; i < meshColors.entries.size(); i++) {
        const vc::PaletteEntry entry = meshColors.entries[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // editing the swatch recolors every match (replace_color, one undo step when the window closes)
        Vector4 edited = entry.value;
        ui::ColorFieldOptions options;
        options.label = "Recolor";
        options.width = ImGui::GetFrameHeight() * 2.0f;
        const ui::EditPhase phase = ui::colorField("##vp_mesh_color", edited, options);
        if (phase == ui::EditPhase::Committed && !(edited == entry.value)) tools.recolor(sceneId, entry.value, edited);
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  %u", vc::toHex(entry.value, entry.value.w < 1.0f).c_str(), entry.corners);
        ImGui::TableSetColumnIndex(2);
        if (ImGui::SmallButton("Select")) {
            tools.selectByColor(sceneId, entry.value);
            if (!tools.isArmedIn(sceneId)) tools.armIn(sceneId, PaintTool::Select);
        }
        UIProbe::item("vertexPaint.meshColor");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void VertexPaintWindow::drawGenerate(uint32_t sceneId) {
    (void)sceneId;
    if (!vpHeader("Generate & Bake", false)) return;
    static const Dialog generators[] = {Dialog::FromTexture, Dialog::AmbientOcclusion, Dialog::Curvature, Dialog::Thickness, Dialog::Gradient,
                                        Dialog::Noise, Dialog::Direction, Dialog::TransferFrom, Dialog::Adjust, Dialog::Channels};
    float lineWidth = 0.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    auto button = [&](Dialog d) {
        const std::string label = std::string(dialogTitle(d)) + "...";
        const float width = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (lineWidth > 0.0f && lineWidth + width < avail) ImGui::SameLine();
        else lineWidth = 0.0f;
        lineWidth += width + ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::Button(label.c_str())) openDialog(d);
        UIProbe::item(std::string("vertexPaint.generate.") + dialogTitle(d));
    };
    for (Dialog d : generators) button(d);
    ImGui::Separator();
    lineWidth = 0.0f;
    button(Dialog::ExportModel);
    button(Dialog::BakeTexture);
}

void VertexPaintWindow::drawView() {
    if (!vpHeader("View")) return;
    VertexPaintTools& tools = VertexPaintTools::get();
    VertexPaintSettings& s = tools.settings();
    const CommandCatalog& catalog = CommandCatalog::editor();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Show");
    static const char* const labels[] = {"RGB", "R", "G", "B", "A"};
    const SceneProject* scene = projectPtr->getSelectedScene();
    const bool vertexView = scene && scene->displaySettings.drawMode == ViewDrawMode::VERTEX_COLOR;
    for (int i = 0; i < 5; i++) {
        ImGui::SameLine();
        const bool on = vertexView && (int)scene->displaySettings.vertexColorChannel == i;
        if (vpToggle((std::string(labels[i]) + "##vp_view").c_str(), on, i == 0 ? catalog.tooltip("vertexPaint.view.rgb", "RGB").c_str() : nullptr)) {
            tools.setViewChannel((ViewVertexColorChannel)i);
        }
    }
    ImGui::SameLine();
    if (vpToggle("Off##vp_view", !vertexView, "The Draw Mode the tool found (Shaded)")) tools.setViewOff();
    ImGui::Checkbox("Wire on selection", &s.wireOnSelection);
    ImGui::SameLine();
    ImGui::Checkbox("Show affected vertices", &s.showAffected);
}

// ==================================================================== the dialog window

bool VertexPaintWindow::dialogFields(uint32_t sceneId) {
    Fields& f = fields;
    if (!vpBeginProperties("vp_dialog")) return false;
    switch (dialog) {
        case Dialog::FromTexture:
            vpRow("Source");
            {
                int source = f.fromMaterial ? 1 : 0;
                if (ImGui::Combo("##vp_ft_source", &source, "Texture asset\0Material slot\0")) f.fromMaterial = source == 1;
            }
            if (f.fromMaterial) {
                vpRow("Slot", "Albedo: the albedo map times the base color factor");
                ImGui::Combo("##vp_ft_slot", &f.materialSlot, "Albedo\0Emissive\0Occlusion\0Metallic Roughness\0");
            } else {
                vpRow("Texture", "An assets-relative path");
                char path[512];
                std::snprintf(path, sizeof(path), "%s", f.texturePath.c_str());
                if (ImGui::InputText("##vp_ft_path", path, sizeof(path))) f.texturePath = path;
                vpRow("Color data (sRGB)");
                ImGui::Checkbox("##vp_ft_srgb", &f.colorData);
            }
            vpRow("UV Set");
            ImGui::Combo("##vp_ft_uv", &f.uvSet, "UV0\0UV1\0");
            vpRow("Samples", "Per corner, stratified (at least 16)");
            ImGui::DragInt("##vp_ft_samples", &f.samples, 1.0f, 16, 1024);
            vpRow("Keep texture seams", "Hard edges where a vertex's corners differ by more than dE 2");
            ImGui::Checkbox("##vp_ft_seams", &f.keepSeams);
            break;
        case Dialog::AmbientOcclusion:
        case Dialog::Thickness:
            vpRow("Rays");
            ImGui::DragInt("##vp_ao_rays", &f.rays, 1.0f, 1, 4096);
            vpRow("Max Distance");
            ImGui::DragFloat("##vp_ao_distance", &f.distance, 0.01f, 0.0001f, 1e5f, "%.3f");
            vpRow("Scope", "This object alone, or every static mesh of the scene");
            {
                int scope = f.sceneScope ? 1 : 0;
                if (ImGui::Combo("##vp_ao_scope", &scope, "Object\0Scene\0")) f.sceneScope = scope == 1;
            }
            vpRow("Seed");
            ImGui::DragInt("##vp_ao_seed", &f.seed, 1.0f, 0, 1 << 30);
            break;
        case Dialog::Curvature:
            vpRow("Blur", "Rings of edges");
            ImGui::DragInt("##vp_curv_blur", &f.curvatureBlur, 0.1f, 0, 64);
            break;
        case Dialog::Gradient:
            vpRow("Type");
            ImGui::Combo("##vp_gr_type", &f.gradientType, "Linear\0Radial\0Height\0Axis\0");
            if (f.gradientType <= 1) {
                vpRow("From (world)");
                ImGui::DragFloat3("##vp_gr_from", &f.gradientFrom.x, 0.01f);
                vpRow("To (world)");
                ImGui::DragFloat3("##vp_gr_to", &f.gradientTo.x, 0.01f);
            } else if (f.gradientType == 3) {
                vpRow("Axis");
                ImGui::DragFloat3("##vp_gr_axis", &f.axis.x, 0.01f, -1.0f, 1.0f);
            }
            vpRow("Interpolation");
            ImGui::Combo("##vp_gr_interp", &f.interpolation, "Linear\0OKLab\0HSV\0");
            vpRow("Extend");
            ImGui::Combo("##vp_gr_extend", &f.extend, "Off\0Forward\0Backward\0Both\0");
            vpRow("Element");
            ImGui::Combo("##vp_gr_element", &f.gradientElement, "Vertex\0Face\0");
            break;
        case Dialog::Noise:
            vpRow("Kind");
            ImGui::Combo("##vp_noise_kind", &f.noiseKind, "Value\0Simplex\0Cellular\0");
            vpRow("Scale");
            ImGui::DragFloat("##vp_noise_scale", &f.noiseScale, 0.01f, 0.0001f, 1e4f, "%.3f");
            vpRow("Octaves");
            ImGui::DragInt("##vp_noise_octaves", &f.octaves, 0.1f, 1, 8);
            vpRow("Seed");
            ImGui::DragInt("##vp_noise_seed", &f.seed, 1.0f, 0, 1 << 30);
            vpRow("World space");
            ImGui::Checkbox("##vp_noise_world", &f.noiseWorld);
            break;
        case Dialog::Direction:
            vpRow("Direction", "World; up paints tops (moss), down paints bottoms (dirt)");
            ImGui::DragFloat3("##vp_dir", &f.direction.x, 0.01f, -1.0f, 1.0f);
            vpRow("Range", "smoothstep of the normal against the direction");
            ImGui::DragFloatRange2("##vp_dir_range", &f.rangeMin, &f.rangeMax, 0.005f, -1.0f, 1.0f, "%.2f");
            break;
        case Dialog::TransferFrom: {
            vpRow("Source");
            SceneProject* sceneProject = projectPtr->getScene(sceneId);
            const std::string current = f.transferSource != NULL_ENTITY && sceneProject && sceneProject->scene->isEntityCreated(f.transferSource)
                                            ? sceneProject->scene->getEntityName(f.transferSource) : std::string("(choose)");
            if (ImGui::BeginCombo("##vp_tr_source", current.c_str())) {
                auto components = sceneProject->scene->getComponentArray<VertexColorsComponent>();
                for (size_t i = 0; i < components->size(); i++) {
                    const Entity entity = components->getEntity(i);
                    if (ImGui::Selectable((sceneProject->scene->getEntityName(entity) + "##vp_tr" + std::to_string(entity)).c_str(), f.transferSource == entity)) {
                        f.transferSource = entity;
                    }
                }
                ImGui::EndCombo();
            }
            vpRow("Max Distance");
            ImGui::DragFloat("##vp_tr_dist", &f.maxDistance, 0.001f, 0.00001f, 1e4f, "%.4f");
            vpRow("Normal Angle");
            ImGui::DragFloat("##vp_tr_angle", &f.normalAngle, 0.5f, 0.0f, 180.0f, "%.0f deg");
            vpRow("Unmatched", "Keep their value, or take the nearest overall");
            {
                int unmatched = f.unmatchedNearest ? 1 : 0;
                if (ImGui::Combo("##vp_tr_unmatched", &unmatched, "Keep\0Nearest\0")) f.unmatchedNearest = unmatched == 1;
            }
            break;
        }
        case Dialog::Adjust:
            vpRow("Adjust");
            ImGui::Combo("##vp_adj_kind", &f.adjustKind, "Levels\0Brightness / Contrast\0HSV\0Invert\0Clamp\0");
            if (f.adjustKind == 0) {
                vpRow("Input");
                ImGui::DragFloatRange2("##vp_adj_in", &f.inBlack, &f.inWhite, 0.005f, 0.0f, 1.0f, "%.2f");
                vpRow("Gamma");
                ImGui::DragFloat("##vp_adj_gamma", &f.gamma, 0.01f, 0.01f, 10.0f, "%.2f");
                vpRow("Output");
                ImGui::DragFloatRange2("##vp_adj_out", &f.outBlack, &f.outWhite, 0.005f, 0.0f, 1.0f, "%.2f");
            } else if (f.adjustKind == 1) {
                vpRow("Brightness");
                ImGui::DragFloat("##vp_adj_b", &f.brightness, 0.005f, -1.0f, 1.0f, "%.2f");
                vpRow("Contrast");
                ImGui::DragFloat("##vp_adj_c", &f.contrast, 0.005f, -1.0f, 1.0f, "%.2f");
            } else if (f.adjustKind == 2) {
                vpRow("Hue");
                ImGui::DragFloat("##vp_adj_h", &f.hueShift, 0.5f, -180.0f, 180.0f, "%.0f deg");
                vpRow("Saturation");
                ImGui::DragFloat("##vp_adj_s", &f.saturationShift, 0.005f, -1.0f, 1.0f, "%.2f");
                vpRow("Value");
                ImGui::DragFloat("##vp_adj_v", &f.valueShift, 0.005f, -1.0f, 1.0f, "%.2f");
            }
            break;
        case Dialog::Channels:
            vpRow("Operation");
            ImGui::Combo("##vp_ch_kind", &f.channelKind, "Copy\0Swap\0Invert\0Pack\0");
            if (f.channelKind == 0 || f.channelKind == 1) {
                vpRow("From");
                ImGui::Combo("##vp_ch_src", &f.sourceChannel, "R\0G\0B\0A\0");
                vpRow("To");
                ImGui::Combo("##vp_ch_dst", &f.destChannel, "R\0G\0B\0A\0");
            } else if (f.channelKind == 2) {
                vpRow("Channel");
                ImGui::Combo("##vp_ch_inv", &f.sourceChannel, "R\0G\0B\0A\0");
            } else {
                for (int c = 0; c < 4; c++) {
                    vpRow(kChannelLetters[c], "Packed from a channel, or a constant");
                    int source = f.packSource[c] + 1;
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                    if (ImGui::Combo((std::string("##vp_pack") + std::to_string(c)).c_str(), &source, "Constant\0R\0G\0B\0A\0")) f.packSource[c] = source - 1;
                    if (f.packSource[c] < 0) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::DragFloat((std::string("##vp_packc") + std::to_string(c)).c_str(), &f.packConstant[c], 0.005f, 0.0f, 1.0f, "%.2f");
                    }
                }
            }
            break;
        case Dialog::ExportModel: {
            vpRow("Path", "A new .glb, project-relative under assets");
            char path[512];
            std::snprintf(path, sizeof(path), "%s", f.exportPath.c_str());
            if (ImGui::InputText("##vp_ex_path", path, sizeof(path))) f.exportPath = path;
            vpRow("Split hard edges", "Corner values become their own vertices in the file");
            ImGui::Checkbox("##vp_ex_split", &f.splitHardEdges);
            vpRow("Replace in scene", "The Model uses the new file and the component goes, in one undo step");
            ImGui::Checkbox("##vp_ex_replace", &f.replaceInScene);
            break;
        }
        case Dialog::BakeTexture: {
            const std::vector<TargetRef> targets = VertexPaintTools::get().actingTargets(sceneId);
            vpRow("Target");
            SceneProject* sceneProject = projectPtr->getScene(sceneId);
            f.bakeTarget = std::clamp(f.bakeTarget, 0, std::max(0, (int)targets.size() - 1));
            const std::string current = targets.empty() || !sceneProject ? std::string("(none)")
                : sceneProject->scene->getEntityName(targets[(size_t)f.bakeTarget].entity) + " / " + targets[(size_t)f.bakeTarget].key.describe();
            if (ImGui::BeginCombo("##vp_bk_target", current.c_str())) {
                for (size_t i = 0; i < targets.size(); i++) {
                    const std::string name = sceneProject->scene->getEntityName(targets[i].entity) + " / " + targets[i].key.describe() + "##vp_bk" + std::to_string(i);
                    if (ImGui::Selectable(name.c_str(), f.bakeTarget == (int)i)) f.bakeTarget = (int)i;
                }
                ImGui::EndCombo();
            }
            vpRow("Path", "A new .png, project-relative under assets");
            char path[512];
            std::snprintf(path, sizeof(path), "%s", f.bakePath.c_str());
            if (ImGui::InputText("##vp_bk_path", path, sizeof(path))) f.bakePath = path;
            vpRow("Size");
            {
                static const int sizes[] = {256, 512, 1024, 2048, 4096};
                int index = 2;
                for (int i = 0; i < 5; i++) {
                    if (sizes[i] == f.bakeSize) index = i;
                }
                if (ImGui::Combo("##vp_bk_sizes", &index, "256\0" "512\0" "1024\0" "2048\0" "4096\0")) f.bakeSize = sizes[index];
            }
            vpRow("Padding", "Texels the edges dilate into");
            ImGui::DragInt("##vp_bk_pad", &f.bakePadding, 0.1f, 0, 64);
            vpRow("UV Set");
            ImGui::Combo("##vp_bk_uv", &f.bakeUvSet, "UV0\0UV1\0");
            vpRow("sRGB", "Off: linear values, for data");
            ImGui::Checkbox("##vp_bk_srgb", &f.bakeSrgb);
            break;
        }
        case Dialog::None:
            break;
    }
    ImGui::EndTable();
    return true;
}

void VertexPaintWindow::drawDialog(uint32_t sceneId) {
    if (dialog == Dialog::None) return;
    bool open = true;
    const std::string title = std::string("Vertex Paint: ") + dialogTitle(dialog) + "###VertexPaintDialog";
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open && !job.running) dialog = Dialog::None;
        return;
    }
    // auto-resize follows the widest item: the stretched fields have none, so a floor keeps the width
    ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 22.0f, 0.0f));
    const size_t targets = VertexPaintTools::get().actingTargets(sceneId).size();
    ImGui::TextDisabled("On %zu target%s%s", targets, targets == 1 ? "" : "s", VertexPaintTools::get().selectedCount(sceneId) ? ", the selection only" : "");
    ImGui::BeginDisabled(job.running);
    dialogFields(sceneId);
    ImGui::EndDisabled();
    if (job.running) {
        ImGui::ProgressBar(job.progress.load(), ImVec2(-ImGui::GetFrameHeight() * 4.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) cancelJob();
        UIProbe::item("vertexPaint.dialog.cancel");
    } else {
        const char* action = dialog == Dialog::ExportModel ? "Export" : dialog == Dialog::BakeTexture ? "Bake" : "Apply";
        if (ImGui::Button(action)) runDialog();
        UIProbe::item("vertexPaint.dialog.run");
        ImGui::SameLine();
        if (ImGui::Button("Close")) open = false;
    }
    if (!dialogNote.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(dialogNote.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    if (!open && !job.running) dialog = Dialog::None;
}

}  // namespace jasperengine::editor::vertexcolor
