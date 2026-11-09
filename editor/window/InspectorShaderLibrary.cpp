// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector rows of the shader library (docs/superpowers/specs/2026-10-10-shader-library-phase1-design.md):
// the Shader picker, the rows a shader's manifest declares (parameters, textures, features,
// keywords), a material's Surface and Lighting rows, and the render state of UI, points and
// lines. Every edit is a property command on the Catalog paths MCP uses too.

#include "InspectorWindow.h"
#include "PaletteAssetField.h"
#include "widget/ColorPickerWindow.h"

#include "App.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Out.h"
#include "command/CommandHandle.h"
#include "command/UndoTimeline.h"
#include "command/type/ForkShaderCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/SetAssetMetaCmd.h"
#include "component/PointsComponent.h"
#include "external/IconsFontAwesome6.h"
#include "jobs/JobSystem.h"
#include "material/MaterialChange.h"
#include "material/MaterialPresets.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteMaterial.h"
#include "pixelart/PaletteSwap.h"
#include "pixelart/PaletteSwapChecks.h"
#include "pixelart/PaletteUse.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureImportSettings.h"
#include "Theme.h"
#include "util/Color.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <system_error>

using namespace jasperengine;

namespace {

    using ShaderTextures = std::vector<std::pair<std::string, Texture>>;

    const char* blendLabel(BlendMode mode) {
        switch (mode) {
            case BlendMode::NONE: return "Opaque";
            case BlendMode::ALPHA: return "Alpha";
            case BlendMode::PREMULTIPLIED: return "Premultiplied";
            case BlendMode::ADDITIVE: return "Additive";
            case BlendMode::MULTIPLY: return "Multiply";
            case BlendMode::ADDITIVE_PREMULTIPLIED: return "Additive (premultiplied)";
            case BlendMode::SCREEN_PREMULTIPLIED: return "Screen (premultiplied)";
            case BlendMode::MULTIPLY_PREMULTIPLIED: return "Multiply (premultiplied)";
            case BlendMode::SUBTRACT_PREMULTIPLIED: return "Subtract (premultiplied)";
            case BlendMode::LIGHT_PREMULTIPLIED: return "Light (premultiplied)";
            case BlendMode::MAX_PREMULTIPLIED: return "Max (premultiplied)";
            case BlendMode::ERASE_PREMULTIPLIED: return "Erase (premultiplied)";
        }
        return "Alpha";
    }

    const char* depthCompareLabel(DepthCompare compare) {
        switch (compare) {
            case DepthCompare::LESS: return "Less";
            case DepthCompare::LESS_EQUAL: return "Less Equal";
            case DepthCompare::EQUAL: return "Equal";
            case DepthCompare::GREATER_EQUAL: return "Greater Equal";
            case DepthCompare::GREATER: return "Greater";
            case DepthCompare::NOT_EQUAL: return "Not Equal";
            case DepthCompare::ALWAYS: return "Always";
        }
        return "Less Equal";
    }

    const char* lightingLabel(MaterialLighting mode) {
        switch (mode) {
            case MaterialLighting::AUTO: return "Auto";
            case MaterialLighting::PIXEL: return "Per Pixel";
            case MaterialLighting::VERTEX: return "Per Vertex";
            case MaterialLighting::MIXED: return "Mixed";
            case MaterialLighting::NONE: return "None (unlit)";
        }
        return "Auto";
    }

    const char* textureDefaultLabel(ShaderTextureDefault value) {
        switch (value) {
            case ShaderTextureDefault::WHITE: return "Default (white)";
            case ShaderTextureDefault::BLACK: return "Default (black)";
            case ShaderTextureDefault::GRAY: return "Default (gray)";
            case ShaderTextureDefault::NORMAL: return "Default (flat normal)";
            case ShaderTextureDefault::RIPPLES: return "Default (built-in ripples)";
            case ShaderTextureDefault::NOISE: return "Default (built-in noise)";
            case ShaderTextureDefault::PARTICLE: return "Default (built-in particle)";
        }
        return "Default";
    }

    // The label a manifest row shows
    std::string rowLabel(const std::string& label, const std::string& fallback) {
        return label.empty() ? fallback : label;
    }

    // Enum entries whose Auto entry names what it resolves to; the strings outlive the row
    struct AutoEnumEntries {
        std::string autoLabel;
        std::vector<editor::EnumEntry> entries;
    };

    const char* stencilCompareLabel(StencilCompare compare) {
        switch (compare) {
            case StencilCompare::NEVER: return "Never";
            case StencilCompare::LESS: return "Less";
            case StencilCompare::EQUAL: return "Equal";
            case StencilCompare::LESS_EQUAL: return "Less Equal";
            case StencilCompare::GREATER: return "Greater";
            case StencilCompare::NOT_EQUAL: return "Not Equal";
            case StencilCompare::GREATER_EQUAL: return "Greater Equal";
            case StencilCompare::ALWAYS: return "Always";
        }
        return "Always";
    }

    const char* stencilOpLabel(StencilOp op) {
        switch (op) {
            case StencilOp::KEEP: return "Keep";
            case StencilOp::ZERO: return "Zero";
            case StencilOp::REPLACE: return "Replace";
            case StencilOp::INCR_CLAMP: return "Increment";
            case StencilOp::DECR_CLAMP: return "Decrement";
            case StencilOp::INVERT: return "Invert";
            case StencilOp::INCR_WRAP: return "Increment Wrap";
            case StencilOp::DECR_WRAP: return "Decrement Wrap";
        }
        return "Keep";
    }

    // "Off", or the resolved test: "Not Equal 1, pass Replace"
    std::string stencilStateLabel(const RenderState& state) {
        if (!state.stencilEnabled)
            return "Off";
        std::string label = std::string(stencilCompareLabel(state.stencilCompare)) + " " + std::to_string(state.stencilRef);
        if (state.stencilPass != StencilOp::KEEP) label += std::string(", pass ") + stencilOpLabel(state.stencilPass);
        if (state.stencilFail != StencilOp::KEEP) label += std::string(", fail ") + stencilOpLabel(state.stencilFail);
        if (state.stencilDepthFail != StencilOp::KEEP) label += std::string(", depth fail ") + stencilOpLabel(state.stencilDepthFail);
        if (state.stencilReadMask != 0xFF || state.stencilWriteMask != 0xFF)
            label += ", masks " + std::to_string(state.stencilReadMask) + "/" + std::to_string(state.stencilWriteMask);
        return label;
    }

    // the explicit MaterialStencil a resolved state stands for (Auto picked into an explicit test)
    MaterialStencil stencilFromState(const RenderState& state, MaterialStencilCompare compare) {
        MaterialStencil stencil;
        stencil.compare = compare;
        if (!state.stencilEnabled)
            return stencil;
        stencil.ref = state.stencilRef;
        stencil.readMask = state.stencilReadMask;
        stencil.writeMask = state.stencilWriteMask;
        stencil.pass = static_cast<MaterialStencilOp>(state.stencilPass);
        stencil.fail = static_cast<MaterialStencilOp>(state.stencilFail);
        stencil.depthFail = static_cast<MaterialStencilOp>(state.stencilDepthFail);
        return stencil;
    }

    // "RGBA", "RGB", "None"
    std::string colorWriteLabel(int mask) {
        if ((mask & MATERIAL_COLOR_WRITE_ALL) == 0)
            return "None";
        std::string label;
        if (mask & MATERIAL_COLOR_WRITE_R) label += 'R';
        if (mask & MATERIAL_COLOR_WRITE_G) label += 'G';
        if (mask & MATERIAL_COLOR_WRITE_B) label += 'B';
        if (mask & MATERIAL_COLOR_WRITE_A) label += 'A';
        return label;
    }

    // a tooltip of a paragraph or more, wrapped
    void wrappedTooltip(const std::string& text) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // The mark of a shader, feature or keyword value that reads the scene (phase 3): an amber
    // gauge and what it reads, on the line of the item before it. True while it is hovered.
    bool sceneInputsBadge(uint32_t inputs) {
        ImGui::SameLine();
        ImGui::TextColored(editor::Theme::Colors::WarningText, ICON_FA_GAUGE_HIGH " %s", editor::Catalog::getSceneInputsLabel(inputs).c_str());
        return ImGui::IsItemHovered();
    }

    // The tooltip of a mark: what is read (and when), then the cost
    std::string sceneInputsTooltip(const std::string& what, uint32_t inputs) {
        return what + ": " + editor::Catalog::getSceneInputsCost(inputs, true) + ".\n\n" + editor::Catalog::getSceneInputsCost(inputs);
    }

    // One row under a shader's rows: what the draw reads of the scene in its current variant and
    // what that costs (the whole cost in its tooltip); nothing when it reads nothing
    void sceneInputsInfoRow(uint32_t inputs) {
        if (!inputs)
            return;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_GAUGE_HIGH " Reads scene %s: %s", editor::Catalog::getSceneInputsLabel(inputs).c_str(),
                           editor::Catalog::getSceneInputsCost(inputs, true).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            wrappedTooltip(editor::Catalog::getSceneInputsCost(inputs));
    }

    // One row under a shader's rows reading a planar reflection (PLANAR_REFLECTION): what the
    // scene's Planar Reflections draw for it, and that it costs a camera pass per water height
    void planarReflectionInfoRow(Scene* scene, const std::string& shader, const std::vector<std::string>& features) {
        std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
        const std::vector<std::string> enabled = ShaderReference::getEnabledFeatures(shader, features);
        if (!scene || !manifest || !(manifest->resolveRequire(enabled) & ShaderRequire_PlanarReflection))
            return;
        static const char* const sizes[] = {"Off", "a quarter", "half", "full"};
        const PlanarReflectionResolution resolution = scene->getPlanarReflectionResolution();
        // a water draws its planar reflection inside its Reflection
        const bool withoutReflection = manifest->role == ShaderRole::WATER &&
            std::find(enabled.begin(), enabled.end(), "REFLECTION") == enabled.end();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::WarningText);
        if (withoutReflection)
            ImGui::TextWrapped(ICON_FA_GAUGE_HIGH " Planar reflection: turn Reflection on too (it draws inside it)");
        else if (resolution == PlanarReflectionResolution::OFF)
            ImGui::TextWrapped(ICON_FA_GAUGE_HIGH " Planar reflection: Off in this scene, the sky or probe reflection is drawn");
        else
            ImGui::TextWrapped(ICON_FA_GAUGE_HIGH " Planar reflection: the scene again at %s resolution, one pass per water height", sizes[(int)resolution]);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            wrappedTooltip("Each camera that sees this water draws the scene mirrored about its rest level, once for every water height "
                           "it sees (the nearest up to Max Planes). Scene settings > Planar Reflections sets the resolution (Off: none), "
                           "the layers, shadows and distance. Made for flat water: a sloped surface mirrors about one level.");
    }

    std::shared_ptr<const ShaderManifest> presentManifest(const std::string& shader) {
        std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
        return (manifest && manifest->present) ? manifest : nullptr;
    }

    // What submesh `submesh` of `entity` resolves to with its stencil and colour write on Auto
    // (its other values as set): the @state of the shader it draws with, else no stencil and
    // every channel
    RenderState resolveMaterialStencilAuto(Scene* scene, Entity entity, unsigned int submesh) {
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || submesh >= mesh->numSubmeshes)
            return RenderState();
        const Submesh& sub = mesh->submeshes[submesh];
        std::shared_ptr<const ShaderManifest> manifest =
            presentManifest(RenderSystem::getSubmeshShader(*mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH)));
        Submesh autoSubmesh;
        autoSubmesh.material = sub.material;
        autoSubmesh.faceCulling = sub.faceCulling;
        autoSubmesh.material.stencil = MaterialStencil();
        autoSubmesh.material.colorWrite = -1;
        bool blended = false;
        bool transparentQueue = false;
        return RenderSystem::resolveSubmeshRenderState(*mesh, autoSubmesh, manifest.get(), blended, transparentQueue);
    }

    // What the extra pass of submesh `submesh` of `entity` resolves to with its render state on
    // Auto: the pass shader's @state (with its features' rules), else the submesh's legacy
    // behaviour (resolveExtraPassRenderState resolves the pass's values like a submesh's: the
    // same resolver with the submesh's legacy inputs, which also says whether Auto blends)
    RenderState resolveExtraPassAuto(Scene* scene, Entity entity, unsigned int submesh, bool* blendedOut = nullptr) {
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || submesh >= mesh->numSubmeshes)
            return RenderState();
        const Submesh& sub = mesh->submeshes[submesh];
        const MaterialPass& pass = sub.material.extraPass;
        std::shared_ptr<const ShaderManifest> manifest = presentManifest(pass.shader);
        Submesh autoSubmesh;
        autoSubmesh.material = sub.material;
        autoSubmesh.faceCulling = sub.faceCulling;
        Material& autoMaterial = autoSubmesh.material;
        autoMaterial.blendMode = MaterialBlendMode::AUTO;
        autoMaterial.cullMode = MaterialCullMode::AUTO;
        autoMaterial.depthWrite = MaterialDepthWrite::AUTO;
        autoMaterial.depthTest = MaterialDepthTest::AUTO;
        autoMaterial.depthOffsetFactor = pass.depthOffsetFactor;
        autoMaterial.depthOffsetUnits = pass.depthOffsetUnits;
        autoMaterial.stencil = MaterialStencil();
        autoMaterial.colorWrite = -1;
        ShaderManifestState manifestState;
        if (manifest)
            manifestState = manifest->resolveState(manifest->resolveFeatures(pass.shaderFeatures));
        bool blended = false;
        bool transparentQueue = false;
        const RenderState resolved = RenderSystem::resolveSubmeshRenderState(*mesh, autoSubmesh, manifest ? &manifestState : nullptr,
                                                                             blended, transparentQueue);
        if (blendedOut)
            *blendedOut = blended;
        return resolved;
    }

    // What a UI, Points or LineRenderer component of `entity` resolves to with its render state
    // on Auto: its shader's @state (with its features' rules), else its legacy state
    RenderState resolveComponentAuto(Scene* scene, Entity entity, editor::ComponentType cpType) {
        static const std::vector<std::string> noFeatures;
        std::shared_ptr<const ShaderManifest> manifest =
            presentManifest(editor::Catalog::getShaderReferenceForProperty(scene, entity, cpType, "customShader"));
        const std::vector<std::string>* features = editor::Catalog::getPropertyRef<std::vector<std::string>>(scene, entity, cpType, "shaderFeatures");
        RenderState state = RenderSystem::resolveComponentRenderState(MaterialBlendMode::AUTO, MaterialDepthWrite::AUTO, manifest.get(),
                                                                      features ? *features : noFeatures, false);
        // Points: Auto writes depth when opaque only (RenderSystem::resolvePointsRenderState)
        if (cpType == editor::ComponentType::PointsComponent){
            if (const PointsComponent* points = scene->findComponent<PointsComponent>(entity)){
                const MaterialDepthWrite manifestDepthWrite = manifest ?
                    manifest->resolveState(manifest->resolveFeatures(features ? *features : noFeatures)).depthWrite : MaterialDepthWrite::AUTO;
                if (manifestDepthWrite == MaterialDepthWrite::AUTO)
                    state.depthWrite = !points->isTransparent();
            }
        }
        return state;
    }

}

// Palette Swap rows' state (S43 R35): what workers found about a sprite's texture, Detect, and the
// caches the rows read every frame. File-local, in a namespace of its own (unity builds).
namespace jasperengine::editor::inspectorpaletterows {
namespace {

    namespace pxc = jasperengine::pixelart;

    // a worker's reading of a sprite's texture: its size, the colours the source does not hold
    // (the rows' analysis) or the project palettes holding all of them (Detect)
    struct TextureFindings {
        bool loaded = false;
        std::string error;
        int width = 0, height = 0;
        int64_t offColours = -1;
        std::vector<std::string> covering;
    };

    struct PaletteSlotState {
        std::string analysisKey;
        jobs::Task<TextureFindings> analysis;
        TextureFindings found;          // the finished analysis of analysisKey
        bool analysisReady = false;

        jobs::Task<TextureFindings> detect;
        bool detecting = false;
        bool detectAuto = false;        // started by ticking the feature: fills an empty Source only
        std::string detectTexture;
        std::string detectStatus;
        std::vector<std::string> detectChoices;

        std::string checkKey;
        double checkTime = -1.0;
        editor::pixelart::PaletteSwapTextureCheck check;

        std::string buildKey;
        std::vector<pxc::Diagnostic> diagnostics;
        std::vector<std::string> rowNames;
    };

    // by scene, GameObject, material and uniform; a selection that moves on leaves its entries,
    // trimmed when they pile up (a running job finishes unseen)
    std::map<std::string, PaletteSlotState>& paletteSlotStates() {
        static std::map<std::string, PaletteSlotState> states;
        return states;
    }

    // materials whose Palette Swap was just ticked on: their rows Detect the Source once
    std::set<std::string>& paletteAutoDetects() {
        static std::set<std::string> requests;
        return requests;
    }

    std::string paletteMaterialKey(uint32_t sceneId, Entity entity, const std::string& materialPrefix) {
        return std::to_string(sceneId) + "/" + std::to_string((uint64_t)entity) + "/" + materialPrefix;
    }

    // a file's modification time and size: an edit on disk reruns the analysis
    std::string paletteFileStamp(const std::filesystem::path& file) {
        std::error_code error;
        const auto written = std::filesystem::last_write_time(file, error);
        const long long time = error ? 0 : (long long)written.time_since_epoch().count();
        const uintmax_t size = std::filesystem::file_size(file, error);
        return std::to_string(time) + ":" + std::to_string(error ? uintmax_t(0) : size);
    }

    std::string paletteBindingKey(const pxc::PaletteBinding& binding) {
        std::string key = binding.source.path + (binding.useVariants ? "|v" : "|-");
        for (const pxc::PaletteTarget& target : binding.targets) key += "|" + target.text();
        return key + "#" + std::to_string(pxc::PaletteLibrary::generation());
    }

    std::string paletteFileName(const std::string& path) {
        return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).filename());
    }

    // the materials' palette bindings with one slot's replaced (an empty binding is dropped)
    pxc::ShaderPalettes withPaletteBinding(const pxc::ShaderPalettes& palettes, const std::string& uniform, const pxc::PaletteBinding& binding) {
        pxc::ShaderPalettes changed;
        bool placed = false;
        for (const auto& entry : palettes) {
            if (entry.first != uniform) {
                changed.push_back(entry);
            } else if (!placed) {
                placed = true;
                if (!binding.empty()) changed.push_back({uniform, binding});
            }
        }
        if (!placed && !binding.empty()) changed.push_back({uniform, binding});
        return changed;
    }

    // a warning line of the value column, wrapped; its tooltip says more
    void paletteWarningRow(const ImVec4& color, const std::string& text, const std::string& tooltip) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
        if (!tooltip.empty() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(tooltip.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // a finished job's findings (a failed or cancelled one reads as unloaded)
    TextureFindings takeFindings(jobs::Task<TextureFindings>& task) {
        TextureFindings found;
        if (task.status() == jobs::TaskStatus::Completed) found = task.get();
        else found.error = "the reading was cancelled";
        task.reset();
        return found;
    }

}
}

using namespace jasperengine::editor::inspectorpaletterows;

void editor::InspectorWindow::drawLibraryShaderCombo(const std::string& currentShader, ShaderType type,
                                                     const std::function<void(const std::string&)>& setShader,
                                                     const std::string& popupName, const char* builtInLabel,
                                                     const std::function<void(const std::string&)>& onForkLibrary){
    const char* builtIn = builtInLabel ? builtInLabel : (type == ShaderType::MESH ? "Lit (built-in)" : "Built-in");

    std::string preview = builtIn;
    if (ShaderReference::isLibrary(currentShader)) {
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(currentShader);
        preview = (manifest && !manifest->name.empty()) ? manifest->name : currentShader;
    } else if (!currentShader.empty()) {
        preview = "Custom: " + currentShader;
    }

    // the library shaders of this type by category, in name order within each
    std::map<std::string, std::vector<std::pair<std::string, std::shared_ptr<const ShaderManifest>>>> categories;
    for (const std::string& name : ShaderManifests::getLibraryShaderNames()) {
        const std::string reference = ShaderReference::LIBRARY_PREFIX + name;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(reference);
        if (!manifest || manifest->type != type)
            continue;
        categories[manifest->category.empty() ? "Other" : manifest->category].push_back({reference, manifest});
    }

    // the files popup opens after the combo closes: opened from inside the combo's own popup
    // window, its id would not match the BeginPopup of the row (drawShaderFilesPopup)
    bool openFiles = false;
    bool forkLibrary = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(("##shader_picker_" + popupName).c_str(), preview.c_str())) {
        if (ImGui::Selectable(builtIn, currentShader.empty()) && !currentShader.empty())
            setShader("");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", builtInLabel ? "No extra draw"
                                    : type == ShaderType::MESH ? "The engine's physically based shader, with the material's PBR values"
                                                               : "The engine's own shader for this component");
        for (const auto& [category, shaders] : categories) {
            ImGui::SeparatorText(category.c_str());
            for (const auto& [reference, manifest] : shaders) {
                const std::string label = (manifest->name.empty() ? reference : manifest->name) + "##" + reference;
                if (ImGui::Selectable(label.c_str(), currentShader == reference) && currentShader != reference)
                    setShader(reference);
                bool hovered = ImGui::IsItemHovered();
                // a shader reading the scene depth or colour (phase 3) costs passes: marked
                const uint32_t sceneInputs = manifest->getPossibleSceneInputs();
                if (sceneInputs)
                    hovered = sceneInputsBadge(sceneInputs) || hovered;
                if (hovered && sceneInputs)
                    wrappedTooltip((manifest->description.empty() ? std::string() : manifest->description + "\n\n") +
                                   sceneInputsTooltip(Catalog::getShaderSceneInputsSummary(*manifest), sceneInputs));
                else if (hovered && !manifest->description.empty())
                    ImGui::SetTooltip("%s", manifest->description.c_str());
            }
        }
        ImGui::Separator();
        const bool fork = !currentShader.empty() && !ShaderReference::isLibrary(currentShader);
        if (ImGui::Selectable("Custom file...", fork))
            openFiles = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A shader of this project: pick its .vert and .frag files");
        // a copy of the library shader in the project, keeping its rows and its role
        if (onForkLibrary && ShaderReference::isLibrary(currentShader)) {
            std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(currentShader);
            const std::string name = (manifest && !manifest->name.empty()) ? manifest->name : currentShader;
            if (ImGui::Selectable(("Fork \"" + name + "\"...").c_str()))
                forkLibrary = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Copy %s into this project to edit it; the copy keeps its parameters, features and what the engine does with it (water floats bodies, the underwater pass switches)",
                                  currentShader.c_str());
        }
        ImGui::EndCombo();
    }
    if (forkLibrary)
        onForkLibrary(ShaderReference::getBase(currentShader));
    if (ShaderReference::isLibrary(currentShader) && ImGui::IsItemHovered()) {
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(currentShader);
        if (manifest && manifest->getPossibleSceneInputs())
            wrappedTooltip(currentShader + (manifest->description.empty() ? std::string() : "\n\n" + manifest->description) + "\n\n" +
                           Catalog::getShaderSceneInputsSummary(*manifest) + " (cost: " +
                           Catalog::getSceneInputsCost(manifest->getPossibleSceneInputs(), true) + ")");
        else if (manifest && !manifest->description.empty())
            ImGui::SetTooltip("%s\n\n%s", currentShader.c_str(), manifest->description.c_str());
        else if (!manifest)
            ImGui::SetTooltip("%s is not a library shader of this engine", currentShader.c_str());
    }
    if (openFiles)
        ImGui::OpenPopup(popupName.c_str());
}

float editor::InspectorWindow::getGlobalLinkButtonWidth() const{
    return ImGui::CalcTextSize(ICON_FA_LINK).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void editor::InspectorWindow::drawGlobalLinkButton(const std::string& member, int components, const ShaderGlobalBindings& bindings,
                                                   const std::function<void(const ShaderGlobalBindings&)>& set){
    const std::string bound = ShaderGlobals::getBinding(bindings, member);
    // at the right end of the cell
    ImGui::SameLine();
    const float buttonWidth = getGlobalLinkButtonWidth();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > buttonWidth)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - buttonWidth);

    const std::string popup = "##global_link_popup_" + member;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, bound.empty() ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] : ImGui::GetStyle().Colors[ImGuiCol_CheckMark]);
    if (ImGui::Button((std::string(ICON_FA_LINK) + "##global_link_" + member).c_str()))
        ImGui::OpenPopup(popup.c_str());
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        if (bound.empty())
            ImGui::SetTooltip("Bind to a global shader value...\nThe parameter then follows the global (Project Settings > Graphics > Shader Globals, Shader.setGlobal*)");
        else
            ImGui::SetTooltip("Follows the global '%s'. Click to change or unbind", bound.c_str());
    }

    if (!ImGui::BeginPopup(popup.c_str()))
        return;
    ImGui::TextDisabled("Bind %s to a global", member.c_str());
    ImGui::Separator();
    // the globals of the member's kind: same components, a colour also takes vector4 and back
    bool any = false;
    for (const std::string& name : ShaderGlobals::getNames()) {
        ShaderGlobalValue value;
        if (!ShaderGlobals::get(ShaderGlobals::findID(name), value))
            continue;
        const int globalComponents = ShaderGlobals::isArray(value.type) || ShaderGlobals::isTexture(value.type) ? 0
                                   : ShaderGlobals::getComponentCount(value.type);
        if (globalComponents != components)
            continue;
        any = true;
        const std::string label = name + "  (" + ShaderGlobals::getTypeName(value.type) + (value.declared ? "" : ", set by a script") + ")";
        if (ImGui::Selectable(label.c_str(), name == bound) && name != bound) {
            ShaderGlobalBindings changed = bindings;
            ShaderGlobals::setBinding(changed, member, name);
            set(changed);
        }
    }
    if (!any) {
        const char* kind = components == 1 ? "float" : components == 2 ? "vector2" : components == 3 ? "vector3" : "vector4 or color";
        ImGui::TextDisabled("No %s global is declared", kind);
    }
    ImGui::Separator();
    if (!bound.empty() && ImGui::Selectable(ICON_FA_LINK_SLASH "  Unbind")) {
        ShaderGlobalBindings changed = bindings;
        ShaderGlobals::setBinding(changed, member, "");
        set(changed);
    }
    if (ImGui::Selectable("Shader Globals..."))
        shaderGlobalsRequested = true;
    ImGui::EndPopup();
}

void editor::InspectorWindow::drawBoundGlobalValue(const std::string& global, int components, bool color, float width){
    Vector4 value;
    const bool has = ShaderGlobals::getBoundValue(global, value);
    const float start = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    if (color && has) {
        const Vector4 srgb = Color::linearTosRGB(value);
        const float size = ImGui::GetFrameHeight();
        ImGui::ColorButton(("##bound_swatch_" + global).c_str(), ImVec4(srgb.x, srgb.y, srgb.z, value.w),
                           ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip, ImVec2(size * 1.5f, size));
        ImGui::SameLine();
    }
    std::string text = "@" + global;
    if (has) {
        const float c[4] = {value.x, value.y, value.z, value.w};
        char number[32];
        text += "  ";
        for (int i = 0; i < std::max(1, std::min(components, 4)); i++) {
            snprintf(number, sizeof(number), i == 0 ? "%.3g" : ", %.3g", c[i]);
            text += number;
        }
    } else {
        text += "  (not set)";
    }
    // clipped to the cell left of the link button
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float room = std::max(1.0f, width - (ImGui::GetCursorPosX() - start));
    ImGui::PushClipRect(min, ImVec2(min.x + room, min.y + ImGui::GetFrameHeight()), true);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopClipRect();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follows the global shader value '%s'%s.\nIts default is in Project Settings > Graphics > Shader Globals; scripts "
                          "set it with Shader.setGlobal*. The link button unbinds it.",
                          global.c_str(), has ? "" : " (no global of this name is declared or set: it reads 0)");
}

bool editor::InspectorWindow::drawShaderPropertyValue(const ShaderManifestProperty& property, const std::string& id, const Vector4& value,
                                                      Vector4& edited, bool& continuous, float itemWidth){
    edited = value;
    continuous = true;
    bool changed = false;
    ImGui::SetNextItemWidth(itemWidth);
    switch (property.kind) {
        case ShaderPropertyKind::COLOR:
        case ShaderPropertyKind::HDRCOLOR: {
            // edited as sRGB in the Color window, stored linear (alpha as is); its session is one
            // undo step (S27 R3)
            ui::ColorFieldOptions colorOptions;
            colorOptions.hdr = property.kind == ShaderPropertyKind::HDRCOLOR;
            colorOptions.label = property.label.empty() ? property.member.c_str() : property.label.c_str();
            Vector4 color = value;
            const ui::EditPhase phase = ui::colorField(id.c_str(), color, colorOptions);
            if (phase == ui::EditPhase::Begin) {
                ui::ColorPickerWindow::beginEdit(CommandHandle::get(project->getSelectedSceneId()), colorOptions.label);
            } else if (phase == ui::EditPhase::Changing) {
                edited = color;
                changed = true;
            }
            break;
        }
        case ShaderPropertyKind::RANGE: {
            float v = value.x;
            if (ImGui::SliderFloat(id.c_str(), &v, property.min, property.max)) {
                edited = Vector4(v, 0.0f, 0.0f, 0.0f);
                changed = true;
            }
            break;
        }
        case ShaderPropertyKind::TOGGLE: {
            bool on = value.x != 0.0f;
            if (ImGui::Checkbox(id.c_str(), &on)) {
                edited = Vector4(on ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                changed = true;
                continuous = false;
            }
            break;
        }
        case ShaderPropertyKind::INT: {
            int v = (int)value.x;
            if (ImGui::DragInt(id.c_str(), &v, 0.1f)) {
                edited = Vector4((float)v, 0.0f, 0.0f, 0.0f);
                changed = true;
            }
            break;
        }
        default: {
            const int components = property.kind == ShaderPropertyKind::VECTOR2 ? 2
                                 : property.kind == ShaderPropertyKind::VECTOR3 ? 3
                                 : property.kind == ShaderPropertyKind::VECTOR4 ? 4 : 1;
            float fields[4] = {value.x, value.y, value.z, value.w};
            // one float is one drag (DragScalarN groups unlabelled parts: the row's id would name no item)
            const bool dragged = components == 1 ? ImGui::DragScalar(id.c_str(), ImGuiDataType_Float, fields, 0.01f)
                                                 : ImGui::DragScalarN(id.c_str(), ImGuiDataType_Float, fields, components, 0.01f);
            if (dragged) {
                edited = Vector4(fields[0], fields[1], fields[2], fields[3]);
                changed = true;
            }
            break;
        }
    }
    return changed;
}

bool editor::InspectorWindow::drawShaderManifestRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::string& shader,
                                                     const std::string& paramsProperty, const std::string& texturesProperty,
                                                     const std::string& featuresProperty, const std::function<void()>& onValueChanged){
    std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
    if (!manifest || !manifest->present)
        return false;

    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    ShaderUniformValues* paramsRef = Catalog::getPropertyRef<ShaderUniformValues>(scene, entity, cpType, paramsProperty);
    ShaderTextures* texturesRef = Catalog::getPropertyRef<ShaderTextures>(scene, entity, cpType, texturesProperty);
    std::vector<std::string>* featuresRef = Catalog::getPropertyRef<std::vector<std::string>>(scene, entity, cpType, featuresProperty);
    if (!paramsRef || !texturesRef || !featuresRef)
        return false;
    // copies: a command may change them while the rows are drawn
    const ShaderUniformValues params = *paramsRef;
    const ShaderTextures textures = *texturesRef;
    const std::vector<std::string> features = *featuresRef;

    // a discrete change is its own undo step; a drag merges until released
    auto setFeatures = [&](const std::vector<std::string>& value) {
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<std::vector<std::string>>(project, sceneId, entity, cpType, featuresProperty, value, onValueChanged));
    };
    auto setParams = [&](const ShaderUniformValues& value, bool continuous) {
        Command* paramsCmd = new PropertyCmd<ShaderUniformValues>(project, sceneId, entity, cpType, paramsProperty, value, onValueChanged);
        if (continuous) {
            cmd = paramsCmd;
            CommandHandle::get(sceneId)->addCommand(paramsCmd);
        } else {
            CommandHandle::get(sceneId)->addCommandNoMerge(paramsCmd);
        }
    };
    auto setTextures = [&](const ShaderTextures& value) {
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<ShaderTextures>(project, sceneId, entity, cpType, texturesProperty, value, onValueChanged));
    };
    auto finishDrag = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
            cmd->setNoMerge();
            cmd = nullptr;
        }
    };

    // a material's Palette Swap rows (S43): "submeshes[N].material." (an extra pass has none)
    std::string palettePrefix;
    const std::string materialParamsSuffix = "material.shaderParams";
    if (!manifest->palettes.empty() && paramsProperty.size() > materialParamsSuffix.size() &&
        paramsProperty.compare(paramsProperty.size() - materialParamsSuffix.size(), materialParamsSuffix.size(), materialParamsSuffix) == 0)
        palettePrefix = paramsProperty.substr(0, paramsProperty.size() - std::strlen("shaderParams"));
    bool paletteSlotOn = false;
    if (!palettePrefix.empty()) {
        const std::vector<std::string> enabledDefines = manifest->resolveFeatures(features);
        for (const ShaderManifestPalette& slot : manifest->palettes)
            paletteSlotOn = paletteSlotOn || slot.isEnabled(enabledDefines);
    }

    // ---- features and keywords (compile-time: a change builds another variant, off the UI thread)
    for (const ShaderManifestFeature& feature : manifest->features) {
        ImGui::PushID(("feature_" + feature.define).c_str());
        prefabRowProperty = featuresProperty;
        propertyHeader(rowLabel(feature.label, feature.define));
        bool enabled = ShaderLibraryValues::isFeatureEnabled(features, shader, feature.define);
        if (ImGui::Checkbox(("##feature_" + feature.define).c_str(), &enabled)) {
            std::vector<std::string> changed = features;
            ShaderLibraryValues::setFeature(changed, shader, feature.define, enabled);
            setFeatures(changed);
            // ticking Palette Swap on fills an empty Source when one project palette covers the
            // sprite (R18): the rows start Detect once the slot shows
            const bool paletteFeature = std::any_of(manifest->palettes.begin(), manifest->palettes.end(), [&](const ShaderManifestPalette& slot) {
                return std::find(slot.features.begin(), slot.features.end(), feature.define) != slot.features.end();
            });
            if (enabled && paletteFeature && !palettePrefix.empty())
                paletteAutoDetects().insert(paletteMaterialKey(sceneId, entity, palettePrefix));
        }
        bool hovered = ImGui::IsItemHovered();
        const std::string featureTip = feature.define + (feature.defaultOn ? " (on by default)" : "");
        // a feature turning on a scene input (its feature= requires): marked like the picker
        const uint32_t onInputs = Catalog::getFeatureSceneInputs(*manifest, feature.define);
        const uint32_t offInputs = Catalog::getFeatureSceneInputs(*manifest, feature.define, true);
        if (onInputs || offInputs) {
            hovered = sceneInputsBadge(onInputs | offInputs) || hovered;
            if (hovered) {
                std::string what;
                if (onInputs) what = "On, it reads the scene " + Catalog::getSceneInputsLabel(onInputs);
                if (offInputs) what += (what.empty() ? "Off, it reads the scene " : "; off, the scene ") + Catalog::getSceneInputsLabel(offInputs);
                wrappedTooltip(featureTip + "\n\n" + sceneInputsTooltip(what, onInputs | offInputs));
            }
        } else if (hovered) {
            ImGui::SetTooltip("%s", featureTip.c_str());
        }
        ImGui::PopID();
    }
    for (const ShaderManifestKeywords& group : manifest->keywords) {
        ImGui::PushID(("keywords_" + group.group).c_str());
        prefabRowProperty = featuresProperty;
        propertyHeader(rowLabel(group.label, group.group));
        std::string current = group.values.empty() ? std::string() : group.values[0];
        for (const std::string& value : group.values) {
            if (ShaderLibraryValues::isFeatureEnabled(features, shader, group.group + "_" + value))
                current = value;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(("##keywords_" + group.group).c_str(), current.c_str())) {
            for (const std::string& value : group.values) {
                if (ImGui::Selectable(value.c_str(), value == current) && value != current) {
                    std::vector<std::string> changed = features;
                    ShaderLibraryValues::setFeature(changed, shader, group.group + "_" + value, true);
                    setFeatures(changed);
                }
                // a value turning on a scene input: marked like a feature
                bool hovered = ImGui::IsItemHovered();
                if (const uint32_t inputs = Catalog::getFeatureSceneInputs(*manifest, group.group + "_" + value)) {
                    hovered = sceneInputsBadge(inputs) || hovered;
                    if (hovered)
                        wrappedTooltip(sceneInputsTooltip(value + " reads the scene " + Catalog::getSceneInputsLabel(inputs), inputs));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    // a row's feature= ("!<DEFINE>": shown while that define is off)
    auto featureOn = [&](const std::string& feature) {
        if (feature.empty())
            return true;
        if (feature[0] == '!')
            return !ShaderLibraryValues::isFeatureEnabled(features, shader, feature.substr(1));
        return ShaderLibraryValues::isFeatureEnabled(features, shader, feature);
    };

    // members that follow a global shader value: a material's or a component's bindings (an
    // extra pass has none)
    std::string bindingsProperty;
    const std::string materialParams = "material.shaderParams";
    if (paramsProperty == "shaderUniforms")
        bindingsProperty = "shaderGlobalBindings";
    else if (paramsProperty.size() > materialParams.size() &&
             paramsProperty.compare(paramsProperty.size() - materialParams.size(), materialParams.size(), materialParams) == 0)
        bindingsProperty = paramsProperty.substr(0, paramsProperty.size() - std::strlen("shaderParams")) + "shaderGlobalBindings";
    ShaderGlobalBindings* bindingsRef = bindingsProperty.empty() ? nullptr
        : Catalog::getPropertyRef<ShaderGlobalBindings>(scene, entity, cpType, bindingsProperty);
    const ShaderGlobalBindings bindings = bindingsRef ? *bindingsRef : ShaderGlobalBindings();
    auto setBindings = [&](const ShaderGlobalBindings& value) {
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<ShaderGlobalBindings>(project, sceneId, entity, cpType, bindingsProperty, value, onValueChanged));
    };

    // what an unset parameter renders with: a mesh's value under its material's, the scene's
    // default-shader value, the manifest default (ShaderUniforms::resolve, as the render system)
    Catalog::UniformChainRefs chain;
    bool chained = false;
    if (paramsProperty == "shaderUniforms") {
        chain = Catalog::getShaderUniformChain(scene, entity, cpType);
        chained = true;
    } else if (unsigned int submesh = 0; std::sscanf(paramsProperty.c_str(), "submeshes[%u].material.shaderParams", &submesh) == 1 &&
                                         paramsProperty == "submeshes[" + std::to_string(submesh) + "].material.shaderParams") {
        chain = Catalog::getShaderUniformChain(scene, entity, cpType, static_cast<int>(submesh));
        chained = true;
    }
    chain.chain.values = &params;
    chain.chain.manifest = manifest.get();

    // ---- parameters
    for (const ShaderManifestProperty& property : manifest->properties) {
        if (!featureOn(property.feature))
            continue;
        // the Palette Swap rows draw these as Active Palette and Cycling Speed
        if (paletteSlotOn && (property.member == jasperengine::pixelart::kPaletteRowParam ||
                              property.member == jasperengine::pixelart::kPaletteCycleSpeedParam))
            continue;
        const bool hasValue = ShaderUniforms::has(params, property.member);
        UniformSource source = UniformSource::ShaderDefault;
        const Vector4 value = hasValue ? ShaderUniforms::get(params, property.member)
                            : chained ? ShaderUniforms::resolve(chain.chain, property.member, &source) : property.defaultValue;
        // an own value over the scene's default shows "Revert to Scene Default"
        const bool overScene = chain.chain.sceneDefault && ShaderUniforms::has(*chain.chain.sceneDefault, property.member);
        const bool color = property.kind == ShaderPropertyKind::COLOR || property.kind == ShaderPropertyKind::HDRCOLOR;
        const int components = property.kind == ShaderPropertyKind::VECTOR2 ? 2
                             : property.kind == ShaderPropertyKind::VECTOR3 ? 3
                             : (property.kind == ShaderPropertyKind::VECTOR4 || color) ? 4 : 1;
        const std::string bound = bindingsRef ? ShaderGlobals::getBinding(bindings, property.member) : std::string();

        ImGui::PushID(("param_" + property.member).c_str());
        if (!bound.empty()) {
            // a bound parameter shows the global's live value; its own stays stored, unused
            prefabRowProperty = bindingsProperty;
            propertyHeader(rowLabel(property.label, property.member), -1, false);
            drawBoundGlobalValue(bound, components, color, ImGui::GetContentRegionAvail().x - getGlobalLinkButtonWidth() - ImGui::GetStyle().ItemSpacing.x);
            drawGlobalLinkButton(property.member, components, bindings, setBindings);
            ImGui::PopID();
            continue;
        }
        prefabRowProperty = paramsProperty;
        const bool revert = propertyHeader(rowLabel(property.label, property.member), -1, hasValue);
        if (hasValue && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", overScene ? "Revert to Scene Default" : "Reset to the shader's default");
        if (revert) {
            // back to what it inherits (the scene's default-shader value, the manifest default)
            ShaderUniformValues changed = params;
            ShaderUniforms::remove(changed, property.member);
            setParams(changed, false);
            ImGui::PopID();
            continue;
        }
        const std::string id = "##param_value_" + property.member;
        Vector4 edited = value;
        bool continuous = true;
        const float itemWidth = bindingsRef ? -(getGlobalLinkButtonWidth() + ImGui::GetStyle().ItemSpacing.x) : -1.0f;
        const bool changed = drawShaderPropertyValue(property, id, value, edited, continuous, itemWidth);
        if (changed) {
            ShaderUniformValues newParams = params;
            ShaderUniforms::set(newParams, property.member, edited);
            setParams(newParams, continuous);
        }
        finishDrag();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
            if (!hasValue && source == UniformSource::SceneDefault)
                ImGui::SetTooltip("%s\nFrom the scene's default shader values (Scene > Default Shaders)", property.member.c_str());
            else
                ImGui::SetTooltip("%s", property.member.c_str());
        }
        if (bindingsRef)
            drawGlobalLinkButton(property.member, components, bindings, setBindings);
        ImGui::PopID();
    }

    // ---- textures
    for (const ShaderManifestTexture& declared : manifest->textures) {
        if (!featureOn(declared.feature))
            continue;
        const std::string current = ShaderLibraryValues::getTexture(textures, declared.uniform);

        ImGui::PushID(("texture_" + declared.uniform).c_str());
        prefabRowProperty = texturesProperty;
        if (propertyHeader(rowLabel(declared.label, declared.uniform), -1, !current.empty())) {
            ShaderTextures changed = textures;
            ShaderLibraryValues::setTexture(changed, declared.uniform, "");
            setTextures(changed);
            ImGui::PopID();
            continue;
        }

        auto assign = [&](const std::string& absolutePath) {
            if (!project->isInsideAssetsPath(absolutePath)) {
                Backend::getApp().registerOutsideAssetsAlert(absolutePath);
                return;
            }
            ShaderTextures changed = textures;
            if (ShaderLibraryValues::setTexture(changed, declared.uniform, project->normalizeToAssetsRelative(absolutePath).generic_string()))
                setTextures(changed);
        };

        const float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        // the default the material's features pick (a @texture's default[<DEFINE>]=)
        const ShaderTextureDefault unsetDefault = (!declared.featureDefaultDefine.empty() && featureOn(declared.featureDefaultDefine))
                                                  ? declared.featureDefaultTexture : declared.defaultTexture;
        const std::string shown = current.empty() ? textureDefaultLabel(unsetDefault)
                                                  : std::filesystem::path(current).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, current.empty() ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] : ImGui::GetStyle().Colors[ImGuiCol_Text]);
        ImGui::Button((shown + "##texture_slot_" + declared.uniform).c_str(),
                      ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x), 0));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s\nDrop an image here", declared.uniform.c_str(), current.empty() ? "" : ": ", current.c_str());
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                std::vector<std::string> received = Util::getStringsFromPayload(payload);
                if (!received.empty() && Util::isImageFile(received[0]))
                    assign(received[0]);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button((std::string(ICON_FA_FOLDER_OPEN "##texture_pick_") + declared.uniform).c_str())) {
            const std::string selected = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
            if (!selected.empty())
                assign(selected);
        }
        ImGui::PopID();
    }

    // ---- palette bindings (S43 Palette Swap)
    if (paletteSlotOn)
        drawShaderPaletteRows(cpType, sceneProject, entity, shader, *manifest, palettePrefix, features, params, onValueChanged);
    prefabRowProperty.clear();

    // what this variant reads of the scene (phase 3) and the cost: a material's, its extra
    // pass's or a points component's
    sceneInputsInfoRow(Catalog::getShaderSceneInputs(shader, features));
    planarReflectionInfoRow(sceneProject->scene, shader, features);

    return true;
}

void editor::InspectorWindow::drawShaderPaletteRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::string& shader,
                                                    const ShaderManifest& manifest, const std::string& materialPrefix,
                                                    const std::vector<std::string>& features, const ShaderUniformValues& params,
                                                    const std::function<void()>& onValueChanged){
    namespace pxc = jasperengine::pixelart;
    namespace epx = jasperengine::editor::pixelart;
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const Material* material = Catalog::getPropertyRef<Material>(scene, entity, cpType, materialPrefix.substr(0, materialPrefix.size() - 1));
    if (!material)
        return;
    const std::string palettesProperty = materialPrefix + "shaderPalettes";
    const std::string paramsProperty = materialPrefix + "shaderParams";
    // copies: a command may change them while the rows are drawn
    const pxc::ShaderPalettes palettes = material->shaderPalettes.get();
    const Texture baseTexture = material->baseColorTexture;
    const std::string texture = baseTexture.isFramebuffer() ? std::string() : baseTexture.getPath();
    const std::vector<std::string> enabled = manifest.resolveFeatures(features);
    const ImGuiStyle& style = ImGui::GetStyle();

    auto setPalettes = [&](const pxc::ShaderPalettes& value, bool wroteCopy) {
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<pxc::ShaderPalettes>(project, sceneId, entity, cpType, palettesProperty, value, onValueChanged));
        // the built-in's copy and the binding that uses it: one undo step
        if (wroteCopy)
            epx::linkPaletteCopy(*project, sceneId);
    };
    auto setParams = [&](const ShaderUniformValues& value, bool continuous) {
        Command* paramsCmd = new PropertyCmd<ShaderUniformValues>(project, sceneId, entity, cpType, paramsProperty, value, onValueChanged);
        if (continuous) {
            cmd = paramsCmd;
            CommandHandle::get(sceneId)->addCommand(paramsCmd);
        } else {
            CommandHandle::get(sceneId)->addCommandNoMerge(paramsCmd);
        }
    };
    // a palette field's pick as stored: an assets-relative path ("" for none); a built-in is
    // copied into the project first
    auto resolvePick = [&](const std::string& picked, std::string& stored, bool& wroteCopy) {
        wroteCopy = false;
        stored.clear();
        if (picked.empty())
            return true;
        epx::ResolvedPalette resolved;
        std::string error;
        if (!epx::usePalette(*project, picked, resolved, wroteCopy, error)) {
            Out::warning("Palette Swap: %s", error.c_str());
            return false;
        }
        stored = resolved.assetPath;
        return true;
    };

    std::map<std::string, PaletteSlotState>& states = paletteSlotStates();
    if (states.size() > 64)
        states.clear();
    const std::string materialKey = paletteMaterialKey(sceneId, entity, materialPrefix);
    bool paramsDrawn = false;

    for (const ShaderManifestPalette& slot : manifest.palettes) {
        if (!slot.isEnabled(enabled))
            continue;
        const std::string& id = slot.uniform;
        ImGui::PushID(("palette_" + id).c_str());
        PaletteSlotState& state = states[materialKey + "/" + id];
        pxc::PaletteBinding binding;
        for (const auto& entry : palettes) {
            if (entry.first == id) {
                binding = entry.second;
                break;
            }
        }
        const std::string suffix = manifest.palettes.size() > 1 ? " (" + rowLabel(slot.label, id) + ")" : std::string();
        std::shared_ptr<const pxc::Palette> source = binding.source.empty() ? nullptr : pxc::PaletteLibrary::get(binding.source.path);
        auto setBinding = [&](const pxc::PaletteBinding& value, bool wroteCopy) {
            setPalettes(withPaletteBinding(palettes, id, value), wroteCopy);
        };

        // ---- Detect (R18): a worker reads the texture against every project palette
        auto startDetect = [&](bool automatic) {
            if (texture.empty() || state.detecting)
                return;
            std::vector<epx::ProjectPalette> candidates = epx::listProjectPalettes(*project);
            const std::filesystem::path file = project->getAssetsPath() / FileUtils::pathFromUtf8(texture);
            jobs::SubmitOptions options;
            options.lane = jobs::Lane::Background;
            options.name = "palette-detect";
            state.detect = jobs::submit([file, candidates]() {
                TextureFindings found;
                std::optional<epx::RgbaImage> image = epx::loadRgbaImage(file, &found.error);
                if (!image)
                    return found;
                found.loaded = true;
                found.width = image->width;
                found.height = image->height;
                found.covering = epx::palettesCoveringImage(*image, candidates);
                return found;
            }, options);
            state.detecting = true;
            state.detectAuto = automatic;
            state.detectTexture = texture;
            state.detectStatus.clear();
            state.detectChoices.clear();
        };
        if (paletteAutoDetects().erase(materialKey) && binding.source.empty())
            startDetect(true);
        if (state.detecting && state.detect.isReady()) {
            state.detecting = false;
            const TextureFindings found = takeFindings(state.detect);
            const std::string name = paletteFileName(state.detectTexture);
            if (!found.loaded) {
                state.detectStatus = "Cannot read " + name + (found.error.empty() ? std::string() : ": " + found.error);
            } else if (found.covering.size() == 1) {
                state.detectStatus = paletteFileName(found.covering[0]) + " holds every colour of " + name;
                if (!state.detectAuto || binding.source.empty()) {
                    pxc::PaletteBinding changed = binding;
                    changed.source = found.covering[0];
                    setBinding(changed, false);
                }
            } else if (found.covering.empty()) {
                state.detectStatus = "No project palette holds every colour of " + name + " (Extract Palette in the Project window makes one)";
            } else {
                state.detectStatus = std::to_string(found.covering.size()) + " project palettes hold every colour of " + name + ": pick one";
                state.detectChoices = found.covering;
            }
            // an automatic Detect that found nothing to fill stays quiet
            if (state.detectAuto && found.covering.size() != 1)
                state.detectStatus.clear();
        }

        // ---- Source Palette
        prefabRowProperty = palettesProperty;
        if (propertyHeader("Source Palette" + suffix, -1, !binding.source.empty())) {
            pxc::PaletteBinding changed = binding;
            changed.source.clear();
            setBinding(changed, false);
        }
        const float detectWidth = ImGui::CalcTextSize("Detect").x + style.FramePadding.x * 2.0f;
        std::string picked;
        if (paletteAssetField(("##palette_source_" + id).c_str(), *project, binding.source.path, picked, true,
                              -(detectWidth + style.ItemSpacing.x))) {
            std::string stored;
            bool wroteCopy = false;
            if (resolvePick(picked, stored, wroteCopy)) {
                pxc::PaletteBinding changed = binding;
                changed.source = stored;
                setBinding(changed, wroteCopy);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(texture.empty() || state.detecting);
        if (ImGui::Button(("Detect##palette_detect_" + id).c_str()))
            startDetect(false);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", texture.empty() ? "The material has no Base Texture to detect a palette from"
                                    : "Fill Source with the one project palette that holds every visible colour of the Base Texture");
        if (state.detecting || !state.detectStatus.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            if (state.detecting)
                ImGui::TextDisabled("Detecting the palette of %s...", paletteFileName(state.detectTexture).c_str());
            else
                ImGui::TextDisabled("%s", state.detectStatus.c_str());
            ImGui::PopTextWrapPos();
            if (!state.detectChoices.empty() && ImGui::IsItemHovered()) {
                std::string choices;
                for (const std::string& choice : state.detectChoices) choices += (choices.empty() ? "" : "\n") + choice;
                ImGui::SetTooltip("%s", choices.c_str());
            }
        }

        // ---- Use Variants
        bool useVariants = binding.useVariants;
        const bool resetVariants = propertyHeader("Use Variants" + suffix, -1, !binding.useVariants);
        if (ImGui::Checkbox(("##palette_use_variants_" + id).c_str(), &useVariants) || resetVariants) {
            if (resetVariants)
                useVariants = true;
            pxc::PaletteBinding changed = binding;
            changed.useVariants = useVariants;
            setBinding(changed, false);
        }
        const int sourceVariants = source ? (int)source->variants.size() : 0;
        ImGui::SameLine();
        ImGui::TextDisabled("%d %s", sourceVariants, sourceVariants == 1 ? "variant" : "variants");
        ImGui::SetItemTooltip("Rows 1 and on are the Source's variants (team colours, made with Create Variant in the Palette window), "
                              "then the Extra Targets");

        // ---- Extra Targets: a reorderable list (drag the grip; its context menu moves and removes)
        const int variantRows = (binding.useVariants && source) ? sourceVariants : 0;
        const int maxRows = pxc::kSwapTargetRows - 1;
        if (propertyHeader("Extra Targets" + suffix, -1, !binding.targets.empty())) {
            pxc::PaletteBinding changed = binding;
            changed.targets.clear();
            setBinding(changed, false);
        }
        ImGui::BeginDisabled(binding.source.empty() || variantRows + (int)binding.targets.size() >= maxRows);
        if (ImGui::Button((std::string(ICON_FA_PLUS " Add Target##palette_target_add_") + id).c_str())) {
            // a new row starts as the source's own colours: pick the palette or variant it shows
            pxc::PaletteBinding changed = binding;
            pxc::PaletteTarget target;
            target.palette = binding.source;
            changed.targets.push_back(target);
            setBinding(changed, false);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("A row of another palette's colours by index, or one of its variants (%d rows at most, the variants "
                              "included)", maxRows);
        int moveFrom = -1, moveTo = -1, removeAt = -1;
        for (size_t i = 0; i < binding.targets.size(); i++) {
            const pxc::PaletteTarget& target = binding.targets[i];
            ImGui::PushID((int)i);
            propertyHeader("Row " + std::to_string(1 + variantRows + (int)i), -1, false, false, true);
            ImGui::Button(ICON_FA_GRIP_VERTICAL "##palette_target_grip");
            if (ImGui::BeginDragDropSource()) {
                const int from = (int)i;
                ImGui::SetDragDropPayload("palette_target_row", &from, sizeof(from));
                ImGui::Text("Row %d: %s", 1 + variantRows + (int)i, target.text().c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("palette_target_row")) {
                    moveFrom = *(const int*)payload->Data;
                    moveTo = (int)i;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SetItemTooltip("Drag to reorder; right-click to move or remove");
            if (ImGui::BeginPopupContextItem("##palette_target_menu")) {
                if (ImGui::MenuItem("Move Up", nullptr, false, i > 0)) {
                    moveFrom = (int)i;
                    moveTo = (int)i - 1;
                }
                if (ImGui::MenuItem("Move Down", nullptr, false, i + 1 < binding.targets.size())) {
                    moveFrom = (int)i;
                    moveTo = (int)i + 1;
                }
                if (ImGui::MenuItem("Remove"))
                    removeAt = (int)i;
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            const float removeWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x + style.FramePadding.x * 2.0f;
            const float variantWidth = std::max(ImGui::GetFontSize() * 4.0f, (ImGui::GetContentRegionAvail().x - removeWidth) * 0.38f);
            std::string targetPick;
            if (paletteAssetField("##palette_target_palette", *project, target.palette.path, targetPick, false,
                                  -(variantWidth + removeWidth + style.ItemSpacing.x * 2.0f))) {
                std::string stored;
                bool wroteCopy = false;
                if (resolvePick(targetPick, stored, wroteCopy) && !stored.empty()) {
                    pxc::PaletteBinding changed = binding;
                    changed.targets[i].palette = stored;
                    changed.targets[i].variant.clear();
                    setBinding(changed, wroteCopy);
                }
            }
            ImGui::SameLine();
            std::shared_ptr<const pxc::Palette> targetPalette = target.palette.empty() ? nullptr : pxc::PaletteLibrary::get(target.palette.path);
            ImGui::SetNextItemWidth(variantWidth);
            if (ImGui::BeginCombo("##palette_target_variant", target.variant.empty() ? "By index" : target.variant.c_str())) {
                auto pickVariant = [&](const std::string& variant) {
                    pxc::PaletteBinding changed = binding;
                    changed.targets[i].variant = variant;
                    setBinding(changed, false);
                };
                if (ImGui::Selectable("By index##palette_variant_none", target.variant.empty()) && !target.variant.empty())
                    pickVariant(std::string());
                ImGui::SetItemTooltip("The palette's own colours, index by index");
                if (targetPalette) {
                    for (const pxc::PaletteVariant& variant : targetPalette->variants) {
                        if (ImGui::Selectable((variant.name + "##palette_variant_" + variant.name).c_str(), variant.name == target.variant) &&
                            variant.name != target.variant)
                            pickVariant(variant.name);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("The colours of the row: the palette's by index, or one of its variants");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK "##palette_target_remove"))
                removeAt = (int)i;
            ImGui::SetItemTooltip("Remove this row");
            ImGui::PopID();
        }
        if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveFrom < (int)binding.targets.size()) {
            pxc::PaletteBinding changed = binding;
            const pxc::PaletteTarget moved = changed.targets[(size_t)moveFrom];
            changed.targets.erase(changed.targets.begin() + moveFrom);
            changed.targets.insert(changed.targets.begin() + moveTo, moved);
            setBinding(changed, false);
        } else if (removeAt >= 0) {
            pxc::PaletteBinding changed = binding;
            changed.targets.erase(changed.targets.begin() + removeAt);
            setBinding(changed, false);
        }

        // the swap textures as the renderer builds them, and the rows they name
        const std::string buildKey = paletteBindingKey(binding);
        if (buildKey != state.buildKey) {
            state.buildKey = buildKey;
            state.diagnostics = epx::paletteSwapDiagnostics(binding);
            state.rowNames = pxc::paletteRowNames(*material, id);
        }

        // ---- Active Palette and Cycling Speed: the material's paletteRow and paletteCycleSpeed
        // (uniform values: the Scene view shows a change at once)
        if (!paramsDrawn) {
            paramsDrawn = true;
            const ShaderManifestProperty* rowProperty = manifest.findProperty(pxc::kPaletteRowParam);
            if (rowProperty && ShaderManifest::isRowShown(rowProperty->feature, enabled)) {
                const bool hasRow = ShaderUniforms::has(params, pxc::kPaletteRowParam);
                const float rowValue = hasRow ? ShaderUniforms::get(params, pxc::kPaletteRowParam).x : rowProperty->defaultValue.x;
                const int row = std::clamp((int)std::floor(rowValue + 0.5f), 0, pxc::kSwapTargetRows - 1);
                auto rowName = [&](int index) {
                    if (index == 0)
                        return std::string("Original");
                    if (index < (int)state.rowNames.size())
                        return state.rowNames[(size_t)index].empty() ? "Row " + std::to_string(index) : state.rowNames[(size_t)index];
                    return "Row " + std::to_string(index) + " (not defined: shows the last row)";
                };
                prefabRowProperty = paramsProperty;
                if (propertyHeader("Active Palette", -1, hasRow)) {
                    ShaderUniformValues changed = params;
                    ShaderUniforms::remove(changed, pxc::kPaletteRowParam);
                    setParams(changed, false);
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo(("##palette_row_" + id).c_str(), rowName(row).c_str())) {
                    for (int index = 0; index < (int)state.rowNames.size(); index++) {
                        const std::string label = rowName(index) + "##palette_row_item_" + std::to_string(index);
                        if (ImGui::Selectable(label.c_str(), index == row) && index != row) {
                            ShaderUniformValues changed = params;
                            ShaderUniforms::set(changed, pxc::kPaletteRowParam, Vector4((float)index, 0.0f, 0.0f, 0.0f));
                            setParams(changed, false);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("The row the sprite draws with (paletteRow; Lua sprite.palette): Original, the Source's "
                                      "variants, then the Extra Targets");
            }
            const ShaderManifestProperty* speedProperty = manifest.findProperty(pxc::kPaletteCycleSpeedParam);
            if (speedProperty && ShaderManifest::isRowShown(speedProperty->feature, enabled)) {
                const bool hasSpeed = ShaderUniforms::has(params, pxc::kPaletteCycleSpeedParam);
                float speed = hasSpeed ? ShaderUniforms::get(params, pxc::kPaletteCycleSpeedParam).x : speedProperty->defaultValue.x;
                prefabRowProperty = paramsProperty;
                if (propertyHeader("Cycling Speed", -1, hasSpeed)) {
                    ShaderUniformValues changed = params;
                    ShaderUniforms::remove(changed, pxc::kPaletteCycleSpeedParam);
                    setParams(changed, false);
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat(("##palette_cycle_speed_" + id).c_str(), &speed, 0.01f, 0.0f, 0.0f, "%.2f")) {
                    ShaderUniformValues changed = params;
                    ShaderUniforms::set(changed, pxc::kPaletteCycleSpeedParam, Vector4(std::isfinite(speed) ? speed : 0.0f, 0.0f, 0.0f, 0.0f));
                    setParams(changed, true);
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
                    cmd->setNoMerge();
                    cmd = nullptr;
                }
                ImGui::SetItemTooltip("How fast the Source's cycling ramps turn (paletteCycleSpeed): 1 as authored, negative "
                                      "reverses, 0 freezes");
            }
        }

        // ---- warnings: the binding's build errors (R14)
        for (const pxc::Diagnostic& diagnostic : state.diagnostics) {
            const bool error = diagnostic.severity == pxc::Diagnostic::Severity::Error;
            paletteWarningRow(error ? Theme::Colors::ErrorText : Theme::Colors::WarningText, diagnostic.message,
                              diagnostic.fix.empty() ? diagnostic.kind : diagnostic.kind + ": " + diagnostic.fix);
        }

        if (!texture.empty()) {
            // the texture against the source, read on a worker (decoding a large sheet is not a
            // frame's work); rerun when the image, the binding or the palette's file change
            const std::filesystem::path file = project->getAssetsPath() / FileUtils::pathFromUtf8(texture);
            const std::string analysisKey = FileUtils::pathToUtf8(file) + "|" + paletteFileStamp(file) + "|" + binding.source.path + "|" +
                                            std::to_string(binding.source.empty() ? 0u : pxc::PaletteLibrary::version(binding.source.path));
            if (analysisKey != state.analysisKey) {
                state.analysisKey = analysisKey;
                state.analysisReady = false;
                state.found = TextureFindings();
                state.analysis.reset();
                if (source) {
                    jobs::SubmitOptions options;
                    options.lane = jobs::Lane::Background;
                    options.name = "palette-swap-usage";
                    state.analysis = jobs::submit([file, source]() {
                        TextureFindings found;
                        std::optional<epx::RgbaImage> image = epx::loadRgbaImage(file, &found.error);
                        if (!image)
                            return found;
                        found.loaded = true;
                        found.width = image->width;
                        found.height = image->height;
                        found.offColours = epx::paletteSwapOffPaletteColours(*image, *source);
                        return found;
                    }, options);
                }
            }
            if (state.analysis.isValid() && state.analysis.isReady()) {
                state.found = takeFindings(state.analysis);
                state.analysisReady = true;
            }

            // ---- R15: lossy import settings or a filtered sampler, rechecked each second (a
            // change in the Texture Importer, undo)
            const std::string checkKey = texture + "|" + std::to_string((int)baseTexture.getMinFilter()) + "|" +
                                         std::to_string((int)baseTexture.getMagFilter()) + "|" + std::to_string(state.found.width) + "x" +
                                         std::to_string(state.found.height);
            const double now = ImGui::GetTime();
            if (checkKey != state.checkKey || now - state.checkTime > 1.0) {
                state.checkKey = checkKey;
                state.checkTime = now;
                state.check = epx::checkPaletteSwapTexture(*project, *material, shader, state.found.width, state.found.height);
            }
            if (state.check.lossy()) {
                paletteWarningRow(Theme::Colors::WarningText, state.check.message(),
                                  "Palette Swap matches exact colours: compression, mipmaps, a resize or filtering change them, "
                                  "and a changed colour draws unswapped.");
                // the project's Texture Strategy is the one reason a texture's settings cannot remove
                const bool fixable = state.check.reasons.size() > (state.check.projectResize ? 1u : 0u);
                ImGui::BeginDisabled(!fixable);
                epx::PaletteSwapTextureFix fix;
                if (ImGui::Button(("Fix##palette_lossless_fix_" + id).c_str()) &&
                    epx::planPaletteSwapTextureFix(*project, *material, state.check, fix)) {
                    CommandHistory* projectHistory = project->getProjectCommandHistory();
                    bool importWritten = false;
                    bool samplerWritten = false;
                    if (fix.importChanged) {
                        Command* meta = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(project->getAssetsPath() / FileUtils::pathFromUtf8(fix.texture)),
                                                            TextureImportSettings::metaKey, fix.importSettings);
                        meta->setNoMerge();
                        importWritten = projectHistory->addCommand(meta);
                    }
                    if (fix.samplerChanged)
                        samplerWritten = CommandHandle::get(sceneId)->addCommandNoMerge(
                            new PropertyCmd<Texture>(project, sceneId, entity, cpType, materialPrefix + "baseColorTexture", fix.sampler, onValueChanged));
                    // the import settings (project history) and the sampler (the scene's): one undo step
                    if (importWritten && samplerWritten)
                        UndoTimeline::link({projectHistory, CommandHandle::get(sceneId)});
                    state.checkKey.clear();
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("%s", state.check.projectResize
                    ? "Lossless, no mipmaps, a Max Size that keeps the image and Nearest filtering, in one undo step. The project's "
                      "Texture Strategy (Project Settings > Graphics) still resizes it."
                    : "Lossless, no mipmaps, a Max Size that keeps the image and Nearest filtering, in one undo step");
            }

            // ---- the colours that will not swap
            if (state.analysisReady && state.found.offColours > 0) {
                const long long count = (long long)state.found.offColours;
                paletteWarningRow(Theme::Colors::WarningText,
                                  count == 1 ? std::string("1 opaque colour of this sprite is not in the source palette (it will not swap)")
                                             : std::to_string(count) + " opaque colours of this sprite are not in the source palette (they will not swap)",
                                  "Anti-aliased, off-palette and filtered pixels draw unchanged. Show lists them in the Palette window's Usage.");
                if (ImGui::Button(("Show##palette_offpalette_show_" + id).c_str())) {
                    const std::string palettePath = FileUtils::pathToGenericUtf8(
                        project->normalizeToProjectRelative(project->getAssetsPath() / FileUtils::pathFromUtf8(binding.source.path)));
                    const std::string texturePath = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file));
                    getEditorHost().openPaletteWindow(palettePath, texturePath);
                }
                ImGui::SetItemTooltip("Open the Source in the Palette window with this texture's usage");
            } else if (state.analysisReady && !state.found.loaded && !state.found.error.empty()) {
                paletteWarningRow(Theme::Colors::WarningText, "Cannot read " + paletteFileName(texture) + ": " + state.found.error, std::string());
            }
        }
        ImGui::PopID();
    }
    prefabRowProperty.clear();
}

bool editor::InspectorWindow::drawMaterialShaderRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                     unsigned int submesh, const RowSettings& settingsMaterial){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material.";
    const std::string shaderProperty = prefix + "shader";
    std::string* shaderRef = Catalog::getPropertyRef<std::string>(scene, entities[0], cpType, shaderProperty);
    if (!shaderRef)
        return true;
    const std::string currentShader = *shaderRef;
    bool mixed = false;
    for (Entity entity : entities) {
        std::string* other = Catalog::getPropertyRef<std::string>(scene, entity, cpType, shaderProperty);
        mixed = mixed || !other || *other != currentShader;
    }

    // the same shader on every selected submesh, one undo step; a lighting mode the shader
    // the submesh then draws with does not list goes back to Auto (its combo lists only those)
    auto setShader = [&](const std::string& value) {
        auto* multi = new MultiPropertyCmd();
        for (Entity entity : entities) {
            multi->addPropertyCmd<std::string>(project, sceneId, entity, cpType, shaderProperty, value, settingsMaterial.onValueChanged);
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh || submesh >= mesh->numSubmeshes)
                continue;
            Submesh drawn = mesh->submeshes[submesh];
            drawn.material.shader = value;
            const std::string shader = RenderSystem::getSubmeshShader(*mesh, drawn, scene->getDefaultCustomShader(ShaderType::MESH));
            std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
            if (!Catalog::supportsLighting(drawn.material.lighting, manifest.get()))
                multi->addPropertyCmd<int>(project, sceneId, entity, cpType, prefix + "lighting", (int)MaterialLighting::AUTO,
                                           settingsMaterial.onValueChanged);
        }
        CommandHandle::get(sceneId)->addCommandNoMerge(multi);
    };

    const std::string popupName = "Edit Shader Files##material_shader_" + std::to_string(submesh);
    prefabRowProperty = shaderProperty;
    propertyHeader("Shader", -1, false, true);
    if (mixed)
        ImGui::TextDisabled("(different shaders)");
    // a fork of the library shader in one step: the files, and every selected submesh on it
    auto onForkLibrary = [this, sceneId, entities, cpType, shaderProperty, onValueChanged = settingsMaterial.onValueChanged](const std::string& library){
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(library);
        const std::string defaultName = (manifest && !manifest->name.empty()) ? manifest->name : ShaderReference::getLibraryName(library);
        shaderForkDialog.open(project, ShaderType::MESH, defaultName,
            [this, sceneId, entities, cpType, shaderProperty, onValueChanged, library](const std::filesystem::path& directory, const std::string& name, bool forkIncludes){
                auto makePropertyCmd = [this, sceneId, entities, cpType, shaderProperty, onValueChanged](const std::string& forkBase){
                    auto multi = std::make_unique<MultiPropertyCmd>();
                    for (Entity entity : entities)
                        multi->addPropertyCmd<std::string>(project, sceneId, entity, cpType, shaderProperty, forkBase, onValueChanged);
                    return std::unique_ptr<Command>(std::move(multi));
                };
                commitShaderFork(sceneId, std::make_unique<ForkShaderCmd>(project, ShaderType::MESH, makePropertyCmd, directory, name,
                                                                         forkIncludes, library));
            }, nullptr, library);
    };
    drawLibraryShaderCombo(mixed ? std::string() : currentShader, ShaderType::MESH, setShader, popupName, nullptr, onForkLibrary);
    const Util::CustomShaderPaths paths = (!currentShader.empty() && !ShaderReference::isLibrary(currentShader))
        ? project->resolveCustomShaderFiles(currentShader) : Util::CustomShaderPaths();
    drawShaderFilesPopup(popupName, setShader, paths.vert, paths.frag);
    prefabRowProperty.clear();

    if (mixed)
        return false;

    // the shader this submesh draws with (the mesh's or the scene's when the material has none)
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entities[0]);
    const std::string drawnShader = RenderSystem::getSubmeshShader(mesh, mesh.submeshes[submesh], scene->getDefaultCustomShader(ShaderType::MESH));
    std::shared_ptr<const ShaderManifest> manifest = drawnShader.empty() ? nullptr : ShaderManifests::get(drawnShader);
    if (manifest && manifest->present) {
        if (entities.size() == 1) {
            drawShaderManifestRows(cpType, sceneProject, entities[0], drawnShader, prefix + "shaderParams", prefix + "shaderTextures",
                                   prefix + "shaderFeatures", settingsMaterial.onValueChanged);
        } else {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Select one GameObject to edit the shader's values");
        }
    } else if (ShaderReference::isLibrary(drawnShader) && !manifest) {
        // a reference the engine does not have (renamed, mistyped): it draws with the built-in one
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s is not a library shader of this engine", drawnShader.c_str());
    }
    // a library shader has its own parameters; the built-in Lit and forks of it read the PBR ones
    return !ShaderReference::isLibrary(drawnShader);
}

void editor::InspectorWindow::drawMaterialSurfaceRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                      unsigned int submesh, const RowSettings& settingsMaterial, bool litRows){
    Scene* scene = sceneProject->scene;
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material.";
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entities[0]);
    if (!mesh || submesh >= mesh->numSubmeshes)
        return;
    const Submesh& sub = mesh->submeshes[submesh];
    const std::string drawnShader = RenderSystem::getSubmeshShader(*mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH));
    std::shared_ptr<const ShaderManifest> manifest = drawnShader.empty() ? nullptr : ShaderManifests::get(drawnShader);
    const ShaderManifest* stateManifest = (manifest && manifest->present) ? manifest.get() : nullptr;

    // what Auto resolves to now: the manifest's @state, else the engine's behaviour
    Material autoMaterial = sub.material;
    autoMaterial.blendMode = MaterialBlendMode::AUTO;
    autoMaterial.cullMode = MaterialCullMode::AUTO;
    autoMaterial.depthWrite = MaterialDepthWrite::AUTO;
    autoMaterial.depthTest = MaterialDepthTest::AUTO;
    Submesh autoSubmesh;
    autoSubmesh.material = autoMaterial;
    autoSubmesh.faceCulling = sub.faceCulling;
    bool blended = false;
    bool transparentQueue = false;
    const RenderState resolved = RenderSystem::resolveSubmeshRenderState(*mesh, autoSubmesh, stateManifest, blended, transparentQueue);

    auto withAuto = [](AutoEnumEntries& entries, const std::string& resolvedLabel, std::initializer_list<EnumEntry> others) {
        entries.autoLabel = "Auto (" + resolvedLabel + ")";
        entries.entries.push_back({0, entries.autoLabel.c_str()});
        for (const EnumEntry& entry : others) entries.entries.push_back(entry);
    };

    AutoEnumEntries blend, cull, depthWrite, depthTest, lighting;
    withAuto(blend, blended ? blendLabel(resolved.blend) : "Opaque", {
        {(int)MaterialBlendMode::NONE, "Opaque"}, {(int)MaterialBlendMode::ALPHA, "Alpha"},
        {(int)MaterialBlendMode::PREMULTIPLIED, "Premultiplied"}, {(int)MaterialBlendMode::ADDITIVE, "Additive"},
        {(int)MaterialBlendMode::MULTIPLY, "Multiply"}});
    withAuto(cull, !resolved.faceCulling ? "Off" : (resolved.cullingMode == CullingMode::FRONT ? "Front" : "Back"), {
        {(int)MaterialCullMode::BACK, "Back"}, {(int)MaterialCullMode::FRONT, "Front"}, {(int)MaterialCullMode::OFF, "Off"}});
    withAuto(depthWrite, resolved.depthWrite ? "On" : "Off", {
        {(int)MaterialDepthWrite::ENABLED, "On"}, {(int)MaterialDepthWrite::DISABLED, "Off"}});
    withAuto(depthTest, depthCompareLabel(resolved.depthCompare), {
        {(int)MaterialDepthTest::LESS, "Less"}, {(int)MaterialDepthTest::LESS_EQUAL, "Less Equal"},
        {(int)MaterialDepthTest::EQUAL, "Equal"}, {(int)MaterialDepthTest::GREATER_EQUAL, "Greater Equal"},
        {(int)MaterialDepthTest::GREATER, "Greater"}, {(int)MaterialDepthTest::NOT_EQUAL, "Not Equal"},
        {(int)MaterialDepthTest::ALWAYS, "Always"}});

    // only the lighting modes the shader lists (the built-in Lit: per pixel, per vertex,
    // mixed, none), its default first: what Auto draws with
    const std::vector<MaterialLighting> modes = Catalog::getShaderLightingModes(stateManifest);
    lighting.autoLabel = std::string("Auto (") + lightingLabel(modes.front()) + ")";
    lighting.entries.push_back({(int)MaterialLighting::AUTO, lighting.autoLabel.c_str()});
    for (MaterialLighting mode : modes)
        lighting.entries.push_back({(int)mode, lightingLabel(mode)});

    RowSettings settingsEnum = settingsMaterial;
    RowSettings settingsFloat = settingsMaterial;
    settingsFloat.stepSize = 0.01f;
    RowSettings settingsInt = settingsMaterial;

    beginTable(cpType, getLabelSize("Depth Offset Units"), "submeshes_material_surface");
    settingsEnum.enumEntries = &blend.entries;
    settingsEnum.help = "How the colour mixes with what is behind. Anything but Opaque draws in the transparent queue, sorted by Sort Priority, then far to near.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "blendMode", "Blend Mode", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &cull.entries;
    settingsEnum.help = "Which faces are left out. Auto follows the shader, else the submesh's Face Culling and the mesh's Culling Mode.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "cullMode", "Cull", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &depthWrite.entries;
    settingsEnum.help = "Whether it writes depth. Auto: on when opaque, off when blended.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "depthWrite", "Depth Write", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &depthTest.entries;
    settingsEnum.help = "The depth comparison that keeps a fragment. A camera with depth test off draws it always.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "depthTest", "Depth Test", sceneProject, entities, settingsEnum);
    settingsFloat.help = "Polygon offset slope factor; negative pulls the surface towards the camera (decals). 0 with Units 0 uses the shader's.";
    propertyRow(RowPropertyType::Float, cpType, prefix + "depthOffsetFactor", "Depth Offset Factor", sceneProject, entities, settingsFloat);
    settingsFloat.help = "Polygon offset constant, in depth units.";
    propertyRow(RowPropertyType::Float, cpType, prefix + "depthOffsetUnits", "Depth Offset Units", sceneProject, entities, settingsFloat);
    settingsInt.help = "Draw order within its queue, lower first: opaque objects by it, then near to far; transparent ones by it, then far to near (masks before their readers, see-through effects after their occluders).";
    propertyRow(RowPropertyType::Int, cpType, prefix + "sortPriority", "Sort Priority", sceneProject, entities, settingsInt);

    settingsEnum.enumEntries = &lighting.entries;
    settingsEnum.help = "Which stage evaluates the lights: Per Pixel (every light per pixel, specular and shadows), "
                        "Per Vertex (diffuse only, cheapest), Mixed (each light's Shading decides), None (unlit). "
                        "Only the modes this shader supports are listed; Auto is its default.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "lighting", "Lighting", sceneProject, entities, settingsEnum);
    if (litRows)
        drawMaterialShadingModelRows(cpType, sceneProject, entities, submesh, settingsMaterial);

    // stencil and colour write (phase 2): Auto names the shader's rule
    drawStencilColorWriteRows(cpType, sceneProject, entities, prefix, [scene, submesh](Entity entity) {
        return resolveMaterialStencilAuto(scene, entity, submesh);
    }, settingsMaterial);

    // a shader reading the scene (phase 3) drawn before the scene is: an error, with a button
    // moving it to the transparent queue (Catalog::getSceneInputQueueFix: Alpha blending, or the
    // mesh's Transparent when its Auto Transparency is off; one undo step for every selected
    // GameObject the same fix applies to)
    bool opaqueQueue = false;
    const uint32_t sceneInputs = RenderSystem::getSubmeshSceneInputs(*mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH), opaqueQueue);
    if (sceneInputs && opaqueQueue){
        const Catalog::SceneInputQueueFix fix = Catalog::getSceneInputQueueFix(scene, entities[0], submesh);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("Error: reads the scene %s but draws in the opaque queue", Catalog::getSceneInputsLabel(sceneInputs).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()){
            std::string advice = Catalog::getSceneInputQueueAdvice(fix, submesh);
            if (!advice.empty()) advice[0] = (char)std::toupper((unsigned char)advice[0]);
            ImGui::SetTooltip("It would read its own depth and a black scene colour. %s.", advice.c_str());
        }
        if (fix != Catalog::SceneInputQueueFix::NONE) {
            const bool alpha = fix == Catalog::SceneInputQueueFix::ALPHA_BLEND;
            const char* label = alpha ? ICON_FA_WRENCH " Use Alpha blending##scene_input_fix_" : ICON_FA_WRENCH " Make the mesh transparent##scene_input_fix_";
            if (ImGui::Button((std::string(label) + std::to_string(submesh)).c_str())) {
                const uint32_t sceneId = sceneProject->id;
                auto* multi = new MultiPropertyCmd();
                bool changed = false;
                for (Entity entity : entities) {
                    if (Catalog::getSceneInputQueueFix(scene, entity, submesh) != fix)
                        continue;
                    if (alpha)
                        multi->addPropertyCmd<int>(project, sceneId, entity, cpType, prefix + "blendMode", (int)MaterialBlendMode::ALPHA, settingsMaterial.onValueChanged);
                    else
                        multi->addPropertyCmd<bool>(project, sceneId, entity, ComponentType::MeshComponent, "transparent", true);
                    changed = true;
                }
                if (changed)
                    CommandHandle::get(sceneId)->addCommandNoMerge(multi);
                else
                    delete multi;
            }
            if (ImGui::IsItemHovered()){
                if (alpha)
                    ImGui::SetTooltip("Set Blend Mode to Alpha: the submesh draws in the transparent queue, after the scene it reads (undoable)");
                else
                    ImGui::SetTooltip("Turn the mesh's Transparent on (its Auto Transparency is off, so the blend does not move it): "
                                      "the mesh draws in the transparent queue, after the scene it reads (undoable)");
            }
        }
    }

    // @state shadows=off (or a @state[...] rule of its features): the mesh's Cast Shadows does
    // not bring this submesh into the shadow maps
    if (stateManifest && stateManifest->resolveState(stateManifest->resolveFeatures(sub.material.shaderFeatures)).shadows == ShaderShadowCasting::OFF){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Casts no shadows (the shader's @state)");
    }
    endTable();
}

void editor::InspectorWindow::drawComponentStateRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    if (!Catalog::findProperty(sceneProject->scene, entities[0], cpType, "blendMode").ref)
        return;

    // what Auto resolves to with the shader the component draws with (its @state, with its
    // features' @state[<DEFINE>] rules, else the component's legacy state: alpha blending,
    // depth write on), as RenderSystem loads it
    Scene* scene = sceneProject->scene;
    const RenderState resolved = resolveComponentAuto(scene, entities[0], cpType);

    AutoEnumEntries blendEntries, depthWriteEntries;
    blendEntries.autoLabel = std::string("Auto (") + blendLabel(resolved.blend) + ")";
    blendEntries.entries = {
        {(int)MaterialBlendMode::AUTO, blendEntries.autoLabel.c_str()}, {(int)MaterialBlendMode::NONE, "Opaque"},
        {(int)MaterialBlendMode::ALPHA, "Alpha"}, {(int)MaterialBlendMode::PREMULTIPLIED, "Premultiplied"},
        {(int)MaterialBlendMode::ADDITIVE, "Additive"}, {(int)MaterialBlendMode::MULTIPLY, "Multiply"}};
    depthWriteEntries.autoLabel = resolved.depthWrite ? "Auto (On)" : "Auto (Off)";
    depthWriteEntries.entries = {
        {(int)MaterialDepthWrite::AUTO, depthWriteEntries.autoLabel.c_str()}, {(int)MaterialDepthWrite::ENABLED, "On"},
        {(int)MaterialDepthWrite::DISABLED, "Off"}};

    RowSettings settings;
    settings.enumEntries = &blendEntries.entries;
    settings.help = "How the colour mixes with what is behind. Auto: the shader's @state, else alpha blending.";
    propertyRow(RowPropertyType::Enum, cpType, "blendMode", "Blend Mode", sceneProject, entities, settings);
    settings.enumEntries = &depthWriteEntries.entries;
    settings.help = cpType == ComponentType::PointsComponent ?
        "Whether it writes depth. Auto: the shader's @state, else on when opaque and off when Transparent (or when Auto Transparency finds alpha in the texture)." :
        "Whether it writes depth. Auto: the shader's @state, else on.";
    propertyRow(RowPropertyType::Enum, cpType, "depthWrite", "Depth Write", sceneProject, entities, settings);

    // stencil and colour write (phase 2): Auto names the shader's rule
    RowSettings stateSettings;
    drawStencilColorWriteRows(cpType, sceneProject, entities, "", [scene, cpType](Entity entity) {
        return resolveComponentAuto(scene, entity, cpType);
    }, stateSettings);
}

void editor::InspectorWindow::drawStencilColorWriteRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                        const std::string& prefix, const std::function<RenderState(Entity)>& resolveAuto,
                                                        const RowSettings& settings){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const std::string stencilProperty = prefix + "stencil";
    const std::string colorWriteProperty = prefix + "colorWrite";
    const MaterialStencil* stencilRef = Catalog::getPropertyRef<MaterialStencil>(scene, entities[0], cpType, stencilProperty);
    const int* colorWriteRef = Catalog::getPropertyRef<int>(scene, entities[0], cpType, colorWriteProperty);
    if (!stencilRef || !colorWriteRef)
        return;
    // the rows show the first selected GameObject's values
    const MaterialStencil stencil = *stencilRef;
    const int colorWrite = *colorWriteRef;
    const RenderState resolvedAuto = resolveAuto(entities[0]);

    // An edit changes each selected GameObject's own value (one undo step): a multi-selection
    // keeps what the edit does not name (another ref or mask, the other channels), and one on
    // Auto starts from the rule its own shader resolves to
    auto editStencil = [&](const std::function<void(MaterialStencil&, Entity)>& edit) {
        auto* multi = new MultiPropertyCmd();
        bool changed = false;
        for (Entity entity : entities) {
            const MaterialStencil* current = Catalog::getPropertyRef<MaterialStencil>(scene, entity, cpType, stencilProperty);
            if (!current)
                continue;
            MaterialStencil value = *current;
            edit(value, entity);
            if (value == *current)
                continue;
            multi->addPropertyCmd<MaterialStencil>(project, sceneId, entity, cpType, stencilProperty, value, settings.onValueChanged);
            changed = true;
        }
        if (changed)
            CommandHandle::get(sceneId)->addCommandNoMerge(multi);
        else
            delete multi;
    };
    auto editColorWrite = [&](const std::function<int(int, Entity)>& edit) {
        auto* multi = new MultiPropertyCmd();
        bool changed = false;
        for (Entity entity : entities) {
            const int* current = Catalog::getPropertyRef<int>(scene, entity, cpType, colorWriteProperty);
            if (!current)
                continue;
            const int value = edit(*current, entity);
            if (value == *current)
                continue;
            multi->addPropertyCmd<int>(project, sceneId, entity, cpType, colorWriteProperty, value, settings.onValueChanged);
            changed = true;
        }
        if (changed)
            CommandHandle::get(sceneId)->addCommandNoMerge(multi);
        else
            delete multi;
    };
    // an entity's channels: its own, else what Auto writes for it
    auto channelsOf = [&](int value, Entity entity) {
        return value >= 0 ? value : (int)resolveAuto(entity).colorWriteMask;
    };

    // ---- the test: Auto names the manifest's rule; picking a test from Auto starts from it
    static const std::pair<MaterialStencilCompare, const char*> compares[] = {
        {MaterialStencilCompare::DISABLED, "Off"}, {MaterialStencilCompare::NEVER, "Never"},
        {MaterialStencilCompare::LESS, "Less"}, {MaterialStencilCompare::EQUAL, "Equal"},
        {MaterialStencilCompare::LESS_EQUAL, "Less Equal"}, {MaterialStencilCompare::GREATER, "Greater"},
        {MaterialStencilCompare::NOT_EQUAL, "Not Equal"}, {MaterialStencilCompare::GREATER_EQUAL, "Greater Equal"},
        {MaterialStencilCompare::ALWAYS, "Always"}};
    const std::string autoLabel = "Auto (" + stencilStateLabel(resolvedAuto) + ")";
    std::string current = autoLabel;
    for (const auto& [value, label] : compares) {
        if (stencil.compare == value) current = label;
    }
    ImGui::PushID(stencilProperty.c_str());
    prefabRowProperty = stencilProperty;
    if (propertyHeader("Stencil", -1, stencil != MaterialStencil(), settings.child))
        editStencil([](MaterialStencil& value, Entity) { value = MaterialStencil(); });
    ImGui::SetNextItemWidth(-1);
    const bool compareOpen = ImGui::BeginCombo("##stencil_compare", current.c_str());
    if (!compareOpen && ImGui::IsItemHovered())
        ImGui::SetTooltip("Stencil test and write of this draw. The stencil is cleared at the start of each camera "
                          "pass: masks and their readers draw in the same camera, masks first (opaque queue before "
                          "transparent, Sort Priority within a queue).\nA test other than Always leaves the depth "
                          "pre-pass. SSAO and SSR ignore the stencil: a masked object darkens and reflects at its "
                          "whole shape.");
    if (compareOpen) {
        if (ImGui::Selectable(autoLabel.c_str(), stencil.compare == MaterialStencilCompare::AUTO))
            editStencil([](MaterialStencil& value, Entity) { value.compare = MaterialStencilCompare::AUTO; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The shader's @state stencil rule, else no stencil test");
        for (const auto& [compare, label] : compares) {
            if (ImGui::Selectable(label, stencil.compare == compare)) {
                const MaterialStencilCompare picked = compare;
                editStencil([&](MaterialStencil& value, Entity entity) {
                    // an explicit test (Off included) keeps its ref, masks and ops
                    if (value.compare == MaterialStencilCompare::AUTO)
                        value = stencilFromState(resolveAuto(entity), picked);
                    value.compare = picked;
                });
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    prefabRowProperty.clear();

    // ref, masks and ops only while the test is explicit (Auto and Off have none of their own)
    if (stencil.compare != MaterialStencilCompare::AUTO && stencil.compare != MaterialStencilCompare::DISABLED) {
        static std::vector<EnumEntry> opEntries = {
            {(int)MaterialStencilOp::KEEP, "Keep"}, {(int)MaterialStencilOp::ZERO, "Zero"},
            {(int)MaterialStencilOp::REPLACE, "Replace"}, {(int)MaterialStencilOp::INCR_CLAMP, "Increment"},
            {(int)MaterialStencilOp::DECR_CLAMP, "Decrement"}, {(int)MaterialStencilOp::INVERT, "Invert"},
            {(int)MaterialStencilOp::INCR_WRAP, "Increment Wrap"}, {(int)MaterialStencilOp::DECR_WRAP, "Decrement Wrap"}};
        RowSettings settingsByte = settings;
        settingsByte.child = true;
        settingsByte.intMin = 0;
        settingsByte.intMax = 255;
        settingsByte.stepSize = 0.2f;
        settingsByte.help = "The stencil reference, 0-255: compared with the buffer (through Read Mask), written by Replace.";
        propertyRow(RowPropertyType::Int, cpType, prefix + "stencil.ref", "Ref", sceneProject, entities, settingsByte);
        settingsByte.help = "Bits of the reference and the buffer the test compares (255: all).";
        propertyRow(RowPropertyType::Int, cpType, prefix + "stencil.readMask", "Read Mask", sceneProject, entities, settingsByte);
        settingsByte.help = "Bits of the buffer the ops may change (255: all).";
        propertyRow(RowPropertyType::Int, cpType, prefix + "stencil.writeMask", "Write Mask", sceneProject, entities, settingsByte);
        RowSettings settingsOp = settings;
        settingsOp.child = true;
        settingsOp.enumEntries = &opEntries;
        settingsOp.help = "What the buffer becomes where the stencil and depth tests pass.";
        propertyRow(RowPropertyType::Enum, cpType, prefix + "stencil.pass", "Pass", sceneProject, entities, settingsOp);
        settingsOp.help = "What the buffer becomes where the stencil test fails.";
        propertyRow(RowPropertyType::Enum, cpType, prefix + "stencil.fail", "Fail", sceneProject, entities, settingsOp);
        settingsOp.help = "What the buffer becomes where the stencil test passes and the depth test fails.";
        propertyRow(RowPropertyType::Enum, cpType, prefix + "stencil.depthFail", "Depth Fail", sceneProject, entities, settingsOp);
    }

    // ---- colour write: Auto (the shader's colorwrite=, else all), or the channels
    ImGui::PushID(colorWriteProperty.c_str());
    prefabRowProperty = colorWriteProperty;
    if (propertyHeader("Color Write", -1, colorWrite != -1, settings.child))
        editColorWrite([](int, Entity) { return -1; });
    bool automatic = colorWrite < 0;
    const int shown = automatic ? (int)resolvedAuto.colorWriteMask : colorWrite;
    if (ImGui::Checkbox("Auto##color_write_auto", &automatic)) {
        // off Auto: each one keeps the channels its Auto wrote
        editColorWrite([&](int value, Entity entity) { return automatic ? -1 : channelsOf(value, entity); });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Auto: the shader's @state colorwrite, else every channel (now %s)", colorWriteLabel(resolvedAuto.colorWriteMask).c_str());
    ImGui::BeginDisabled(colorWrite < 0);
    static const std::pair<int, const char*> channels[] = {
        {MATERIAL_COLOR_WRITE_R, "R"}, {MATERIAL_COLOR_WRITE_G, "G"}, {MATERIAL_COLOR_WRITE_B, "B"}, {MATERIAL_COLOR_WRITE_A, "A"}};
    // one toggle per channel, highlighted while it is written (narrow enough for the column)
    for (const auto& [bit, label] : channels) {
        ImGui::SameLine(0.0f, 2.0f);
        const bool on = (shown & bit) != 0;
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[on ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[on ? ImGuiCol_Text : ImGuiCol_TextDisabled]);
        if (ImGui::Button((std::string(label) + "##color_write_" + label).c_str(), ImVec2(ImGui::GetFrameHeight(), 0))) {
            // this channel on or off for each one, its other channels kept
            const int channel = bit;
            editColorWrite([&](int value, Entity entity) {
                const int channels = channelsOf(value, entity);
                return on ? (channels & ~channel) : (channels | channel);
            });
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s channel: %s", label, on ? "written" : "not written");
    }
    ImGui::EndDisabled();
    if (colorWrite == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(depth/stencil only)");
    }
    ImGui::PopID();
    prefabRowProperty.clear();
}

void editor::InspectorWindow::drawMaterialPresetRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                    unsigned int submesh, const RowSettings& settingsMaterial){
    const std::vector<MaterialPreset>& presets = getMaterialPresets();
    propertyHeader("Preset", -1, false, settingsMaterial.child);
    const std::string popupId = "material_presets_" + std::to_string(submesh);
    ImGui::BeginDisabled(presets.empty());
    if (ImGui::Button((std::string(ICON_FA_WAND_MAGIC_SPARKLES "  Presets...##presets_") + std::to_string(submesh)).c_str(), ImVec2(-1, 0)))
        ImGui::OpenPopup(popupId.c_str());
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(presets.empty() ? "No material presets in this build"
                                          : "Set the shader, extra pass and render state of a common effect (one undo step)");
    if (ImGui::BeginPopup(popupId.c_str())) {
        for (const MaterialPreset& preset : presets) {
            if (ImGui::Selectable((preset.name + "##" + preset.id).c_str())) {
                Scene* scene = sceneProject->scene;
                const uint32_t sceneId = sceneProject->id;
                auto* multi = new MultiPropertyCmd();
                bool changed = false;
                for (Entity entity : entities) {
                    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                    if (!mesh || submesh >= mesh->numSubmeshes)
                        continue;
                    const Material& from = mesh->submeshes[submesh].material;
                    // judged by the shader the submesh draws with (the mesh's or the scene's when it names none)
                    changed |= addMaterialChangeCommands(*multi, project, sceneId, entity, submesh, from,
                                                         applyMaterialPreset(preset, from, meshShaderFor(scene, entity)),
                                                         settingsMaterial.onValueChanged);
                }
                if (changed)
                    CommandHandle::get(sceneId)->addCommandNoMerge(multi);
                else
                    delete multi;
            }
            if (!preset.description.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", preset.description.c_str());
        }
        ImGui::EndPopup();
    }
}

void editor::InspectorWindow::drawMaterialExtraPassRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                        unsigned int submesh, const RowSettings& settingsMaterial){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material.extraPass.";
    const std::string shaderProperty = prefix + "shader";
    std::string* shaderRef = Catalog::getPropertyRef<std::string>(scene, entities[0], cpType, shaderProperty);
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entities[0]);
    if (!shaderRef || !mesh || submesh >= mesh->numSubmeshes)
        return;
    const std::string currentShader = *shaderRef;
    bool mixed = false;
    for (Entity entity : entities) {
        std::string* other = Catalog::getPropertyRef<std::string>(scene, entity, cpType, shaderProperty);
        mixed = mixed || !other || *other != currentShader;
    }

    auto setShader = [&](const std::string& value) {
        auto* multi = new MultiPropertyCmd();
        for (Entity entity : entities)
            multi->addPropertyCmd<std::string>(project, sceneId, entity, cpType, shaderProperty, value, settingsMaterial.onValueChanged);
        CommandHandle::get(sceneId)->addCommandNoMerge(multi);
    };

    beginTable(cpType, getLabelSize("Depth Offset Units"), "submeshes_material_extra_pass");
    const std::string popupName = "Edit Shader Files##extra_pass_shader_" + std::to_string(submesh);
    prefabRowProperty = shaderProperty;
    propertyHeader("Shader", -1, false, true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A second draw of this submesh with its own shader (outlines, silhouettes, masks)");
    if (mixed)
        ImGui::TextDisabled("(different shaders)");
    drawLibraryShaderCombo(mixed ? std::string() : currentShader, ShaderType::MESH, setShader, popupName, "None");
    const Util::CustomShaderPaths paths = (!currentShader.empty() && !ShaderReference::isLibrary(currentShader))
        ? project->resolveCustomShaderFiles(currentShader) : Util::CustomShaderPaths();
    drawShaderFilesPopup(popupName, setShader, paths.vert, paths.frag);
    prefabRowProperty.clear();

    if (mixed || currentShader.empty()) {
        endTable();
        return;
    }

    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(currentShader);
    const ShaderManifest* stateManifest = (manifest && manifest->present) ? manifest.get() : nullptr;
    if (stateManifest) {
        if (entities.size() == 1) {
            drawShaderManifestRows(cpType, sceneProject, entities[0], currentShader, prefix + "shaderParams", prefix + "shaderTextures",
                                   prefix + "shaderFeatures", settingsMaterial.onValueChanged);
        } else {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Select one GameObject to edit the pass shader's values");
        }
    } else if (ShaderReference::isLibrary(currentShader) && !manifest) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s is not a library shader of this engine", currentShader.c_str());
    }

    // what Auto resolves to now (resolveExtraPassAuto)
    bool blended = false;
    const RenderState resolved = resolveExtraPassAuto(scene, entities[0], submesh, &blended);

    auto withAuto = [](AutoEnumEntries& entries, const std::string& resolvedLabel, std::initializer_list<EnumEntry> others) {
        entries.autoLabel = "Auto (" + resolvedLabel + ")";
        entries.entries.push_back({0, entries.autoLabel.c_str()});
        for (const EnumEntry& entry : others) entries.entries.push_back(entry);
    };
    AutoEnumEntries blend, cull, depthWrite, depthTest;
    withAuto(blend, blended ? blendLabel(resolved.blend) : "Opaque", {
        {(int)MaterialBlendMode::NONE, "Opaque"}, {(int)MaterialBlendMode::ALPHA, "Alpha"},
        {(int)MaterialBlendMode::PREMULTIPLIED, "Premultiplied"}, {(int)MaterialBlendMode::ADDITIVE, "Additive"},
        {(int)MaterialBlendMode::MULTIPLY, "Multiply"}});
    withAuto(cull, !resolved.faceCulling ? "Off" : (resolved.cullingMode == CullingMode::FRONT ? "Front" : "Back"), {
        {(int)MaterialCullMode::BACK, "Back"}, {(int)MaterialCullMode::FRONT, "Front"}, {(int)MaterialCullMode::OFF, "Off"}});
    withAuto(depthWrite, resolved.depthWrite ? "On" : "Off", {
        {(int)MaterialDepthWrite::ENABLED, "On"}, {(int)MaterialDepthWrite::DISABLED, "Off"}});
    withAuto(depthTest, depthCompareLabel(resolved.depthCompare), {
        {(int)MaterialDepthTest::LESS, "Less"}, {(int)MaterialDepthTest::LESS_EQUAL, "Less Equal"},
        {(int)MaterialDepthTest::EQUAL, "Equal"}, {(int)MaterialDepthTest::GREATER_EQUAL, "Greater Equal"},
        {(int)MaterialDepthTest::GREATER, "Greater"}, {(int)MaterialDepthTest::NOT_EQUAL, "Not Equal"},
        {(int)MaterialDepthTest::ALWAYS, "Always"}});
    static std::vector<EnumEntry> orderEntries = {
        {(int)MaterialPassOrder::AFTER, "After the submesh"}, {(int)MaterialPassOrder::BEFORE, "Before the submesh"}};

    RowSettings settingsEnum = settingsMaterial;
    settingsEnum.child = true;
    RowSettings settingsFloat = settingsEnum;
    settingsFloat.stepSize = 0.01f;

    settingsEnum.enumEntries = &orderEntries;
    settingsEnum.help = "Drawn right after the submesh, or right before it (an outline behind a transparent object goes before).";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "order", "Order", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &blend.entries;
    settingsEnum.help = "How the pass's colour mixes with what is behind. Auto: the pass shader's @state, else the submesh's.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "blendMode", "Blend Mode", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &cull.entries;
    settingsEnum.help = "Which faces the pass leaves out (an inverted hull culls the front ones).";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "cullMode", "Cull", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &depthWrite.entries;
    settingsEnum.help = "Whether the pass writes depth.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "depthWrite", "Depth Write", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &depthTest.entries;
    settingsEnum.help = "The pass's depth comparison (Greater: only where something is in front, a see-through silhouette).";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "depthTest", "Depth Test", sceneProject, entities, settingsEnum);
    settingsFloat.help = "Polygon offset slope factor of the pass.";
    propertyRow(RowPropertyType::Float, cpType, prefix + "depthOffsetFactor", "Depth Offset Factor", sceneProject, entities, settingsFloat);
    settingsFloat.help = "Polygon offset constant of the pass, in depth units.";
    propertyRow(RowPropertyType::Float, cpType, prefix + "depthOffsetUnits", "Depth Offset Units", sceneProject, entities, settingsFloat);

    RowSettings settingsState = settingsMaterial;
    settingsState.child = true;
    drawStencilColorWriteRows(cpType, sceneProject, entities, prefix, [scene, submesh](Entity entity) {
        return resolveExtraPassAuto(scene, entity, submesh);
    }, settingsState);
    endTable();
}
