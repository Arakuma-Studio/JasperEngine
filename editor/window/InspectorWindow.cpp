// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "animation/BoneOverlay.h"
#include "pixelart/PixelPerfectEditor.h"
#include "animation/AnimatorEditorHooks.h"
#include "graph/GraphEditorHooks.h"
#include "component/ShadowCaster2DComponent.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"
#include "component/CanvasGroupComponent.h"
#include "component/InputFieldComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/TextEffectComponent.h"
#include "EditorModal.h"
#include "window/dialog/RagdollWizardDialog.h"
#include "InspectorWindow.h"

#include "audio/SoundDrop.h"
#include "window/SoundBrowserWindow.h"
#include "reflect/Reflect.h"
#include "reflect/Enums.h"
#include "util/EditorScripts.h"
#include "widget/ColorPickerWindow.h"
#include "widget/CurveField.h"
#include "widget/GradientField.h"
#include "util/NumericExpression.h"
#include "profiler/Profile.h"
#include "command/PlayChangeReview.h"
#include "command/type/RemovePreservedComponentCmd.h"
#include "StreamPreserved.h"
#include "DocumentFile.h"
#include "window/CanvasScalerFields.h"
#include "shader/ShaderGlobals.h"
#include "SceneWindow.h"
#include "script/ScriptRuntime.h"

#include "imgui_internal.h"

#include "util/Util.h"
#include "util/Fnv.h"
#include "util/UIUtils.h"
#include "util/FileDialogs.h"
#include "script/ScriptScheduler.h"
#include "util/FileUtils.h"
#include "window/CodeEditor.h"
#include "subsystem/UIDocumentSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/RenderSystem.h"
#include "component/ButtonComponent.h"
#include "htmlui/HtmlUiImportPanel.h"
#include "util/EntityPayload.h"
#include "util/CameraTextureLink.h"
#include "util/FluidTextureLink.h"
#include "util/VideoTextureLink.h"
#include "component/VideoPlayerComponent.h"
#include "util/FluidEditorTools.h"
#include "util/UIProbe.h"
#include "procmat/ProceduralBake.h"
#include "util/ScriptAttach.h"
#include "component/ScriptComponent.h"
#include "util/ProjectChecks.h"
#include "search/SearchScorer.h"
#include "component/FluidSimulationComponent.h"
#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "lattice/LatticeEditorOps.h"
#include "lattice/LatticeSolver.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/SceneNameCmd.h"
#include "command/type/MeshChangeCmd.h"
#include "command/type/ModelClearCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/ComponentToPrefabSharedCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/AtmospherePresetCmd.h"
#include "render/AtmospherePresets.h"
#include "render/RenderUtil.h"
#include "util/EntityClipboard.h"
#include "command/type/ForkShaderCmd.h"
#include "shader/ShaderManifest.h"
#include "command/type/ScenePropertyCmd.h"
#include "util/Color.h"
#include "util/LightPhotometry.h"
#include "render/SceneRender2D.h"
#include "render/CapsuleShadows.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "window/ProjectWindow.h"
#include "window/HierarchyWindow.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "Platform.h"
#include "Screen.h"
#include "window/DeviceProfiles.h"
#include "Widgets.h"
#include "Factory.h"
#include "audio/AudioMixer.h"
#include "pool/SoundPool.h"
#include "component/ActionComponent.h"
#include "component/AudioSnapshotComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/LineRendererComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PanelComponent.h"
#include "util/SHA1.h"
#include "util/ProjectUtils.h"
#include "util/NormalMapTilt.h"
#include "Stream.h"
#include "Out.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "audio/AudioCapture.h"
#include "subsystem/AudioSystem.h"
#include "pose/PoseAnimatorRuntime.h"
#include "pose/PoseInspectorHooks.h"
#include "window/InspectorVatBake.h"
#include "window/SpriteNormalsEntry.h"
#include "pose/PoseEditSession.h"
#include "animation/PreviewOwner.h"
#include "animation/ClipRecordHooks.h"
#include "animation/PropertyBindings.h"
#include "command/type/MorphWeightsCmd.h"
#include "window/InspectorWindowClips.h"
#include "component/AnimationPlayerComponent.h"
#include "action/Action.h"
#include "pool/TexturePool.h"
#include "pixelart/aseprite/AsepriteEditorHooks.h"
#include "pixelart/aseprite/AsepriteSpriteSetup.h"
#include "sprite/SpriteSheetPool.h"
#include "subsystem/MeshSystem.h"
#include "sprite/SpriteSheetSources.h"
#include "window/SpriteAtlasInspector.h"
#include "command/CommandHistory.h"
#include "yaml-cpp/yaml.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <map>
#include <type_traits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cctype>
#include <cfloat>
#include "util/EntityLabels.h"
#include "util/TextureLabels.h"

#include <algorithm>
#include <unordered_set>
#include <memory>

using namespace jasperengine;

// Assets are stored relative to the assets root. A file outside it still previews from
// its absolute path, but the drop is refused on delivery.
static std::string droppedAssetPath(editor::Project* project, const std::string& path, bool insideAssets) {
    return insideAssets ? project->normalizeToAssetsRelative(path).generic_string()
                        : std::filesystem::path(path).generic_string();
}

// Redraws the component a dropped texture belongs to, used when a preview is undone.
static void markTextureUpdate(Scene* scene, Entity entity, editor::ComponentType componentType, const std::string& property) {
    // the row's own update flags redraw any component (a Button's state textures included)
    const editor::PropertyData prop = editor::Catalog::findProperty(scene, entity, componentType, property);
    if (prop.ref && prop.updateFlags) {
        editor::Catalog::updateEntity(scene, entity, prop.updateFlags & ~(uint64_t)editor::UpdateFlags_SceneWide);
    }
    if (componentType == editor::ComponentType::MeshComponent) {
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        for (unsigned int i = 0; i < mesh.numSubmeshes; i++) {
            mesh.submeshes[i].needUpdateTexture = true;
        }
    }
    if (componentType == editor::ComponentType::UIComponent) {
        scene->getComponent<UIComponent>(entity).needUpdateTexture = true;
    }
    if (componentType == editor::ComponentType::SkyboxComponent) {
        scene->getComponent<SkyboxComponent>(entity).needUpdateTexture = true;
    }
}

// Record (animation clips 9.R5): a row value as ClipRecordValue lays it out
static std::vector<float> vector3Value(const Vector3& value) {
    return {value.x, value.y, value.z};
}

static std::vector<float> rotationValue(const Quaternion& value) {
    std::vector<float> out(4);
    quaternionToXYZW(value, out.data());
    return out;
}

// the dropdowns of the component enums: their reflection tables' labels and Inspector order
// (editor/reflect/Enums.cpp)
static std::vector<editor::EnumEntry>& entriesPrimitiveType = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kPrimitiveType);
static std::vector<editor::EnumEntry>& entriesMaterialAlphaMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kMaterialAlphaMode);
static std::vector<editor::EnumEntry>& entriesPivotPreset = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kPivotPreset);
static std::vector<editor::EnumEntry>& entriesSpriteDrawMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kSpriteDrawMode);
static std::vector<editor::EnumEntry>& entriesLightType = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLightType);
static std::vector<editor::EnumEntry>& entriesShadowCaster2DShape = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kShadowCaster2DShape);
static std::vector<editor::EnumEntry>& entriesLightShading = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLightShading);
static std::vector<editor::EnumEntry>& entriesCullingMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kCullingMode);
static std::vector<editor::EnumEntry>& entriesLodFadeMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLodFadeMode);
static std::vector<editor::EnumEntry>& entriesFogType = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kFogType);
static std::vector<editor::EnumEntry>& entriesFogColorMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kFogColorMode);
static std::vector<editor::EnumEntry>& entriesAtmosphereQualityOverride = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kAtmosphereQualityOverride);
static std::vector<editor::EnumEntry>& entriesCloudsRendering = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kCloudsRendering);
static std::vector<editor::EnumEntry>& entriesReflectionProbeMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kReflectionProbeMode);
static std::vector<editor::EnumEntry>& entriesReflectionProbeUpdateMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kReflectionProbeUpdateMode);
static std::vector<editor::EnumEntry>& entriesCameraType = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kCameraType);
static std::vector<editor::EnumEntry>& entriesTransparencyMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kTransparencyMode);
static std::vector<editor::EnumEntry>& entriesCapsuleCastShadows = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kCapsuleCastShadows);
static std::vector<editor::EnumEntry>& entriesCapsuleDirection = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kCapsuleDirection);
// render-target framebuffers have no mipmaps, so only the non-mipmap filters apply
static std::vector<editor::EnumEntry>& entriesTextureFilter = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kTextureFilter, editor::reflect::enums::bit((int)TextureFilter::NEAREST) | editor::reflect::enums::bit((int)TextureFilter::LINEAR));
// full filter set for a texture's minification (mipmap variants trigger mipmap generation)
static std::vector<editor::EnumEntry>& entriesTextureMinFilter = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kTextureFilter);
static std::vector<editor::EnumEntry>& entriesTextureWrap = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kTextureWrap);
static std::vector<editor::EnumEntry>& entriesTextureAlphaBorder = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kTextureAlphaBorder);
static std::vector<editor::EnumEntry>& entriesAudioAttenuation = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kAudioSourceAttenuation);
static std::vector<editor::EnumEntry>& entriesUIDocumentRenderMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kUIDocumentRenderMode);
static std::vector<editor::EnumEntry>& entriesAudioState = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kAudioSourceState);

// the AudioSource Load Type row (audio-mixer; drawAudioComponent)
static std::vector<editor::EnumEntry> entriesAudioLoadType = {
    { (int)AudioLoadType::Auto, "Auto" },
    { (int)AudioLoadType::DecompressOnLoad, "Decompress On Load" },
    { (int)AudioLoadType::CompressedInMemory, "Compressed In Memory" },
    { (int)AudioLoadType::Streaming, "Streaming" }
};
static std::vector<int> cascadeValues = { 1, 2, 3, 4, 5, 6 };
// reflection probe captures are clamped to [16, 1024] in RenderSystem
static std::vector<int> probeResolutionValues = { 16, 32, 64, 128, 256, 512, 1024 };

static std::vector<editor::EnumEntry>& entriesEaseType = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kEaseType);
std::vector<editor::InspectorWindow::DirtyMaterialEntry> editor::InspectorWindow::dirtyMaterials;

bool editor::InspectorWindow::lastHeaderRowClicked() {
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    max.x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    return ImGui::IsMouseHoveringRect(min, max) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void editor::InspectorWindow::markMaterialDirty(unsigned int sceneId, Entity entity, int submeshIndex, const std::string& relativePath) {
    for (auto& entry : dirtyMaterials) {
        if (entry.sceneId == sceneId && entry.entity == entity && entry.submeshIndex == submeshIndex) {
            entry.timer = 0.0f;
            entry.relativePath = relativePath;
            return;
        }
    }

    dirtyMaterials.push_back({sceneId, entity, submeshIndex, relativePath, 0.0f});
}

void editor::InspectorWindow::writeDirtyMaterial(const DirtyMaterialEntry& entry) {
    SceneProject* sp = project->getScene(entry.sceneId);
    if (!sp || !sp->scene) {
        return;
    }

    MeshComponent* mesh = sp->scene->findComponent<MeshComponent>(entry.entity);
    if (!mesh || entry.submeshIndex >= mesh->numSubmeshes) {
        return;
    }

    Material& material = mesh->submeshes[entry.submeshIndex].material;
    std::filesystem::path absolutePath = project->getProjectPath() / entry.relativePath;

    try {
        std::string error;
        const SaveResult saved = project->writeMaterialFile(entry.relativePath, material, DegradedSavePolicy::Refuse, &error);
        if (saved == SaveResult::RefusedDegraded) {
            // the editor's lossy-save modal decides (Save Anyway writes it)
            DegradedSaveRequest request;
            request.documentName = absolutePath.filename().string();
            request.path = entry.relativePath;
            const LoadReport report = DocumentFiles::materialFileReport(absolutePath);
            for (const LoadIssue& issue : report.issues) {
                if (issue.fate == LoadIssueFate::Dropped) request.dropped.push_back(issue);
            }
            Project* owner = project;
            const std::filesystem::path relativePath = entry.relativePath;
            const Material copy = material;
            request.saveAnyway = [owner, relativePath, copy]() {
                owner->writeMaterialFile(relativePath, copy, DegradedSavePolicy::SaveAnyway);
            };
            editor::getEditorHost().confirmDegradedSave(request);
        }
        if (saved == SaveResult::Saved) {

            // the other submeshes linked to the file follow through the hot reload (the
            // save is the editor's own write, delivered to it: S05 R10)
            project->linkMaterialFile(entry.sceneId, entry.entity, entry.submeshIndex, entry.relativePath);

            if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
                projectWindow->notifyResourceFileChanged(absolutePath);
            }
        } else {
            Out::error("Failed to write linked material file '%s'", absolutePath.string().c_str());
        }
    } catch (const std::exception& e) {
        Out::error("Error saving linked material file '%s': %s", absolutePath.string().c_str(), e.what());
    }
}

void editor::InspectorWindow::flushDirtyMaterials(float deltaTime) {
    if (dirtyMaterials.empty()) {
        return;
    }

    auto it = dirtyMaterials.begin();
    while (it != dirtyMaterials.end()) {
        it->timer += deltaTime;
        if (it->timer < materialWriteDelaySec) {
            ++it;
            continue;
        }

        writeDirtyMaterial(*it);

        it = dirtyMaterials.erase(it);
    }
}

// The scene is about to go away, so write its pending edits without waiting out the debounce
void editor::InspectorWindow::flushDirtyMaterialsForScene(uint32_t sceneId) {
    auto it = dirtyMaterials.begin();
    while (it != dirtyMaterials.end()) {
        if (it->sceneId != sceneId) {
            ++it;
            continue;
        }

        writeDirtyMaterial(*it);

        it = dirtyMaterials.erase(it);
    }
}

template<typename T>
void editor::InspectorWindow::drawScenePropertyRow(SceneProject* sceneProject, const std::string& propertyName, const char* label, ScenePropertyInputType inputType, float inputWidth, float minValue, float maxValue, const std::string& help) {
    T value = Catalog::getSceneProperty<T>(sceneProject->scene, propertyName);
    bool changed = false;

    Command* cmd = nullptr;

    // Detect whether the value differs from the factory default so we can show the
    // "reset to default" arrow next to the label (like component property rows).
    T defaultValue = Catalog::getScenePropertyDefault<T>(propertyName);
    bool defChanged = false;
    if constexpr (std::is_same_v<T, float>) {
        defChanged = std::fabs(value - defaultValue) > 1e-4f;
    } else if constexpr (std::is_same_v<T, Vector2>) {
        defChanged = std::fabs(value.x - defaultValue.x) > 1e-4f || std::fabs(value.y - defaultValue.y) > 1e-4f;
    } else if constexpr (std::is_same_v<T, Vector3>) {
        defChanged = std::fabs(value.x - defaultValue.x) > 1e-4f || std::fabs(value.y - defaultValue.y) > 1e-4f
            || std::fabs(value.z - defaultValue.z) > 1e-4f;
    } else if constexpr (std::is_same_v<T, Vector4>) {
        defChanged = std::fabs(value.x - defaultValue.x) > 1e-4f || std::fabs(value.y - defaultValue.y) > 1e-4f
            || std::fabs(value.z - defaultValue.z) > 1e-4f || std::fabs(value.w - defaultValue.w) > 1e-4f;
    } else {
        defChanged = (value != defaultValue);
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("%s", label);

    // Reset-to-default arrow, only shown when the value differs from the default.
    if (defChanged) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button((ICON_FA_ROTATE_LEFT"##reset_" + propertyName).c_str())) {
            Command* resetCmd = new ScenePropertyCmd<T>(project, sceneProject->id, propertyName, defaultValue);
            CommandHandle::get(sceneProject->id)->addCommandNoMerge(resetCmd);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    ImGui::TableSetColumnIndex(1);

    if (inputWidth > 0.0f) {
        if (!help.empty()) {
            const float reservedHelpWidth = ImGui::CalcTextSize("(?)").x + ImGui::GetStyle().ItemSpacing.x;
            const float availableInputWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - reservedHelpWidth);
            ImGui::SetNextItemWidth(std::min(inputWidth, availableInputWidth));
        } else {
            ImGui::SetNextItemWidth(inputWidth);
        }
    } else if (!help.empty()){
        // leave room in the value column for the trailing "(?)" marker
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("(?)").x + ImGui::GetStyle().ItemSpacing.x));
    }

    switch (inputType) {
        case ScenePropertyInputType::Checkbox:
            if constexpr (std::is_same_v<T, bool>) {
                changed = ImGui::Checkbox(("##" + propertyName).c_str(), &value);
            }
            break;
        case ScenePropertyInputType::DragFloat:
            if constexpr (std::is_same_v<T, float>) {
                changed = ImGui::DragFloat(("##" + propertyName).c_str(), &value, 0.01f);
            } else if constexpr (std::is_same_v<T, Vector2>) {
                changed = ImGui::DragFloat2(("##" + propertyName).c_str(), (float*)&value.x);
            } else if constexpr (std::is_same_v<T, Vector3>) {
                changed = ImGui::DragFloat3(("##" + propertyName).c_str(), (float*)&value.x);
            } else if constexpr (std::is_same_v<T, Vector4>) {
                changed = ImGui::DragFloat4(("##" + propertyName).c_str(), (float*)&value.x);
            }
            break;
        case ScenePropertyInputType::DragInt:
            if constexpr (std::is_same_v<T, int>) {
                changed = ImGui::DragInt(("##" + propertyName).c_str(), &value, 1.0f, (int)minValue, (int)maxValue);
            }
            break;
        case ScenePropertyInputType::SliderFloat:
            if constexpr (std::is_same_v<T, float>) {
                // Slider that can be double-clicked to type an exact value (also allows values above the max).
                ImGui::SetNextItemWidth(-1);
                changed = UIUtils::sliderFloatInput(("##" + propertyName).c_str(), &value, minValue, maxValue);
            }
            break;
        case ScenePropertyInputType::ColorRGB:
        case ScenePropertyInputType::ColorRGBA:
            if constexpr (std::is_same_v<T, Vector3> || std::is_same_v<T, Vector4>) {
                // a scene colour is stored as shown (S27 R1: the UI tints keep sRGB storage)
                ui::ColorFieldOptions options;
                options.alpha = std::is_same_v<T, Vector4>;
                options.linearStorage = false;
                options.label = label;
                const ui::EditPhase phase = ui::colorField(("##" + propertyName).c_str(), value, options);
                if (phase == ui::EditPhase::Begin) ui::ColorPickerWindow::beginEdit(CommandHandle::get(sceneProject->id), label);
                changed = phase == ui::EditPhase::Changing;
            }
            break;
        case ScenePropertyInputType::Combo:
            if constexpr (std::is_same_v<T, LightState>) {
                const char* lightStateNames[] = { "Off", "On", "Auto" };
                int currentItem = static_cast<int>(value);
                changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, lightStateNames, IM_ARRAYSIZE(lightStateNames));
                if (changed) {
                    value = static_cast<LightState>(currentItem);
                }
            }
            if constexpr (std::is_same_v<T, ShadowQuality>) {
                if (propertyName == "shadows_quality") {
                    // the 3D quality tiers, in Unity's order
                    static const ShadowQuality qualityValues[] = { ShadowQuality::AUTO, ShadowQuality::NONE, ShadowQuality::LOW,
                                                                   ShadowQuality::MEDIUM, ShadowQuality::HIGH, ShadowQuality::ULTRA };
                    const char* qualityNames[] = { "Auto", "Hard Only", "Low", "Medium", "High", "Ultra" };
                    int currentItem = 0;
                    for (int q = 0; q < IM_ARRAYSIZE(qualityValues); q++) {
                        if (qualityValues[q] == value) currentItem = q;
                    }
                    changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, qualityNames, IM_ARRAYSIZE(qualityNames));
                    if (changed) {
                        value = qualityValues[currentItem];
                    }
                } else {
                    const char* qualityNames[] = { "None", "Low", "Medium", "High" };
                    int currentItem = std::clamp(static_cast<int>(value), 0, 3);
                    changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, qualityNames, IM_ARRAYSIZE(qualityNames));
                    if (changed) {
                        value = static_cast<ShadowQuality>(currentItem);
                    }
                }
            }
            if constexpr (std::is_same_v<T, LitQuality>) {
                const char* qualityNames[] = { "Standard", "High" };
                int currentItem = static_cast<int>(value);
                changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, qualityNames, IM_ARRAYSIZE(qualityNames));
                if (changed) {
                    value = static_cast<LitQuality>(currentItem);
                }
            }
            if constexpr (std::is_same_v<T, PlanarReflectionResolution>) {
                const char* resolutionNames[] = { "Off", "Quarter", "Half", "Full" };
                int currentItem = std::clamp(static_cast<int>(value), 0, 3);
                changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, resolutionNames, IM_ARRAYSIZE(resolutionNames));
                if (changed) {
                    value = static_cast<PlanarReflectionResolution>(currentItem);
                }
            }
            if constexpr (std::is_same_v<T, WaterWakeResolution>) {
                const char* resolutionNames[] = { "Auto", "Low (128)", "Medium (256)", "High (512)" };
                int currentItem = std::clamp(static_cast<int>(value), 0, 3);
                changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, resolutionNames, IM_ARRAYSIZE(resolutionNames));
                if (changed) {
                    value = static_cast<WaterWakeResolution>(currentItem);
                }
            }
            if constexpr (std::is_same_v<T, SceneColorScale>) {
                const char* scaleNames[] = { "Full", "Half" };
                int currentItem = static_cast<int>(value);
                changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, scaleNames, IM_ARRAYSIZE(scaleNames));
                if (changed) {
                    value = static_cast<SceneColorScale>(currentItem);
                }
            }
            if constexpr (std::is_same_v<T, TextureFilter>) {
                const char* filterNames[] = { "Nearest", "Linear" };
                int currentItem = (value == TextureFilter::LINEAR) ? 1 : 0;
                changed = ImGui::Combo(("##" + propertyName).c_str(), &currentItem, filterNames, IM_ARRAYSIZE(filterNames));
                if (changed) {
                    value = (currentItem == 1) ? TextureFilter::LINEAR : TextureFilter::NEAREST;
                }
            }
            break;
        case ScenePropertyInputType::LayerMask:
            if constexpr (std::is_same_v<T, uint32_t>) {
                // Unity's mask field: toggles keep the list open, Nothing and Everything set every bit
                if (ImGui::BeginCombo(("##" + propertyName).c_str(), sceneLayerMaskLabel(value).c_str())) {
                    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
                    uint32_t namedMask = 0;
                    for (int layer = 0; layer < MAX_LAYERS; layer++) {
                        if (!TagManager::layerToName(layer).empty()) namedMask |= (1u << layer);
                    }
                    if (ImGui::MenuItem("Nothing", nullptr, value == LAYER_MASK_NOTHING)) {
                        value = LAYER_MASK_NOTHING;
                        changed = true;
                    }
                    if (ImGui::MenuItem("Everything", nullptr, value == LAYER_MASK_EVERYTHING)) {
                        value = LAYER_MASK_EVERYTHING;
                        changed = true;
                    }
                    for (int layer = 0; layer < MAX_LAYERS; layer++) {
                        const std::string name = TagManager::layerToName(layer);
                        if (name.empty()) continue;
                        const uint32_t bit = 1u << layer;
                        if (ImGui::MenuItem((std::to_string(layer) + ": " + name).c_str(), nullptr, (value & bit) != 0)) {
                            value = (value & bit) ? (value & ~bit) : (value | bit);
                            // every named layer on reads as Everything, as Unity stores it
                            if ((value & namedMask) == namedMask) value = LAYER_MASK_EVERYTHING;
                            changed = true;
                        }
                    }
                    ImGui::PopItemFlag();
                    ImGui::EndCombo();
                }
                if (changed) {
                    // one undo step per toggle
                    Command* maskCmd = new ScenePropertyCmd<T>(project, sceneProject->id, propertyName, value);
                    CommandHandle::get(sceneProject->id)->addCommandNoMerge(maskCmd);
                    changed = false;
                }
            }
            break;
    }

    if (changed) {
        cmd = new ScenePropertyCmd<T>(project, sceneProject->id, propertyName, value);
        CommandHandle::get(sceneProject->id)->addCommand(cmd);
    }

    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (cmd) {
            cmd->setNoMerge();
            cmd = nullptr;
        }
    }

    // after IsItemDeactivatedAfterEdit so that check still refers to the widget
    if (!help.empty()){
        ImGui::SameLine(); helpMarker(help);
    }
}

bool editor::InspectorWindow::ensureSoundPreviewInitialized() {
    SoundPreviewRuntime& ap = soundPreviewRuntime;
    if (!AudioSystem::init()) {
        ap.error = "Failed to initialize audio preview output.";
        return false;
    }

    return true;
}

void editor::InspectorWindow::stopSoundPreview(bool unload) {
    SoundPreviewRuntime& ap = soundPreviewRuntime;
    if (ap.handle != 0) {
        AudioSystem::getSoloud().stop(ap.handle);
    }

    ap.active = false;
    ap.playing = false;
    ap.handle = 0;
    ap.time = 0.0;

    if (unload) {
        ap.loaded = false;
        ap.filename.clear();
        ap.length = 0.0;
        ap.entity = NULL_ENTITY;
        ap.sceneId = 0;
    }
}

void editor::InspectorWindow::applySoundPreviewSettings(const AudioSourceComponent& audio) {
    SoundPreviewRuntime& ap = soundPreviewRuntime;
    if (!ap.active || ap.handle == 0) {
        return;
    }

    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    soloud.setVolume(ap.handle, static_cast<float>(audio.volume));
    soloud.setRelativePlaySpeed(ap.handle, audio.speed);
    soloud.setPan(ap.handle, audio.pan);
    // Preview should always terminate even if the runtime sound is configured to loop.
    soloud.setLooping(ap.handle, false);
    soloud.setLoopPoint(ap.handle, audio.loopingPoint);
    soloud.setProtectVoice(ap.handle, audio.protectVoice);
    soloud.setInaudibleBehavior(ap.handle, audio.inaudibleBehaviorMustTick, audio.inaudibleBehaviorKill);
}

bool editor::InspectorWindow::loadSoundPreview(SceneProject* sceneProject, Entity entity, const AudioSourceComponent& audio) {
    SoundPreviewRuntime& ap = soundPreviewRuntime;
    if (audio.filename.empty()) {
        ap.error = "Select an audio clip before previewing.";
        return false;
    }

    if (!ensureSoundPreviewInitialized()) {
        return false;
    }

    if (ap.loaded && ap.sceneId == sceneProject->id && ap.entity == entity && ap.filename == audio.filename) {
        return true;
    }

    stopSoundPreview(true);

    std::filesystem::path audioPath = project->resolveAssetPath(audio.filename);
    SoLoud::result result = ap.sample.load(audioPath.string().c_str());
    if (result != SoLoud::SOLOUD_ERRORS::SO_NO_ERROR) {
        ap.error = "Failed to load audio preview file:\n" + audioPath.string();
        return false;
    }

    ap.sample.setVolume(1.0f);
    ap.loaded = true;
    ap.sceneId = sceneProject->id;
    ap.entity = entity;
    ap.filename = audio.filename;
    ap.length = ap.sample.getLength();
    ap.error.clear();

    return true;
}

// the preview plays through the source's mixer group (its id, else its name; Master when neither is in the mixer)
static unsigned int playPreviewVoice(SoLoud::AudioSource& sample, const AudioSourceComponent& audio) {
    std::string group = audio.outputGroup.empty() ? "" : AudioMixer::findGroupId(audio.outputGroup);
    if (!audio.outputGroupName.empty()) {
        const std::string byName = AudioMixer::findGroupId(audio.outputGroupName);
        if (!byName.empty()) group = byName;
    }
    const unsigned int handle = AudioMixer::play(group, sample, false, Vector3(0, 0, 0), static_cast<float>(audio.volume), false, 0.0);
    return handle;
}

bool editor::InspectorWindow::startSoundPreview(SceneProject* sceneProject, Entity entity, const AudioSourceComponent& audio) {
    if (!loadSoundPreview(sceneProject, entity, audio)) {
        return false;
    }

    SoundPreviewRuntime& ap = soundPreviewRuntime;
    if (ap.active && ap.handle != 0) {
        AudioSystem::getSoloud().setPause(ap.handle, false);
        ap.playing = true;
        applySoundPreviewSettings(audio);
        return true;
    }

    ap.handle = playPreviewVoice(ap.sample, audio);
    if (ap.handle == 0) {
        ap.error = "Failed to start audio preview.";
        return false;
    }

    ap.active = true;
    ap.playing = true;
    ap.time = 0.0;
    applySoundPreviewSettings(audio);
    AudioSystem::getSoloud().setPause(ap.handle, false);

    return true;
}

void editor::InspectorWindow::pauseSoundPreview() {
    SoundPreviewRuntime& ap = soundPreviewRuntime;
    if (!ap.active || ap.handle == 0) {
        return;
    }

    AudioSystem::getSoloud().setPause(ap.handle, true);
    ap.playing = false;
}

bool editor::InspectorWindow::seekSoundPreview(SceneProject* sceneProject, Entity entity, const AudioSourceComponent& audio, double time) {
    if (!loadSoundPreview(sceneProject, entity, audio)) {
        return false;
    }

    SoundPreviewRuntime& ap = soundPreviewRuntime;
    bool shouldKeepPlaying = ap.active && ap.playing;
    if (!ap.active || ap.handle == 0) {
        ap.handle = playPreviewVoice(ap.sample, audio);
        if (ap.handle == 0) {
            ap.error = "Failed to start audio preview.";
            return false;
        }
        ap.active = true;
        ap.playing = false;
        shouldKeepPlaying = false;
    }

    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    SoLoud::result result = soloud.seek(ap.handle, time);
    if (result != SoLoud::SOLOUD_ERRORS::SO_NO_ERROR) {
        ap.error = "Failed to seek audio preview.";
        return false;
    }

    ap.time = time;
    ap.playing = shouldKeepPlaying;
    soloud.setPause(ap.handle, !shouldKeepPlaying);
    applySoundPreviewSettings(audio);

    return true;
}

void editor::InspectorWindow::updateSoundPreview(const AudioSourceComponent& audio) {
    SoundPreviewRuntime& ap = soundPreviewRuntime;
    if (!ap.active || ap.handle == 0) {
        return;
    }

    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    if (!soloud.isValidVoiceHandle(ap.handle)) {
        stopSoundPreview();
        return;
    }

    applySoundPreviewSettings(audio);
    ap.time = soloud.getStreamPosition(ap.handle);
}

std::string editor::InspectorWindow::formatPropertyLabelValue(const PropertyData& prop) {
    if (!prop.ref) {
        return "-";
    }

    char buffer[64];
    switch (prop.type) {
        case PropertyType::Bool:
            return (*static_cast<bool*>(prop.ref)) ? "true" : "false";
        case PropertyType::Float:
            snprintf(buffer, sizeof(buffer), "%.3f", *static_cast<float*>(prop.ref));
            return buffer;
        case PropertyType::Double:
            snprintf(buffer, sizeof(buffer), "%.3f", *static_cast<double*>(prop.ref));
            return buffer;
        case PropertyType::Int:
            return std::to_string(*static_cast<int*>(prop.ref));
        case PropertyType::UInt:
            return std::to_string(*static_cast<unsigned int*>(prop.ref));
        case PropertyType::Entity:
        case PropertyType::EntityReference:
            return std::to_string(*static_cast<Entity*>(prop.ref));
        case PropertyType::String:
            return *static_cast<std::string*>(prop.ref);
        case PropertyType::Asset:
            return static_cast<AssetRef*>(prop.ref)->path;
        case PropertyType::Shader:
            return static_cast<ShaderRef*>(prop.ref)->path;
        case PropertyType::Font:
            return static_cast<FontArray*>(prop.ref)->front();
        case PropertyType::Ease: {
            EaseType type = static_cast<Ease*>(prop.ref)->getType();
            for (const auto& entry : entriesEaseType) {
                if (entry.value == static_cast<int>(type)) {
                    return entry.name;
                }
            }
            return "Ease";
        }
        default:
            return "-";
    }
}

editor::InspectorWindow::InspectorWindow(Project* project){
    this->project = project;
    this->sceneWindow = nullptr;
    this->cmd = nullptr;

    this->windowOpen = true;
    this->focusRequested = false;
    this->finishProperty = false;
}

void editor::InspectorWindow::setSceneWindow(SceneWindow* sceneWindow){
    this->sceneWindow = sceneWindow;
}

void editor::InspectorWindow::setOpen(bool open){
    if (open){
        if (!windowOpen){
            focusRequested = true;
        }
        windowOpen = true;
        return;
    }

    if (windowOpen) {
        stopTransientPreviews();
    }

    windowOpen = false;
    focusRequested = false;
    nameFocusFrames = 0;
}

// F2: edit the name of what the window shows — a single entity, or the main scene
void editor::InspectorWindow::focusNameInput(){
    uint32_t sceneId = project->getSelectedSceneForProperties();
    if (!project->getScene(sceneId)) sceneId = project->getSelectedSceneId();
    if (!project->getScene(sceneId)) return;

    const size_t selected = project->getSelectedEntities(sceneId).size();
    const bool mainScene = (sceneId == project->getSelectedSceneId());
    if (selected != 1 && !(selected == 0 && mainScene)) return;

    setOpen(true);
    focusRequested = true;
    nameFocusFrames = 3; // the field activates a frame after SetKeyboardFocusHere
}

void editor::InspectorWindow::requestFocus(){
    setOpen(true);
    focusRequested = true;
}

void editor::InspectorWindow::requestAddComponent(){
    setOpen(true);
    focusRequested = true;
    addComponentRequested = true;
}

void editor::InspectorWindow::stopTransientPreviews() {
    stopSoundPreview();
    stopActionPreviewIfActive();
    // the fluid previews and the Paint Dye brush (play starts, a save, the window closes)
    FluidEditorTools::stopAll(project);
    // the Vertex Animator preview (InspectorWindowVat.cpp)
    stopVertexAnimatorPreview();
}

void editor::InspectorWindow::clearSceneState(uint32_t sceneId) {
    FluidEditorTools::forgetScene(sceneId);
    // a Vertex Animator previewing in the closed scene: nothing to put back (its scene is gone)
    clearVertexAnimatorPreviewScene(sceneId);

    if (actionPreviewing && actionPreviewSceneId == sceneId) {
        // The scene is already deleted, drop the snapshots instead of restoring them
        actionPreviewSnapshot.clear();
        releaseActionPreviewOwners();
        actionPreviewing = false;
        actionPreviewPlaying = false;
        actionPreviewEntity = NULL_ENTITY;
        actionPreviewSceneId = 0;
    }

    if (soundPreviewRuntime.sceneId == sceneId) {
        stopSoundPreview(true);
    }

    // Pending writes would otherwise be flushed against whatever scene takes the id
    dirtyMaterials.erase(
        std::remove_if(dirtyMaterials.begin(), dirtyMaterials.end(),
            [sceneId](const DirtyMaterialEntry& entry){ return entry.sceneId == sceneId; }),
        dirtyMaterials.end());
}

bool editor::InspectorWindow::isOpen() const{
    return windowOpen;
}

editor::RowPropertyType editor::InspectorWindow::scriptPropertyTypeToRowPropertyType(ScriptPropertyType scriptType){
    switch (scriptType) {
        case jasperengine::ScriptPropertyType::Bool: return RowPropertyType::Bool;
        case jasperengine::ScriptPropertyType::Int: return RowPropertyType::Int;
        case jasperengine::ScriptPropertyType::Float: return RowPropertyType::Float;
        case jasperengine::ScriptPropertyType::String: return RowPropertyType::String;
        case jasperengine::ScriptPropertyType::Vector2: return RowPropertyType::Vector2;
        case jasperengine::ScriptPropertyType::Vector3: return RowPropertyType::Vector3;
        case jasperengine::ScriptPropertyType::Vector4: return RowPropertyType::Vector4;
        case jasperengine::ScriptPropertyType::Color3: return RowPropertyType::Color3L;
        case jasperengine::ScriptPropertyType::Color4: return RowPropertyType::Color4L;
        case jasperengine::ScriptPropertyType::EntityReference: return RowPropertyType::ExternalEntity;
        default: return RowPropertyType::Custom;
    }
}

Vector3 editor::InspectorWindow::roundZero(const Vector3& val, const float threshold) const{
    return Vector3(
        (fabs(val.x) < threshold) ? 0.0f : val.x,
        (fabs(val.y) < threshold) ? 0.0f : val.y,
        (fabs(val.z) < threshold) ? 0.0f : val.z
    );
}

float editor::InspectorWindow::snapDisplayedAngle(float angle, float threshold) const{
    float rounded = std::round(angle);
    return (std::fabs(angle - rounded) <= threshold) ? rounded : angle;
}

Vector3 editor::InspectorWindow::getDisplayedEulerAngles(const Quaternion& value, const RotationOrder& order, float zeroThreshold) const{
    constexpr float displaySnapThreshold = 0.05f;

    Vector3 euler = roundZero(Quaternion(value).normalize().getEulerAngles(order), zeroThreshold);
    euler.x = snapDisplayedAngle(euler.x, displaySnapThreshold);
    euler.y = snapDisplayedAngle(euler.y, displaySnapThreshold);
    euler.z = snapDisplayedAngle(euler.z, displaySnapThreshold);

    return euler;
}

bool editor::InspectorWindow::compareVectorFloat(const float* a, const float* b, size_t elements, const float threshold){
    for (size_t i = 0; i < elements; ++i) {
        if (fabs(a[i] - b[i]) > threshold) {
            return true;
        }
    }
    return false;
}

float editor::InspectorWindow::getLabelSize(std::string label, bool addRotateIconSpace){
    float iconSize = ImGui::CalcTextSize(ICON_FA_ROTATE_LEFT).x;
    float labelSize = ImGui::CalcTextSize(label.c_str()).x;
    return labelSize + (addRotateIconSpace ? iconSize : iconSize / 2.0f);
}

void editor::InspectorWindow::helpMarker(std::string desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

Texture* editor::InspectorWindow::findThumbnail(const std::string& path) {
    if (path.empty()) return nullptr;

    std::filesystem::path texPath = project->resolveAssetPath(path);

    if (!texPath.is_absolute()) return nullptr;

    std::error_code ec;
    if (!std::filesystem::exists(texPath, ec) || ec) {
        return nullptr;
    }

    const std::filesystem::path thumbnailPath = project->getThumbnailPath(texPath);
    const std::string thumbPathStr = thumbnailPath.string();

    // Fast path: return from cache if already loaded
    auto thumbIt = thumbnailTextures.find(thumbPathStr);
    if (thumbIt != thumbnailTextures.end() && !thumbIt->second.empty()) {
        return &thumbIt->second;
    }

    std::error_code thumbEc;
    const bool thumbnailExists = std::filesystem::exists(thumbnailPath, thumbEc) && !thumbEc;

    if (thumbnailExists) {
        TextureData thumbData(thumbnailPath.string().c_str());
        if (thumbData.getData() && thumbData.getSize() > 0) {
            Texture thumbTexture(thumbPathStr, thumbData);
            thumbnailTextures[thumbPathStr] = thumbTexture;
            return &thumbnailTextures[thumbPathStr];
        }
    }

    // Thumbnail missing or failed to load — request generation for all file types
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        projectWindow->requestThumbnailGeneration(texPath, thumbnailExists);
    }
    return nullptr;
}

void editor::InspectorWindow::drawImageWithBorderAndRounding(Texture* texture, const ImVec2& size, float rounding, ImU32 border_col, float border_thickness, bool flipY) {
    if (!texture) return;

    ImTextureID tex_id = Backend::getImGuiTexture(texture->getRender());
    int texWidth = texture->getWidth();
    int texHeight = texture->getHeight();

    ImVec2 cursor = ImGui::GetCursorScreenPos();

    // Calculate source aspect and target aspect
    float srcAspect = static_cast<float>(texWidth) / texHeight;
    float dstAspect = size.x / size.y;

    // Default UVs (full image)
    ImVec2 uv0(0, 0);
    ImVec2 uv1(1, 1);

    // If aspect ratios differ, calculate the crop
    if (fabs(srcAspect - dstAspect) > 1e-3f) {
        if (srcAspect > dstAspect) {
            // Source is wider; crop left and right
            float newWidth = texHeight * dstAspect;
            float x0 = (texWidth - newWidth) / 2.0f;
            uv0.x = x0 / texWidth;
            uv1.x = (x0 + newWidth) / texWidth;
        } else {
            // Source is taller; crop top and bottom
            float newHeight = texWidth / dstAspect;
            float y0 = (texHeight - newHeight) / 2.0f;
            uv0.y = y0 / texHeight;
            uv1.y = (y0 + newHeight) / texHeight;
        }
    }

    if (flipY) {
        float temp = uv0.y;
        uv0.y = uv1.y;
        uv1.y = temp;
    }

    ImVec2 p_min = cursor;
    ImVec2 p_max = ImVec2(cursor.x + size.x, cursor.y + size.y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Draw the cropped image with rounding
    Widgets::addImageRounded(draw_list, tex_id, p_min, p_max, uv0, uv1, IM_COL32_WHITE, rounding, ImDrawFlags_RoundCornersAll);

    // Draw the border
    draw_list->AddRect(p_min, p_max, border_col, rounding, ImDrawFlags_RoundCornersAll, border_thickness);

    // Reserve space for interaction
    ImGui::InvisibleButton("##image", size);
}


void editor::InspectorWindow::dragDropResourcesFont(ComponentType cpType, std::string id, size_t slot, SceneProject* sceneProject, std::vector<Entity> entities){
    // Block DnD while playing for non-script components
    if (sceneProject && sceneProject->playState != ScenePlayState::STOPPED) {
        return;
    }

    if (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) {
        return;
    }

    const std::string slotId = id + "#" + std::to_string(slot);

    auto markTextDirty = [sceneProject, cpType](Entity entity){
        if (cpType == ComponentType::TextComponent){
            sceneProject->scene->getComponent<TextComponent>(entity).needReloadAtlas = true;
            sceneProject->scene->getComponent<TextComponent>(entity).needUpdateText = true;
        }
    };

    if (ImGui::BeginDragDropTarget()){

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
            if (receivedStrings.size() > 0){
                const bool insideAssets = project->isInsideAssetsPath(receivedStrings[0]);
                const std::string droppedRelativePath = droppedAssetPath(project, receivedStrings[0], insideAssets);

                bool isFont = Util::isFontFile(droppedRelativePath);

                if (isFont) {
                    if (!hasFontDrag.count(slotId)){
                        hasFontDrag.insert(slotId);
                        for (Entity& entity : entities){
                            FontArray* valueRef = Catalog::getPropertyRef<FontArray>(sceneProject->scene, entity, cpType, id);
                            if (!valueRef)
                                continue;
                            originalFont[slotId][entity] = *valueRef;
                            if ((*valueRef)[slot] != droppedRelativePath){
                                (*valueRef)[slot] = droppedRelativePath;
                                markTextDirty(entity);
                            }
                        }
                    }
                    if (payload->IsDelivery()){
                        for (Entity& entity : entities){
                            FontArray* valueRef = Catalog::getPropertyRef<FontArray>(sceneProject->scene, entity, cpType, id);
                            if (!valueRef)
                                continue;
                            *valueRef = originalFont[slotId][entity];
                            if (!insideAssets){
                                markTextDirty(entity);
                                continue;
                            }
                            FontArray newValue = originalFont[slotId][entity];
                            newValue[slot] = droppedRelativePath;
                            cmd = new PropertyCmd<FontArray>(project, sceneProject->id, entity, cpType, id, newValue);
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
                                finishProperty = true;
                            }
                        }

                        if (!insideAssets){
                            Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                        }

                        ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                        hasFontDrag.erase(slotId);
                        originalFont.erase(slotId);
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }else{
        if (hasFontDrag.count(slotId)){
            for (Entity& entity : entities){
                FontArray* valueRef = Catalog::getPropertyRef<FontArray>(sceneProject->scene, entity, cpType, id);
                if (!valueRef)
                    continue;
                if (*valueRef != originalFont[slotId][entity]){
                    *valueRef = originalFont[slotId][entity];
                    markTextDirty(entity);
                }
            }

            hasFontDrag.erase(slotId);
            originalFont.erase(slotId);
        }
    }
}

// The Catalog property is the whole array, so a slot edit commits a modified copy of it
void editor::InspectorWindow::setFontSlot(ComponentType cpType, const std::string& id, size_t slot, const std::string& path, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged){
    for (Entity& entity : entities){
        FontArray* valueRef = Catalog::getPropertyRef<FontArray>(sceneProject->scene, entity, cpType, id);
        if (!valueRef || (*valueRef)[slot] == path)
            continue;

        FontArray newValue = *valueRef;
        newValue[slot] = path;

        cmd = new PropertyCmd<FontArray>(project, sceneProject->id, entity, cpType, id, newValue, onValueChanged);
        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        finishProperty = true;
    }
}

void editor::InspectorWindow::drawFontSlot(ComponentType cpType, const std::string& id, size_t slot, bool dif, const FontArray& value, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged, float width, const std::function<void(const ImVec2&)>& extraButton){
    ImGui::BeginGroup();
    ImGui::PushID(("font_" + id + "_" + std::to_string(slot)).c_str());

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);

    // reserve room for the action buttons, with one gap between each item and a trailing one
    ImVec2 fontButtonSize = ImGui::CalcItemSize(ImVec2(0, 0), ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    int fontButtonCount = extraButton ? 3 : 2;
    float fontButtonsReserve = fontButtonSize.x * fontButtonCount + ImGui::GetStyle().ItemSpacing.x * (fontButtonCount + 1);

    // a fill width cannot be measured by an auto sized window, those pass an explicit one
    float frameWidth = (width > 0.0f) ? std::max(1.0f, width - fontButtonsReserve) : -fontButtonsReserve;

    ImGui::BeginChild("fontframe", ImVec2(frameWidth, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2),
        false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    std::string fontName = value[slot];
    std::error_code ec;
    if (std::filesystem::exists(fontName, ec)) {
        fontName = std::filesystem::path(fontName).filename().string();
    }
    if (fontName.empty()) {
        fontName = (slot == 0) ? "< Default >" : "< Not set >";
    }

    float textWidth = ImGui::CalcTextSize(fontName.c_str()).x;
    float availWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(availWidth - textWidth - 2);
    ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
    if (dif)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::Text("%s", fontName.c_str());
    if (dif)
        ImGui::PopStyleColor();

    ImGui::EndChild();
    if (!value[slot].empty()){
        ImGui::SetItemTooltip("%s", value[slot].c_str());
    }

    ImGui::PopStyleColor();

    ImGui::SameLine();

    // no horizontal frame padding so the icons fit centered in the square buttons
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetStyle().FramePadding.y));

    if (ImGui::Button(ICON_FA_FOLDER_OPEN, fontButtonSize)) {
        std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_FONT);
        if (!path.empty()) {
            std::filesystem::path assetsPath = project->getAssetsPath();
            std::filesystem::path filePath = std::filesystem::absolute(path);

            // References are stored relative to the assets root
            auto relative = std::filesystem::relative(filePath, assetsPath, ec);
            if (ec || relative.empty() || *relative.begin() == "..") {
                ImGui::OpenPopup("File Import Error");
            }else{
                setFontSlot(cpType, id, slot, FileUtils::pathToGenericUtf8(relative), sceneProject, entities, onValueChanged);
            }
        }
    }
    ImGui::SetItemTooltip("Font file");

    ImGui::SameLine();

    ImGui::BeginDisabled(value[slot].empty());
    if (ImGui::Button(ICON_FA_XMARK, fontButtonSize)) {
        setFontSlot(cpType, id, slot, "", sceneProject, entities, onValueChanged);
    }
    ImGui::EndDisabled();
    if (!value[slot].empty()){
        ImGui::SetItemTooltip(slot == 0 ? "Use the built-in font" : "Clear this font");
    }

    if (extraButton){
        ImGui::SameLine();
        extraButton(fontButtonSize);
    }

    ImGui::PopStyleVar();

    // Error popup modal
    if (EditorModal::begin("File Import Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Selected file must be within the assets directory.");
        EditorModal::message("Selected file must be within the assets directory.");
        ImGui::Separator();

        float buttonWidth = 120;
        float windowWidth = ImGui::GetWindowSize().x;
        ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);
        if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();
    ImGui::EndGroup();

    dragDropResourcesFont(cpType, id, slot, sceneProject, entities);
}

void editor::InspectorWindow::drawFontFallbacksPopup(const char* popupId, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged){
    const float slotWidth = 16 * ImGui::GetFontSize();
    const float labelWidth = getLabelSize("Fallback 1");

    ImGui::SetNextWindowSizeConstraints(ImVec2(labelWidth + slotWidth + ImGui::GetStyle().WindowPadding.x * 2 + ImGui::GetStyle().CellPadding.x * 2, 0), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopup(popupId))
        return;

    // first selected entity is the representative state, like the texture settings popup
    PropertyData fontProp = Catalog::getProperty(sceneProject->scene, entities.front(), cpType, id);
    FontArray* current = static_cast<FontArray*>(fontProp.ref);
    if (!current){
        ImGui::EndPopup();
        return;
    }
    const FontArray defaults = fontProp.def ? *static_cast<FontArray*>(fontProp.def) : FontArray();

    ImGui::Text("Additional fonts");
    ImGui::SameLine();
    helpMarker("Used, in order, for the characters the main font does not have.");
    ImGui::Separator();

    beginTable(cpType, labelWidth, "fontfallbacks_" + id);

    for (size_t slot = 1; slot < current->size(); slot++){
        bool dif = false;
        for (Entity& entity : entities){
            FontArray* valueRef = Catalog::getPropertyRef<FontArray>(sceneProject->scene, entity, cpType, id);
            if (valueRef && (*valueRef)[slot] != (*current)[slot])
                dif = true;
        }

        std::string label = "Fallback " + std::to_string(slot);
        if (propertyHeader(label, slotWidth, (*current)[slot] != defaults[slot], false)){
            setFontSlot(cpType, id, slot, defaults[slot], sceneProject, entities, onValueChanged);
        }

        drawFontSlot(cpType, id, slot, dif, *current, sceneProject, entities, onValueChanged, slotWidth);
    }

    endTable();

    ImGui::EndPopup();
}


void editor::InspectorWindow::applyCameraTexture(Entity cameraEntity, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged){
    Scene* scene = sceneProject->scene;
    CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity);
    if (!camera)
        return;

    Texture texture = CameraTextureLink::make(scene, cameraEntity);

    auto multiCmd = new MultiPropertyCmd();
    if (!camera->renderToTexture) {
        multiCmd->addPropertyCmd<bool>(project, sceneProject->id, cameraEntity, ComponentType::CameraComponent, "renderToTexture", true);
    }
    if (sceneProject->mainCamera == cameraEntity) {
        // a render-to-texture camera cannot be the scene main camera
        multiCmd->addCommand(std::make_unique<SetMainCameraCmd>(project, sceneProject->id, NULL_ENTITY));
    }
    for (Entity& entity : entities){
        multiCmd->addPropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, texture, onValueChanged);
    }
    cmd = multiCmd;
    CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
    cmd->setNoMerge();
}

// Popup that lets the user configure the sampler settings (min/mag filter, U/V wrap), the
// alpha border fix and the SVG rasterization scale for vector sources of a texture
// property. Laid out with the project's standard "<label> | <input>" property table,
// including the rotate-left reset button when a row differs from the Catalog default. Each
// change commits a PropertyCmd that copies the current per-entity Texture and mutates only
// the chosen field (the sampler is part of the TexturePool key, so a new one is its own texture).
void editor::InspectorWindow::drawTextureSettingsPopup(const char* popupId, ComponentType cpType, const std::string& id, SceneProject* sceneProject, std::vector<Entity>& entities, std::function<void()> onValueChanged){
    ImGui::SetNextWindowSizeConstraints(ImVec2(19 * ImGui::GetFontSize(), 0), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopup(popupId))
        return;

    // Applies mutator to a copy of each selected entity's texture and commits a PropertyCmd
    // per entity whose texture actually changed. The mutator returns false to skip an entity.
    auto applyToTextures = [&](const std::function<bool(Texture&)>& mutator){
        for (Entity& entity : entities){
            Texture* valueRef = Catalog::getPropertyRef<Texture>(sceneProject->scene, entity, cpType, id);
            if (!valueRef)
                continue;

            Texture modified = *valueRef;
            if (!mutator(modified) || modified == *valueRef)
                continue;

            cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, modified, onValueChanged);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        finishProperty = true;
    };

    // Renders a "<label> | <combo>" property row for an enum-backed sampler field, matching
    // propertyRow's Enum layout, including the rotate-left reset button when the value
    // differs from the default. propertyRow itself can't be used here: it resolves ids
    // through the Catalog and commits PropertyCmd<int>, while these fields live inside the
    // Texture value (private members, not Catalog properties), so the whole Texture must be
    // committed instead. Returns true and sets outValue when the selection changes; a reset
    // click reports the default as the new selection.
    auto comboRow = [this](const char* label, const char* comboId, const std::vector<editor::EnumEntry>& entries, int currentValue, int defaultValue, int& outValue) -> bool {
        int item = 0;
        for (size_t i = 0; i < entries.size(); ++i){
            if (entries[i].value == currentValue){
                item = (int)i;
                break;
            }
        }
        std::vector<const char*> names;
        for (const auto& entry : entries)
            names.push_back(entry.name);

        bool reset = propertyHeader(label, 12 * ImGui::GetFontSize(), currentValue != defaultValue, false);
        if (ImGui::Combo(comboId, &item, names.data(), (int)names.size())){
            outValue = entries[item].value;
            return true;
        }
        if (reset){
            outValue = defaultValue;
            return true;
        }
        return false;
    };

    // Read current values from the first selected entity as the representative state; the
    // Catalog default texture provides the per-field reset targets.
    PropertyData textureProp = Catalog::getProperty(sceneProject->scene, entities.front(), cpType, id);
    Texture* current = static_cast<Texture*>(textureProp.ref);
    if (!current){
        ImGui::EndPopup();
        return;
    }
    const Texture defaults = textureProp.def ? *static_cast<Texture*>(textureProp.def) : Texture();

    // SVG rasterization scale is only relevant for vector sources.
    bool isSvg = TextureData::hasSvgExtension(current->getPath().c_str());
    float svgCurrentScale = current->getSvgScale();

    // setSvgScale re-keys the pool entry (the scale is part of the texture identity) while
    // the sampler fields stay untouched, so a scale change never resets filter/wrap.
    auto applyScale = [&](float scale){
        applyToTextures([scale](Texture& t){
            if (!TextureData::hasSvgExtension(t.getPath().c_str()))
                return false;
            t.setSvgScale(scale);
            return true;
        });
    };

    ImGui::Text("Texture settings");
    ImGui::Separator();

    beginTable(cpType, getLabelSize("Alpha Border"), "texsettings_" + id);

    struct SamplerRow {
        const char* label;
        const char* comboId;
        const std::vector<editor::EnumEntry>* entries;
        int current;
        int def;
        void (*set)(Texture&, int);
    };
    const SamplerRow samplerRows[] = {
        {"Min Filter", "##texset_min", &entriesTextureMinFilter, (int)current->getMinFilter(), (int)defaults.getMinFilter(), [](Texture& t, int v){ t.setMinFilter((TextureFilter)v); }},
        {"Mag Filter", "##texset_mag", &entriesTextureFilter, (int)current->getMagFilter(), (int)defaults.getMagFilter(), [](Texture& t, int v){ t.setMagFilter((TextureFilter)v); }},
        {"Wrap U", "##texset_wrapu", &entriesTextureWrap, (int)current->getWrapU(), (int)defaults.getWrapU(), [](Texture& t, int v){ t.setWrapU((TextureWrap)v); }},
        {"Wrap V", "##texset_wrapv", &entriesTextureWrap, (int)current->getWrapV(), (int)defaults.getWrapV(), [](Texture& t, int v){ t.setWrapV((TextureWrap)v); }},
        {"Alpha Border", "##texset_alphaborder", &entriesTextureAlphaBorder, (int)current->getAlphaBorder(), (int)defaults.getAlphaBorder(), [](Texture& t, int v){ t.setAlphaBorder((TextureAlphaBorder)v); }},
    };
    for (const SamplerRow& row : samplerRows){
        int chosen = 0;
        if (comboRow(row.label, row.comboId, *row.entries, row.current, row.def, chosen)){
            applyToTextures([&](Texture& t){ row.set(t, chosen); return true; });
        }
    }

    endTable();

    if (isSvg){
        beginTable(cpType, getLabelSize("Alpha Border"), "texsettings_svg_" + id);

        // "SVG Scale" | drag field; commit only on release so each distinct scale rasterizes
        // a new texture at most once.
        static std::map<std::string, float> svgScaleEditing;
        const std::string editKey = std::to_string(static_cast<int>(cpType)) + ":" + id;
        auto editIt = svgScaleEditing.find(editKey);
        float editScale = (editIt != svgScaleEditing.end()) ? editIt->second : svgCurrentScale;

        float svgDefaultScale = defaults.getSvgScale();

        bool resetScale = propertyHeader("SVG Scale", 6 * ImGui::GetFontSize(), std::fabs(svgCurrentScale - svgDefaultScale) > 1e-4f, false);
        if (ImGui::DragFloat("##texset_svgscale", &editScale, 0.05f, 0.1f, 16.0f, "%.2fx")) {
            svgScaleEditing[editKey] = editScale;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            svgScaleEditing.erase(editKey);
            applyScale(editScale);
        }
        if (resetScale) {
            svgScaleEditing.erase(editKey);
            applyScale(svgDefaultScale);
        }

        // "Presets" | quick-pick scale buttons (the active scale is highlighted)
        propertyHeader("Presets", -1, false, false);
        struct ScalePreset { const char* label; float value; };
        static const ScalePreset presets[] = {
            {"0.5x", 0.5f}, {"1x", 1.0f}, {"2x", 2.0f}, {"3x", 3.0f}, {"4x", 4.0f}
        };
        for (int i = 0; i < IM_ARRAYSIZE(presets); ++i){
            if (i > 0)
                ImGui::SameLine();
            bool selected = std::fabs(svgCurrentScale - presets[i].value) < 1e-4f;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x / 1.4, ImGui::GetStyle().FramePadding.y));
            if (ImGui::SmallButton(presets[i].label)) {
                applyScale(presets[i].value);
            }
            ImGui::PopStyleVar(1);
            if (selected)
                ImGui::PopStyleColor();
        }

        endTable();
    }

    ImGui::EndPopup();
}

void editor::InspectorWindow::dragDropResourcesTexture(ComponentType cpType, std::string id, SceneProject* sceneProject, std::vector<Entity> entities){
    // Block DnD while playing for non-script components
    if (sceneProject && sceneProject->playState != ScenePlayState::STOPPED) {
        return;
    }

    if (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) {
        return;
    }

    if (ImGui::BeginDragDropTarget()){

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
            if (receivedStrings.size() > 0){
                const bool insideAssets = project->isInsideAssetsPath(receivedStrings[0]);
                const std::string droppedRelativePath = droppedAssetPath(project, receivedStrings[0], insideAssets);

                if (Util::isImageFile(droppedRelativePath)) {
                    if (!hasTextureDrag.count(id)){
                        hasTextureDrag.insert(id);
                        for (Entity& entity : entities){
                            Texture* valueRef = Catalog::getPropertyRef<Texture>(sceneProject->scene, entity, cpType, id);
                            originalTex[id][entity] = Texture(*valueRef);
                            if (*valueRef != Texture(droppedRelativePath)){
                                *valueRef = Texture(droppedRelativePath);
                                markTextureUpdate(sceneProject->scene, entity, cpType, id);
                            }
                        }
                    }
                    if (payload->IsDelivery()){
                        Texture texture(droppedRelativePath);
                        for (Entity& entity : entities){
                            Texture* valueRef = Catalog::getPropertyRef<Texture>(sceneProject->scene, entity, cpType, id);
                            *valueRef = originalTex[id][entity];
                            if (!insideAssets){
                                markTextureUpdate(sceneProject->scene, entity, cpType, id);
                                continue;
                            }
                            cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, texture);
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
                                finishProperty = true;
                            }
                        }

                        if (!insideAssets){
                            Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                        }

                        ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                        hasTextureDrag.erase(id);
                        originalTex.erase(id);
                    }
                }
            }
        }

        // camera entity dragged from the structure window (render to texture)
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity")) {
            const EntityPayload* entityPayload = reinterpret_cast<const EntityPayload*>(payload->Data);
            bool sameScene = sameSceneDropProblem(*project, *entityPayload, sceneProject->id).empty();
            if (sameScene && sceneProject->scene->findComponent<CameraComponent>(entityPayload->entity)) {
                applyCameraTexture(entityPayload->entity, cpType, id, sceneProject, entities, nullptr);
                finishProperty = true;
                ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
            }
        }
        ImGui::EndDragDropTarget();
    }else{
        if (hasTextureDrag.count(id)){
            for (Entity& entity : entities){
                Texture* valueRef = Catalog::getPropertyRef<Texture>(sceneProject->scene, entity, cpType, id);
                if (*valueRef != originalTex[id][entity]){
                    *valueRef = originalTex[id][entity];
                    markTextureUpdate(sceneProject->scene, entity, cpType, id);
                }
            }

            hasTextureDrag.erase(id);
            originalTex.erase(id);
        }
    }
}

void editor::InspectorWindow::dragDropResourcesTextureCube(ComponentType cpType, const std::string& id, const ImVec2& rectMin, const ImVec2& rectMax, SceneProject* sceneProject, const std::vector<Entity>& entities, int faceIndex){
    // Block DnD while playing for non-script components
    if (sceneProject && sceneProject->playState != ScenePlayState::STOPPED) {
        return;
    }

    const std::string dragId = faceIndex < 0 ? id + "##cube_single" : id + "##cube_face_" + std::to_string(faceIndex);

    const ImRect rect(rectMin, rectMax);
    if (ImGui::BeginDragDropTargetCustom(rect, ImGui::GetID(dragId.c_str()))){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
            if (!receivedStrings.empty()){
                const bool insideAssets = project->isInsideAssetsPath(receivedStrings[0]);
                const std::string droppedRelativePath = droppedAssetPath(project, receivedStrings[0], insideAssets);

                if (Util::isImageFile(droppedRelativePath)) {
                    if (!hasTextureDrag.count(dragId)){
                        hasTextureDrag.insert(dragId);
                        for (const Entity& entity : entities){
                            Texture* valueRef = Catalog::getPropertyRef<Texture>(sceneProject->scene, entity, cpType, id);
                            originalTex[dragId][entity] = Texture(*valueRef);

                            Texture updated = Texture(*valueRef);
                            if (faceIndex < 0){
                                updated.setCubeMap(droppedRelativePath);
                            }else{
                                updated.setCubePath(faceIndex, droppedRelativePath);
                            }
                            if (*valueRef != updated){
                                *valueRef = updated;
                                markTextureUpdate(sceneProject->scene, entity, cpType, id);
                            }
                        }
                    }

                    if (payload->IsDelivery()){
                        for (const Entity& entity : entities){
                            Texture* valueRef = Catalog::getPropertyRef<Texture>(sceneProject->scene, entity, cpType, id);
                            *valueRef = originalTex[dragId][entity];
                            if (!insideAssets){
                                markTextureUpdate(sceneProject->scene, entity, cpType, id);
                                continue;
                            }

                            Texture updated = Texture(*valueRef);
                            if (faceIndex < 0){
                                updated.setCubeMap(droppedRelativePath);
                            }else{
                                updated.setCubePath(faceIndex, droppedRelativePath);
                            }
                            cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, updated);
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
                                finishProperty = true;
                            }
                        }

                        if (!insideAssets){
                            Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                        }

                        ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                        hasTextureDrag.erase(dragId);
                        originalTex.erase(dragId);
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }else{
        if (hasTextureDrag.count(dragId)){
            for (const Entity& entity : entities){
                Texture* valueRef = Catalog::getPropertyRef<Texture>(sceneProject->scene, entity, cpType, id);
                if (*valueRef != originalTex[dragId][entity]){
                    *valueRef = originalTex[dragId][entity];
                    markTextureUpdate(sceneProject->scene, entity, cpType, id);
                }
            }

            hasTextureDrag.erase(dragId);
            originalTex.erase(dragId);
        }
    }
}

void editor::InspectorWindow::handleComponentMenu(const char* popupId, SceneProject* sceneProject, std::vector<Entity> entities, ComponentType cpType, bool isPrefab, bool& headerOpen, bool readOnly) {
    if (ImGui::BeginPopup(popupId)) {
        // Paste reflects what another editor instance copied
        if (ImGui::IsWindowAppearing()) {
            EntityClipboard::refresh();
        }
        ImGui::TextDisabled("%s", Catalog::getComponentDisplayName(cpType).c_str());
        ImGui::Separator();

        ImGui::BeginDisabled(readOnly); // disable all actions while playing

        auto addMenuCommand = [this](Command* command) {
            if (command) {
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(command);
            }
        };
        auto reasonTooltip = [](bool enabled, const std::string& reason) {
            if (!enabled && !reason.empty()) {
                ImGui::SetItemTooltip("%s", reason.c_str());
            }
        };

        std::string reason;
        const bool canReset = ComponentValuesCmd::canReset(project, sceneProject->id, entities, cpType, &reason);
        if (ImGui::MenuItem("Reset", nullptr, false, canReset)) {
            addMenuCommand(ComponentValuesCmd::buildReset(project, sceneProject->id, entities, cpType));
        }
        reasonTooltip(canReset, reason);
        if (cpType == ComponentType::ModelComponent && entities.size() == 1) {
            // a model root: the Ragdoll wizard over its bones
            if (ImGui::MenuItem(ICON_FA_PERSON_FALLING " Create Ragdoll...")) {
                RagdollWizardDialog::instance().open(project, sceneProject->id, entities[0]);
            }
        }
        ImGui::Separator();

        bool canRemove = !(cpType == ComponentType::Transform && isPrefab);
        if (ImGui::MenuItem("Remove Component", nullptr, false, canRemove)) {
            for (Entity& entity : entities){
                cmd = new RemoveComponentCmd(project, sceneProject->id, entity, cpType);
                CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
            }
            cmd->setNoMerge();

            headerOpen = false;
        }

        ImGui::EndDisabled();

        // S29: the component against the version control baseline
        drawVcsComponentMenuItems(sceneProject, entities, cpType, readOnly);

        // S17: a hot reload patches an .html document in place; this one loads it again as before
        if (cpType == ComponentType::UIDocumentComponent && !entities.empty()) {
            std::shared_ptr<UIDocumentSystem> documents = sceneProject->scene->getSystem<UIDocumentSystem>();
            const bool loaded = documents && documents->isDocumentLoaded(entities.front());
            if (ImGui::MenuItem("Reload (Rebuild)", nullptr, false, loaded)) {
                for (Entity entity : entities)
                    documents->reloadDocument(entity, true, UIReloadMode::REBUILD);
                sceneProject->needUpdateRender = true;
            }
            ImGui::SetItemTooltip("Load the document again: new elements, scripts run again, onUnload and onLoad (Reload patches it in place)");
            ImGui::Separator();
        }
        // the project's editor scripts: CONTEXT/<Component>/... items (S23)
        {
            EditorMenuRunContext scriptContext;
            scriptContext.sceneId = sceneProject->id;
            for (Entity entity : entities) scriptContext.selection.push_back(entity);
            // the component as CONTEXT/<name>/ names it: Mesh, MeshComponent or its display name
            std::vector<std::string> names;
            for (const std::string& name : {Catalog::getComponentName(cpType, true), Catalog::getComponentName(cpType),
                                            Catalog::getComponentDisplayName(cpType)}) {
                if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            }
            for (const std::string& name : names) EditorScripts::get().drawContextItems("component", name, scriptContext);
        }

        // Copying only reads, so a running scene's values can be copied to paste after Stop
        const bool canCopy = cpType != ComponentType::PrefabComponent && !entities.empty();
        if (ImGui::MenuItem("Copy Component", nullptr, false, canCopy)) {
            EntityClipboard::copyComponent(project, sceneProject->id, entities.front(), cpType);
        }

        // Play Mode Changes: the values a script gives the component are offered at Stop
        const bool playing = sceneProject->playState == ScenePlayState::PLAYING || sceneProject->playState == ScenePlayState::PAUSED;
        if (playing && !project->isPlayRuntimeScene(sceneProject->id) && !entities.empty()) {
            const bool marked = PlayChangeReview::get().isMarked(sceneProject->id, entities.front(), cpType);
            if (ImGui::MenuItem("Keep Component Values After Play", nullptr, marked, !marked)) {
                for (Entity entity : entities) {
                    PlayChangeReview::get().markComponent(sceneProject->id, entity, cpType);
                }
            }
            ImGui::SetItemTooltip("At Stop, Play Mode Changes offers to keep this component's values as they are then");
        }

        ImGui::BeginDisabled(readOnly);

        std::shared_ptr<const ComponentClip> copied = EntityClipboard::peekComponent(project);
        reason.clear();
        const bool canPasteNew = ComponentValuesCmd::canPasteAsNew(project, sceneProject->id, entities, copied.get(), &reason);
        if (ImGui::MenuItem("Paste Component As New", nullptr, false, canPasteNew)) {
            if (std::shared_ptr<const ComponentClip> clip = EntityClipboard::readComponent(project)) {
                addMenuCommand(ComponentValuesCmd::buildPasteAsNew(project, sceneProject->id, entities, clip));
            }
        }
        if (canPasteNew) {
            ImGui::SetItemTooltip("Adds the copied %s", Catalog::getComponentDisplayName(copied->type).c_str());
        }
        reasonTooltip(canPasteNew, reason);

        reason.clear();
        const bool canPasteValues = ComponentValuesCmd::canPasteValues(project, sceneProject->id, entities, cpType, copied.get(), &reason);
        if (ImGui::MenuItem("Paste Component Values", nullptr, false, canPasteValues)) {
            if (std::shared_ptr<const ComponentClip> clip = EntityClipboard::readComponent(project)) {
                addMenuCommand(ComponentValuesCmd::buildPasteValues(project, sceneProject->id, entities, cpType, clip));
            }
        }
        reasonTooltip(canPasteValues, reason);

        ImGui::EndDisabled();

        // Unity's "Modified Component" / "Added Component" entries: Apply to the prefab(s), Revert
        if (isPrefab){
            drawPrefabApplyRevertItems(sceneProject, entities, cpType, std::string(), 0, readOnly);
        }

        ImGui::EndPopup();
    }
}

void editor::InspectorWindow::addComponents(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType cpType) {
    // Add component to all selected entities
    Command* last = nullptr;
    for (const Entity& entity : entities) {
        cmd = new AddComponentCmd(project, sceneProject->id, entity, cpType);
        last = cmd;
        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
    }
    if (last) last->setNoMerge();

    // Mark scene as modified
    sceneProject->isModified = true;
}

bool editor::InspectorWindow::canAddComponentToSelection(ComponentType type) {
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene || project->isAnyScenePlaying()) return false;
    const std::vector<Entity>& entities = project->getSelectedEntities(sceneProject->id);
    if (entities.empty()) return false;
    for (Entity entity : entities) {
        if (!canAddComponent(sceneProject, entity, type)) return false;
    }
    return true;
}

bool editor::InspectorWindow::addComponentToSelection(ComponentType type, std::string* why) {
    if (!canAddComponentToSelection(type)) {
        if (why) *why = "Select GameObjects that do not have a " + Catalog::getComponentDisplayName(type) + " yet (not while playing).";
        return false;
    }
    SceneProject* sceneProject = project->getSelectedScene();
    addComponents(sceneProject, project->getSelectedEntities(sceneProject->id), type);
    return true;
}

bool editor::InspectorWindow::canAddComponent(SceneProject* sceneProject, Entity entity, ComponentType cpType) {
    // Check if entity already has this component
    std::vector<ComponentType> existingComponents = Catalog::findComponents(sceneProject->scene, entity);
    if (std::find(existingComponents.begin(), existingComponents.end(), cpType) != existingComponents.end())
        return false;
    // a character controller and a rigidbody of its dimension never share a GameObject
    return Catalog::getComponentConflict(sceneProject->scene, entity, cpType).empty();
}

// Prefab (prefab) text color, as in the Hierarchy
static const ImVec4 prefabTextColor = ImVec4(0.4f, 0.7f, 1.0f, 1.0f);

// Unity draws component names bold; a second pass one pixel over thickens the regular font
static void addBoldText(ImDrawList* drawList, const ImVec2& pos, ImU32 color, const char* text){
    drawList->AddText(pos, color, text);
    drawList->AddText(ImVec2(pos.x + 1.0f, pos.y), color, text);
}

static std::string layerDisplayName(int layer){
    std::string name = TagManager::layerToName(layer);
    return name.empty() ? "Layer " + std::to_string(layer) : name;
}

void editor::InspectorWindow::drawScenePlanarReflections(SceneProject* sceneProject, ImGuiTableFlags tableFlags){
    ImGui::SeparatorText("Planar Reflections");
    if (!ImGui::BeginTable("scene_planar_reflection_table", 2, tableFlags))
        return;
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Clip Plane Offset"));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

    const float comboWidth = ImGui::CalcTextSize("Quarter").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    drawScenePropertyRow<PlanarReflectionResolution>(sceneProject, "planar_reflection_resolution", "Resolution", ScenePropertyInputType::Combo, comboWidth, 0.0f, 1.0f,
        "Water with Planar Reflection shows the scene mirrored in it: each camera draws the scene again, mirrored, once for every water height it sees. "
        "This is the size of that pass, per side of the camera's view: Half (default) draws a quarter of the pixels, Quarter a sixteenth (mobile). "
        "Off draws none: such water shows the sky or reflection probe reflection instead.");
    if (jasperengine::editor::Catalog::getSceneProperty<PlanarReflectionResolution>(sceneProject->scene, "planar_reflection_resolution") != PlanarReflectionResolution::OFF) {
        drawScenePropertyRow<uint32_t>(sceneProject, "planar_reflection_layers", "Layers", ScenePropertyInputType::LayerMask, -1.0f, 0.0f, 1.0f,
            "The layers the reflection draws (with the camera's Culling Mask): leave out small or far objects to save draws.");
        drawScenePropertyRow<bool>(sceneProject, "planar_reflection_shadows", "Shadows", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
            "The reflection samples the camera's shadow maps (no shadow map is drawn for it). Off: the reflected scene is lit unshadowed, cheaper on mobile.");
        drawScenePropertyRow<float>(sceneProject, "planar_reflection_far_clip", "Far Clip Plane", ScenePropertyInputType::DragFloat, -1.0f, 0.0f, 100000.0f,
            "How far the reflection draws (metres; 0: as far as the camera). A shorter distance culls more objects.");
        drawScenePropertyRow<float>(sceneProject, "planar_reflection_clip_offset", "Clip Plane Offset", ScenePropertyInputType::DragFloat, -1.0f, -10.0f, 10.0f,
            "The reflection leaves out what is within this distance above the water's rest level (Unity's Clip Plane Offset), so objects crossing the surface do not show their wet parts mirrored.");
        drawScenePropertyRow<int>(sceneProject, "max_planar_reflections", "Max Planes", ScenePropertyInputType::DragInt, -1.0f, 1.0f, 4.0f,
            "The most water heights a camera reflects (the nearest ones); water at other heights shows the sky or reflection probe reflection.");
    }

    // what the last frame drew for them
    const RenderSystem::PlanarReflectionStats stats = sceneProject->scene->getSystem<RenderSystem>()->getPlanarReflectionStats();
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("In Use");
    ImGui::TableSetColumnIndex(1);
    if (stats.passes > 0) {
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_GAUGE_HIGH " %u pass%s, %u x %u", stats.passes, stats.passes == 1 ? "" : "es", stats.width, stats.height);
    } else {
        ImGui::TextDisabled("None");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        if (stats.passes > 0 || stats.fallbacks > 0)
            ImGui::Text("Last frame: %u water height%s seen, %u reflection pass%s drawn, %u over Max Planes (showing the sky reflection).",
                        stats.planes, stats.planes == 1 ? "" : "s", stats.passes, stats.passes == 1 ? "" : "es", stats.fallbacks);
        else
            ImGui::TextUnformatted("Last frame drew no planar reflection: no visible water with Planar Reflection, so they cost nothing.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::EndTable();
}

void editor::InspectorWindow::drawSceneWaterWakes(SceneProject* sceneProject, ImGuiTableFlags tableFlags){
    ImGui::SeparatorText("Water Wakes");
    if (!ImGui::BeginTable("scene_water_wakes_table", 2, tableFlags))
        return;
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Resolution"));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

    const float comboWidth = ImGui::CalcTextSize("Medium (256)").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    drawScenePropertyRow<WaterWakeResolution>(sceneProject, "water_wake_resolution", "Resolution", ScenePropertyInputType::Combo, comboWidth, 0.0f, 1.0f,
        "The wakes of Water Wake components live in one map around the main camera, updated on the CPU and sent to the GPU each frame "
        "while a wake shows. Auto: 256 texels a side, 128 on mobile and web.");
    drawScenePropertyRow<float>(sceneProject, "water_wake_area", "Area", ScenePropertyInputType::DragFloat, -1.0f, 8.0f, 1024.0f,
        "Metres the wake map spans around the main camera; wakes farther away are not drawn. A smaller area shows finer wakes.");
    drawScenePropertyRow<float>(sceneProject, "water_wake_fade", "Fade", ScenePropertyInputType::DragFloat, -1.0f, 0.1f, 120.0f,
        "Seconds a wake takes to fade (an eighth of its foam is left by then, as scattered foam).");
    ImGui::EndTable();
}

// Unity's mask field label: Nothing, Everything, the one layer's name, or Mixed...
static std::string layerMaskLabel(uint32_t mask){
    return editor::InspectorWindow::sceneLayerMaskLabel(mask);
}

std::string editor::InspectorWindow::sceneLayerMaskLabel(uint32_t mask){
    if (mask == LAYER_MASK_NOTHING) return "Nothing";
    if (mask == LAYER_MASK_EVERYTHING) return "Everything";
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        if (mask == (1u << layer)){
            std::string name = TagManager::layerToName(layer);
            if (!name.empty()) return name;
            break;
        }
    }
    return "Mixed...";
}

void editor::InspectorWindow::drawGameObjectHeader(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& names, bool readOnly){
    Scene* scene = sceneProject->scene;
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool single = entities.size() == 1;

    bool active = scene->isEntityActive(entities[0]);
    bool activeMixed = false;
    for (Entity entity : entities){
        if (scene->isEntityActive(entity) != active){
            activeMixed = true;
        }
    }
    const bool isPrefab = !project->findPrefabPathFor(sceneProject->id, entities[0]).empty();

    // Unity: an overridden name, active state, tag or layer shows bold, with the blue bar
    auto stateOverridden = [&](uint8_t bit){
        for (Entity entity : entities){
            if (project->getPrefabObjectOverrides(sceneProject->id, entity).state & bit){
                return true;
            }
        }
        return false;
    };
    const bool nameOverridden = stateOverridden(PrefabObjectOverrides::StateName);
    const bool tagOverridden = stateOverridden(PrefabObjectOverrides::StateTag);
    const bool layerOverridden = stateOverridden(PrefabObjectOverrides::StateLayer);

    ImGui::PushID("gameobject_header");

    // Row 1: active checkbox, GameObject icon and name
    ImGui::BeginDisabled(readOnly);
    bool activeValue = active && !activeMixed;
    if (activeMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool activeClicked = ImGui::Checkbox("##active", &activeValue);
    UIProbe::item("inspector.active");
    if (activeClicked && recordPropertyToggle(sceneProject, entities, ComponentType::Transform, ClipRecordHooks::activeProperty(),
                                              activeMixed ? true : activeValue)){
        // Record keyed it (gameobject.active, anim-properties): no scene step
    }else if (activeClicked){
        // after this frame's drawing: in Play it runs onEnable / onDisable (deferredCommands)
        const uint32_t commandSceneId = project->getSelectedSceneId();
        const uint32_t stateSceneId = sceneProject->id;
        const std::vector<Entity> targets = entities;
        const bool value = activeMixed ? true : activeValue;
        deferredCommands.push_back([this, commandSceneId, stateSceneId, targets, value]() {
            if (!project->getScene(stateSceneId)) return;
            CommandHandle::get(commandSceneId)->addCommandNoMerge(EntityStateCmd::active(project, stateSceneId, targets, value));
        });
    }
    if (activeMixed) ImGui::PopItemFlag();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Active: an inactive GameObject and its children are not rendered, simulated or updated");
    const float headerRowTop = ImGui::GetItemRectMin().y;
    const float headerRowBottom = ImGui::GetItemRectMax().y;
    prefabStateContext(sceneProject, entities, PrefabObjectOverrides::StateActive, readOnly, headerRowTop, headerRowBottom);

    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImVec4 iconColor = isPrefab ? prefabTextColor : style.Colors[ImGuiCol_Text];
    if (!activeValue && !activeMixed) iconColor.w *= 0.5f;
    ImGui::TextColored(iconColor, ICON_FA_CUBE);
    if (isPrefab) ImGui::SetItemTooltip("Prefab instance");

    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(-1);
    static char nameBuffer[128];
    strncpy(nameBuffer, single ? names.c_str() : "-", sizeof(nameBuffer) - 1);
    nameBuffer[sizeof(nameBuffer) - 1] = '\0';
    ImGui::BeginDisabled(!single || readOnly);
    if (nameFocusFrames > 0 && single) ImGui::SetKeyboardFocusHere();
    if (nameOverridden) ImGui::PushFont(App::getBoldFont(), 0.0f);
    // an object a script made without a name: "GameObject <id>" as the hint (the name stays empty)
    const std::string nameHint = single ? entityDisplayName(std::string(), entities[0]) : std::string();
    ImGui::InputTextWithHint("##input_name", nameHint.c_str(), nameBuffer, IM_ARRAYSIZE(nameBuffer));
    if (nameOverridden) ImGui::PopFont();
    if (ImGui::IsItemActive()) nameFocusFrames = 0;
    ImGui::EndDisabled();
    prefabStateContext(sceneProject, entities, PrefabObjectOverrides::StateName, readOnly, headerRowTop, headerRowBottom);
    if (ImGui::IsItemDeactivatedAfterEdit() && single) {
        if (nameBuffer[0] != '\0' && strcmp(nameBuffer, scene->getEntityName(entities[0]).c_str()) != 0) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new EntityNameCmd(project, sceneProject->id, entities[0], nameBuffer));
        }
    }
    if (!single){
        ImGui::SetItemTooltip("%zu GameObjects selected: %s", entities.size(), names.c_str());
    }

    // Row 2: Tag and Layer side by side
    const float tagLabelWidth = ImGui::CalcTextSize("Tag").x;
    const float layerLabelWidth = ImGui::CalcTextSize("Layer").x;
    const float gap = style.ItemSpacing.x * 2.0f;
    const float comboWidth = std::max(Theme::dpi(40.0f),
        (ImGui::GetContentRegionAvail().x - tagLabelWidth - layerLabelWidth - style.ItemInnerSpacing.x * 2.0f - gap) * 0.5f);

    ImGui::AlignTextToFramePadding();
    if (tagOverridden) ImGui::PushFont(App::getBoldFont(), 0.0f);
    ImGui::TextUnformatted("Tag");
    if (tagOverridden) ImGui::PopFont();
    const float tagRowTop = ImGui::GetItemRectMin().y;
    const float tagRowBottom = tagRowTop + ImGui::GetFrameHeight();
    prefabStateContext(sceneProject, entities, PrefabObjectOverrides::StateTag, readOnly, tagRowTop, tagRowBottom);
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    drawTagCombo(sceneProject, entities, readOnly, comboWidth);
    prefabStateContext(sceneProject, entities, PrefabObjectOverrides::StateTag, readOnly, tagRowTop, tagRowBottom);
    ImGui::SameLine(0.0f, gap);
    ImGui::AlignTextToFramePadding();
    if (layerOverridden) ImGui::PushFont(App::getBoldFont(), 0.0f);
    ImGui::TextUnformatted("Layer");
    if (layerOverridden) ImGui::PopFont();
    prefabStateContext(sceneProject, entities, PrefabObjectOverrides::StateLayer, readOnly, tagRowTop, tagRowBottom);
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    drawLayerCombo(sceneProject, entities, readOnly, -1.0f);
    prefabStateContext(sceneProject, entities, PrefabObjectOverrides::StateLayer, readOnly, tagRowTop, tagRowBottom);

    // Row 3: Unity's Prefab bar, for the outermost instance holding each selected GameObject
    drawPrefabBar(sceneProject, entities, readOnly, gap);

    ImGui::PopID();
}

void editor::InspectorWindow::drawTagCombo(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, float width){
    Scene* scene = sceneProject->scene;

    std::string tag = scene->getEntityTag(entities[0]);
    bool mixed = false;
    for (Entity entity : entities){
        if (scene->getEntityTag(entity) != tag){
            mixed = true;
        }
    }

    auto applyTag = [&](const std::string& newTag){
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            EntityStateCmd::tag(project, sceneProject->id, entities, newTag));
    };

    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(readOnly);
    if (ImGui::BeginCombo("##tag", mixed ? "-" : tag.c_str())){
        // the project's tags (built-in first); a GameObject may still hold a removed one,
        // which only shows as its current value, as in Unity
        for (const std::string& option : TagManager::getAllTags()){
            bool selected = !mixed && option == tag;
            if (ImGui::Selectable(option.c_str(), selected) && !selected){
                applyTag(option);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        // Unity opens the Tags and Layers settings to add one
        if (ImGui::Selectable("Add Tag...")){
            tagsAndLayersRequested = true;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (!mixed && !TagManager::isTagDefined(tag)){
        ImGui::SetItemTooltip("Tag \"%s\" is not in Project Settings > Tags and Layers", tag.c_str());
    }
}

bool editor::InspectorWindow::consumeTagsAndLayersRequest(){
    bool requested = tagsAndLayersRequested;
    tagsAndLayersRequested = false;
    return requested;
}

bool editor::InspectorWindow::consumeShaderGlobalsRequest(){
    bool requested = shaderGlobalsRequested;
    shaderGlobalsRequested = false;
    return requested;
}

// The entities and every Transform descendant, each once
static std::vector<Entity> withDescendants(Scene* scene, const std::vector<Entity>& entities){
    std::vector<Entity> all = entities;
    auto transforms = scene->getComponentArray<Transform>();
    for (Entity entity : entities){
        if (!scene->findComponent<Transform>(entity)) continue;
        size_t index = transforms->getIndex(entity);
        size_t branchEnd = scene->findBranchLastIndex(entity);
        for (size_t i = index + 1; i <= branchEnd; i++){
            Entity descendant = transforms->getEntity(i);
            if (std::find(all.begin(), all.end(), descendant) == all.end()){
                all.push_back(descendant);
            }
        }
    }
    return all;
}

static bool hasTransformChildren(Scene* scene, const std::vector<Entity>& entities){
    auto transforms = scene->getComponentArray<Transform>();
    for (Entity entity : entities){
        if (scene->findComponent<Transform>(entity) && scene->findBranchLastIndex(entity) > transforms->getIndex(entity)){
            return true;
        }
    }
    return false;
}

void editor::InspectorWindow::drawLayerCombo(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, float width){
    Scene* scene = sceneProject->scene;

    int layer = scene->getEntityLayer(entities[0]);
    bool mixed = false;
    for (Entity entity : entities){
        if (scene->getEntityLayer(entity) != layer){
            mixed = true;
        }
    }

    auto applyLayer = [&](const std::vector<Entity>& targets, int newLayer){
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            EntityStateCmd::layer(project, sceneProject->id, targets, newLayer));
    };

    int chosenLayer = -1;
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(readOnly);
    if (ImGui::BeginCombo("##layer", mixed ? "-" : layerDisplayName(layer).c_str())){
        // Unity lists the named layers as "N: Name"
        for (int option = 0; option < MAX_LAYERS; option++){
            std::string name = TagManager::layerToName(option);
            if (name.empty()) continue;
            bool selected = !mixed && option == layer;
            if (ImGui::Selectable((std::to_string(option) + ": " + name).c_str(), selected) && !selected){
                chosenLayer = option;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Add Layer...")){
            tagsAndLayersRequested = true;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (chosenLayer >= 0){
        if (hasTransformChildren(scene, entities)){
            // Unity asks whether the children follow
            pendingLayerSceneId = sceneProject->id;
            pendingLayerEntities = entities;
            pendingLayer = chosenLayer;
            ImGui::OpenPopup("Change Layer##change_layer_children");
        }else{
            applyLayer(entities, chosenLayer);
        }
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin("Change Layer##change_layer_children", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)){
        const std::string layerName = TagManager::layerToName(pendingLayer);
        ImGui::Text("Do you want to set layer to %s for all child objects as well?",
            layerName.empty() ? std::to_string(pendingLayer).c_str() : layerName.c_str());
        EditorModal::message("Do you want to set layer to " + (layerName.empty() ? std::to_string(pendingLayer) : layerName) +
                             " for all child objects as well?");
        ImGui::Spacing();

        int choice = 0; // 1 children, 2 this object only, 3 cancel
        if (EditorModal::button("Yes, change children", "yes")) choice = 1;
        ImGui::SameLine();
        if (EditorModal::button("No, this object only", "no")) choice = 2;
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) choice = 3;

        if (choice != 0){
            // the selection may have changed scene or lost entities while the dialog was open
            if (choice != 3 && pendingLayerSceneId == sceneProject->id){
                std::vector<Entity> targets;
                for (Entity entity : pendingLayerEntities){
                    if (scene->isEntityCreated(entity)) targets.push_back(entity);
                }
                if (!targets.empty()){
                    applyLayer(choice == 1 ? withDescendants(scene, targets) : targets, pendingLayer);
                }
            }
            pendingLayerEntities.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool editor::InspectorWindow::drawComponentHeader(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType cpType, bool isPrefab, bool readOnly){
    Scene* scene = sceneProject->scene;
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::string displayName = Catalog::getComponentDisplayName(cpType);

    // Unity: a component with overridden values gets the blue margin bar, one the instance
    // added a "+" badge too
    bool prefabOverridden = false;
    bool prefabAdded = false;
    if (isPrefab){
        for (Entity entity : entities){
            const Prefab::ObjectOverrides overrides = project->getPrefabObjectOverrides(sceneProject->id, entity);
            prefabOverridden = prefabOverridden || overrides.hasComponent(cpType);
            prefabAdded = prefabAdded || overrides.addedComponents.count(cpType) != 0;
        }
    }

    // component checkbox over the selection, mixed when the entities disagree
    bool hasEnabled = false;
    bool enabled = true;
    bool enabledMixed = false;
    if (Catalog::hasEnabledProperty(cpType)){
        for (Entity entity : entities){
            PropertyData prop = Catalog::findProperty(scene, entity, cpType, "enabled");
            if (!prop.ref) continue;
            bool value = *static_cast<bool*>(prop.ref);
            if (!hasEnabled){
                enabled = value;
                hasEnabled = true;
            }else if (value != enabled){
                enabledMixed = true;
            }
        }
    }

    ImGui::PushID(static_cast<int>(cpType));

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, Theme::dpi(3.0f)));
    Theme::pushFoldoutHeaderColors();
    bool open = ImGui::CollapsingHeader("##component_header", ImGuiTreeNodeFlags_AllowOverlap);
    Theme::popFoldoutHeaderColors();
    ImGui::PopStyleVar();
    const ImVec2 headerMin = ImGui::GetItemRectMin();
    const ImVec2 headerMax = ImGui::GetItemRectMax();
    const bool headerHovered = ImGui::IsItemHovered();
    const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const ImVec2 afterHeader = ImGui::GetCursorScreenPos();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // Unity separates components with a dark line over each header
    drawList->AddLine(headerMin, ImVec2(headerMax.x, headerMin.y), ImGui::GetColorU32(ImGuiCol_Border), Theme::dpi(1.0f));

    const float height = headerMax.y - headerMin.y;
    const float fontSize = ImGui::GetFontSize();
    const float textY = headerMin.y + (height - fontSize) * 0.5f;
    // past the foldout arrow CollapsingHeader draws at the frame padding
    float x = headerMin.x + style.FramePadding.x + fontSize + style.ItemInnerSpacing.x;

    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);

    if (prefabOverridden || prefabAdded){
        drawPrefabOverrideBar(headerMin.y, headerMax.y);
    }

    const char* icon = Catalog::getComponentIcon(cpType);
    const float iconBox = fontSize * 1.25f;
    drawList->AddText(ImVec2(x + (iconBox - ImGui::CalcTextSize(icon).x) * 0.5f, textY), textColor, icon);
    if (prefabAdded){
        // Unity's "+" badge over the icon of an added component
        const float badge = fontSize * 0.55f;
        const ImVec2 center(x + iconBox - badge * 0.35f, textY + fontSize - badge * 0.35f);
        drawList->AddCircleFilled(center, badge * 0.62f, IM_COL32(40, 40, 40, 255));
        drawList->AddCircleFilled(center, badge * 0.5f, IM_COL32(60, 160, 255, 255));
        const float arm = badge * 0.3f;
        drawList->AddLine(ImVec2(center.x - arm, center.y), ImVec2(center.x + arm, center.y), IM_COL32(255, 255, 255, 255), Theme::dpi(1.5f));
        drawList->AddLine(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y + arm), IM_COL32(255, 255, 255, 255), Theme::dpi(1.5f));
    }
    x += iconBox + style.ItemInnerSpacing.x;

    if (hasEnabled){
        const float boxPadding = Theme::dpi(1.0f);
        const float boxSize = fontSize + boxPadding * 2.0f;
        ImGui::SetCursorScreenPos(ImVec2(x, headerMin.y + (height - boxSize) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(boxPadding, boxPadding));
        bool value = enabled && !enabledMixed;
        ImGui::BeginDisabled(readOnly);
        if (enabledMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        ClipPropertyKey enabledKey;
        ClipRecordHooks::propertyOf(cpType, "enabled", enabledKey);
        if (ImGui::Checkbox("##enabled", &value) &&
            !recordPropertyToggle(sceneProject, entities, cpType, enabledKey, enabledMixed ? true : value)){
            const bool newValue = enabledMixed ? true : value;
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities){
                multiCmd->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, "enabled", newValue);
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }
        if (enabledMixed) ImGui::PopItemFlag();
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("Enabled: a disabled %s is skipped at runtime", displayName.c_str());
        x = ImGui::GetItemRectMax().x + style.ItemInnerSpacing.x;
    }

    addBoldText(drawList, ImVec2(x, textY), textColor, displayName.c_str());

    // kebab button at the right edge, same menu as a right click on the header
    const char* popupId = "component_menu";
    const float buttonWidth = fontSize + style.FramePadding.x;
    ImGui::SetCursorScreenPos(ImVec2(headerMax.x - buttonWidth - style.FramePadding.x * 0.5f, headerMin.y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, (height - fontSize) * 0.5f));
    bool menuClicked = ImGui::Button(ICON_FA_ELLIPSIS_VERTICAL "##component_menu_button", ImVec2(buttonWidth, height));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Component options");

    if (menuClicked || rightClicked){
        ImGui::OpenPopup(popupId);
    }
    handleComponentMenu(popupId, sceneProject, entities, cpType, isPrefab, open, readOnly);

    if ((prefabOverridden || prefabAdded) && headerHovered && !ImGui::IsPopupOpen(popupId)){
        ImGui::SetTooltip("%s", prefabAdded ? "Added Component: this Prefab instance added it. Right click to apply or revert." :
            "Modified Component: this Prefab instance overrides some of its values (bold). Right click to apply or revert.");
    }

    ImGui::SetCursorScreenPos(afterHeader);
    ImGui::PopID();

    return open;
}

void editor::InspectorWindow::drawAddComponentButton(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, bool forceOpen){
    const ImGuiStyle& style = ImGui::GetStyle();

    // Components this editor does not know, kept in the file (docs/data-safety.md)
    if (entities.size() == 1 && sceneProject && sceneProject->scene) {
        if (const PreservedStore* store = findPreservedStore(sceneProject->scene)) {
            for (const std::string& name : store->preservedComponentNames(entities[0])) {
                ImGui::PushID(name.c_str());
                ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));
                ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("not loaded: this editor does not know it; kept in the file");
                ImGui::SameLine();
                ImGui::BeginDisabled(readOnly);
                if (ImGui::SmallButton("Remove")) {
                    CommandHandle::get(sceneProject->id)->addCommandNoMerge(
                        new RemovePreservedComponentCmd(project, sceneProject->id, entities[0], name));
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Remove it from the GameObject: the next save leaves it out");
                ImGui::PopID();
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));

    const float avail = ImGui::GetContentRegionAvail().x;
    const float width = std::min(avail, Theme::dpi(230.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);

    ImGui::BeginDisabled(readOnly);
    if (ImGui::Button("Add Component", ImVec2(width, ImGui::GetFrameHeight() + Theme::dpi(4.0f))) || (forceOpen && !readOnly)) {
        componentAddDialog.open(
            [this, sceneProject, entities](ComponentType cpType) {
                addComponents(sceneProject, entities, cpType);
            },
            [this, sceneProject, entities](ComponentType cpType) {
                // Check if component can be added to all selected entities
                for (const Entity& entity : entities) {
                    if (!canAddComponent(sceneProject, entity, cpType)) {
                        return false;
                    }
                }
                return true;
            },
            [](){},
            [this, sceneProject, entities](){
                openNewScriptDialog(sceneProject, entities, false);
            },
            ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax()
        );
        if (!project->isAnyScenePlaying()) {
            // the scripts the selection can take and does not all have already
            std::vector<ScriptCandidate> scripts;
            for (ScriptCandidate& candidate : ScriptAttach::findProjectScripts(project)) {
                const bool fits = entities.empty() || ScriptAttach::fitsEntity(candidate, sceneProject, entities[0]);
                if (fits && !scriptOnAll(sceneProject, entities, candidate)) scripts.push_back(std::move(candidate));
            }
            componentAddDialog.setScripts(std::move(scripts), [this, sceneProject, entities](const ScriptCandidate& script) {
                attachScriptFile(sceneProject, entities, script.file);
            });
        }
    }
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 2.0f));

    // the popup opens below the button
    componentAddDialog.show();

    // Unity: a script dropped on the Inspector goes on the selected GameObjects (where nothing
    // smaller takes the drop: ImGui prefers the smallest target)
    if (!readOnly && !project->isAnyScenePlaying()) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (ImGui::BeginDragDropTargetCustom(window->InnerRect, ImGui::GetID("##inspector_script_drop"))) {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            std::vector<std::string> dropped;
            if (peek && peek->IsDataType("resource_files")) dropped = Util::getStringsFromPayload(peek);
            if (!dropped.empty() && ScriptAttach::isScriptFile(dropped[0])) {
                if (ImGui::AcceptDragDropPayload("resource_files")) {
                    attachScriptFile(sceneProject, entities, dropped[0]);
                    ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
}

void editor::InspectorWindow::openNewScriptDialog(SceneProject* sceneProject, const std::vector<Entity>& entities, bool existing){
    Entity firstEntity = entities.empty() ? NULL_ENTITY : entities[0];
    std::string defaultName = "NewScript";
    if (firstEntity != NULL_ENTITY) {
        std::string entityName = sceneProject->scene->getEntityName(firstEntity);
        if (!entityName.empty())
            defaultName = entityName + "Script";
    }
    const uint32_t sceneId = sceneProject->id;
    scriptCreateDialog.open(
        sceneProject->scene,
        firstEntity,
        project->getProjectPath(),
        project->getLuaPath(),
        defaultName,
        [this, sceneId, entities](const std::filesystem::path& headerPath,
                                  const std::filesystem::path& sourcePath,
                                  const std::string& name,
                                  ScriptType type) {
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject) return;

            ScriptEntry entry;
            entry.type = type;
            entry.enabled = true;
            if (type == ScriptType::CPP) {
                entry.headerPath = FileUtils::pathToGenericUtf8(headerPath);
                entry.path = FileUtils::pathToGenericUtf8(sourcePath);
                entry.className = name;
            } else if (type == ScriptType::LUA) {
                entry.headerPath.clear();
                entry.path = FileUtils::pathToGenericUtf8(sourcePath);
                entry.className = name; // module (file base) name
            }

            std::vector<Entity> skipped;
            std::string error;
            if (!ScriptAttach::attach(project, sceneProject, entities, entry, -1, skipped, error, nullptr, project->getSelectedSceneId())) {
                Out::warning("%s", error.c_str());
            }
        },
        [](){},
        existing
    );
}

void editor::InspectorWindow::attachScriptFile(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::filesystem::path& file){
    ScriptAttach::attachFile(project, sceneProject, entities, file, project->getSelectedSceneId());
}

bool editor::InspectorWindow::scriptOnAll(SceneProject* sceneProject, const std::vector<Entity>& entities, const ScriptCandidate& candidate) const{
    const fs::path file = candidate.type == ScriptType::LUA ? candidate.file : candidate.source;
    for (Entity entity : entities) {
        const ScriptComponent* component = sceneProject->scene->findComponent<ScriptComponent>(entity);
        if (!component) return false;
        const bool has = std::any_of(component->scripts.begin(), component->scripts.end(), [&](const ScriptEntry& entry) {
            return entry.type == candidate.type && project->scriptSourceFile(entry) == file;
        });
        if (!has) return false;
    }
    return !entities.empty();
}

void editor::InspectorWindow::drawAddScriptPopup(SceneProject* sceneProject, const std::vector<Entity>& entities){
    if (!ImGui::BeginPopup("script_add_menu")) return;

    if (ImGui::IsWindowAppearing()) {
        addScriptCandidates = ScriptAttach::findProjectScripts(project);
        addScriptSearch[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(Theme::dpi(240.0f));
    ImGui::InputTextWithHint("##script_search", ICON_FA_MAGNIFYING_GLASS " Search scripts", addScriptSearch, sizeof(addScriptSearch));
    UIProbe::item("inspector.addScript.search");
    const std::string query = search::normalize(addScriptSearch);

    // rows and the message line, inside the child's padding
    const float listHeight = ImGui::GetTextLineHeightWithSpacing() * (addScriptCandidates.size() + 1.0f) + ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::BeginChild("##script_list", ImVec2(Theme::dpi(320.0f), std::min(Theme::dpi(260.0f), listHeight)), false);
    size_t shown = 0;
    for (const ScriptCandidate& candidate : addScriptCandidates) {
        if (!query.empty() && search::normalize(candidate.name).find(query) == std::string::npos) continue;
        shown++;
        const bool attached = scriptOnAll(sceneProject, entities, candidate);
        const bool fits = entities.empty() || ScriptAttach::fitsEntity(candidate, sceneProject, entities[0]);
        const std::string label = std::string(ICON_FA_FILE_CODE " ") + candidate.name + (candidate.type == ScriptType::LUA ? "  [Lua]" : "  [C++]") +
                                  "##" + candidate.file.string();
        ImGui::BeginDisabled(attached || !fits);
        if (ImGui::Selectable(label.c_str(), false)) {
            attachScriptFile(sceneProject, entities, candidate.file);
            ImGui::CloseCurrentPopup();
        }
        UIProbe::item("inspector.addScript.row." + candidate.name);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            std::string tip = ProjectChecks::projectRelative(project->getProjectPath(), candidate.file);
            if (attached) tip += "\nAlready attached";
            else if (!fits) tip += "\nA subclass of " + candidate.baseClass + ": for GameObjects of that class";
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    if (shown == 0) {
        ImGui::TextDisabled(addScriptCandidates.empty() ? "No scripts in the project yet" : "No script matches");
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Selectable(ICON_FA_PLUS " New Script...")) {
        openNewScriptDialog(sceneProject, entities, false);
        ImGui::CloseCurrentPopup();
    }
    UIProbe::item("inspector.addScript.new");
    if (ImGui::Selectable(ICON_FA_FOLDER_OPEN " Browse...")) {
        openNewScriptDialog(sceneProject, entities, true);
        ImGui::CloseCurrentPopup();
    }
    UIProbe::item("inspector.addScript.browse");
    ImGui::EndPopup();
}

Texture editor::InspectorWindow::getMaterialPreview(const Material& material, const std::string id, bool receiveIBL,
                                                   const std::string& meshShader){
    MaterialRender& materialRender = materialRenders[id];

    // drawn again until the sphere has the material's shader variant and textures: a library
    // shader builds off the UI thread, and the first frames would keep the previous image
    const std::string previewShader = material.shader.empty() ? meshShader : std::string();
    if ((materialRender.getMaterial() != material) || materialRender.getMeshShader() != previewShader ||
        materialRender.getReceiveIBL() != receiveIBL || !materialRender.getFramebuffer()->isCreated() ||
        materialRender.isPending()){
        materialRender.setReceiveIBL(receiveIBL);
        materialRender.applyMaterial(material, previewShader);
        Engine::executeSceneOnce(materialRender.getScene());
    }

    usedPreviewIds.insert(id);

    return materialRender.getTexture();
}

Texture editor::InspectorWindow::getDirectionPreview(const Vector3& direction, const std::string id){
    DirectionRender& directionRender = directionRenders[id];

    if ((directionRender.getDirection() != direction) || !directionRender.getFramebuffer()->isCreated()){
        directionRender.setDirection(direction);
        Engine::executeSceneOnce(directionRender.getScene());
    }

    usedPreviewIds.insert(id);

    return directionRender.getTexture();
}

bool editor::InspectorWindow::drawSpriteFramePreview(Texture* texture, const Rect& rect, const ImVec2& size, const char* itemId){
    if (!texture || texture->empty() || !texture->getRender()) {
        return false;
    }

    const float texWidth = static_cast<float>(texture->getWidth());
    const float texHeight = static_cast<float>(texture->getHeight());
    const float rectWidth = rect.getWidth();
    const float rectHeight = rect.getHeight();

    if (texWidth <= 0.0f || texHeight <= 0.0f || rectWidth <= 0.0f || rectHeight <= 0.0f) {
        return false;
    }

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 p_min = cursor;
    ImVec2 p_max = ImVec2(cursor.x + size.x, cursor.y + size.y);

    float srcAspect = rectWidth / rectHeight;
    float dstAspect = size.x / size.y;

    float drawWidth = size.x;
    float drawHeight = size.y;
    if (fabs(srcAspect - dstAspect) > 1e-3f) {
        if (srcAspect > dstAspect) {
            drawHeight = size.x / srcAspect;
        } else {
            drawWidth = size.y * srcAspect;
        }
    }

    ImVec2 imageMin(cursor.x + (size.x - drawWidth) * 0.5f, cursor.y + (size.y - drawHeight) * 0.5f);
    ImVec2 imageMax(imageMin.x + drawWidth, imageMin.y + drawHeight);

    ImVec2 uv0(rect.getX() / texWidth, rect.getY() / texHeight);
    ImVec2 uv1((rect.getX() + rectWidth) / texWidth, (rect.getY() + rectHeight) / texHeight);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float rounding = ImGui::GetStyle().FrameRounding;
    ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);

    drawList->AddRectFilled(p_min, p_max, ImGui::GetColorU32(Theme::Colors::filenameLabel), rounding, ImDrawFlags_RoundCornersAll);
    Widgets::addImageRounded(drawList, Backend::getImGuiTexture(texture->getRender()), imageMin, imageMax, uv0, uv1, IM_COL32_WHITE, rounding, ImDrawFlags_RoundCornersAll);
    drawList->AddRect(p_min, p_max, borderColor, rounding, ImDrawFlags_RoundCornersAll, Theme::dpi(1.0f));

    ImGui::InvisibleButton(itemId, size);
    return true;
}

void editor::InspectorWindow::updateShapePreview(const PrimitiveParameters& shapeParams){
    ImVec4 frameBgColor = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    std::shared_ptr<jasperengine::MeshSystem> meshSys = shapePreviewRender.getScene()->getSystem<MeshSystem>();

    MeshComponent meshComp;

    updateMeshShape(meshComp, meshSys.get(), shapeParams);

    shapePreviewRender.applyMesh(Stream::encodeMeshComponent(meshComp), true, true);
    shapePreviewRender.setBackground(Vector4(frameBgColor.x, frameBgColor.y, frameBgColor.z, frameBgColor.w));

    Engine::executeSceneOnce(shapePreviewRender.getScene());
}

void editor::InspectorWindow::updateMeshShape(MeshComponent& meshComp, MeshSystem* meshSys, const PrimitiveParameters& shapeParams){
    switch (shapeParams.geometryType) {
        case 0: // Plane
            meshSys->createPlane(meshComp, shapeParams.planeWidth, shapeParams.planeDepth, shapeParams.planeTiles);
            break;
        case 1: // Box
            meshSys->createBox(meshComp, shapeParams.boxWidth, shapeParams.boxHeight, shapeParams.boxDepth, shapeParams.boxTiles);
            break;
        case 2: // Sphere
            meshSys->createSphere(meshComp, shapeParams.sphereRadius, shapeParams.sphereSlices, shapeParams.sphereStacks);
            break;
        case 3: // Cylinder
            meshSys->createCylinder(meshComp, shapeParams.cylinderBaseRadius, shapeParams.cylinderTopRadius, shapeParams.cylinderHeight, shapeParams.cylinderSlices, shapeParams.cylinderStacks);
            break;
        case 4: // Capsule
            meshSys->createCapsule(meshComp, shapeParams.capsuleBaseRadius, shapeParams.capsuleTopRadius, shapeParams.capsuleHeight, shapeParams.capsuleSlices, shapeParams.capsuleStacks);
            break;
        case 5: // Torus
            meshSys->createTorus(meshComp, shapeParams.torusRadius, shapeParams.torusRingRadius, shapeParams.torusSides, shapeParams.torusRings);
            break;
        case 6: // Wall
            meshSys->createWall(meshComp, shapeParams.wallWidth, shapeParams.wallHeight, shapeParams.wallTiles);
            break;
    }
}

void editor::InspectorWindow::drawNinePatchesPreview(const ImageComponent& img, Texture* texture, Texture* thumbTexture, const ImVec2& size){
    float availWidth = ImGui::GetContentRegionAvail().x;

    float displayWidth = (size.x > 0) ? size.x : thumbTexture->getWidth();
    float displayHeight = (size.y > 0) ? size.y : thumbTexture->getHeight();

    // If only one dimension is specified, maintain aspect ratio
    if (size.x > 0 && size.y <= 0) {
        float aspectRatio = (float)thumbTexture->getHeight() / thumbTexture->getWidth();
        displayHeight = displayWidth * aspectRatio;
    } else if (size.x <= 0 && size.y > 0) {
        float aspectRatio = (float)thumbTexture->getWidth() / thumbTexture->getHeight();
        displayWidth = displayHeight * aspectRatio;
    }

    float xPos = (availWidth - displayWidth) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + xPos);

    ImVec2 cursor = ImGui::GetCursorScreenPos();

    Widgets::image(Backend::getImGuiTexture(thumbTexture->getRender()), ImVec2(displayWidth, displayHeight));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImU32 line_color = IM_COL32(255, 0, 0, 255);

    float origWidth = static_cast<float>(texture->getWidth());
    float origHeight = static_cast<float>(texture->getHeight());

    float scaleX = displayWidth / origWidth;
    float scaleY = displayHeight / origHeight;

    float scaledLeftMargin = img.patchMarginLeft * scaleX;
    float scaledRightMargin = img.patchMarginRight * scaleX;
    float scaledTopMargin = img.patchMarginTop * scaleY;
    float scaledBottomMargin = img.patchMarginBottom * scaleY;

    float leftX = cursor.x + scaledLeftMargin;
    draw_list->AddLine(
        ImVec2(leftX, cursor.y),
        ImVec2(leftX, cursor.y + displayHeight),
        line_color, 1.0f
    );

    float rightX = cursor.x + displayWidth - scaledRightMargin;
    draw_list->AddLine(
        ImVec2(rightX, cursor.y),
        ImVec2(rightX, cursor.y + displayHeight),
        line_color, 1.0f
    );

    float topY = cursor.y + scaledTopMargin;
    draw_list->AddLine(
        ImVec2(cursor.x, topY),
        ImVec2(cursor.x + displayWidth, topY),
        line_color, 1.0f
    );

    float bottomY = cursor.y + displayHeight - scaledBottomMargin;
    draw_list->AddLine(
        ImVec2(cursor.x, bottomY),
        ImVec2(cursor.x + displayWidth, bottomY),
        line_color, 1.0f
    );
}

void editor::InspectorWindow::syncSubSelection(SceneProject* sceneProject, Entity entity, int tileIndex, int instanceIndex) {
    if (!sceneProject) {
        return;
    }

    project->setSelectedSceneForProperties(sceneProject->id);
    project->setSelectedEntity(sceneProject->id, entity);

    if (!sceneProject->sceneRender) {
        return;
    }

    sceneProject->sceneRender->clearTileSelection();
    sceneProject->sceneRender->clearInstanceSelection();

    if (tileIndex >= 0) {
        sceneProject->sceneRender->selectTile(entity, tileIndex);
    }
    if (instanceIndex >= 0) {
        sceneProject->sceneRender->selectInstance(entity, instanceIndex);
    }

    project->updateSceneRender(sceneProject->id);
}


void editor::InspectorWindow::beginTable(ComponentType cpType, float firstColSize, std::string nameAddon, uint64_t widthKey){
    ImGui::PushItemWidth(-1);
    if (!nameAddon.empty()){
        nameAddon = "_"+nameAddon;
    }
    if (widthKey != 0){
        char key[24];
        std::snprintf(key, sizeof(key), "_%016llx", static_cast<unsigned long long>(widthKey));
        nameAddon += key;
    }
    // Unity keeps one label width (about 40% of the Inspector) so values line up across
    // components; a longer label still widens its own table
    const float unityLabelWidth = ImGui::GetContentRegionAvail().x * 0.4f;
    firstColSize = std::max(firstColSize, std::min(unityLabelWidth, Theme::dpi(180.0f)));
    // compact rows, no column divider (the column stays resizable at its edge)
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, Theme::dpi(1.0f)));
    ImGui::BeginTable(("table_"+Catalog::getComponentName(cpType)+nameAddon).c_str(), 2, ImGuiTableFlags_Resizable);
    ImGui::PopStyleVar();
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, firstColSize);
    ImGui::TableSetupColumn("Value");

}

void editor::InspectorWindow::endTable(){
    ImGui::EndTable();
}

bool editor::InspectorWindow::drawSummaryAddButton(const std::string& label, float trailingReservedWidth, bool enabled, const char* disabledTooltip) {
    ImGui::SameLine();
    float buttonWidth = std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingReservedWidth);

    if (!enabled) {
        ImGui::BeginDisabled();
    }

    bool clicked = ImGui::Button(label.c_str(), ImVec2(buttonWidth, 0));

    if (!enabled && disabledTooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", disabledTooltip);
    }

    if (!enabled) {
        ImGui::EndDisabled();
    }

    return clicked;
}

editor::InspectorWindow::EntityPickerResult editor::InspectorWindow::drawEntityPickerPopup(
        const std::string& popupId, const Signature& filter,
        SceneProject* owningScene, bool includeRelatedScenes,
        Entity currentValue, uint32_t currentValueSceneId,
        const std::vector<Entity>& excludeEntities) {

    EntityPickerResult result;
    if (!ImGui::BeginPopup(popupId.c_str())) return result;

    ImGui::Text("Select GameObject");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##ep_search", entityPickerSearchBuffer, sizeof(entityPickerSearchBuffer));

    if (ImGui::BeginChild("##ep_list", ImVec2(300.0f, 220.0f), true)) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().ItemSpacing.y);

        std::string searchStr = entityPickerSearchBuffer;
        for (char& c : searchStr) c = (char)tolower((unsigned char)c);

        // Build list of (SceneProject*, storedSceneId) to display: storedSceneId = 0 for the owning
        // scene's GameObjects, the scene's id for those of the scenes loaded with it (its parents,
        // children and siblings: Project::getRelatedScenes), which the runtime resolves together
        std::vector<std::pair<SceneProject*, uint32_t>> scenesToShow;
        scenesToShow.emplace_back(owningScene, (uint32_t)0);
        std::vector<std::string> notOpen;

        if (includeRelatedScenes) {
            for (uint32_t sid : project->getRelatedScenes(owningScene->id)) {
                if (sid == owningScene->id) continue;
                SceneProject* related = project->getScene(sid);
                if (!related) continue;
                if (related->scene) {
                    scenesToShow.emplace_back(related, sid);
                } else {
                    notOpen.push_back(related->name);
                }
            }
        }
        const std::vector<uint32_t> ownChildren = project->getChildScenes(owningScene->id);
        auto relation = [&](uint32_t sid) -> const char* {
            if (std::find(ownChildren.begin(), ownChildren.end(), sid) != ownChildren.end()) return "child";
            const std::vector<uint32_t> itsChildren = project->getChildScenes(sid);
            if (std::find(itsChildren.begin(), itsChildren.end(), owningScene->id) != itsChildren.end()) return "parent";
            return "related";
        };

        bool anyVisible = false;
        for (auto& [sp, storedSceneId] : scenesToShow) {
            if (storedSceneId != 0) {
                ImGui::SeparatorText((std::string(ICON_FA_LAYER_GROUP "  ") + sp->name + " (" + relation(storedSceneId) + ")").c_str());
            }
            bool sceneMatches = false;
            if (!searchStr.empty() && storedSceneId != 0) {
                std::string slow = sp->name;
                for (char& c : slow) c = (char)tolower((unsigned char)c);
                sceneMatches = slow.find(searchStr) != std::string::npos;
            }

            for (Entity e : sp->entities) {
                if (!sp->scene->isEntityCreated(e)) continue;
                if (filter.any() && (sp->scene->getSignature(e) & filter) != filter) continue;
                if (storedSceneId == 0 &&
                    std::find(excludeEntities.begin(), excludeEntities.end(), e) != excludeEntities.end()) continue;

                std::string ename = sp->scene->getEntityName(e);
                if (ename.empty()) ename = "GameObject " + std::to_string(e);

                if (!searchStr.empty() && !sceneMatches) {
                    std::string elow = ename;
                    for (char& c : elow) c = (char)tolower((unsigned char)c);
                    if (elow.find(searchStr) == std::string::npos) continue;
                }

                anyVisible = true;
                bool isSelected = (e == currentValue && storedSceneId == currentValueSceneId);
                std::string eicon = HierarchyWindow::getObjectIcon(sp->scene->getSignature(e), sp->scene);
                std::string selId = "##ep_ent_" + std::to_string(storedSceneId) + "_" + std::to_string(e);

                if (ImGui::Selectable(selId.c_str(), isSelected)) {
                    result.chosen = true;
                    result.entity = e;
                    result.sceneId = storedSceneId;
                    ImGui::CloseCurrentPopup();
                }
                UIProbe::item("entityPicker." + std::to_string(sp->id) + "." + std::to_string(e));
                ImGui::SameLine();
                if (storedSceneId != 0)
                    ImGui::Text("%s  %s  [%s]  (%u)", eicon.c_str(), ename.c_str(), sp->name.c_str(), static_cast<unsigned int>(e));
                else
                    ImGui::Text("%s  %s  (%u)", eicon.c_str(), ename.c_str(), static_cast<unsigned int>(e));
            }
        }

        if (!anyVisible) ImGui::TextDisabled("No GameObjects found");
        for (const std::string& name : notOpen) {
            ImGui::TextDisabled("%s  %s (not open)", ICON_FA_LAYER_GROUP, name.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();

    return result;
}

bool editor::InspectorWindow::propertyHeader(std::string label, float secondColSize, bool defChanged, bool child, bool indent){
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
    ImGui::TableNextRow();
    if (child){
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_PopupBg]));
    }
    ImGui::TableNextColumn();
    const float indentWidth = ImGui::GetFontSize();
    if (indent){
        ImGui::Indent(indentWidth);
    }
    drawPrefabPropertyLabel(label);
    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    bool button = false;
    if (defChanged){
        button = ImGui::Button((ICON_FA_ROTATE_LEFT"##"+label).c_str());
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (indent){
        ImGui::Unindent(indentWidth);
    }

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(secondColSize);

    return button;
}

namespace {

    // Numeric fields of a row (S23 R25-R26): its unit and the selection size for the
    // expression hook, and a multi-edit expression that differs per object (+=, L(), R())
    // applied to each object from the value it had when the field was activated
    std::map<ImGuiID, std::vector<double>>& multiEditBases() {
        static std::map<ImGuiID, std::vector<double>> bases;
        return bases;
    }

    using editor::NumericUnit;
    using editor::NumericFieldScope;
    using editor::NumericExpression;
    using editor::NumericContext;

    template<class Draw>
    bool numericField(NumericUnit unit, size_t objects, bool integer, Draw&& draw) {
        NumericFieldScope scope(unit, static_cast<int>(objects), integer);
        return draw();
    }

    template<class Current>
    std::vector<double> perObjectValues(bool edited, const std::vector<Entity>& entities, Current&& current, NumericUnit unit, bool integer = false) {
        std::vector<double> out;
        const ImGuiID id = ImGui::GetItemID();
        auto& bases = multiEditBases();
        if (ImGui::IsItemActivated() && entities.size() > 1) {
            std::vector<double> base;
            for (Entity entity : entities) base.push_back(static_cast<double>(current(entity)));
            bases[id] = std::move(base);
        }
        std::string text;
        if (edited && entities.size() > 1 && NumericExpression::takeMultiEdit(text)) {
            std::vector<double> currents;
            auto it = bases.find(id);
            if (it != bases.end() && it->second.size() == entities.size()) {
                currents = it->second;
            } else {
                for (Entity entity : entities) currents.push_back(static_cast<double>(current(entity)));
            }
            NumericContext context;
            context.unit = unit;
            context.integer = integer;
            if (!NumericExpression::evaluateForObjects(text, currents, context, out)) out.clear();
        }
        if (ImGui::IsItemDeactivated()) bases.erase(id);
        return out;
    }

}

bool editor::InspectorWindow::propertyRow(RowPropertyType type, ComponentType cpType, std::string id, std::string label, SceneProject* sceneProject, std::vector<Entity> entities, RowSettings settings){
    bool result = true;

    // The label shows whether the selection's prefab instances override it (propertyHeader);
    // a row that draws none leaves nothing for the next one
    prefabRowProperty = id;
    struct PrefabRowPropertyReset{
        std::string& property;
        ~PrefabRowPropertyReset(){ property.clear(); }
    } prefabRowPropertyReset{prefabRowProperty};

    // Script properties and submesh fields are per entity, a selection may not share all of them
    entities.erase(std::remove_if(entities.begin(), entities.end(), [&](Entity entity){
        return !Catalog::findProperty(sceneProject->scene, entity, cpType, id).ref;
    }), entities.end());
    if (entities.empty()){
        return false;
    }
    recordLayoutRow(type, cpType, id, label, sceneProject, entities);

    // Record (anim-properties spec 10): a row of an animatable property is a record source;
    // Unity tints the fields Record keys (animated ones stronger)
    bool rowRecorded = false;
    bool rowAnimated = false;
    if (cpType != ComponentType::Transform && ClipRecordHooks::hasRecordListeners()){
        for (Entity entity : entities){
            const ClipPropertyKey key = rowPropertyKey(sceneProject, entity, cpType, id);
            if (key.empty() || !ClipRecordHooks::claimsProperty(sceneProject->id, entity, key)) continue;
            rowRecorded = true;
            rowAnimated = rowAnimated || ClipRecordHooks::animatesProperty(sceneProject->id, entity, key);
        }
    }
    struct RowTint{
        bool pushed;
        ~RowTint(){ if (pushed) ImGui::PopStyleColor(); }
    } rowTint{rowRecorded};
    if (rowRecorded) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImLerp(Theme::Colors::ErrorText, ImVec4(0, 0, 0, 1), rowAnimated ? 0.55f : 0.78f));

    constexpr float compThreshold = 1e-4;
    constexpr float zeroThreshold = 1e-4;

    if (type == RowPropertyType::Label){
        std::string displayValue;
        bool different = false;

        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            std::string value = formatPropertyLabelValue(prop);
            if (displayValue.empty()) {
                displayValue = value;
            } else if (displayValue != value) {
                different = true;
            }
        }

        propertyHeader(label, settings.secondColSize, false, settings.child, settings.indent);
        if (different) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextUnformatted("---");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(displayValue.c_str());
        }

    }else if (type == RowPropertyType::Vector2){
        Vector2* value = nullptr;
        bool difX = false;
        bool difY = false;
        std::map<Entity, Vector2> eValue;
        float* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<float*>(prop.def);
            eValue[entity] = *static_cast<Vector2*>(prop.ref);
            if (value){
                if (std::fabs(value->x - eValue[entity].x) > compThreshold)
                    difX = true;
                if (std::fabs(value->y - eValue[entity].y) > compThreshold)
                    difY = true;
            }
            value = &eValue[entity];
        }

        Vector2 newValue = *value;
    bool clampNonNegativeTilePosition = cpType == ComponentType::TilemapComponent
        && id.rfind("tiles[", 0) == 0
        && id.size() >= 9
        && id.compare(id.size() - 9, 9, ".position") == 0;

        bool defChanged = false;
        if (defArr){
            defChanged = compareVectorFloat((float*)&newValue, defArr, 2, compThreshold);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Vector2>(project, sceneProject->id, entity, cpType, id, static_cast<Vector2>(defArr), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ImGui::BeginGroup();
        ImGui::PushMultiItemsWidths(2, ImGui::CalcItemWidth());

        // Define axis colors
        ImU32 axisColors[2] = {
            IM_COL32(220, 60, 60, 255),   // Red for X
            IM_COL32(60, 220, 60, 255)    // Green for Y
        };

        // Get draw list for drawing inside input fields
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float colorBarWidth = 4.0f; // Width of the colored bar

        if (difX)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosX = ImGui::GetCursorScreenPos();

        const bool edited_x = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_x_"+id).c_str(), &(newValue.x), settings.stepSize, 0.0f, 0.0f, settings.format); });
        const std::vector<double> perObject_x = perObjectValues(edited_x, entities, [&](Entity e) { return eValue[e].x; }, settings.unit);
        if (edited_x){
            size_t objectIndex_x = 0;
            for (Entity& entity : entities){
                if (!perObject_x.empty()) newValue.x = static_cast<float>(perObject_x[objectIndex_x++]);
                Vector2 nextValue(newValue.x, eValue[entity].y);
                if (clampNonNegativeTilePosition){
                    nextValue.x = std::max(0.0f, nextValue.x);
                    nextValue.y = std::max(0.0f, nextValue.y);
                }
                cmd = new PropertyCmd<Vector2>(project, sceneProject->id, entity, cpType, id, nextValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

        // Draw red bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosX.x + 2, inputPosX.y + 2);
            float inputHeightX = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosX.x + 2 + colorBarWidth, inputPosX.y + inputHeightX - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[0]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("X");
        }

        if (difX)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difY)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosY = ImGui::GetCursorScreenPos();

        const bool edited_y = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_y_"+id).c_str(), &(newValue.y), settings.stepSize, 0.0f, 0.0f, settings.format); });
        const std::vector<double> perObject_y = perObjectValues(edited_y, entities, [&](Entity e) { return eValue[e].y; }, settings.unit);
        if (edited_y){
            size_t objectIndex_y = 0;
            for (Entity& entity : entities){
                if (!perObject_y.empty()) newValue.y = static_cast<float>(perObject_y[objectIndex_y++]);
                Vector2 nextValue(eValue[entity].x, newValue.y);
                if (clampNonNegativeTilePosition){
                    nextValue.x = std::max(0.0f, nextValue.x);
                    nextValue.y = std::max(0.0f, nextValue.y);
                }
                cmd = new PropertyCmd<Vector2>(project, sceneProject->id, entity, cpType, id, nextValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

        // Draw green bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosY.x + 2, inputPosY.y + 2);
            float inputHeightY = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosY.x + 2 + colorBarWidth, inputPosY.y + inputHeightY - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[1]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("Y");
        }

        if (difY)
            ImGui::PopStyleColor();

        ImGui::EndGroup();

    }else if (type == RowPropertyType::Vector3 || type == RowPropertyType::Direction){
        Vector3* value = nullptr;
        bool difX = false;
        bool difY = false;
        bool difZ = false;
        std::map<Entity, Vector3> eValue;
        float* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<float*>(prop.def);
            eValue[entity] = *static_cast<Vector3*>(prop.ref);
            if (value){
                if (std::fabs(value->x - eValue[entity].x) > compThreshold)
                    difX = true;
                if (std::fabs(value->y - eValue[entity].y) > compThreshold)
                    difY = true;
                if (std::fabs(value->z - eValue[entity].z) > compThreshold)
                    difZ = true;
            }
            value = &eValue[entity];
        }

        Vector3 newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = compareVectorFloat((float*)&newValue, defArr, 3, compThreshold);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ (void)entity; return vector3Value(static_cast<Vector3>(defArr)); })){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, id, static_cast<Vector3>(defArr), settings.onValueChanged);
                    submitRowCommand(sceneProject, cpType, id);
                    finishProperty = true;
                }
            }
            finishProperty = true;
        }

        ImGui::BeginGroup();

        float min = 0.0f;
        float max = 0.0f;
        if (type == RowPropertyType::Direction){
            min = -1.0f;
            max = 1.0f;

            Texture dirTexRender = getDirectionPreview(newValue, id);
            float thumbSize = ImGui::GetFrameHeight() * 3;
            ImU32 border_col = IM_COL32(128, 128, 128, 255); // Gray border

            drawImageWithBorderAndRounding(&dirTexRender, ImVec2(thumbSize, thumbSize), 4.0f, border_col);

            static bool draggingDirection = false;

            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                draggingDirection = true;

                ImVec2 mouseDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);

                float sensitivity = 1.5f;

                float yawAngle = mouseDelta.x * sensitivity;   // Horizontal movement -> roll (around Y)
                float pitchAngle = -mouseDelta.y * sensitivity; // Vertical movement -> pitch (around X)

                Quaternion rollRotation(yawAngle, Vector3(0, 0, 1));     // Roll around world Z-axis
                Quaternion pitchRotation(pitchAngle, Vector3(1, 0, 0)); // Pitch around X-axis
                // Combine rotations (order matters: apply roll first, then pitch)
                Vector3 newDirection =  rollRotation * pitchRotation * newValue;

                // Apply to all entities
                for (Entity& entity : entities) {
                    cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, id, newDirection, settings.onValueChanged);
                    submitRowCommand(sceneProject, cpType, id);
                }

                newValue = newDirection;
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            }

            if (draggingDirection && ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
                finishProperty = true;
            }
        }

        ImGui::SetNextItemWidth(settings.secondColSize);

        ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());

        // Define axis colors
        ImU32 axisColors[3] = {
            IM_COL32(220, 60, 60, 255),   // Red for X
            IM_COL32(60, 220, 60, 255),   // Green for Y
            IM_COL32(60, 60, 220, 255)    // Blue for Z
        };

        // Get draw list for drawing inside input fields
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float colorBarWidth = 4.0f; // Width of the colored bar

        if (difX)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosX = ImGui::GetCursorScreenPos();

        const bool edited_x = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_x_"+id).c_str(), &(newValue.x), settings.stepSize, min, max, settings.format); });
        const std::vector<double> perObject_x = perObjectValues(edited_x, entities, [&](Entity e) { return eValue[e].x; }, settings.unit);
        if (edited_x){
            // each object's value: the field's, or its own result of a per-object expression
            std::map<Entity, float> value_x;
            for (size_t objectIndex = 0; objectIndex < entities.size(); objectIndex++){
                value_x[entities[objectIndex]] = perObject_x.empty() ? newValue.x : static_cast<float>(perObject_x[objectIndex]);
            }
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ return vector3Value(Vector3(value_x[entity], eValue[entity].y, eValue[entity].z)); })){
                for (Entity& entity : entities){
                    auto* rowCmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, id, Vector3(value_x[entity], eValue[entity].y, eValue[entity].z), settings.onValueChanged);
                    rowCmd->setShownComponents(0b110);   // y and z as shown: x is the edit
                    cmd = rowCmd;
                    submitRowCommand(sceneProject, cpType, id);
                }
            }
        }

        // Draw red bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosX.x + 2, inputPosX.y + 2);
            float inputHeightX = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosX.x + 2 + colorBarWidth, inputPosX.y + inputHeightX - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[0]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("X");
        }

        if (difX)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difY)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosY = ImGui::GetCursorScreenPos();

        const bool edited_y = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_y_"+id).c_str(), &(newValue.y), settings.stepSize, min, max, settings.format); });
        const std::vector<double> perObject_y = perObjectValues(edited_y, entities, [&](Entity e) { return eValue[e].y; }, settings.unit);
        if (edited_y){
            // each object's value: the field's, or its own result of a per-object expression
            std::map<Entity, float> value_y;
            for (size_t objectIndex = 0; objectIndex < entities.size(); objectIndex++){
                value_y[entities[objectIndex]] = perObject_y.empty() ? newValue.y : static_cast<float>(perObject_y[objectIndex]);
            }
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ return vector3Value(Vector3(eValue[entity].x, value_y[entity], eValue[entity].z)); })){
                for (Entity& entity : entities){
                    auto* rowCmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, id, Vector3(eValue[entity].x, value_y[entity], eValue[entity].z), settings.onValueChanged);
                    rowCmd->setShownComponents(0b101);   // x and z as shown: y is the edit
                    cmd = rowCmd;
                    submitRowCommand(sceneProject, cpType, id);
                }
            }
        }

        // Draw green bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosY.x + 2, inputPosY.y + 2);
            float inputHeightY = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosY.x + 2 + colorBarWidth, inputPosY.y + inputHeightY - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[1]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("Y");
        }

        if (difY)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difZ)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosZ = ImGui::GetCursorScreenPos();

        const bool edited_z = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_z_"+id).c_str(), &(newValue.z), settings.stepSize, min, max, settings.format); });
        const std::vector<double> perObject_z = perObjectValues(edited_z, entities, [&](Entity e) { return eValue[e].z; }, settings.unit);
        if (edited_z){
            // each object's value: the field's, or its own result of a per-object expression
            std::map<Entity, float> value_z;
            for (size_t objectIndex = 0; objectIndex < entities.size(); objectIndex++){
                value_z[entities[objectIndex]] = perObject_z.empty() ? newValue.z : static_cast<float>(perObject_z[objectIndex]);
            }
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ return vector3Value(Vector3(eValue[entity].x, eValue[entity].y, value_z[entity])); })){
                for (Entity& entity : entities){
                    auto* rowCmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, id, Vector3(eValue[entity].x, eValue[entity].y, value_z[entity]), settings.onValueChanged);
                    rowCmd->setShownComponents(0b011);   // x and y as shown: z is the edit
                    cmd = rowCmd;
                    submitRowCommand(sceneProject, cpType, id);
                }
            }
        }

        // Draw blue bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosZ.x + 2, inputPosZ.y + 2);
            float inputHeightZ = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosZ.x + 2 + colorBarWidth, inputPosZ.y + inputHeightZ - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[2]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("Z");
        }

        if (difZ)
            ImGui::PopStyleColor();

        ImGui::EndGroup();

    }else if (type == RowPropertyType::Vector4){
        Vector4* value = nullptr;
        bool difX = false;
        bool difY = false;
        bool difZ = false;
        bool difW = false;
        std::map<Entity, Vector4> eValue;
        float* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<float*>(prop.def);
            eValue[entity] = *static_cast<Vector4*>(prop.ref);
            if (value){
                if (std::fabs(value->x - eValue[entity].x) > compThreshold)
                    difX = true;
                if (std::fabs(value->y - eValue[entity].y) > compThreshold)
                    difY = true;
                if (std::fabs(value->z - eValue[entity].z) > compThreshold)
                    difZ = true;
                if (std::fabs(value->w - eValue[entity].w) > compThreshold)
                    difW = true;
            }
            value = &eValue[entity];
        }

        Vector4 newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = compareVectorFloat((float*)&newValue, defArr, 4, compThreshold);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, id, static_cast<Vector4>(defArr), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ImGui::BeginGroup();
        ImGui::PushMultiItemsWidths(4, ImGui::CalcItemWidth());

        // Define axis colors
        ImU32 axisColors[4] = {
            IM_COL32(220, 60, 60, 255),   // Red for X
            IM_COL32(60, 220, 60, 255),   // Green for Y
            IM_COL32(60, 60, 220, 255),   // Blue for Z
            IM_COL32(220, 220, 220, 255)  // White/Grey for W
        };

        // Get draw list for drawing inside input fields
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float colorBarWidth = 4.0f; // Width of the colored bar

        if (difX)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosX = ImGui::GetCursorScreenPos();

        const bool edited_x = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_x_"+id).c_str(), &(newValue.x), settings.stepSize, 0.0f, 0.0f, settings.format); });
        const std::vector<double> perObject_x = perObjectValues(edited_x, entities, [&](Entity e) { return eValue[e].x; }, settings.unit);
        if (edited_x){
            size_t objectIndex_x = 0;
            for (Entity& entity : entities){
                if (!perObject_x.empty()) newValue.x = static_cast<float>(perObject_x[objectIndex_x++]);
                cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, id, Vector4(newValue.x, eValue[entity].y, eValue[entity].z, eValue[entity].w), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

        // Draw red bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosX.x + 2, inputPosX.y + 2);
            float inputHeightX = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosX.x + 2 + colorBarWidth, inputPosX.y + inputHeightX - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[0]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("X");
        }

        if (difX)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difY)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosY = ImGui::GetCursorScreenPos();

        const bool edited_y = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_y_"+id).c_str(), &(newValue.y), settings.stepSize, 0.0f, 0.0f, settings.format); });
        const std::vector<double> perObject_y = perObjectValues(edited_y, entities, [&](Entity e) { return eValue[e].y; }, settings.unit);
        if (edited_y){
            size_t objectIndex_y = 0;
            for (Entity& entity : entities){
                if (!perObject_y.empty()) newValue.y = static_cast<float>(perObject_y[objectIndex_y++]);
                cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, id, Vector4(eValue[entity].x, newValue.y, eValue[entity].z, eValue[entity].w), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

        // Draw green bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosY.x + 2, inputPosY.y + 2);
            float inputHeightY = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosY.x + 2 + colorBarWidth, inputPosY.y + inputHeightY - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[1]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("Y");
        }

        if (difY)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difZ)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosZ = ImGui::GetCursorScreenPos();

        const bool edited_z = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_z_"+id).c_str(), &(newValue.z), settings.stepSize, 0.0f, 0.0f, settings.format); });
        const std::vector<double> perObject_z = perObjectValues(edited_z, entities, [&](Entity e) { return eValue[e].z; }, settings.unit);
        if (edited_z){
            size_t objectIndex_z = 0;
            for (Entity& entity : entities){
                if (!perObject_z.empty()) newValue.z = static_cast<float>(perObject_z[objectIndex_z++]);
                cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, id, Vector4(eValue[entity].x, eValue[entity].y, newValue.z, eValue[entity].w), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

        // Draw blue bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosZ.x + 2, inputPosZ.y + 2);
            float inputHeightZ = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosZ.x + 2 + colorBarWidth, inputPosZ.y + inputHeightZ - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[2]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("Z");
        }

        if (difZ)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difW)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosW = ImGui::GetCursorScreenPos();

        const bool edited_w = numericField(settings.unit, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_w_"+id).c_str(), &(newValue.w), settings.stepSize, 0.0f, 0.0f, settings.format); });
        const std::vector<double> perObject_w = perObjectValues(edited_w, entities, [&](Entity e) { return eValue[e].w; }, settings.unit);
        if (edited_w){
            size_t objectIndex_w = 0;
            for (Entity& entity : entities){
                if (!perObject_w.empty()) newValue.w = static_cast<float>(perObject_w[objectIndex_w++]);
                cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, id, Vector4(eValue[entity].x, eValue[entity].y, eValue[entity].z, newValue.w), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

        // Draw white/grey bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosW.x + 2, inputPosW.y + 2);
            float inputHeightW = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosW.x + 2 + colorBarWidth, inputPosW.y + inputHeightW - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[3]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("W");
        }

        if (difW)
            ImGui::PopStyleColor();

        ImGui::EndGroup();

    }else if (type == RowPropertyType::Quat){
        RotationOrder order = RotationOrder::ZYX;
        Vector3* value = nullptr;
        bool difX = false;
        bool difY = false;
        bool difZ = false;
        std::map<Entity, Vector3> eValue;
        Quaternion qValue;
        float* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<float*>(prop.def);
            qValue = *static_cast<Quaternion*>(prop.ref);
            eValue[entity] = getDisplayedEulerAngles(qValue, order, zeroThreshold);
            if (value){
                if (std::fabs(value->x - eValue[entity].x) > compThreshold)
                    difX = true;
                if (std::fabs(value->y - eValue[entity].y) > compThreshold)
                    difY = true;
                if (std::fabs(value->z - eValue[entity].z) > compThreshold)
                    difZ = true;
            }
            value = &eValue[entity];
        }

        Vector3 newValue = *value;

        // using 'qValue' to compare quaternions
        bool defChanged = false;
        if (defArr){
            defChanged = compareVectorFloat((float*)&qValue, defArr, 4, compThreshold);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ (void)entity; return rotationValue(static_cast<Quaternion>(defArr)); })){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<Quaternion>(project, sceneProject->id, entity, cpType, id, static_cast<Quaternion>(defArr), settings.onValueChanged);
                    submitRowCommand(sceneProject, cpType, id);
                    finishProperty = true;
                }
            }
            finishProperty = true;
        }

        ImGui::BeginGroup();
        ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());

        // Define axis colors
        ImU32 axisColors[3] = {
            IM_COL32(220, 60, 60, 255),   // Red for X
            IM_COL32(60, 220, 60, 255),   // Green for Y
            IM_COL32(60, 60, 220, 255)    // Blue for Z
        };

        // Get draw list for drawing inside input fields
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float colorBarWidth = 4.0f; // Width of the colored bar

        if (difX)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosX = ImGui::GetCursorScreenPos();

        const bool edited_x = numericField(NumericUnit::Degrees, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_x_"+id).c_str(), &(newValue.x), settings.stepSize, 0.0f, 0.0f, (std::string(settings.format) + "°").c_str()); });
        const std::vector<double> perObject_x = perObjectValues(edited_x, entities, [&](Entity e) { return eValue[e].x; }, NumericUnit::Degrees);
        if (edited_x){
            // each object's value: the field's, or its own result of a per-object expression
            std::map<Entity, float> value_x;
            for (size_t objectIndex = 0; objectIndex < entities.size(); objectIndex++){
                value_x[entities[objectIndex]] = perObject_x.empty() ? newValue.x : static_cast<float>(perObject_x[objectIndex]);
            }
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ return rotationValue(Quaternion(value_x[entity], eValue[entity].y, eValue[entity].z, order)); })){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<Quaternion>(project, sceneProject->id, entity, cpType, id, Quaternion(value_x[entity], eValue[entity].y, eValue[entity].z, order), settings.onValueChanged);
                    submitRowCommand(sceneProject, cpType, id);
                }
            }
        }

        // Draw red bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosX.x + 2, inputPosX.y + 2);
            float inputHeightX = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosX.x + 2 + colorBarWidth, inputPosX.y + inputHeightX - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[0]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("X");
        }

        if (difX)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difY)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosY = ImGui::GetCursorScreenPos();

        const bool edited_y = numericField(NumericUnit::Degrees, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_y_"+id).c_str(), &(newValue.y), settings.stepSize, 0.0f, 0.0f, "%.2f°"); });
        const std::vector<double> perObject_y = perObjectValues(edited_y, entities, [&](Entity e) { return eValue[e].y; }, NumericUnit::Degrees);
        if (edited_y){
            // each object's value: the field's, or its own result of a per-object expression
            std::map<Entity, float> value_y;
            for (size_t objectIndex = 0; objectIndex < entities.size(); objectIndex++){
                value_y[entities[objectIndex]] = perObject_y.empty() ? newValue.y : static_cast<float>(perObject_y[objectIndex]);
            }
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ return rotationValue(Quaternion(eValue[entity].x, value_y[entity], eValue[entity].z, order)); })){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<Quaternion>(project, sceneProject->id, entity, cpType, id, Quaternion(eValue[entity].x, value_y[entity], eValue[entity].z, order), settings.onValueChanged);
                    submitRowCommand(sceneProject, cpType, id);
                }
            }
        }

        // Draw green bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosY.x + 2, inputPosY.y + 2);
            float inputHeightY = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosY.x + 2 + colorBarWidth, inputPosY.y + inputHeightY - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[1]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("Y");
        }

        if (difY)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        if (difZ)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Store cursor position before drawing the input
        ImVec2 inputPosZ = ImGui::GetCursorScreenPos();

        const bool edited_z = numericField(NumericUnit::Degrees, entities.size(), false, [&]() { return ImGui::DragFloat(("##input_z_"+id).c_str(), &(newValue.z), settings.stepSize, 0.0f, 0.0f, "%.2f°"); });
        const std::vector<double> perObject_z = perObjectValues(edited_z, entities, [&](Entity e) { return eValue[e].z; }, NumericUnit::Degrees);
        if (edited_z){
            // each object's value: the field's, or its own result of a per-object expression
            std::map<Entity, float> value_z;
            for (size_t objectIndex = 0; objectIndex < entities.size(); objectIndex++){
                value_z[entities[objectIndex]] = perObject_z.empty() ? newValue.z : static_cast<float>(perObject_z[objectIndex]);
            }
            // Record keys a claimed Transform channel instead (animation clips 9.R5)
            if (!recordRowEdit(sceneProject, cpType, id, entities, [&](Entity entity){ return rotationValue(Quaternion(eValue[entity].x, eValue[entity].y, value_z[entity], order)); })){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<Quaternion>(project, sceneProject->id, entity, cpType, id, Quaternion(eValue[entity].x, eValue[entity].y, value_z[entity], order), settings.onValueChanged);
                    submitRowCommand(sceneProject, cpType, id);
                }
            }
        }

        // Draw blue bar inside the input field
        if (settings.showColors) {
            ImVec2 barMin(inputPosZ.x + 2, inputPosZ.y + 2);
            float inputHeightZ = ImGui::GetItemRectSize().y;
            ImVec2 barMax(inputPosZ.x + 2 + colorBarWidth, inputPosZ.y + inputHeightZ - 2);
            drawList->AddRectFilled(
                barMin,
                barMax,
                axisColors[2]
            );
            if (ImGui::IsMouseHoveringRect(barMin, barMax))
                ImGui::SetTooltip("Z");
        }

        if (difZ)
            ImGui::PopStyleColor();

        ImGui::EndGroup();

    }else if (type == RowPropertyType::Asset){
        // an asset file reference (S04): its path, editable or dropped from the Project window,
        // kept by GUID; a missing asset shows red with its last path and stays until cleared
        AssetRef* value = nullptr;
        std::map<Entity, AssetRef> eValue;
        bool dif = false;
        AssetRef* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            if (prop.type != PropertyType::Asset || !prop.ref) continue;
            defArr = static_cast<AssetRef*>(prop.def);
            eValue[entity] = *static_cast<AssetRef*>(prop.ref);
            if (value && *value != eValue[entity])
                dif = true;
            value = &eValue[entity];
        }
        if (value){
            const AssetRef current = *value;
            if (propertyHeader(label, settings.secondColSize, defArr && current != *defArr, settings.child)){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<AssetRef>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    finishProperty = true;
                }
            }
            bool missing = false;
            std::string tooltip;
            if (!dif && current.isSet() && project->getAssetDatabase().isOpen()){
                const std::string hint = project->assetHintToProjectPath(current.path, AssetPathBase::Assets);
                const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(current.guid, hint, AssetType::Unknown, true);
                missing = !found.found();
                tooltip = missing ? "Missing (GUID " + (current.guid.isNull() ? std::string("none") : current.guid.str()) + ")\nLast path: " + current.path
                                  : found.path + "\nGUID " + found.guid.str();
            }
            std::vector<char> buffer(current.path.begin(), current.path.end());
            buffer.resize(std::max((size_t)1024, current.path.size() + 256));
            buffer[current.path.size()] = '\0';
            if (missing)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
            else if (dif)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            const bool edited = ImGui::InputText(("##input_asset_"+id).c_str(), buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (missing || dif)
                ImGui::PopStyleColor();
            if (!tooltip.empty())
                ImGui::SetItemTooltip("%s", tooltip.c_str());
            AssetRef assigned;
            bool assign = false;
            if (edited){
                assigned = project->makeAssetRef(std::string(buffer.data()), AssetPathBase::Assets);
                assign = true;
            }
            if (ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                    std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                    if (!dropped.empty() && project->isInsideAssetsPath(dropped[0])){
                        assigned = project->makeAssetRef(project->normalizeToAssetsRelative(dropped[0]).generic_string(), AssetPathBase::Assets);
                        assign = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (assign){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<AssetRef>(project, sceneProject->id, entity, cpType, id, assigned, settings.onValueChanged);
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    finishProperty = true;
                }
            }
        }

    }else if (type == RowPropertyType::String || type == RowPropertyType::MultilineString){
        std::string* value = nullptr;
        std::map<Entity, std::string> eValue;
        bool dif = false;
        std::string* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            // a reference drawn as a text row edits its path (PropertyCmd<std::string> drops the GUID)
            if (prop.type == PropertyType::Asset) {
                defArr = prop.def ? &static_cast<AssetRef*>(prop.def)->path : nullptr;
                eValue[entity] = static_cast<AssetRef*>(prop.ref)->path;
            } else if (prop.type == PropertyType::Shader) {
                defArr = prop.def ? &static_cast<ShaderRef*>(prop.def)->path : nullptr;
                eValue[entity] = static_cast<ShaderRef*>(prop.ref)->path;
            } else {
                defArr = static_cast<std::string*>(prop.def);
                eValue[entity] = *static_cast<std::string*>(prop.ref);
            }
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        std::string newValue = *value;

        std::vector<char> buffer(newValue.begin(), newValue.end());
        buffer.resize(std::max((size_t)1024, newValue.size() + 256));
        buffer[newValue.size()] = '\0';

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        bool inputChanged = false;
        const bool suggestions = settings.stringSuggestions && type == RowPropertyType::String;
        if (type == RowPropertyType::MultilineString){
            inputChanged = ImGui::InputTextMultiline(("##input_string_"+id).c_str(), buffer.data(), buffer.size(), ImVec2(0, ImGui::GetTextLineHeight() * 6));
        }else{
            if (suggestions)
                ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x));
            inputChanged = ImGui::InputText(("##input_string_"+id).c_str(), buffer.data(), buffer.size());
        }

        if (inputChanged){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, id, std::string(buffer.data()), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }
        if (dif)
            ImGui::PopStyleColor();

        if (suggestions){
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            const std::string popupId = "string_suggestions_" + id;
            if (ImGui::ArrowButton(("##suggest_" + id).c_str(), ImGuiDir_Down))
                ImGui::OpenPopup(popupId.c_str());
            if (ImGui::BeginPopup(popupId.c_str())){
                if (settings.stringSuggestions->empty() && settings.suggestionsEmpty)
                    ImGui::TextDisabled("%s", settings.suggestionsEmpty);
                for (const std::string& option : *settings.stringSuggestions){
                    if (ImGui::Selectable(option.c_str(), !dif && option == newValue)){
                        for (Entity& entity : entities){
                            cmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, id, option, settings.onValueChanged);
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                        }
                        finishProperty = true;
                    }
                }
                ImGui::EndPopup();
            }
        }

    }else if (type == RowPropertyType::Bool){
        bool* value = nullptr;
        std::map<Entity, bool> eValue;
        bool dif = false;
        bool* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<bool*>(prop.def);
            eValue[entity] = *static_cast<bool*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        bool newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<bool>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Checkbox(("##checkbox_"+id).c_str(), &newValue)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<bool>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }
        if (dif)
            ImGui::PopStyleColor();

        if (!settings.help.empty()){
            ImGui::SameLine(); helpMarker(settings.help);
        }

    }else if (type == RowPropertyType::Float || type == RowPropertyType::FloatPositive || type == RowPropertyType::Float_0_1 || type == RowPropertyType::HalfCone){
        float* value = nullptr;
        std::map<Entity, float> eValue;
        bool dif = false;
        float* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<float*>(prop.def);
            eValue[entity] = *static_cast<float*>(prop.ref);
            if (type == RowPropertyType::HalfCone){
                eValue[entity] = Angle::radToDefault(std::acos(eValue[entity]) * 2);
            }
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        float newValue = *value;

        bool defChanged = false;
        if (defArr){
            if (type == RowPropertyType::HalfCone){
                float def = *defArr;
                defChanged = (newValue != Angle::radToDefault(std::acos(def) * 2));
            }else{
                defChanged = (newValue != *defArr);
            }
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        float v_min = (0.0F);
        float v_max = (0.0F);
        if (type == RowPropertyType::Float_0_1){
            v_min = 0.0F;
            v_max = 1.0F;
        }else if (type == RowPropertyType::FloatPositive){
            v_min = 0.0F;
            v_max = FLT_MAX;
        }

        std::string newFormat = settings.format;
        if (type == RowPropertyType::HalfCone){
            newFormat = std::string(settings.format) + "°";
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        const NumericUnit floatUnit = type == RowPropertyType::HalfCone ? NumericUnit::Degrees : settings.unit;
        // a property with a range is a slider (logarithmic when it says so)
        const bool slider = type != RowPropertyType::HalfCone && settings.floatMin < settings.floatMax;
        const bool editedFloat = numericField(floatUnit, entities.size(), false, [&]() {
            if (slider){
                ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp | (settings.logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
                return ImGui::SliderFloat(("##input_float_"+id).c_str(), &newValue, settings.floatMin, settings.floatMax, newFormat.c_str(), flags);
            }
            return ImGui::DragFloat(("##input_float_"+id).c_str(), &newValue, settings.stepSize, v_min, v_max, newFormat.c_str());
        });
        const std::vector<double> perObjectFloat = perObjectValues(editedFloat, entities, [&](Entity e) { return eValue[e]; }, floatUnit);
        if (editedFloat){
            size_t objectIndexFloat = 0;
            for (Entity& entity : entities){
                if (!perObjectFloat.empty()) newValue = static_cast<float>(perObjectFloat[objectIndexFloat++]);
                if (type == RowPropertyType::HalfCone){
                    cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, id, cos(Angle::defaultToRad(newValue / 2)), settings.onValueChanged);
                }else{
                    cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                }
                submitRowCommand(sceneProject, cpType, id);
            }
        }
        if (dif)
            ImGui::PopStyleColor();

        if (!settings.help.empty()){
            ImGui::SameLine(); helpMarker(settings.help);
        }

    }else if (type == RowPropertyType::Double || type == RowPropertyType::DoublePositive){
        double* value = nullptr;
        std::map<Entity, double> eValue;
        bool dif = false;
        double* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<double*>(prop.def);
            eValue[entity] = *static_cast<double*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        double newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<double>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        double v_min = 0.0;
        double v_max = DBL_MAX;
        const void* minPtr = nullptr;
        const void* maxPtr = nullptr;
        if (type == RowPropertyType::DoublePositive){
            minPtr = &v_min;
            maxPtr = &v_max;
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::DragScalar(("##input_double_"+id).c_str(), ImGuiDataType_Double, &newValue, settings.stepSize, minPtr, maxPtr, settings.format)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<double>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }
        if (dif)
            ImGui::PopStyleColor();

        if (!settings.help.empty()){
            ImGui::SameLine(); helpMarker(settings.help);
        }

    }else if (type == RowPropertyType::LayerMask){
        std::map<Entity, unsigned int> eValue;
        bool dif = false;
        unsigned int* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<unsigned int*>(prop.def);
            eValue[entity] = *static_cast<unsigned int*>(prop.ref);
            if (eValue[entity] != eValue[entities[0]])
                dif = true;
        }
        const unsigned int value = eValue[entities[0]];

        bool defChanged = defArr && (dif || value != *defArr);
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        }

        // the bits of the layers that have a name, the only ones Unity lists
        uint32_t namedMask = 0;
        for (int layer = 0; layer < MAX_LAYERS; layer++){
            if (!TagManager::layerToName(layer).empty()) namedMask |= (1u << layer);
        }

        auto setMask = [&](auto computeValue){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, computeValue(eValue[entity]), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        };

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        const bool comboOpen = ImGui::BeginCombo(("##layermask_" + id).c_str(), dif ? "-" : layerMaskLabel(value).c_str());
        if (dif)
            ImGui::PopStyleColor();
        if (comboOpen){
            // Unity's mask field: toggles keep the list open, Nothing and Everything set every bit
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            if (ImGui::MenuItem("Nothing", nullptr, !dif && value == LAYER_MASK_NOTHING)){
                setMask([](unsigned int){ return (unsigned int)LAYER_MASK_NOTHING; });
            }
            if (ImGui::MenuItem("Everything", nullptr, !dif && value == LAYER_MASK_EVERYTHING)){
                setMask([](unsigned int){ return (unsigned int)LAYER_MASK_EVERYTHING; });
            }
            for (int layer = 0; layer < MAX_LAYERS; layer++){
                const std::string name = TagManager::layerToName(layer);
                if (name.empty()) continue;
                const uint32_t bit = 1u << layer;
                bool allOn = true;
                for (Entity& entity : entities){
                    if (!(eValue[entity] & bit)) allOn = false;
                }
                if (ImGui::MenuItem((std::to_string(layer) + ": " + name).c_str(), nullptr, allOn)){
                    if (allOn){
                        setMask([bit](unsigned int old){ return old & ~bit; });
                    }else{
                        // every named layer on reads as Everything, as Unity stores it
                        setMask([bit, namedMask](unsigned int old){
                            unsigned int updated = old | bit;
                            return ((updated & namedMask) == namedMask) ? (unsigned int)LAYER_MASK_EVERYTHING : updated;
                        });
                    }
                }
            }
            ImGui::PopItemFlag();
            ImGui::EndCombo();
        }

        if (!settings.help.empty()){
            ImGui::SameLine(); helpMarker(settings.help);
        }

    }else if (type == RowPropertyType::UInt){
        unsigned int* value = nullptr;
        std::map<Entity, unsigned int> eValue;
        bool dif = false;
        unsigned int* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<unsigned int*>(prop.def);
            eValue[entity] = *static_cast<unsigned int*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        unsigned int newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::DragInt(("##input_uint_"+id).c_str(), (int*)&newValue, static_cast<int>(ceil(settings.stepSize)), 0.0f, INT_MAX)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }
        if (dif)
            ImGui::PopStyleColor();

    }else if (type == RowPropertyType::Int){
        int* value = nullptr;
        std::map<Entity, int> eValue;
        bool dif = false;
        int* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<int*>(prop.def);
            eValue[entity] = *static_cast<int*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        int newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<int>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        // Clamp (drag and typed) only when a real range is configured; otherwise stay
        // unbounded as before (ImGui treats min == max as "no limit").
        const bool clampInt = settings.intMax > settings.intMin;
        const bool editedInt = numericField(settings.unit, entities.size(), true, [&]() {
            return ImGui::DragInt(("##input_int_"+id).c_str(), &newValue, static_cast<int>(ceil(settings.stepSize)),
                                  clampInt ? settings.intMin : 0, clampInt ? settings.intMax : 0, "%d",
                                  clampInt ? ImGuiSliderFlags_AlwaysClamp : 0);
        });
        const std::vector<double> perObjectInt = perObjectValues(editedInt, entities, [&](Entity e) { return eValue[e]; }, settings.unit, true);
        if (editedInt){
            size_t objectIndexInt = 0;
            for (Entity& entity : entities){
                if (!perObjectInt.empty()) {
                    newValue = static_cast<int>(perObjectInt[objectIndexInt++]);
                    if (clampInt) newValue = std::clamp(newValue, settings.intMin, settings.intMax);
                }
                cmd = new PropertyCmd<int>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }
        if (dif)
            ImGui::PopStyleColor();

    }else if (type == RowPropertyType::Color3L){
        Vector3* value = nullptr;
        std::map<Entity, Vector3> eValue;
        bool dif = false;
        float* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<float*>(prop.def);
            eValue[entity] = *static_cast<Vector3*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        Vector3 newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = compareVectorFloat((float*)value, defArr, 3, compThreshold);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, id, static_cast<Vector3>(defArr), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        // the Color window (S27 R1-R3): one session is one undo step for every selected GameObject
        ui::ColorFieldOptions colorOptions;
        colorOptions.alpha = false;
        colorOptions.hdr = settings.colorHdr;
        colorOptions.linearStorage = !settings.colorSrgb;
        colorOptions.label = label.c_str();
        colorOptions.mixed = dif;
        const ui::EditPhase colorPhase = ui::colorField((label+"##color_"+id).c_str(), newValue, colorOptions);
        if (colorPhase == ui::EditPhase::Begin){
            ui::ColorPickerWindow::beginEdit(CommandHandle::get(project->getSelectedSceneId()), label.c_str());
        }else if (colorPhase == ui::EditPhase::Changing){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

    }else if (type == RowPropertyType::Curve){
        // an animation curve (S27 R9): the Curve field, each window gesture one undo step
        Curve* value = nullptr;
        std::map<Entity, Curve> eValue;
        bool dif = false;
        const Curve* defValue = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defValue = static_cast<const Curve*>(prop.def);
            eValue[entity] = *static_cast<Curve*>(prop.ref);
            if (value && *value != eValue[entity])
                dif = true;
            value = &eValue[entity];
        }
        const bool defChanged = defValue && value && *value != *defValue;
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent) && defValue){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Curve>(project, sceneProject->id, entity, cpType, id, *defValue, settings.onValueChanged);
                CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                finishProperty = true;
            }
        }
        ui::CurveFieldOptions curveOptions;
        curveOptions.range = curveops::CurveRange{0.0f, settings.curveValueMin, 1.0f, settings.curveValueMax, false};
        curveOptions.label = label.c_str();
        curveOptions.mixed = dif;
        curveOptions.resetValue = defValue;
        Curve newValue = value ? *value : Curve();
        const ui::EditPhase curvePhase = ui::curveField(("##curve_" + id).c_str(), newValue, curveOptions);
        if (curvePhase == ui::EditPhase::Begin){
            ui::beginFieldEdit(CommandHandle::get(project->getSelectedSceneId()), label.c_str());
        }else if (curvePhase == ui::EditPhase::Changing || curvePhase == ui::EditPhase::Applied){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Curve>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
            }
            if (curvePhase == ui::EditPhase::Applied) finishProperty = true;
        }

    }else if (type == RowPropertyType::Color4L){
        Vector4* value = nullptr;
        std::map<Entity, Vector4> eValue;
        bool dif = false;
        float* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<float*>(prop.def);
            eValue[entity] = *static_cast<Vector4*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        Vector4 newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = compareVectorFloat((float*)value, defArr, 4, compThreshold);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, id, static_cast<Vector4>(defArr), settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ui::ColorFieldOptions colorOptions;
        colorOptions.alpha = true;
        colorOptions.hdr = settings.colorHdr;
        colorOptions.linearStorage = !settings.colorSrgb;
        colorOptions.label = label.c_str();
        colorOptions.mixed = dif;
        const ui::EditPhase colorPhase = ui::colorField((label+"##color_"+id).c_str(), newValue, colorOptions);
        UIProbe::item("inspector.color." + id);
        if (colorPhase == ui::EditPhase::Begin){
            ui::ColorPickerWindow::beginEdit(CommandHandle::get(project->getSelectedSceneId()), label.c_str());
        }else if (colorPhase == ui::EditPhase::Changing){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
        }

    }else if ((type == RowPropertyType::IntSlider || type == RowPropertyType::UIntSlider) && settings.sliderValues){
        int* value = nullptr;
        std::map<Entity, int> eValue;
        bool dif = false;
        void* defArr = nullptr;
        std::vector<int>* sliderValues = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = prop.def;
            sliderValues = settings.sliderValues;
            if (type == RowPropertyType::IntSlider) {
                eValue[entity] = *static_cast<int*>(prop.ref);
            } else {
                eValue[entity] = *static_cast<unsigned int*>(prop.ref);
            }
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        int newValue = *value;

        bool defChanged = false;
        if (defArr){
            if (type == RowPropertyType::IntSlider) {
                defChanged = (newValue != *static_cast<int*>(defArr));
            } else {
                defChanged = (newValue != *static_cast<unsigned int*>(defArr));
            }
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                if (type == RowPropertyType::IntSlider) {
                    cmd = new PropertyCmd<int>(project, sceneProject->id, entity, cpType, id, *static_cast<int*>(defArr), settings.onValueChanged);
                } else {
                    cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, *static_cast<unsigned int*>(defArr), settings.onValueChanged);
                }
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        // Find current value index in the slider values array
        int currentIndex = 0;
        for (size_t i = 0; i < sliderValues->size(); ++i) {
            if ((*sliderValues)[i] == newValue) {
                currentIndex = static_cast<int>(i);
                break;
            }
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // Create format string with current value
        char formatStr[32];
        snprintf(formatStr, sizeof(formatStr), "%d", (*sliderValues)[currentIndex]);

        if (ImGui::SliderInt(("##intslider_"+id).c_str(), &currentIndex, 0, static_cast<int>(sliderValues->size() - 1), formatStr)) {
            int newSliderValue = (*sliderValues)[currentIndex];
            for (Entity& entity : entities){
                if (type == RowPropertyType::IntSlider) {
                    cmd = new PropertyCmd<int>(project, sceneProject->id, entity, cpType, id, newSliderValue, settings.onValueChanged);
                } else {
                    cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, static_cast<unsigned int>(newSliderValue), settings.onValueChanged);
                }
                submitRowCommand(sceneProject, cpType, id);
            }
        }

        if (dif)
            ImGui::PopStyleColor();

    }else if (type == RowPropertyType::Enum && settings.enumEntries) {
        int* value = nullptr;
        std::map<Entity, int> eValue;
        bool dif = false;
        int* defArr = nullptr;
        std::vector<EnumEntry>* enumEntries = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<int*>(prop.def);
            enumEntries = settings.enumEntries;
            eValue[entity] = *static_cast<int*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        int item_current = 0;
        // Find current index in enumEntries
        for (size_t i = 0; i < enumEntries->size(); ++i) {
            if ((*enumEntries)[i].value == *value) {
                item_current = static_cast<int>(i);
                break;
            }
        }
        int item_default = item_current;

        bool defChanged = false;
        if (defArr){
            int defValue = *defArr;
            // Find index of default value in enumEntries
            for (size_t i = 0; i < enumEntries->size(); ++i) {
                if ((*enumEntries)[i].value == defValue) {
                    item_default = static_cast<int>(i);
                    break;
                }
            }
            defChanged = (item_current != item_default);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                int defValue = (*enumEntries)[item_default].value;
                cmd = new PropertyCmd<int>(project, sceneProject->id, entity, cpType, id, defValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        // Build names array
        std::vector<const char*> names;
        for (const auto& entry : *enumEntries) {
            names.push_back(entry.name);
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        bool chosen = false;
        if (settings.enumExtraLabel) {
            // a value not among the entries (a removed Sorting Layer) shows as its number
            const bool known = !enumEntries->empty() && (*enumEntries)[(size_t)item_current].value == *value;
            const std::string preview = known ? std::string(names[(size_t)item_current]) : std::to_string(*value);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo(("##combo_" + id).c_str(), preview.c_str())) {
                for (size_t i = 0; i < names.size(); i++) {
                    const bool selected = known && (int)i == item_current;
                    if (ImGui::Selectable(names[i], selected) && !selected) {
                        item_current = (int)i;
                        chosen = true;
                    }
                }
                ImGui::Separator();
                if (ImGui::Selectable(settings.enumExtraLabel) && settings.enumExtraAction)
                    settings.enumExtraAction();
                ImGui::EndCombo();
            }
        } else {
            chosen = ImGui::Combo(("##combo_" + id).c_str(), &item_current, names.data(), static_cast<int>(names.size()));
        }
        if (chosen) {
            int newValue = (*enumEntries)[item_current].value;
            for (Entity& entity : entities){
                cmd = new PropertyCmd<int>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        }
        if (dif)
            ImGui::PopStyleColor();

    }else if (type == RowPropertyType::Ease) {
        Ease* value = nullptr;
        std::map<Entity, Ease> eValue;
        bool dif = false;
        Ease* defValue = nullptr;
        std::vector<EnumEntry>* enumEntries = settings.enumEntries ? settings.enumEntries : &entriesEaseType;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defValue = static_cast<Ease*>(prop.def);
            eValue[entity] = *static_cast<Ease*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        int item_current = 0;
        EaseType currentType = value ? value->getType() : EaseType::LINEAR;
        for (size_t i = 0; i < enumEntries->size(); ++i) {
            if ((*enumEntries)[i].value == static_cast<int>(currentType)) {
                item_current = static_cast<int>(i);
                break;
            }
        }

        int item_default = item_current;
        bool defChanged = false;
        if (defValue){
            EaseType defaultType = defValue->getType();
            for (size_t i = 0; i < enumEntries->size(); ++i) {
                if ((*enumEntries)[i].value == static_cast<int>(defaultType)) {
                    item_default = static_cast<int>(i);
                    break;
                }
            }
            defChanged = (currentType != defaultType);
        }

        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                Ease defaultEase(static_cast<EaseType>((*enumEntries)[item_default].value));
                cmd = new PropertyCmd<Ease>(project, sceneProject->id, entity, cpType, id, defaultEase, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        std::vector<const char*> names;
        for (const auto& entry : *enumEntries) {
            names.push_back(entry.name);
        }

        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Combo(("##combo_" + id).c_str(), &item_current, names.data(), static_cast<int>(names.size()))) {
            EaseType newType = static_cast<EaseType>((*enumEntries)[item_current].value);
            for (Entity& entity : entities){
                Ease newValue = eValue[entity];
                newValue.setType(newType);
                cmd = new PropertyCmd<Ease>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        }
        if (dif)
            ImGui::PopStyleColor();

    }else if (type == RowPropertyType::Gradient){
        // a colour gradient (S27 R12): the Gradient field, each Gradient Editor gesture one undo step
        Gradient* value = nullptr;
        std::map<Entity, Gradient> eValue;
        bool dif = false;
        const Gradient* defValue = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defValue = static_cast<const Gradient*>(prop.def);
            eValue[entity] = *static_cast<Gradient*>(prop.ref);
            if (value && *value != eValue[entity])
                dif = true;
            value = &eValue[entity];
        }
        const bool defChanged = defValue && value && *value != *defValue;
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent) && defValue){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Gradient>(project, sceneProject->id, entity, cpType, id, *defValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }
        ui::GradientFieldOptions gradientOptions;
        gradientOptions.label = label.c_str();
        gradientOptions.mixed = dif;
        Gradient newValue = value ? *value : Gradient();
        const ui::EditPhase gradientPhase = ui::gradientField(("##gradient_" + id).c_str(), newValue, gradientOptions);
        if (gradientPhase == ui::EditPhase::Begin){
            ui::beginFieldEdit(CommandHandle::get(project->getSelectedSceneId()), label.c_str());
        }else if (gradientPhase == ui::EditPhase::Changing || gradientPhase == ui::EditPhase::Applied){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Gradient>(project, sceneProject->id, entity, cpType, id, newValue, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            if (gradientPhase == ui::EditPhase::Applied) finishProperty = true;
        }
        if (!settings.help.empty()){
            ImGui::SameLine();
            helpMarker(settings.help);
        }

    }else if (type == RowPropertyType::Font){
        FontArray* value = nullptr;
        std::map<Entity, FontArray> eValue;
        bool dif = false;
        FontArray* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<FontArray*>(prop.def);
            eValue[entity] = *static_cast<FontArray*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        FontArray newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<FontArray>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        // the row edits the main font, the fallbacks are in the popup
        bool hasFallbacks = false;
        for (size_t slot = 1; slot < newValue.size(); slot++){
            if (!newValue[slot].empty()){
                hasFallbacks = true;
                break;
            }
        }

        // OpenPopup only reaches BeginPopup from the same id scope, outside drawFontSlot
        bool openFallbacks = false;

        drawFontSlot(cpType, id, 0, dif, newValue, sceneProject, entities, settings.onValueChanged, -1.0f,
            [&openFallbacks, hasFallbacks](const ImVec2& buttonSize){
                if (hasFallbacks)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                if (ImGui::Button(ICON_FA_LAYER_GROUP, buttonSize)) {
                    openFallbacks = true;
                }
                if (hasFallbacks)
                    ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Additional fonts, used for what the main font misses");
            });

        const std::string fallbacksPopup = "fontfallbackspopup_" + id;
        if (openFallbacks){
            ImGui::OpenPopup(fallbacksPopup.c_str());
        }
        drawFontFallbacksPopup(fallbacksPopup.c_str(), cpType, id, sceneProject, entities, settings.onValueChanged);

    }else if (type == RowPropertyType::Texture){
        Texture* value = nullptr;
        std::map<Entity, Texture> eValue;
        bool dif = false;
        Texture* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<Texture*>(prop.def);
            eValue[entity] = *static_cast<Texture*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        Texture newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ImGui::BeginGroup();
        ImGui::PushID(("texture_"+id).c_str());

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);

        bool isCameraTexture = CameraTextureLink::isCameraTexture(newValue);
        bool isFluidTexture = FluidTextureLink::isFluidTexture(newValue);
        const bool isVideoTexture = VideoTextureLink::isVideoTexture(newValue);

        float thumbSize = ImGui::GetFrameHeight() * 3;
        Texture* thumbTexture = findThumbnail(newValue.getPath());
        if (thumbTexture) {
            ImU32 border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
            if (dif){
                border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            }
            drawImageWithBorderAndRounding(thumbTexture, ImVec2(thumbSize, thumbSize), ImGui::GetStyle().FrameRounding, border_col);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                Widgets::image(Backend::getImGuiTexture(thumbTexture->getRender()), ImVec2(thumbTexture->getWidth(), thumbTexture->getHeight()));
                ImGui::EndTooltip();
            }
        } else if ((isCameraTexture || isFluidTexture || isVideoTexture) && newValue.isFramebuffer() && newValue.getFramebuffer()->isCreated()) {
            // live preview of the linked camera framebuffer or fluid output
            ImU32 border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
            if (dif){
                border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            }
            drawImageWithBorderAndRounding(&newValue, ImVec2(thumbSize, thumbSize), ImGui::GetStyle().FrameRounding, border_col);
        }

        // sampler settings (min/mag filter, wrap) only apply to an actual image texture,
        // not to a camera/framebuffer link or an empty slot
        bool showTexSettings = settings.allowTextureSettings && !isCameraTexture && !isFluidTexture && !isVideoTexture && !newValue.empty();

        ImVec2 texButtonSize = ImGui::CalcItemSize(ImVec2(0, 0), ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
        // reserve room for the action buttons (file + camera + fluid, plus a settings button
        // when applicable), with one gap between each item and a trailing gap on the right
        int texButtonCount = settings.allowCameraTexture ? 3 : 1;
        if (showTexSettings) texButtonCount++;
        if (settings.extraButton) texButtonCount++;
        float texButtonsReserve = texButtonSize.x * texButtonCount + ImGui::GetStyle().ItemSpacing.x * (texButtonCount + 1);
        ImGui::BeginChild("textureframe", ImVec2(-texButtonsReserve, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2),
            false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // the file's name, never the pool id ("ship.png", not "sprites/ship.png|alphaborder")
        const TextureSlotLabel slotLabel = textureSlotLabel(newValue, [this](const std::string& path) {
            return project->assetHintToProjectPath(path, AssetPathBase::Assets);
        });
        std::string texName = slotLabel.text;
        if (isCameraTexture) {
            texName = std::string(ICON_FA_VIDEO) + " " + CameraTextureLink::cameraName(sceneProject->scene, newValue);
        } else if (isFluidTexture) {
            texName = std::string(ICON_FA_DROPLET) + " " + FluidTextureLink::fluidName(sceneProject->scene, newValue);
        } else if (isVideoTexture) {
            texName = std::string(ICON_FA_FILM) + " " + VideoTextureLink::videoName(sceneProject->scene, newValue);
        }
        // S04 R28: a reference whose asset is gone stays, shown red with its last path
        std::string missingTooltip;
        if (!isCameraTexture && !newValue.getPath(0).empty() && project->getAssetDatabase().isOpen()) {
            const std::string hint = project->assetHintToProjectPath(newValue.getPath(0), AssetPathBase::Assets);
            if (!project->getAssetDatabase().resolve(newValue.getPathGuid(0), hint, AssetType::Texture, true).found()) {
                texName = "Missing (" + std::filesystem::path(newValue.getPath(0)).filename().string() + ")";
                missingTooltip = "Missing (GUID " + (newValue.getPathGuid(0).isNull() ? std::string("none") : newValue.getPathGuid(0).str()) +
                                 ")\nLast path: " + newValue.getPath(0);
            }
        }

        float textWidth = ImGui::CalcTextSize(texName.c_str()).x;
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(availWidth - textWidth - 2);
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        if (!missingTooltip.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        else if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Text("%s", texName.c_str());
        if (!missingTooltip.empty() || dif)
            ImGui::PopStyleColor();

        ImGui::EndChild();
        if (!missingTooltip.empty()){
            ImGui::SetItemTooltip("%s", missingTooltip.c_str());
        } else if (!slotLabel.tooltip.empty()){
            ImGui::SetItemTooltip("%s", (isCameraTexture || isFluidTexture || isVideoTexture ? newValue.getId() : slotLabel.tooltip).c_str());
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        // no horizontal frame padding so the icons fit centered in the square buttons
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetStyle().FramePadding.y));

        if (ImGui::Button(ICON_FA_FOLDER_OPEN, texButtonSize)) {
            std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
            if (!path.empty()) {
                std::filesystem::path assetsPath = project->getAssetsPath();
                std::filesystem::path filePath = std::filesystem::absolute(path);

                // References are stored relative to the assets root
                std::error_code ec;
                auto relative = std::filesystem::relative(filePath, assetsPath, ec);
                if (ec || relative.empty() || *relative.begin() == "..") {
                    ImGui::OpenPopup("File Import Error");
                }else{
                    Texture texture(FileUtils::pathToGenericUtf8(relative));
                    for (Entity& entity : entities){
                        cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, texture, settings.onValueChanged);
                        submitRowCommand(sceneProject, cpType, id);
                        finishProperty = true;
                    }
                }
            }
        }
        ImGui::SetItemTooltip("Image file");

        if (settings.allowCameraTexture) {
            ImGui::SameLine();

            if (ImGui::Button(ICON_FA_VIDEO, texButtonSize)) {
                ImGui::OpenPopup("cameratexturepopup");
            }
            ImGui::SetItemTooltip("Camera as source (render to texture)");

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_DROPLET, texButtonSize)) {
                ImGui::OpenPopup("fluidtexturepopup");
            }
            UIProbe::item("texture.fluid." + id);
            ImGui::SetItemTooltip("Fluid Simulation as source (its output, live)");
        }

        if (showTexSettings) {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_SLIDERS, texButtonSize)) {
                ImGui::OpenPopup("texturesettingspopup");
            }
            ImGui::SetItemTooltip("Texture settings (filter, wrap, SVG scale)");
        }

        if (settings.extraButton) {
            ImGui::SameLine();
            settings.extraButton(texButtonSize);
        }

        ImGui::PopStyleVar();

        drawTextureSettingsPopup("texturesettingspopup", cpType, id, sceneProject, entities, settings.onValueChanged);

        if (settings.allowCameraTexture && ImGui::BeginPopup("cameratexturepopup")) {
            Scene* scene = sceneProject->scene;
            auto camerasArray = scene->getComponentArray<CameraComponent>();
            bool hasCamera = false;
            for (size_t c = 0; c < camerasArray->size(); c++) {
                Entity cameraEntity = camerasArray->getEntity(c);
                // only user scene entities; skips editor-internal cameras
                if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), cameraEntity) == sceneProject->entities.end())
                    continue;

                hasCamera = true;
                std::string cameraLabel = std::string(ICON_FA_VIDEO) + " " + entityDisplayName(scene->getEntityName(cameraEntity), cameraEntity);
                if (ImGui::MenuItem(cameraLabel.c_str())) {
                    applyCameraTexture(cameraEntity, cpType, id, sceneProject, entities, settings.onValueChanged);
                    finishProperty = true;
                }
            }
            if (!hasCamera) {
                ImGui::TextDisabled("No camera in scene");
            }
            // the Video Players' render textures (video|<entity>)
            if (scene->isComponentRegistered<VideoPlayerComponent>()) {
                auto videosArray = scene->getComponentArray<VideoPlayerComponent>();
                bool separated = false;
                for (size_t v = 0; v < videosArray->size(); v++) {
                    Entity videoEntity = videosArray->getEntity(v);
                    if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), videoEntity) == sceneProject->entities.end())
                        continue;
                    if (!separated) {
                        ImGui::Separator();
                        separated = true;
                    }
                    const std::string videoLabel = std::string(ICON_FA_FILM) + " Video Player: " + entityDisplayName(scene->getEntityName(videoEntity), videoEntity) +
                                                   "##video_" + std::to_string(videoEntity);
                    const bool linked = VideoTextureLink::parse(newValue.getId()) == videoEntity && !dif;
                    if (ImGui::MenuItem(videoLabel.c_str(), nullptr, linked)) {
                        applyVideoTexture(videoEntity, cpType, id, sceneProject, entities, settings.onValueChanged);
                        finishProperty = true;
                    }
                    UIProbe::item("texture.video.item." + std::to_string(videoEntity));
                }
            }
            ImGui::EndPopup();
        }

        if (settings.allowCameraTexture && ImGui::BeginPopup("fluidtexturepopup")) {
            Scene* scene = sceneProject->scene;
            auto fluidsArray = scene->getComponentArray<FluidSimulationComponent>();
            bool hasFluid = false;
            for (size_t f = 0; f < fluidsArray->size(); f++) {
                Entity fluidEntity = fluidsArray->getEntity(f);
                // only user scene entities
                if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), fluidEntity) == sceneProject->entities.end())
                    continue;

                hasFluid = true;
                const FluidSimulationComponent& fluid = fluidsArray->getComponentFromIndex(f);
                std::string fluidLabel = std::string(ICON_FA_DROPLET) + " " + entityDisplayName(scene->getEntityName(fluidEntity), fluidEntity) + "##fluid_" + std::to_string(fluidEntity);
                const bool linked = FluidTextureLink::parse(newValue.getId()) == fluidEntity && !dif;
                if (ImGui::MenuItem(fluidLabel.c_str(), nullptr, linked)) {
                    applyFluidTexture(fluidEntity, cpType, id, sceneProject, entities, settings.onValueChanged);
                    finishProperty = true;
                }
                UIProbe::item("texture.fluid.item." + std::to_string(fluidEntity));
                if (fluid.output != FluidOutput::TEXTURE) {
                    ImGui::SetItemTooltip("Its output; it is also drawn %s. Output Texture draws it only here.",
                                          fluid.output == FluidOutput::SCREEN ? "over the screen" : "on its own GameObject");
                }
            }
            if (!hasFluid) {
                ImGui::TextDisabled("No Fluid Simulation in scene");
                ImGui::SetItemTooltip("GameObject > Effects > Fluid Simulation");
            }
            ImGui::EndPopup();
        }

        // Error popup modal
        if (EditorModal::begin("File Import Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Selected file must be within the assets directory.");
            EditorModal::message("Selected file must be within the assets directory.");
            ImGui::Separator();

            float buttonWidth = 120;
            float windowWidth = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);
            if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        ImGui::EndGroup();

        dragDropResourcesTexture(cpType, id, sceneProject, entities);

    }else if (type == RowPropertyType::TextureCube){
        Texture* value = nullptr;
        std::map<Entity, Texture> eValue;
        bool dif = false;
        Texture* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<Texture*>(prop.def);
            eValue[entity] = *static_cast<Texture*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }

        Texture newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ImGui::BeginGroup();
        ImGui::PushID(("texturecube_"+id).c_str());

        auto isSingleFileCube = [](const Texture& t) -> bool {
            for (size_t i = 1; i < 6; i++){
                if (!t.getPath(i).empty())
                    return false;
            }
            return true;
        };

        if (!textureCubeSingleMode.count(id)){
            textureCubeSingleMode[id] = isSingleFileCube(newValue);
        }

        bool& singleMode = textureCubeSingleMode[id];

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Mode");
        ImGui::SameLine();
        {
            static const char* modeItems[] = {"Cubemap file", "Separate files"};
            int modeIndex = singleMode ? 0 : 1;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##cube_mode", &modeIndex, modeItems, IM_ARRAYSIZE(modeItems))){
                singleMode = (modeIndex == 0);

                for (Entity& entity : entities){
                    cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                    submitRowCommand(sceneProject, cpType, id);
                    finishProperty = true;
                }
            }
        }

        if (singleMode){
            std::string cubePath = newValue.getPath(0);
            std::string cubeName = cubePath;
            if (std::filesystem::exists(cubeName)) {
                cubeName = std::filesystem::path(cubeName).filename().string();
            }
            if (cubeName.empty()) {
                cubeName = "< Not set >";
            }

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Cubemap");
            ImGui::PopStyleVar();

            ImVec2 dragMin = ImGui::GetCursorScreenPos();
            ImVec2 dragMax = dragMin;

            float thumbSize = ImGui::GetFrameHeight() * 3;
            Texture* thumbTexture = findThumbnail(cubePath);
            if (thumbTexture) {
                ImU32 border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
                if (dif){
                    border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                }
                drawImageWithBorderAndRounding(thumbTexture, ImVec2(thumbSize, thumbSize), ImGui::GetStyle().FrameRounding, border_col);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    Widgets::image(Backend::getImGuiTexture(thumbTexture->getRender()), ImVec2(thumbTexture->getWidth(), thumbTexture->getHeight()));
                    ImGui::EndTooltip();
                }
                dragMax = ImGui::GetItemRectMax();
            }

            float iconButtonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float rowHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);

            ImGui::BeginChild("textureframe", ImVec2(- iconButtonWidth - ImGui::GetStyle().ItemSpacing.x, rowHeight),
                false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            float textWidth = ImGui::CalcTextSize(cubeName.c_str()).x;
            float availWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(availWidth - textWidth - 2);
            ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
            if (dif)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::Text("%s", cubeName.c_str());
            if (dif)
                ImGui::PopStyleColor();

            ImGui::EndChild();
            if (!cubePath.empty()){
                ImGui::SetItemTooltip("%s", cubePath.c_str());
            }

            ImGui::PopStyleColor();

            dragMax.x = std::max(dragMax.x, ImGui::GetItemRectMax().x);
            dragMax.y = std::max(dragMax.y, ImGui::GetItemRectMax().y);

            ImGui::SameLine();

            if (ImGui::Button(ICON_FA_FOLDER_OPEN)) {
                std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
                if (!path.empty()) {
                    std::filesystem::path assetsPath = project->getAssetsPath();
                    std::filesystem::path filePath = std::filesystem::absolute(path);

                    std::error_code ec;
                    auto relative = std::filesystem::relative(filePath, assetsPath, ec);
                    if (ec || relative.empty() || *relative.begin() == "..") {
                        ImGui::OpenPopup("File Import Error##cube");
                    }else{
                        Texture texture = newValue;
                        texture.setCubeMap(FileUtils::pathToGenericUtf8(relative));
                        for (Entity& entity : entities){
                            cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, texture, settings.onValueChanged);
                            submitRowCommand(sceneProject, cpType, id);
                            finishProperty = true;
                        }
                    }
                }
            }

            dragMax.x = std::max(dragMax.x, ImGui::GetItemRectMax().x);
            dragMax.y = std::max(dragMax.y, ImGui::GetItemRectMax().y);

            dragDropResourcesTextureCube(cpType, id, dragMin, dragMax, sceneProject, entities);

            if (EditorModal::begin("File Import Error##cube", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Selected file must be within the assets directory.");
                EditorModal::message("Selected file must be within the assets directory.");
                ImGui::Separator();

                float buttonWidth = 120;
                float windowWidth = ImGui::GetWindowSize().x;
                ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);
                if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }else{
            // Engine cubemap indexing (sokol/OpenGL-style):
            // 0=(+X), 1=(-X), 2=(+Y), 3=(-Y), 4=(+Z), 5=(-Z)
            static const char* faceNames[6] = {"Positive X", "Negative X", "Positive Y", "Negative Y", "Positive Z", "Negative Z"};
            static const size_t faceIndexMap[6] = {0, 1, 2, 3, 4, 5};

            for (size_t uiFace = 0; uiFace < 6; uiFace++){
                const size_t faceIndex = faceIndexMap[uiFace];
                ImGui::PushID(static_cast<int>(uiFace));

                std::string facePath = newValue.getPath(faceIndex);
                std::string faceName = facePath;
                if (std::filesystem::exists(faceName)) {
                    faceName = std::filesystem::path(faceName).filename().string();
                }
                if (faceName.empty()) {
                    faceName = "< Not set >";
                }

                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", faceNames[uiFace]);
                ImGui::PopStyleVar();

                // Drag/drop target rect should cover thumbnail + path + button
                ImVec2 dragMin = ImGui::GetCursorScreenPos();
                ImVec2 dragMax = dragMin;

                float thumbSize = ImGui::GetFrameHeight() * 3;
                Texture* thumbTexture = findThumbnail(facePath);
                if (thumbTexture) {
                    ImU32 border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
                    if (dif){
                        border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    }
                    drawImageWithBorderAndRounding(thumbTexture, ImVec2(thumbSize, thumbSize), ImGui::GetStyle().FrameRounding, border_col);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        Widgets::image(Backend::getImGuiTexture(thumbTexture->getRender()), ImVec2(thumbTexture->getWidth(), thumbTexture->getHeight()));
                        ImGui::EndTooltip();
                    }
                    dragMax = ImGui::GetItemRectMax();
                }

                // Path row under thumbnail
                float iconButtonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                float rowHeight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2;

                ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);

                ImGui::BeginChild("textureframe", ImVec2(- iconButtonWidth - ImGui::GetStyle().ItemSpacing.x, rowHeight),
                    false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                float textWidth = ImGui::CalcTextSize(faceName.c_str()).x;
                float availWidth = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(availWidth - textWidth - 2);
                ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
                if (dif)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::Text("%s", faceName.c_str());
                if (dif)
                    ImGui::PopStyleColor();

                ImGui::EndChild();
                if (!facePath.empty()){
                    ImGui::SetItemTooltip("%s", facePath.c_str());
                }

                ImGui::PopStyleColor();

                dragMax.x = std::max(dragMax.x, ImGui::GetItemRectMax().x);
                dragMax.y = std::max(dragMax.y, ImGui::GetItemRectMax().y);

                ImGui::SameLine();

                if (ImGui::Button(ICON_FA_FOLDER_OPEN)) {
                    std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
                    if (!path.empty()) {
                        std::filesystem::path assetsPath = project->getAssetsPath();
                        std::filesystem::path filePath = std::filesystem::absolute(path);

                        // References are stored relative to the assets root
                        std::error_code ec;
                        auto relative = std::filesystem::relative(filePath, assetsPath, ec);
                        if (ec || relative.empty() || *relative.begin() == "..") {
                            ImGui::OpenPopup("File Import Error##cube");
                        }else{
                            Texture texture = newValue;
                            texture.setCubePath(faceIndex, FileUtils::pathToGenericUtf8(relative));
                            for (Entity& entity : entities){
                                cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, texture, settings.onValueChanged);
                                submitRowCommand(sceneProject, cpType, id);
                                finishProperty = true;
                            }
                        }
                    }
                }

                dragMax.x = std::max(dragMax.x, ImGui::GetItemRectMax().x);
                dragMax.y = std::max(dragMax.y, ImGui::GetItemRectMax().y);

                dragDropResourcesTextureCube(cpType, id, dragMin, dragMax, sceneProject, entities, static_cast<int>(faceIndex));

                // Error popup modal
                if (EditorModal::begin("File Import Error##cube", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Selected file must be within the assets directory.");
                    EditorModal::message("Selected file must be within the assets directory.");
                    ImGui::Separator();

                    float buttonWidth = 120;
                    float windowWidth = ImGui::GetWindowSize().x;
                    ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);
                    if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
        }

        ImGui::PopID();
        ImGui::EndGroup();

    }else if (type == RowPropertyType::Material){
        Material* value = nullptr;
        std::map<Entity, Material> eValue;
        bool dif = false;
        Material* defArr = nullptr;
        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defArr = static_cast<Material*>(prop.def);
            eValue[entity] = *static_cast<Material*>(prop.ref);
            if (value){
                if (*value != eValue[entity])
                    dif = true;
            }
            value = &eValue[entity];
        }
        Material newValue = *value;

        bool defChanged = false;
        if (defArr){
            defChanged = (newValue != *defArr);
        }
        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<Material>(project, sceneProject->id, entity, cpType, id, *defArr, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ImGui::BeginGroup();

        bool receiveIBL = true;
        std::string meshShader;
        if (cpType == ComponentType::MeshComponent && !entities.empty()) {
            const MeshComponent& previewMesh = sceneProject->scene->getComponent<MeshComponent>(entities[0]);
            receiveIBL = previewMesh.receiveIBL;
            // a material without its own shader draws with the mesh's, else the scene's
            meshShader = previewMesh.customShader.empty() ? sceneProject->scene->getDefaultCustomShader(ShaderType::MESH)
                                                          : previewMesh.customShader.path;
        }

        Texture texRender = getMaterialPreview(newValue, id, receiveIBL, meshShader);
        float thumbSize = ImGui::GetFrameHeight() * 3;
        Widgets::image(Backend::getImGuiTexture(texRender.getRender()), ImVec2(thumbSize, thumbSize));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            materialButtonGroups[id] = !materialButtonGroups[id];
        }

        unsigned int materialSubmeshIndex = 0;
        bool hasMaterialSubmeshIndex = false;
        auto submeshPos = id.find('[');
        auto submeshEnd = id.find(']');
        if (submeshPos != std::string::npos && submeshEnd != std::string::npos) {
            try {
                materialSubmeshIndex = std::stoul(id.substr(submeshPos + 1, submeshEnd - submeshPos - 1));
                hasMaterialSubmeshIndex = true;
            } catch (const std::exception&) {
                materialSubmeshIndex = 0;
            }
        }

        bool hasLinkedMaterial = false;
        if (sceneProject && hasMaterialSubmeshIndex) {
            for (Entity& entity : entities) {
                if (project->isMaterialFileLinked(sceneProject->id, entity, materialSubmeshIndex)) {
                    hasLinkedMaterial = true;
                    break;
                }
            }
        }

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);

        ImVec2 arrowButtonSize = ImGui::CalcItemSize(ImVec2(0, 0), ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
        ImVec2 unlinkButtonSize = arrowButtonSize;
        float frameWidth = -arrowButtonSize.x - unlinkButtonSize.x - ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::BeginChild("textureframe", ImVec2(frameWidth, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2), 
            false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        std::string matName = newValue.name;
        if (!matName.empty()) {
            std::filesystem::path matPath = std::filesystem::path(matName);
            if (!matPath.is_absolute()) {
                matPath = project->getProjectPath() / matPath;
            }
            if (std::filesystem::exists(matPath)) {
                matName = std::filesystem::path(newValue.name).filename().string();
            }
        }
        if (matName.empty()) {
            matName = "< Not defined >";
        }

        float textWidth = ImGui::CalcTextSize(matName.c_str()).x;
        float availWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(availWidth - textWidth - 2);
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        if (dif)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Text("%s", matName.c_str());
        if (dif)
            ImGui::PopStyleColor();

        ImGui::EndChild();
        if (!newValue.name.empty()){
            ImGui::SetItemTooltip("%s", newValue.name.c_str());
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (ImGui::ArrowButton("##toggle_mesh", materialButtonGroups[id] ? ImGuiDir_Up : ImGuiDir_Down)){
            materialButtonGroups[id] = !materialButtonGroups[id];
        }
        UIProbe::item("inspector.materialToggle." + id);

        ImGui::SameLine();

        ImGui::BeginDisabled(!hasLinkedMaterial);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetStyle().FramePadding.y));

        float currentScale = ImGui::GetFont()->Scale;
        ImGui::SetWindowFontScale(currentScale * 0.75f);

        if (ImGui::Button(ICON_FA_LINK_SLASH "##unlink_material", unlinkButtonSize)){
            if (sceneProject && hasMaterialSubmeshIndex) {
                auto* unlinkCmd = new UnlinkMaterialCmd(project, sceneProject->id, cpType, id, materialSubmeshIndex, entities, settings.onValueChanged);
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(unlinkCmd);
                finishProperty = true;
            }
        }

        ImGui::SetWindowFontScale(currentScale);
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(hasLinkedMaterial ? "Unlink material file" : "Material is not linked to a file");
        }

        ImGui::EndGroup();

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            std::string materialStr = YAML::Dump(Stream::encodeMaterial(newValue));

            std::vector<char> materialPayload;
            MaterialPayload header{0x4D54524C, NULL_PROJECT_SCENE, NULL_ENTITY, 0};

            if (cpType == ComponentType::MeshComponent && !entities.empty()) {
                header.sceneId = sceneProject->id;
                header.entity = entities[0];

                auto p = id.find('[');
                auto e = id.find(']');
                if (p != std::string::npos && e != std::string::npos) {
                    try {
                        header.submeshIndex = std::stoul(id.substr(p + 1, e - p - 1));
                    } catch (const std::exception&) {
                        header.submeshIndex = 0;
                    }
                }
            }

            materialPayload.resize(sizeof(MaterialPayload) + materialStr.size());
            std::memcpy(materialPayload.data(), &header, sizeof(MaterialPayload));
            std::memcpy(materialPayload.data() + sizeof(MaterialPayload), materialStr.data(), materialStr.size());

            ImGui::SetDragDropPayload("material", materialPayload.data(), materialPayload.size());
            ImGui::Text("Moving material");
            float imageDragSize = 32;
            float availWidth = ImGui::GetCurrentWindow()->Size.x;
            float xPos = (availWidth - imageDragSize) * 0.5f;
            ImGui::SetCursorPosX(xPos);
            Widgets::image(Backend::getImGuiTexture(texRender.getRender()), ImVec2(imageDragSize, imageDragSize));
            ImGui::EndDragDropSource();
        }

        // Material file drop preview state (must be outside drag-drop blocks for restore access)
        static std::string cachedMatDropPath;
        static Material cachedMatDropMaterial;
        static std::map<Entity, Material> matDropOriginals;
        static bool matDropPreviewing = false;
        static std::string matDropPropertyId;

        auto restoreMatDropPreview = [&]() {
            if (matDropPreviewing) {
                for (auto& [ent, origMat] : matDropOriginals) {
                    PropertyData prop = Catalog::findProperty(sceneProject->scene, ent, cpType, matDropPropertyId);
                    if (prop.ref) {
                        Material* matRef = static_cast<Material*>(prop.ref);
                        *matRef = origMat;
                        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(ent);
                        if (mesh) {
                            auto p = matDropPropertyId.find('[');
                            auto e = matDropPropertyId.find(']');
                            if (p != std::string::npos && e != std::string::npos) {
                                unsigned int sIdx = std::stoul(matDropPropertyId.substr(p + 1, e - p - 1));
                                if (sIdx < mesh->numSubmeshes) {
                                    mesh->submeshes[sIdx].needUpdateTexture = true;
                                }
                            }
                        }
                    }
                }
                matDropPreviewing = false;
                matDropOriginals.clear();
                cachedMatDropPath.clear();
            }
        };

        if (!(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                std::vector<std::string> receivedStrings = Util::getStringsFromPayload(payload);

                if (!receivedStrings.empty()) {
                    std::error_code ec;
                    std::filesystem::path relativePath = std::filesystem::relative(receivedStrings[0], project->getProjectPath(), ec);

                    if (ec) {
                        if (payload->IsDelivery()) {
                            restoreMatDropPreview();
                            ImGui::OpenPopup("File Import Error##material");
                        }
                    } else {
                        std::string droppedRelativePath = relativePath.lexically_normal().generic_string();
                        // Material files are project files; the image below is an asset
                        const bool insideAssets = project->isInsideAssetsPath(receivedStrings[0]);
                        const std::string droppedImagePath = droppedAssetPath(project, receivedStrings[0], insideAssets);

                        if (Util::isMaterialFile(droppedRelativePath)) {
                            try {
                                if (cachedMatDropPath != droppedRelativePath) {
                                    cachedMatDropPath = droppedRelativePath;
                                    YAML::Node materialNode = DocumentFiles::read(project->getProjectPath() / relativePath, DocumentKind::Material).root;
                                    cachedMatDropMaterial = Stream::decodeMaterial(materialNode);
                                    cachedMatDropMaterial.name = droppedRelativePath;
                                }

                                if (!payload->IsDelivery()) {
                                    // Preview: save originals and apply
                                    if (!matDropPreviewing) {
                                        matDropOriginals.clear();
                                        matDropPropertyId = id;
                                        for (Entity& entity : entities) {
                                            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
                                            matDropOriginals[entity] = *static_cast<Material*>(prop.ref);
                                        }
                                        matDropPreviewing = true;
                                    }
                                    for (Entity& entity : entities) {
                                        PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
                                        Material* matRef = static_cast<Material*>(prop.ref);
                                        if (*matRef != cachedMatDropMaterial) {
                                            *matRef = cachedMatDropMaterial;
                                            MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
                                            if (mesh) {
                                                auto pos = id.find('[');
                                                auto end = id.find(']');
                                                if (pos != std::string::npos && end != std::string::npos) {
                                                    unsigned int sIdx = std::stoul(id.substr(pos + 1, end - pos - 1));
                                                    if (sIdx < mesh->numSubmeshes) {
                                                        mesh->submeshes[sIdx].needUpdateTexture = true;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                } else {
                                    // Delivery: restore originals, then issue commands
                                    restoreMatDropPreview();

                                    for (Entity& entity : entities) {
                                        auto pos = id.find('[');
                                        auto end = id.find(']');
                                        if (pos != std::string::npos && end != std::string::npos) {
                                            unsigned int sIdx = std::stoul(id.substr(pos + 1, end - pos - 1));
                                            cmd = new LinkMaterialCmd(project, sceneProject->id, entity, cpType, id, sIdx, cachedMatDropMaterial, settings.onValueChanged);
                                        } else {
                                            cmd = new PropertyCmd<Material>(project, sceneProject->id, entity, cpType, id, cachedMatDropMaterial, settings.onValueChanged);
                                        }
                                        submitRowCommand(sceneProject, cpType, id);
                                        finishProperty = true;
                                    }

                                    cachedMatDropPath.clear();
                                    ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                                }
                            } catch (const std::exception& e) {
                                Out::error("Error loading material file '%s': %s", droppedRelativePath.c_str(), e.what());
                            }
                        } else if (Util::isImageFile(droppedRelativePath)) {
                            std::string baseTexId = id + ".baseColorTexture";

                            if (!payload->IsDelivery()) {
                                // Preview: save originals and apply texture
                                if (!matDropPreviewing) {
                                    matDropOriginals.clear();
                                    matDropPropertyId = id;
                                    for (Entity& entity : entities) {
                                        PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
                                        matDropOriginals[entity] = *static_cast<Material*>(prop.ref);
                                    }
                                    matDropPreviewing = true;
                                }
                                for (Entity& entity : entities) {
                                    PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
                                    Material* matRef = static_cast<Material*>(prop.ref);
                                    if (matRef->baseColorTexture != Texture(droppedImagePath)) {
                                        matRef->baseColorTexture = Texture(droppedImagePath);
                                        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
                                        if (mesh) {
                                            auto pos = id.find('[');
                                            auto end = id.find(']');
                                            if (pos != std::string::npos && end != std::string::npos) {
                                                unsigned int sIdx = std::stoul(id.substr(pos + 1, end - pos - 1));
                                                if (sIdx < mesh->numSubmeshes) {
                                                    mesh->submeshes[sIdx].needUpdateTexture = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                // Delivery: restore originals, then issue texture command
                                restoreMatDropPreview();

                                if (insideAssets) {
                                    Texture texture(droppedImagePath);
                                    for (Entity& entity : entities) {
                                        cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, baseTexId, texture, settings.onValueChanged);
                                        submitRowCommand(sceneProject, cpType, id);
                                        finishProperty = true;
                                    }
                                } else {
                                    Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                                }

                                cachedMatDropPath.clear();
                                ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                            }
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        } else if (matDropPreviewing && matDropPropertyId == id) {
            // Drag ended without delivery — restore preview
            restoreMatDropPreview();
        }

        if (EditorModal::begin("File Import Error##material", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Selected file must be within the project directory.");
            EditorModal::message("Selected file must be within the project directory.");
            ImGui::Separator();

            float buttonWidth = 120;
            float windowWidth = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);
            if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        result = materialButtonGroups[id];

    }else if (type == RowPropertyType::LocalEntity){
        Entity* value = nullptr;
        std::map<Entity, Entity> eValue;
        bool different = false;
        unsigned int* defVal = nullptr;

        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defVal = static_cast<unsigned int*>(prop.def);
            eValue[entity] = *static_cast<unsigned int*>(prop.ref);
            if (value && *value != eValue[entity]){
                different = true;
            }
            value = &eValue[entity];
        }

        Entity newValue = value ? *value : NULL_ENTITY;
        bool defChanged = (defVal && newValue != *defVal);

        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, *defVal, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ImGui::BeginGroup();

        std::string entityName = "None";
        if (newValue != NULL_ENTITY && sceneProject->scene->isEntityCreated(newValue)) {
            entityName = sceneProject->scene->getEntityName(newValue);
            if (entityName.empty()) {
                entityName = "GameObject " + std::to_string(newValue);
            }
        }

        bool invalidSelection = false;

        if (different || invalidSelection) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (different) {
                entityName = "---";
            }
        }

        std::string buttonLabel = ICON_FA_CIRCLE_DOT " " + entityName + "##local_entity_" + id;
        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x;
        float availableWidth = settings.secondColSize > 0 ? settings.secondColSize : ImGui::GetContentRegionAvail().x;
        ImVec2 inputSize = ImVec2(std::max(0.0f, availableWidth - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2), 0);

        std::string pickerPopupId = "##ep_" + id;

        if (ImGui::Button(buttonLabel.c_str(), inputSize)) {
            if (newValue != NULL_ENTITY && sceneProject->scene->isEntityCreated(newValue)) {
                project->clearSelectedEntities(sceneProject->id);
                project->addSelectedEntity(sceneProject->id, newValue);
            } else if (!different) {
                ImGui::OpenPopup(pickerPopupId.c_str());
                memset(entityPickerSearchBuffer, 0, sizeof(entityPickerSearchBuffer));
            }
        }

        if (ImGui::IsItemHovered() && !different) {
            if (newValue != NULL_ENTITY) {
                ImGui::SetTooltip("GameObject: %s (ID: %u)", entityName.c_str(), static_cast<unsigned int>(newValue));
            } else {
                ImGui::SetTooltip("Click to select the GameObject");
            }
        }

        // Entity picker popup
        auto pickerResult = drawEntityPickerPopup(pickerPopupId, settings.entityFilter, sceneProject, false, newValue, 0, settings.excludeEntities);
        if (pickerResult.chosen) {
            for (Entity& entity : entities) {
                cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, pickerResult.entity, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                const EntityPayload* entityPayload = static_cast<const EntityPayload*>(payload->Data);
                Entity droppedEntity = entityPayload->entity;
                // the id means a GameObject of this scene only
                const std::string otherScene = sameSceneDropProblem(*project, *entityPayload, sceneProject->id);
                bool excluded = std::find(settings.excludeEntities.begin(), settings.excludeEntities.end(), droppedEntity) != settings.excludeEntities.end();
                bool valid = otherScene.empty() && sceneProject->scene->isEntityCreated(droppedEntity) && !excluded;

                if (!otherScene.empty()){
                    ImGui::SetTooltip("%s", otherScene.c_str());
                }else if (!valid && ImGui::IsItemHovered()){
                    ImGui::SetTooltip(excluded ? "Cannot reference this GameObject here" : "Invalid GameObject");
                }

                if (payload->IsDelivery() && valid) {
                    for (Entity& entity : entities) {
                        cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, droppedEntity, settings.onValueChanged);
                        submitRowCommand(sceneProject, cpType, id);
                    }
                    finishProperty = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (different || invalidSelection) {
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(newValue == NULL_ENTITY && !different);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button((ICON_FA_XMARK "##clear_local_entity_" + id).c_str())) {
            for (Entity& entity : entities) {
                cmd = new PropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, id, NULL_ENTITY, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Clear GameObject reference");
        }

        ImGui::EndGroup();

    }else if (type == RowPropertyType::ExternalEntity){
        Entity* value = nullptr;
        std::map<Entity, Entity> eValue;
        std::map<Entity, uint32_t> eSceneId; // sceneId per selected entity
        bool different = false;
        unsigned int* defVal = nullptr;

        // Parse script index and property name from id (format: "scripts[N].propName")
        size_t scriptIdx = 0;
        std::string propName;
        {
            size_t bracket = id.find('[');
            size_t closeBracket = id.find(']');
            size_t dot = id.find('.', closeBracket != std::string::npos ? closeBracket : 0);
            if (bracket != std::string::npos && closeBracket != std::string::npos && dot != std::string::npos) {
                scriptIdx = std::stoul(id.substr(bracket + 1, closeBracket - bracket - 1));
                propName = id.substr(dot + 1);
            }
        }

        for (Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, id);
            defVal = static_cast<unsigned int*>(prop.def);
            eValue[entity] = *static_cast<unsigned int*>(prop.ref);

            // Read sceneId from ScriptProperty
            uint32_t sid = 0;
            ScriptComponent* sc = sceneProject->scene->findComponent<ScriptComponent>(entity);
            if (sc && scriptIdx < sc->scripts.size()) {
                for (auto& sp : sc->scripts[scriptIdx].properties) {
                    if (sp.name == propName && std::holds_alternative<EntityReference>(sp.value)) {
                        sid = std::get<EntityReference>(sp.value).sceneId;
                        break;
                    }
                }
            }
            eSceneId[entity] = sid;

            if (value && (*value != eValue[entity] || eSceneId[entities[0]] != sid)){
                different = true;
            }
            value = &eValue[entity];
        }

        Entity newValue = value ? *value : NULL_ENTITY;
        uint32_t currentSceneId = !entities.empty() ? eSceneId[entities[0]] : 0;
        bool defChanged = (defVal && newValue != *defVal);

        if (propertyHeader(label, settings.secondColSize, defChanged, settings.child, settings.indent)){
            for (Entity& entity : entities){
                // Write entity and sceneId as one command so prefab propagation sees the
                // complete reference (a split write would propagate a stale sceneId).
                cmd = new PropertyCmd<EntityReference>(project, sceneProject->id, entity, cpType, id, EntityReference{*defVal, 0}, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
                finishProperty = true;
            }
        }

        ImGui::BeginGroup();

        // Resolve entity name, potentially from a different scene
        std::string entityName = "None";
        std::string sceneSuffix = "";
        Scene* resolvedScene = nullptr;

        if (newValue != NULL_ENTITY) {
            if (currentSceneId != 0) {
                SceneProject* targetSceneProject = project->getScene(currentSceneId);
                if (targetSceneProject && targetSceneProject->scene && targetSceneProject->scene->isEntityCreated(newValue)) {
                    resolvedScene = targetSceneProject->scene;
                    entityName = resolvedScene->getEntityName(newValue);
                    if (entityName.empty()) {
                        entityName = "GameObject " + std::to_string(newValue);
                    }
                    sceneSuffix = " (" + targetSceneProject->name + ")";
                }
            } else if (sceneProject->scene->isEntityCreated(newValue)) {
                resolvedScene = sceneProject->scene;
                entityName = resolvedScene->getEntityName(newValue);
                if (entityName.empty()) {
                    entityName = "GameObject " + std::to_string(newValue);
                }
            }
        }

        bool invalidSelection = (newValue != NULL_ENTITY && !resolvedScene);

        if (different || invalidSelection) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (different) {
                entityName = "---";
                sceneSuffix = "";
            } else if (invalidSelection) {
                entityName = "GameObject " + std::to_string(newValue);
                SceneProject* storedScene = (currentSceneId != 0) ? project->getScene(currentSceneId) : sceneProject;
                sceneSuffix = storedScene ? " (" + storedScene->name + ")" : "";
            }
        }

        std::string displayLabel = entityName + sceneSuffix;
        std::string buttonLabel = ICON_FA_CIRCLE_DOT " " + displayLabel + "##ext_entity_" + id;
        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x;
        ImVec2 inputSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);

        // Tint button color to differentiate ExternalEntity from LocalEntity
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ExtEntityButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Colors::ExtEntityButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::ExtEntityButtonActive);

        std::string extPickerPopupId = "##ep_ext_" + id;

        if (ImGui::Button(buttonLabel.c_str(), inputSize)) {
            if (resolvedScene && newValue != NULL_ENTITY) {
                uint32_t selectSceneId = (currentSceneId != 0) ? currentSceneId : sceneProject->id;
                project->clearSelectedEntities(selectSceneId);
                project->addSelectedEntity(selectSceneId, newValue);
            } else if (!different) {
                ImGui::OpenPopup(extPickerPopupId.c_str());
                memset(entityPickerSearchBuffer, 0, sizeof(entityPickerSearchBuffer));
            }
        }
        ImGui::PopStyleColor(3);

        if (ImGui::IsItemHovered() && !different) {
            if (newValue != NULL_ENTITY) {
                std::string tipSceneName = sceneProject->name;
                uint32_t tipSceneId = sceneProject->id;
                if (currentSceneId != 0) {
                    SceneProject* tipScene = project->getScene(currentSceneId);
                    if (tipScene) {
                        tipSceneName = tipScene->name;
                        tipSceneId = currentSceneId;
                    }
                }
                ImGui::SetTooltip("GameObject: %s (ID: %u)\nScene: %s (ID: %u)",
                    entityName.c_str(), static_cast<unsigned int>(newValue),
                    tipSceneName.c_str(), static_cast<unsigned int>(tipSceneId));
            } else {
                ImGui::SetTooltip("Click to select the GameObject");
            }
        }

        // Entity picker popup
        auto extPickerResult = drawEntityPickerPopup(extPickerPopupId, settings.entityFilter, sceneProject, true, newValue, currentSceneId);
        if (extPickerResult.chosen) {
            for (Entity& entity : entities) {
                cmd = new PropertyCmd<EntityReference>(project, sceneProject->id, entity, cpType, id, EntityReference{extPickerResult.entity, extPickerResult.sceneId}, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                const EntityPayload* entityPayload = static_cast<const EntityPayload*>(payload->Data);
                Entity droppedEntity = entityPayload->entity;
                uint32_t droppedSceneId = payloadSceneId(*entityPayload, project->getSelectedSceneId());

                // a scene loaded with this one (its parents, children and siblings), else the
                // reference would find nothing when the game runs
                const std::string problem = referenceDropProblem(*project, *entityPayload, sceneProject->id);
                const bool valid = problem.empty();

                // Determine sceneId to store: 0 if same scene, source scene ID if cross-scene
                uint32_t storedSceneId = (droppedSceneId != sceneProject->id) ? droppedSceneId : 0;

                if (!valid){
                    ImGui::SetTooltip("%s", problem.c_str());
                }

                if (payload->IsDelivery() && valid) {
                    for (Entity& entity : entities) {
                        cmd = new PropertyCmd<EntityReference>(project, sceneProject->id, entity, cpType, id, EntityReference{droppedEntity, storedSceneId}, settings.onValueChanged);
                        submitRowCommand(sceneProject, cpType, id);
                    }
                    finishProperty = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (different || invalidSelection) {
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(newValue == NULL_ENTITY && !different);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button((ICON_FA_XMARK "##clear_ext_entity_" + id).c_str())) {
            for (Entity& entity : entities) {
                cmd = new PropertyCmd<EntityReference>(project, sceneProject->id, entity, cpType, id, EntityReference{NULL_ENTITY, 0}, settings.onValueChanged);
                submitRowCommand(sceneProject, cpType, id);
            }
            finishProperty = true;
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Clear GameObject reference");
        }

        ImGui::EndGroup();

    }

    if (ImGui::IsItemDeactivatedAfterEdit() || finishProperty) {
        if (cmd){
            cmd->setNoMerge();
            cmd = nullptr;
        }
        finishProperty = false;
        // a recorded edit: one key edit per row edit
        finishRecordRow();
    }

    return result;
}

// ---- Record sources (animation clips 9.R5) ----

bool editor::InspectorWindow::recordRowEdit(SceneProject* sceneProject, ComponentType cpType, const std::string& id,
                                            const std::vector<Entity>& entities, const std::function<std::vector<float>(Entity)>& valueOf){
    if (!sceneProject || cpType != ComponentType::Transform) return false;
    const std::vector<ClipChannel> channels = ClipRecordHooks::channelsOf(cpType, id);
    if (channels.size() != 1) return false;
    const ClipChannel channel = channels.front();
    if (!ClipRecordHooks::claimsAny(sceneProject->id, entities, channel)) return false;
    const std::string mergeKey = ClipRecordHooks::gestureMergeKey(RecordSource::Inspector, nullptr);
    for (Entity entity : entities){
        // the selection's GameObjects Record does not key stay as they are
        if (!ClipRecordHooks::claims(sceneProject->id, entity, channel)) continue;
        ClipRecordValue value;
        value.channel = channel;
        value.value = valueOf(entity);
        ClipRecordHooks::record(sceneProject->id, entity, value, mergeKey);
    }
    return true;
}

editor::ClipPropertyKey editor::InspectorWindow::rowPropertyKey(SceneProject* sceneProject, Entity entity, ComponentType cpType, const std::string& id) const{
    ClipPropertyKey key;
    if (!sceneProject || !sceneProject->scene) return key;
    if (cpType == ComponentType::ScriptComponent){
        // "scripts[N].<field>": a declared property (or enabled) of a Lua script
        if (id.compare(0, 8, "scripts[") != 0) return key;
        const size_t close = id.find("].", 8);
        if (close == std::string::npos) return key;
        const size_t index = (size_t)std::strtoul(id.c_str() + 8, nullptr, 10);
        const ScriptComponent* scripts = sceneProject->scene->findComponent<ScriptComponent>(entity);
        if (!scripts || index >= scripts->scripts.size() || scripts->scripts[index].type != ScriptType::LUA) return key;
        return ClipRecordHooks::scriptPropertyOf(scripts->scripts[index].className, id.substr(close + 2));
    }
    ClipRecordHooks::propertyOf(cpType, id, key);
    return key;
}

bool editor::InspectorWindow::recordPropertyCommand(SceneProject* sceneProject, ComponentType cpType, const std::string& id, Command* command){
    const RecordablePropertyCommand* recordable = dynamic_cast<const RecordablePropertyCommand*>(command);
    std::vector<std::pair<Entity, std::vector<float>>> values;
    if (!sceneProject || !ClipRecordHooks::hasRecordListeners() || !recordable || !recordable->newValueFloats(values)) return false;
    bool claimed = false;
    for (const auto& entry : values){
        const ClipPropertyKey key = rowPropertyKey(sceneProject, entry.first, cpType, id);
        if (!key.empty() && ClipRecordHooks::claimsProperty(sceneProject->id, entry.first, key)) claimed = true;
    }
    if (!claimed) return false;
    const std::string mergeKey = ClipRecordHooks::gestureMergeKey(RecordSource::Inspector, nullptr);
    for (const auto& entry : values){
        // the selection's GameObjects Record does not key stay as they are
        const ClipPropertyKey key = rowPropertyKey(sceneProject, entry.first, cpType, id);
        if (key.empty() || !ClipRecordHooks::claimsProperty(sceneProject->id, entry.first, key)) continue;
        anim::PropertyRef ref;
        anim::parsePropertyRef(key.component, key.property, ref);
        const anim::PropertyBinding binding = anim::bindProperty(sceneProject->scene, entry.first, ref, 0);
        if (binding.status != anim::PropertyBindStatus::Bound) continue;
        // the stored value the row computed (linear colours, cone cosines) in the keys' units
        float value[4] = {0, 0, 0, 0};
        for (size_t k = 0; k < 4 && k < entry.second.size(); k++) value[k] = entry.second[k];
        anim::storedToKey(binding.kind, binding.display, value, binding.width);
        ClipRecordValue recorded;
        recorded.channel = ClipChannel::Property;
        recorded.property = key;
        recorded.value.assign(value, value + binding.width);
        ClipRecordHooks::record(sceneProject->id, entry.first, recorded, mergeKey);
    }
    return true;
}

void editor::InspectorWindow::submitRowCommand(SceneProject* sceneProject, ComponentType cpType, const std::string& id){
    if (cpType != ComponentType::Transform && cmd && recordPropertyCommand(sceneProject, cpType, id, cmd)){
        delete cmd;
        cmd = nullptr;
        return;
    }
    CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
}

bool editor::InspectorWindow::recordPropertyToggle(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType cpType,
                                                   const ClipPropertyKey& key, bool value){
    (void)cpType;
    if (!sceneProject || key.empty() || !ClipRecordHooks::claimsAnyProperty(sceneProject->id, entities, key)) return false;
    // one key edit per click
    const std::string mergeKey;
    for (Entity entity : entities){
        if (!ClipRecordHooks::claimsProperty(sceneProject->id, entity, key)) continue;
        ClipRecordValue recorded;
        recorded.channel = ClipChannel::Property;
        recorded.property = key;
        recorded.value = {value ? 1.0f : 0.0f};
        ClipRecordHooks::record(sceneProject->id, entity, recorded, mergeKey);
    }
    return true;
}

void editor::InspectorWindow::finishRecordRow(){
    // the row's record gesture (only the Inspector's: another source's stays open)
    ClipRecordHooks::endGesture(RecordSource::Inspector);
}

bool editor::InspectorWindow::setMorphWeight(SceneProject* sceneProject, const std::vector<Entity>& entities, int index, float value){
    if (!sceneProject || !sceneProject->scene || index < 0 || index >= MAX_MORPHTARGETS || !std::isfinite(value)) return false;
    Scene* scene = sceneProject->scene;
    // meshes with blend shapes (the rows show for those only)
    std::vector<Entity> meshes;
    for (Entity entity : entities){
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        bool morphs = false;
        for (unsigned int s = 0; mesh && s < mesh->numSubmeshes; s++) morphs = morphs || mesh->submeshes[s].hasMorphTarget;
        if (morphs) meshes.push_back(entity);
    }
    if (meshes.empty()) return false;
    // Record keys the claimed meshes (the others stay as they are)
    if (ClipRecordHooks::claimsAny(sceneProject->id, meshes, ClipChannel::Morph)){
        const std::string mergeKey = ClipRecordHooks::gestureMergeKey(RecordSource::Inspector, nullptr);
        for (Entity entity : meshes){
            if (!ClipRecordHooks::claims(sceneProject->id, entity, ClipChannel::Morph)) continue;
            const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            ClipRecordValue record;
            record.channel = ClipChannel::Morph;
            record.value.assign(mesh.morphWeights, mesh.morphWeights + MAX_MORPHTARGETS);
            record.value[(size_t)index] = value;
            ClipRecordHooks::record(sceneProject->id, entity, record, mergeKey);
        }
        return true;
    }
    std::vector<editor::MorphWeightsCmd::Target> targets;
    for (Entity entity : meshes){
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        editor::MorphWeightsCmd::Target target;
        target.entity = entity;
        std::copy(mesh.morphWeights, mesh.morphWeights + MAX_MORPHTARGETS, target.after.begin());
        target.after[(size_t)index] = value;
        targets.push_back(target);
    }
    auto* weightsCommand = new editor::MorphWeightsCmd(project, sceneProject->id, targets);
    weightsCommand->setEditedWeights({(size_t)index});   // the row's weight is the edit, the others as shown
    Command* command = weightsCommand;
    if (!CommandHandle::get(sceneProject->id)->addCommand(command)) return false;
    morphEditScene = sceneProject->id;
    return true;
}

void editor::InspectorWindow::finishMorphEdit(){
    // the next slider edit is its own undo step (the history is marked; no pointer is kept)
    if (morphEditScene != 0 && project->getScene(morphEditScene)) CommandHandle::get(morphEditScene)->mark();
    morphEditScene = 0;
    finishRecordRow();
}

void editor::InspectorWindow::drawBlendShapes(SceneProject* sceneProject, const std::vector<Entity>& entities){
    Scene* scene = sceneProject->scene;
    // meshes with morph targets only
    std::vector<Entity> meshes;
    for (Entity entity : entities){
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        bool morphs = false;
        for (unsigned int s = 0; mesh && s < mesh->numSubmeshes; s++) morphs = morphs || mesh->submeshes[s].hasMorphTarget;
        if (morphs) meshes.push_back(entity);
    }
    if (meshes.empty() || meshes.size() != entities.size()) return;

    // the target names of the first mesh (its own, from the model file: two meshes of a model can
    // name their targets differently), else its model's (the ModelComponent on it or an ancestor)
    std::map<int, std::string> names;
    const std::vector<std::string>& ownNames = scene->getComponent<MeshComponent>(meshes.front()).morphTargetNames;
    for (size_t i = 0; i < ownNames.size(); i++) names[(int)i] = ownNames[i];
    Entity owner = ownNames.empty() ? meshes.front() : NULL_ENTITY;
    for (int depth = 0; owner != NULL_ENTITY && depth < 256; depth++){
        if (const ModelComponent* model = scene->findComponent<ModelComponent>(owner)){
            for (const auto& entry : model->morphNameMapping) names[entry.second] = entry.first;
            break;
        }
        const Transform* transform = scene->findComponent<Transform>(owner);
        owner = transform ? transform->parent : NULL_ENTITY;
    }
    // the meshes' own target count (their morph position attributes)
    int count = 0;
    for (Entity entity : meshes){
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            int targets = 0;
            for (int a = (int)AttributeType::MORPHTARGET0; a <= (int)AttributeType::MORPHTARGET7; a++){
                if (mesh.submeshes[s].attributes.count((AttributeType)a)) targets++;
            }
            count = std::max(count, targets);
        }
    }
    if (count == 0) count = names.empty() ? MAX_MORPHTARGETS : std::min(MAX_MORPHTARGETS, names.rbegin()->first + 1);

    if (!ImGui::TreeNodeEx("Blend Shapes##mesh_blend_shapes", ImGuiTreeNodeFlags_SpanAvailWidth)) return;
    const bool recording = ClipRecordHooks::claimsAny(sceneProject->id, meshes, ClipChannel::Morph);
    size_t claimed = 0;
    bool animated = false;
    for (Entity entity : meshes){
        if (ClipRecordHooks::claims(sceneProject->id, entity, ClipChannel::Morph)) claimed++;
        animated = animated || ClipRecordHooks::animates(sceneProject->id, entity, ClipChannel::Morph);
    }
    if (recording && claimed < meshes.size()){
        ImGui::TextColored(Theme::Colors::WarningText, "Record keys %zu of the %zu selected; the others stay as they are", claimed, meshes.size());
    }
    // Unity: animated fields red, fields Record would start keying fainter
    const ImVec4 recordTint = ImLerp(Theme::Colors::ErrorText, ImVec4(0, 0, 0, 1), animated ? 0.55f : 0.78f);
    beginTable(ComponentType::MeshComponent, getLabelSize("Blend Shape 00"), "mesh_blend_shapes");
    for (int i = 0; i < count; i++){
        float value = scene->getComponent<MeshComponent>(meshes.front()).morphWeights[i];
        bool mixed = false;
        for (Entity entity : meshes){
            mixed = mixed || std::fabs(scene->getComponent<MeshComponent>(entity).morphWeights[i] - value) > 1e-5f;
        }
        auto it = names.find(i);
        const std::string label = it != names.end() && !it->second.empty() ? it->second : "Weight " + std::to_string(i);
        propertyHeader(label);
        ImGui::PushID(i);
        // Unity tints the fields Record keys
        if (recording) ImGui::PushStyleColor(ImGuiCol_FrameBg, recordTint);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##weight", &value, 0.0f, 1.0f, mixed ? "--" : "%.3f")){
            setMorphWeight(sceneProject, meshes, i, value);
        }
        if (recording) ImGui::PopStyleColor();
        if (ImGui::IsItemDeactivated()) finishMorphEdit();
        ImGui::SetItemTooltip("%s", !recording ? "The blend shape's weight (0 to 1)"
                                    : claimed < meshes.size() ? "Record keys this weight at the playhead on the selected meshes it animates; the others stay as they are"
                                    : "Record keys this weight at the playhead");
        ImGui::PopID();
    }
    endTable();
    ImGui::TreePop();
}

bool editor::InspectorWindow::propertyRowWithAutoButton(RowPropertyType propType, ComponentType cpType, std::string id, std::string label, std::string autoId, std::string autoLabel, SceneProject* sceneProject, std::vector<Entity> entities, RowSettings settings) {
    auto onValueChanged = settings.onValueChanged;
    settings.onValueChanged = [this, sceneProject, entities, cpType, autoId, onValueChanged]() {
        if (onValueChanged) {
            onValueChanged();
        }
        for (auto& entity : entities){
            PropertyData autoProp = Catalog::findProperty(sceneProject->scene, entity, cpType, autoId);
            bool* autoVal = static_cast<bool*>(autoProp.ref);
            if (autoVal && *autoVal) {
                editor::MultiPropertyCmd* cmd = new editor::MultiPropertyCmd();
                cmd->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, autoId, false);
                cmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
            }
        }
    };

    std::string helpText = settings.help;
    settings.help = "";

    bool rowChanged = propertyRow(propType, cpType, id, label, sceneProject, entities, settings);

    ImGui::SameLine();
    bool allAuto = true;
    bool anyAuto = false;
    for (auto& entity : entities){
        PropertyData autoProp = Catalog::findProperty(sceneProject->scene, entity, cpType, autoId);
        bool val = autoProp.ref && *static_cast<bool*>(autoProp.ref);
        if (val) anyAuto = true;
        else allAuto = false;
    }

    if (allAuto) {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
    } else if (anyAuto) {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
    }

    float btnPaddingY = std::max(0.0f, (ImGui::GetFrameHeight() - ImGui::GetFontSize()) / 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x / 4.0f, btnPaddingY));

    std::string autoBtnId = ICON_FA_WAND_MAGIC_SPARKLES "##auto_" + id;
    if (ImGui::Button(autoBtnId.c_str())) {
        bool targetValue = !allAuto;
        editor::MultiPropertyCmd* cmdAuto = new editor::MultiPropertyCmd();
        for (auto& entity : entities) {
            cmdAuto->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, autoId, targetValue);
        }
        cmdAuto->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmdAuto);
    }

    ImGui::PopStyleVar();

    if (anyAuto) {
        ImGui::PopStyleColor(allAuto ? 1 : 2);
    } else {
        ImGui::PopStyleColor(1);
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", autoLabel.c_str());
    }

    if (!helpText.empty()) {
        ImGui::SameLine(); helpMarker(helpText);
    }

    return rowChanged;
}

void editor::InspectorWindow::drawTransform(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    // Add this code to calculate appropriate step size based on selected scene
    RowSettings settingsPos;
    if (sceneProject && sceneProject->sceneRender) {
        Camera* camera = sceneProject->sceneRender->getCamera();
        if (sceneProject->sceneType == SceneType::SCENE_3D) {
            // For 3D scenes, scale step based on distance from target
            float distanceFromTarget = camera->getDistanceFromTarget();
            settingsPos.stepSize = std::max(0.01f, distanceFromTarget / 200.0f);
        } else {
            // For 2D scenes, use the zoom level
            SceneRender2D* sceneRender2D = static_cast<SceneRender2D*>(sceneProject->sceneRender);
            float zoom = sceneRender2D->getZoom();
            settingsPos.stepSize = std::max(0.01f, zoom * 1.0f);
        }
    }

    // the bones of a character the Pose Animation window poses belong to the pose: read-only
    bool posedBones = false;
    if (PoseEditSession* pose = PoseEditSession::active(); pose && sceneProject && pose->bonesApplied()){
        for (Entity entity : entities){
            if (entity != pose->model() && pose->ownsEntity(sceneProject->id, entity)) posedBones = true;
        }
    }
    if (posedBones){
        ImGui::TextColored(Theme::Colors::WarningText, "Posed by the Pose Animation window (read-only)");
    }
    // Write Bones: Exposed leaves this Transform at rest while the bone moves in the pose
    if (sceneProject && sceneProject->scene && entities.size() == 1 && sceneProject->scene->findComponent<BoneComponent>(entities[0]) &&
            isUnwrittenAnimatorBone(sceneProject->scene, entities[0])){
        ImGui::TextColored(Theme::Colors::WarningText, "Not written to this GameObject (Write Bones: Exposed)");
    }

    // a Lattice Follower's driven channels: LatticeSystem puts them back every frame, so they
    // are shown read-only (Unity's driven properties); the Scene view's tools move the rest
    bool drivenChannel[3] = {false, false, false};
    Entity drivingRest = NULL_ENTITY;
    if (sceneProject && sceneProject->scene) {
        for (Entity entity : entities) {
            for (int channel = 0; channel < 3; channel++) {
                Entity rest = LatticeEditorOps::getFollowerDrivingRest(sceneProject->scene, entity, channel == 0, channel == 1, channel == 2);
                if (rest != NULL_ENTITY) {
                    drivenChannel[channel] = true;
                    drivingRest = rest;
                }
            }
        }
    }

    // Record (animation clips 9.R5): the rows it keys are tinted red (Unity); GameObjects of the
    // selection it does not key stay as they are
    bool recorded[3] = {false, false, false};
    bool animated[3] = {false, false, false};
    const ClipChannel recordChannels[3] = {ClipChannel::Position, ClipChannel::Rotation, ClipChannel::Scale};
    size_t unrecorded = 0;
    if (sceneProject) {
        for (int channel = 0; channel < 3; channel++){
            recorded[channel] = ClipRecordHooks::claimsAny(sceneProject->id, entities, recordChannels[channel]);
            for (Entity entity : entities){
                animated[channel] = animated[channel] || (recorded[channel] && ClipRecordHooks::animates(sceneProject->id, entity, recordChannels[channel]));
            }
        }
        if (recorded[0]) {
            for (Entity entity : entities) {
                if (!ClipRecordHooks::claims(sceneProject->id, entity, ClipChannel::Position)) unrecorded++;
            }
        }
    }
    if (unrecorded > 0) {
        ImGui::TextColored(Theme::Colors::WarningText, "Record keys %zu of the %zu selected; the others stay as they are",
                           entities.size() - unrecorded, entities.size());
    }
    // Unity: an animated row red, a row Record would start keying a fainter red
    const ImVec4 animatedTint = ImLerp(Theme::Colors::ErrorText, ImVec4(0, 0, 0, 1), 0.55f);
    const ImVec4 recordTint = ImLerp(Theme::Colors::ErrorText, ImVec4(0, 0, 0, 1), 0.78f);

    beginTable(cpType, getLabelSize("billboard"));

    settingsPos.unit = NumericUnit::Meters;
    ImGui::BeginDisabled(posedBones || drivenChannel[0]);
    if (recorded[0]) ImGui::PushStyleColor(ImGuiCol_FrameBg, animated[0] ? animatedTint : recordTint);
    propertyRow(RowPropertyType::Vector3, cpType, "position", "Position", sceneProject, entities, settingsPos);
    if (recorded[0]) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(posedBones || drivenChannel[1]);
    if (recorded[1]) ImGui::PushStyleColor(ImGuiCol_FrameBg, animated[1] ? animatedTint : recordTint);
    propertyRow(RowPropertyType::Quat, cpType, "rotation", "Rotation", sceneProject, entities);
    if (recorded[1]) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(posedBones || drivenChannel[2]);
    if (recorded[2]) ImGui::PushStyleColor(ImGuiCol_FrameBg, animated[2] ? animatedTint : recordTint);
    propertyRow(RowPropertyType::Vector3, cpType, "scale", "Scale", sceneProject, entities);
    if (recorded[2]) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    propertyRow(RowPropertyType::Bool, cpType, "visible", "Visible", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "billboard", "Billboard", sceneProject, entities);

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GEAR)){
        ImGui::OpenPopup("menusettings_billboard");
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(19 * ImGui::GetFontSize(), 0), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopup("menusettings_billboard")){
        ImGui::Text("Billboard settings");
        ImGui::Separator();

        RowSettings settingsRotation;
        settingsRotation.secondColSize = 12 * ImGui::GetFontSize();

        beginTable(cpType, getLabelSize("cylindrical"));
        propertyRow(RowPropertyType::Bool, cpType, "fakeBillboard", "Fake", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "cylindricalBillboard", "Cylindrical", sceneProject, entities);
        propertyRow(RowPropertyType::Quat, cpType, "billboardRotation", "Rotation", sceneProject, entities, settingsRotation);
        endTable();

        ImGui::EndPopup();
    }

    endTable();

    if (drivenChannel[0] || drivenChannel[1] || drivenChannel[2]) {
        std::string channels;
        const char* const names[3] = {"Position", "Rotation", "Scale"};
        for (int channel = 0; channel < 3; channel++) {
            if (drivenChannel[channel]) {
                channels += std::string(channels.empty() ? "" : ", ") + names[channel];
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " %s driven by the Lattice Follower: move its Rest (the Scene view's tools do).", channels.c_str());
        ImGui::PopStyleColor();
        if (entities.size() == 1 && drivingRest != NULL_ENTITY && ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS " Select Rest")) {
            project->clearSelectedEntities(sceneProject->id);
            project->addSelectedEntity(sceneProject->id, drivingRest);
        }
    }
}

void editor::InspectorWindow::commitShaderFork(uint32_t sceneId, std::unique_ptr<ForkShaderCmd> forkCmd){
    if (!forkCmd->isValid()) {
        std::string error = forkCmd->getError();
        Backend::getApp().registerAlert("Error", error.empty() ? "Could not prepare the shader fork." : error);
        return;
    }

    std::filesystem::path vert = project->getProjectPath() / (forkCmd->getBase() + ".vert");
    std::filesystem::path frag = project->getProjectPath() / (forkCmd->getBase() + ".frag");
    CommandHandle::get(sceneId)->addCommandNoMerge(forkCmd.release());

    // execute() runs inside addCommandNoMerge; a failed write leaves nothing behind.
    if (!std::filesystem::exists(vert) || !std::filesystem::exists(frag)) {
        Backend::getApp().registerAlert("Error", "The shader files could not be created.");
        return;
    }

    if (CodeEditor* codeEditor = Backend::getApp().getCodeEditor()) {
        codeEditor->openFile(vert.string(), true);
        codeEditor->openFile(frag.string(), true);
    }
    if (ProjectWindow* resources = Backend::getApp().getProjectWindow())
        resources->refreshCurrentDirectory();
}

namespace {
    // A shader as a header names it: a library shader's manifest name (or its id), a fork's
    // file name ("WaterTint" for shaders/WaterTint or shaders/a.vert|shaders/WaterTint.frag)
    std::string shaderHeaderName(const std::string& shader) {
        if (ShaderReference::isLibrary(shader)) {
            const std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
            if (manifest && manifest->present && !manifest->name.empty())
                return manifest->name;
            return shader.substr(shader.find(':') + 1);
        }
        return std::filesystem::path(editor::Util::resolveCustomShaderPaths(shader).frag).stem().string();
    }
}

float editor::InspectorWindow::getShaderPropertiesLabelSize(const std::vector<ShaderUniform>& members, const ShaderManifest* manifest){
    // every row may show a revert arrow
    float width = 0.0f;
    for (const ShaderUniform& uniform : members)
        width = std::max(width, getLabelSize(uniform.name, true));
    if (manifest) {
        for (const ShaderManifestProperty& property : manifest->properties)
            width = std::max(width, getLabelSize(property.label.empty() ? property.member : property.label, true));
        for (const ShaderManifestTexture& texture : manifest->textures)
            width = std::max(width, getLabelSize(texture.label.empty() ? texture.uniform : texture.label, true));
        for (const ShaderManifestFeature& feature : manifest->features)
            width = std::max(width, getLabelSize(feature.label.empty() ? feature.define : feature.label, true));
    }
    return width;
}

uint64_t editor::InspectorWindow::shaderMembersKey(const std::vector<ShaderUniform>& members, const ShaderManifest* manifest){
    uint64_t hash = FNV1A64_SEED;
    for (const ShaderUniform& uniform : members)
        hash = fnv1a64(uniform.name + "\n", hash);
    if (manifest) {
        for (const ShaderManifestProperty& property : manifest->properties)
            hash = fnv1a64(property.member + "\n", hash);
    }
    return hash;
}

void editor::InspectorWindow::drawCustomShaderRow(ComponentType cpType, ShaderType shaderType, SceneProject* sceneProject, std::vector<Entity> entities){
    // The custom shader is per-component. The fork dialog chooses its project-relative
    // destination; engine includes still resolve through the embedded library unless the
    // user creates private copies. Shown only for a single selection.
    if (entities.size() != 1)
        return;

    Entity shaderEntity = entities[0];
    if (!Catalog::getPropertyRef<std::string>(sceneProject->scene, shaderEntity, cpType, "customShader"))
        return;

    // the Shader rows have a table of their own, so long member names never widen it
    ShaderUniformRows uniformRows = resolveShaderUniformRows(cpType, sceneProject, shaderEntity);
    const float labelSize = getLabelSize(cpType == ComponentType::MeshComponent ? "Depth Shader" : "Shader", false);

    beginTable(cpType, labelSize, "custom_shader_table");
    prefabRowProperty = "customShader";
    propertyHeader("Shader");
    drawComponentShaderRow(cpType, shaderType, sceneProject, shaderEntity, "customShader", "custom_shader");
    if (uniformRows.buildFailed) {
        // a failed fork keeps rendering, the runtime falls back to the built-in
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Shader failed to build");
    }

    // shadow maps and the depth pre-pass have their own fork, so a mesh fork that displaces
    // vertices or discards can keep its shadows in step
    if (cpType == ComponentType::MeshComponent) {
        prefabRowProperty = "customDepthShader";
        propertyHeader("Depth Shader");
        drawComponentShaderRow(cpType, ShaderType::DEPTH, sceneProject, shaderEntity, "customDepthShader", "custom_depth_shader");
        ImGui::SameLine(); helpMarker("Shader of the shadow maps, and of the depth pre-pass that feeds SSAO and post-process depth while SSR is off (with SSR on they read the G-buffer, which keeps the built-in shader). Fork it when the mesh shader moves vertices or discards fragments, so shadows follow.");
        if (Catalog::isCustomDepthShaderBuildFailed(sceneProject->scene, shaderEntity)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Depth shader failed to build");
        }
    }

    drawComponentStateRows(cpType, sceneProject, entities);

    endTable();

    // the shader's properties under a header naming it: a library shader's labelled rows
    // (parameters in shaderUniforms, textures, features; a mesh keeps those on its materials),
    // else the members of the loaded fork
    const std::string shader = Catalog::getShaderReferenceForProperty(sceneProject->scene, shaderEntity, cpType, "customShader");
    std::shared_ptr<const ShaderManifest> manifest = shader.empty() || cpType == ComponentType::MeshComponent ? nullptr : ShaderManifests::get(shader);
    if (manifest && !manifest->present)
        manifest.reset();
    const bool manifestRows = manifest && (!manifest->properties.empty() || !manifest->textures.empty() ||
                                           !manifest->features.empty() || !manifest->keywords.empty());
    // (block warnings come with the members they are about)
    if (!manifestRows && uniformRows.members.empty())
        return;
    ImGui::SeparatorText((shaderHeaderName(shader) + " Properties").c_str());
    beginTable(cpType, getShaderPropertiesLabelSize(uniformRows.members, manifest.get()), "custom_shader_properties",
               shaderMembersKey(uniformRows.members, manifest.get()));
    if (manifestRows)
        drawShaderManifestRows(cpType, sceneProject, shaderEntity, shader, "shaderUniforms", "shaderTextures", "shaderFeatures", nullptr);
    else
        drawShaderUniformRows(cpType, sceneProject, shaderEntity, uniformRows);
    endTable();
}

void editor::InspectorWindow::drawComponentShaderRow(ComponentType cpType, ShaderType shaderType, SceneProject* sceneProject, Entity entity, const char* propertyName, const std::string& idSuffix){
    const std::string currentShader = *Catalog::getPropertyRef<std::string>(sceneProject->scene, entity, cpType, propertyName);

    auto setShader = [&](const std::string& value) {
        Command* shaderCmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, propertyName, value);
        CommandHandle::get(sceneProject->id)->addCommand(shaderCmd);
    };
    const uint32_t sceneId = sceneProject->id;
    std::string defaultName = sceneProject->scene->getEntityName(entity);
    if (shaderType == ShaderType::DEPTH)
        defaultName += "_depth";
    auto onFork = [this, sceneId, entity, cpType, shaderType, propertyName, defaultName]() {
        shaderForkDialog.open(project, shaderType, defaultName,
            [this, sceneId, entity, cpType, shaderType, propertyName](const std::filesystem::path& directory,
                                                                     const std::string& name,
                                                                     bool forkIncludes) {
                // The dialog answers frames later, so the scene is resolved again here.
                SceneProject* currentScene = project->getScene(sceneId);
                if (!currentScene || !currentScene->scene) {
                    Backend::getApp().registerAlert("Error", "The scene is no longer available.");
                    return;
                }
                std::string* current = Catalog::getPropertyRef<std::string>(
                    currentScene->scene, entity, cpType, propertyName);
                if (!current || !current->empty()) {
                    Backend::getApp().registerAlert("Error", "The component no longer uses the built-in shader.");
                    return;
                }
                commitShaderFork(sceneId, std::make_unique<ForkShaderCmd>(
                    project, sceneId, entity, cpType, shaderType, directory, name, forkIncludes));
            });
    };

    // the library shaders of this type, or the built-in one (a depth program comes from the
    // library shader's @depth, so the depth row has no picker)
    if (shaderType != ShaderType::DEPTH)
        drawLibraryShaderCombo(currentShader, shaderType, setShader, "Edit Shader Files##" + idSuffix);

    drawShaderRowContents(currentShader, setShader, onFork, idSuffix);
}

editor::InspectorWindow::ShaderUniformRows editor::InspectorWindow::resolveShaderUniformRows(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    ShaderUniformRows rows;
    Scene* scene = sceneProject->scene;

    std::vector<const CustomUniformBlock*> blocks = Catalog::getShaderUniformBlocks(scene, entity, cpType);
    if (cpType == ComponentType::MeshComponent) {
        // a submesh whose material names its own shader takes its values from the material
        // (its rows are in the material section), not from the mesh's shaderUniforms
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [mesh](const CustomUniformBlock* block) {
                for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                    const Submesh& sub = mesh->submeshes[s];
                    if (!sub.material.shader.empty() && (block == &sub.customVSParams || block == &sub.customFSParams ||
                            block == &sub.customVSDepthParams || block == &sub.customFSDepthParams))
                        return true;
                }
                return false;
            }), blocks.end());
        }
    }
    for (const CustomUniformBlock* block : blocks) {
        for (const ShaderUniform& uniform : block->members) {
            bool listed = false;
            for (const ShaderUniform& existing : rows.members) {
                if (existing.name == uniform.name) {
                    listed = true;
                    break;
                }
            }
            if (!listed)
                rows.members.push_back(uniform);
        }
    }
    rows.warnings = Catalog::getShaderUniformWarnings(blocks);
    rows.buildFailed = Catalog::isCustomShaderBuildFailed(scene, entity, cpType);

    return rows;
}

void editor::InspectorWindow::drawStochasticRotationWarning(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                           const std::string& rotationProperty, float rotation, const Texture& normalTexture,
                                                           const std::function<void()>& onValueChanged){
    if (!(rotation > 0.0f) || normalTexture.empty() || normalTexture.isFramebuffer())
        return;
    const std::string path = normalTexture.getPath(0);
    if (path.empty())
        return;
    const std::filesystem::path file = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(path));
    // measured once per file (and modification), off this thread
    const NormalMapTiltResult tilt = NormalMapTilt::request(file);
    if (!tilt.ready || !tilt.valid || tilt.degrees < NormalMapTilt::WARNING_DEGREES)
        return;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
    ImGui::TextWrapped("%s: rotated tiles will look tilted. Keep Rotation at 0.", NormalMapTilt::describe(file, tilt.degrees).c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton(("Set Rotation to 0##" + rotationProperty).c_str())){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (const Entity& entity : entities){
            multiCmd->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, rotationProperty, 0.0f, onValueChanged);
        }
        CommandHandle::get(sceneProject->id)->addCommand(multiCmd);
    }
}

void editor::InspectorWindow::drawShaderUniformWarnings(const std::vector<std::string>& warnings){
    for (const std::string& warning : warnings) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        ImGui::TextWrapped("%s", warning.c_str());
        ImGui::PopStyleColor();
    }
}

namespace {
    // the components a value member of a custom block has (0: not a value a global can feed)
    int uniformComponents(const ShaderUniform& uniform){
        if (uniform.arrayCount > 1 || ShaderUniforms::isReserved(uniform.name) || !ShaderUniforms::isEditable(uniform))
            return 0;
        switch (uniform.type){
            case ShaderUniformType::FLOAT: case ShaderUniformType::INT: return 1;
            case ShaderUniformType::FLOAT2: case ShaderUniformType::INT2: return 2;
            case ShaderUniformType::FLOAT3: case ShaderUniformType::INT3: return 3;
            case ShaderUniformType::FLOAT4: case ShaderUniformType::INT4: return 4;
            default: return 0;
        }
    }
}

bool editor::InspectorWindow::drawShaderUniformRow(const ShaderUniform& uniform, const ShaderUniformValues& values, const std::string& idPrefix, Vector4& newValue,
                                                   float reserveRight, const UniformChain* chain, bool* revert, const char* revertTooltip){
    const std::string& name = uniform.name;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", name.c_str());
    // an own value the caller can drop (back to the scene's value, or the shader's default)
    const bool ownValue = ShaderUniforms::has(values, name);
    if (revert && revertTooltip && ownValue && !ShaderUniforms::isReserved(name) && ShaderUniforms::isEditable(uniform)) {
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        *revert = ImGui::Button((ICON_FA_ROTATE_LEFT "##revert_" + name).c_str());
        ImGui::SetItemTooltip("%s", revertTooltip);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
    ImGui::TableSetColumnIndex(1);

    // RenderSystem rewrites these every frame
    if (ShaderUniforms::isReserved(name)){
        ImGui::TextDisabled("Set by the engine");
        ImGui::SameLine();
        helpMarker(name == "resolution"
            ? "Reserved uniform, written every frame with the size of the render target: xy = width and height, zw = 1 / width and 1 / height.\n\nRename it to edit a value of your own here."
            : name == "sceneTime"
            ? "Reserved uniform, written every frame with the scene's game clock: seconds the scene has run, scaled by the time scale, held while the game is paused.\n\nRename it to edit a value of your own here."
            : name.rfind("water", 0) == 0
            ? "Reserved uniform, written every draw with the Water Excluders over the mesh and the scene's Water Wake map (includes/water_interaction_params.glsl)."
            : "Reserved uniform, written every frame with the seconds elapsed since startup.\n\nRename it to edit a value of your own here.");
        return false;
    }

    if (!ShaderUniforms::isEditable(uniform)){
        ImGui::TextDisabled("Not editable");
        ImGui::SameLine();
        helpMarker(uniform.arrayCount > 1
            ? "Array members take no value and stay zero. Declare one scalar or vector member per value instead."
            : "Only float and int scalars and vectors take a value; this member stays zero.");
        return false;
    }

    int components = 0;
    bool isInt = false;
    switch (uniform.type){
        case ShaderUniformType::FLOAT:  components = 1; break;
        case ShaderUniformType::FLOAT2: components = 2; break;
        case ShaderUniformType::FLOAT3: components = 3; break;
        case ShaderUniformType::FLOAT4: components = 4; break;
        case ShaderUniformType::INT:    components = 1; isInt = true; break;
        case ShaderUniformType::INT2:   components = 2; isInt = true; break;
        case ShaderUniformType::INT3:   components = 3; isInt = true; break;
        case ShaderUniformType::INT4:   components = 4; isInt = true; break;
        default: return false;
    }

    UniformSource source = UniformSource::Own;
    Vector4 value = ShaderUniforms::get(values, name);
    if (chain && !ownValue)
        value = ShaderUniforms::resolve(*chain, name, &source);
    float fields[4] = {value.x, value.y, value.z, value.w};
    bool changed = false;
    std::string uniformId = idPrefix + name;
    ImGui::SetNextItemWidth(reserveRight > 0.0f ? -reserveRight : -1.0f);
    if (isInt){
        int intFields[4] = {(int)fields[0], (int)fields[1], (int)fields[2], (int)fields[3]};
        changed = ImGui::DragScalarN(uniformId.c_str(), ImGuiDataType_S32, intFields, components, 1.0f);
        for (int c = 0; c < 4; c++)
            fields[c] = (float)intFields[c];
    }else{
        changed = ImGui::DragScalarN(uniformId.c_str(), ImGuiDataType_Float, fields, components, 0.01f);
    }
    if (source == UniformSource::SceneDefault && ImGui::IsItemHovered() && !ImGui::IsItemActive())
        ImGui::SetTooltip("From the scene's default shader values (Scene > Default Shaders)");

    if (changed)
        newValue = Vector4(fields[0], fields[1], fields[2], fields[3]);

    return changed;
}

void editor::InspectorWindow::drawShaderUniformRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const ShaderUniformRows& rows){
    if (rows.members.empty())
        return;

    ShaderUniformValues* valuesRef = Catalog::getPropertyRef<ShaderUniformValues>(sceneProject->scene, entity, cpType, "shaderUniforms");
    if (!valuesRef)
        return;
    const ShaderUniformValues values = *valuesRef;
    const uint32_t sceneId = sceneProject->id;
    // what an unset member renders with (the scene's default-shader value, ...), as the engine
    Catalog::UniformChainRefs chain = Catalog::getShaderUniformChain(sceneProject->scene, entity, cpType);
    chain.chain.values = &values;
    // members that follow a global shader value (a mesh keeps them on its materials)
    ShaderGlobalBindings* bindingsRef = cpType == ComponentType::MeshComponent ? nullptr
        : Catalog::getPropertyRef<ShaderGlobalBindings>(sceneProject->scene, entity, cpType, "shaderGlobalBindings");
    const ShaderGlobalBindings bindings = bindingsRef ? *bindingsRef : ShaderGlobalBindings();
    auto setBindings = [&](const ShaderGlobalBindings& value) {
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<ShaderGlobalBindings>(project, sceneId, entity, cpType, "shaderGlobalBindings", value));
    };
    const float linkWidth = getGlobalLinkButtonWidth() + ImGui::GetStyle().ItemSpacing.x;

    for (const ShaderUniform& uniform : rows.members) {
        const int components = bindingsRef ? uniformComponents(uniform) : 0;
        const std::string bound = components > 0 ? ShaderGlobals::getBinding(bindings, uniform.name) : std::string();
        ImGui::PushID(("uniform_" + uniform.name).c_str());
        if (!bound.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", uniform.name.c_str());
            ImGui::TableSetColumnIndex(1);
            drawBoundGlobalValue(bound, components, false, ImGui::GetContentRegionAvail().x - linkWidth);
            drawGlobalLinkButton(uniform.name, components, bindings, setBindings);
            ImGui::PopID();
            continue;
        }
        Vector4 newValue;
        bool revert = false;
        // an own value over the scene's default-shader value can go back to it
        const bool overScene = chain.chain.sceneDefault && ShaderUniforms::has(*chain.chain.sceneDefault, uniform.name);
        const bool changed = drawShaderUniformRow(uniform, values, "##shader_uniform_", newValue, components > 0 ? linkWidth : 0.0f,
                                                  &chain.chain, &revert, overScene ? "Revert to Scene Default" : nullptr);
        if (revert) {
            // back to the scene's value: the own one goes
            ShaderUniformValues newValues = values;
            ShaderUniforms::remove(newValues, uniform.name);
            CommandHandle::get(sceneId)->addCommandNoMerge(
                new PropertyCmd<ShaderUniformValues>(project, sceneId, entity, cpType, "shaderUniforms", newValues));
            ImGui::PopID();
            continue;
        }
        if (changed) {
            ShaderUniformValues newValues = values;
            ShaderUniforms::set(newValues, uniform.name, newValue);

            // dragging merges into one undo step, closed when the widget is released
            cmd = new PropertyCmd<ShaderUniformValues>(project, sceneId, entity, cpType, "shaderUniforms", newValues);
            CommandHandle::get(sceneId)->addCommand(cmd);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
            cmd->setNoMerge();
            cmd = nullptr;
        }
        if (components > 0)
            drawGlobalLinkButton(uniform.name, components, bindings, setBindings);
        ImGui::PopID();
    }

    drawShaderUniformWarnings(rows.warnings);
}

void editor::InspectorWindow::drawSceneShaderRow(SceneProject* sceneProject, ShaderType shaderType, const char* scenePropertyName, const char* label){
    // Scene default shader: used by every component of this type whose customShader is
    // empty (priority: component shader > scene default > built-in).
    const std::string currentShader = Catalog::getSceneProperty<std::string>(sceneProject->scene, scenePropertyName);

    auto setShader = [&](const std::string& value) {
        Command* shaderCmd = new ScenePropertyCmd<std::string>(project, sceneProject->id, scenePropertyName, value);
        CommandHandle::get(sceneProject->id)->addCommand(shaderCmd);
    };
    const uint32_t sceneId = sceneProject->id;
    const std::string propertyName = scenePropertyName;
    const std::string defaultName = sceneProject->name + " " + label;
    auto onFork = [this, sceneId, shaderType, propertyName, defaultName]() {
        shaderForkDialog.open(project, shaderType, defaultName,
            [this, sceneId, shaderType, propertyName](const std::filesystem::path& directory,
                                                      const std::string& name,
                                                      bool forkIncludes) {
                SceneProject* currentScene = project->getScene(sceneId);
                if (!currentScene || !currentScene->scene) {
                    Backend::getApp().registerAlert("Error", "The scene is no longer available.");
                    return;
                }
                if (!Catalog::getSceneProperty<std::string>(currentScene->scene, propertyName).empty()) {
                    Backend::getApp().registerAlert("Error", "The scene no longer uses the built-in shader for this type.");
                    return;
                }
                commitShaderFork(sceneId, std::make_unique<ForkShaderCmd>(
                    project, currentScene, shaderType, propertyName, directory, name, forkIncludes));
            });
    };

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("%s", label);
    ImGui::TableSetColumnIndex(1);

    drawLibraryShaderCombo(currentShader, shaderType, setShader, std::string("Edit Shader Files##") + scenePropertyName);
    drawShaderRowContents(currentShader, setShader, onFork, scenePropertyName);
}

void editor::InspectorWindow::drawSceneDefaultShader(SceneProject* sceneProject, ShaderType shaderType, const char* scenePropertyName, const char* label){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const std::string shader = scene->getDefaultCustomShader(shaderType);
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame;
    const Catalog::DefaultShaderUse use = shader.empty() ? Catalog::DefaultShaderUse() : Catalog::getDefaultShaderUse(scene, shaderType);

    ImGui::PushID(scenePropertyName);
    if (ImGui::BeginTable("default_shader_table", 2, tableFlags)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Points").x);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        drawSceneShaderRow(sceneProject, shaderType, scenePropertyName, label);
        if (!shader.empty()) {
            // who draws with it: a fork that failed everywhere renders the built-in
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            if (use.users > 0 && use.failed == use.users)
                ImGui::TextDisabled("Shader failed to build");
            else if (use.users == 0)
                ImGui::TextDisabled("Not used by any object");
            else
                ImGui::TextDisabled("Used by %d object%s", use.users, use.users == 1 ? "" : "s");
            drawShaderUniformWarnings(Catalog::getShaderUniformWarnings(use.blocks));
        }
        ImGui::EndTable();
    }
    if (shader.empty()) {
        ImGui::PopID();
        return;
    }

    // the members the objects drawing with it declare (each name once, engine-written first),
    // or the manifest's parameters of a library shader
    std::vector<ShaderUniform> members;
    for (bool reserved : {true, false}) {
        for (const CustomUniformBlock* block : use.blocks) {
            for (const ShaderUniform& uniform : block->members) {
                if (ShaderUniforms::isReserved(uniform.name) != reserved)
                    continue;
                if (std::none_of(members.begin(), members.end(), [&](const ShaderUniform& m) { return m.name == uniform.name; }))
                    members.push_back(uniform);
            }
        }
    }
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
    if (manifest && (!manifest->present || manifest->properties.empty()))
        manifest.reset();
    const char* property = Catalog::getDefaultShaderUniformsProperty(shaderType);
    const ShaderUniformValues values = scene->getDefaultShaderUniforms(shaderType);

    ImGui::SeparatorText((shaderHeaderName(shader) + " Properties").c_str());
    if (!manifest && members.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", use.users == 0 ? "Properties show once an object uses this shader"
                                                : "This shader declares no properties (u_vs_customParams / u_fs_customParams)");
        ImGui::PopStyleColor();
        ImGui::PopID();
        return;
    }

    // a discrete change is its own undo step; a drag merges until released
    auto setValues = [&](const ShaderUniformValues& newValues, bool continuous) {
        Command* valuesCmd = new ScenePropertyCmd<ShaderUniformValues>(project, sceneId, property, newValues);
        if (continuous) {
            cmd = valuesCmd;
            CommandHandle::get(sceneId)->addCommand(valuesCmd);
        } else {
            CommandHandle::get(sceneId)->addCommandNoMerge(valuesCmd);
        }
    };
    auto finishDrag = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
            cmd->setNoMerge();
            cmd = nullptr;
        }
    };

    char tableId[48];
    std::snprintf(tableId, sizeof(tableId), "default_shader_properties_%016llx",
                  static_cast<unsigned long long>(shaderMembersKey(members, manifest.get())));
    if (ImGui::BeginTable(tableId, 2, tableFlags)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getShaderPropertiesLabelSize(members, manifest.get()));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        prefabRowProperty.clear();
        if (manifest) {
            for (const ShaderManifestProperty& declared : manifest->properties) {
                ImGui::PushID(("param_" + declared.member).c_str());
                const bool hasValue = ShaderUniforms::has(values, declared.member);
                const Vector4 value = hasValue ? ShaderUniforms::get(values, declared.member) : declared.defaultValue;
                const bool reset = propertyHeader(declared.label.empty() ? declared.member : declared.label, -1, hasValue);
                if (hasValue && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Reset to the shader's default");
                if (reset) {
                    ShaderUniformValues changed = values;
                    ShaderUniforms::remove(changed, declared.member);
                    setValues(changed, false);
                    ImGui::PopID();
                    continue;
                }
                Vector4 edited = value;
                bool continuous = true;
                if (drawShaderPropertyValue(declared, "##scene_param_" + declared.member, value, edited, continuous)) {
                    ShaderUniformValues changed = values;
                    ShaderUniforms::set(changed, declared.member, edited);
                    setValues(changed, continuous);
                }
                finishDrag();
                if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                    ImGui::SetTooltip("%s\nInherited by every object without a shader of its own", declared.member.c_str());
                ImGui::PopID();
            }
        } else {
            UniformChain chain;
            chain.values = &values;
            for (const ShaderUniform& uniform : members) {
                ImGui::PushID(("uniform_" + uniform.name).c_str());
                Vector4 newValue;
                bool reset = false;
                const bool changed = drawShaderUniformRow(uniform, values, "##scene_uniform_", newValue, 0.0f, &chain, &reset,
                                                          "Reset to the shader's default (zero)");
                if (reset) {
                    ShaderUniformValues next = values;
                    ShaderUniforms::remove(next, uniform.name);
                    setValues(next, false);
                } else if (changed) {
                    ShaderUniformValues next = values;
                    ShaderUniforms::set(next, uniform.name, newValue);
                    setValues(next, true);
                }
                finishDrag();
                ImGui::PopID();
            }
        }

        // values kept from another shader (or set before any object used this one), unused
        ShaderUniformValues used;
        size_t unused = 0;
        for (const auto& value : values) {
            const bool declared = manifest ? manifest->findProperty(value.first) != nullptr
                                           : std::any_of(members.begin(), members.end(), [&](const ShaderUniform& m) { return m.name == value.first; });
            if (declared) used.push_back(value);
            else unused++;
        }
        if (unused > 0 && (manifest || use.users > 0)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Unused values (%zu)", unused);
            ImGui::TableSetColumnIndex(1);
            if (ImGui::SmallButton("Remove##unused_values"))
                setValues(used, false);
            ImGui::SetItemTooltip("Values this shader does not declare (kept from another shader)");
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

editor::InspectorWindow::PostProcessShader editor::InspectorWindow::resolvePostProcessShader(const PostProcessPass& pass){
    PostProcessShader resolved;

    uint16_t customId = ShaderPool::registerCustomShader(pass.shader);
    resolved.buildFailed = customId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::POSTPROCESS, 0, customId);
    resolved.shader = ShaderPool::get(ShaderType::POSTPROCESS, 0, customId);

    if (!resolved.buildFailed && resolved.shader && resolved.shader->isCreated()){
        resolved.block.resolve(resolved.shader->shaderData, "u_fs_postParams");
    }

    if (!pass.shader.empty()){
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(pass.shader);
        if (manifest && manifest->present)
            resolved.manifest = manifest;
    }

    return resolved;
}

// widest uniform name the pass declares, so the rows are not truncated to "Shader"
float editor::InspectorWindow::getPostProcessLabelSize(const PostProcessShader& resolved, const PostProcessPass& pass){
    // no reset arrow on these rows, so they only need getLabelSize's half-icon padding
    float width = getLabelSize("Shader", false);
    for (const ShaderUniform& uniform : resolved.block.members){
        const ShaderManifestProperty* property = resolved.manifest ? resolved.manifest->findProperty(uniform.name) : nullptr;
        // a manifest row shows its label and the reset arrow
        if (property && !property->label.empty())
            width = std::max(width, getLabelSize(property->label));
        else
            width = std::max(width, getLabelSize(uniform.name, property != nullptr));
    }
    // the texture slots (drawPostProcessPassFields): the manifest's labels and any set by name
    if (resolved.manifest){
        for (const ShaderManifestTexture& texture : resolved.manifest->textures)
            width = std::max(width, getLabelSize(texture.label.empty() ? texture.uniform : texture.label, false));
    }
    for (const auto& entry : pass.textures)
        width = std::max(width, getLabelSize(entry.first, false));

    return width;
}

void editor::InspectorWindow::drawPostProcessUniforms(uint32_t sceneId, const std::vector<PostProcessPass>& passes, size_t index,
                                                const PostProcessShader& resolved){
    if (resolved.buildFailed || !resolved.shader || !resolved.shader->isCreated()){
        if (!passes[index].shader.empty()){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            // a failed fork keeps rendering, the runtime falls back to the built-in
            ImGui::TextDisabled("%s", resolved.buildFailed ? "Shader failed to build" : "Building shader...");
        }
        return;
    }

    const ShaderUniformValues& values = passes[index].uniforms;
    // members that follow a global shader value
    const ShaderGlobalBindings& bindings = passes[index].shaderGlobalBindings;
    auto setBindings = [&](const ShaderGlobalBindings& value){
        std::vector<PostProcessPass> newPasses = passes;
        newPasses[index].shaderGlobalBindings = value;
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", newPasses));
    };
    const float linkWidth = getGlobalLinkButtonWidth() + ImGui::GetStyle().ItemSpacing.x;
    for (const ShaderUniform& uniform : resolved.block.members){
        ImGui::PushID(uniform.name.c_str());
        const int components = uniformComponents(uniform);
        const std::string bound = components > 0 ? ShaderGlobals::getBinding(bindings, uniform.name) : std::string();
        if (!bound.empty()){
            const ShaderManifestProperty* property = resolved.manifest ? resolved.manifest->findProperty(uniform.name) : nullptr;
            const bool color = property && (property->kind == ShaderPropertyKind::COLOR || property->kind == ShaderPropertyKind::HDRCOLOR);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(property && !property->label.empty() ? property->label.c_str() : uniform.name.c_str());
            ImGui::TableSetColumnIndex(1);
            drawBoundGlobalValue(bound, components, color, ImGui::GetContentRegionAvail().x - linkWidth);
            drawGlobalLinkButton(uniform.name, components, bindings, setBindings);
            ImGui::PopID();
            continue;
        }
        // a member the shader's manifest declares (@property): its label, range and widget, and
        // its default while unset (what the engine renders), with a reset arrow once set
        const ShaderManifestProperty* property = resolved.manifest ? resolved.manifest->findProperty(uniform.name) : nullptr;
        if (property && !ShaderUniforms::isReserved(uniform.name) && ShaderUniforms::isEditable(uniform)){
            const bool hasValue = ShaderUniforms::has(values, uniform.name);
            const Vector4 value = hasValue ? ShaderUniforms::get(values, uniform.name) : property->defaultValue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(property->label.empty() ? uniform.name.c_str() : property->label.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", uniform.name.c_str());
            bool reset = false;
            if (hasValue){
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                reset = ImGui::Button(ICON_FA_ROTATE_LEFT "##reset");
                ImGui::SetItemTooltip("Reset to the shader's default");
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
            }
            if (reset){
                std::vector<PostProcessPass> newPasses = passes;
                ShaderUniforms::remove(newPasses[index].uniforms, uniform.name);
                CommandHandle::get(sceneId)->addCommandNoMerge(
                    new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", newPasses));
                ImGui::PopID();
                continue;
            }

            ImGui::TableSetColumnIndex(1);
            Vector4 edited;
            bool continuous = true;
            if (drawShaderPropertyValue(*property, "##pp_param_" + uniform.name, value, edited, continuous, components > 0 ? -linkWidth : -1.0f)){
                std::vector<PostProcessPass> newPasses = passes;
                newPasses[index].setUniform(uniform.name, edited);
                Command* passCmd = new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", newPasses);
                if (continuous){
                    // dragging merges into one undo step, closed when the widget is released
                    cmd = passCmd;
                    CommandHandle::get(sceneId)->addCommand(passCmd);
                }else{
                    CommandHandle::get(sceneId)->addCommandNoMerge(passCmd);
                }
            }
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                ImGui::SetTooltip("%s", uniform.name.c_str());
        }else{
            Vector4 newValue;
            if (drawShaderUniformRow(uniform, values, "##pp_uniform_", newValue, components > 0 ? linkWidth : 0.0f)){
                std::vector<PostProcessPass> newPasses = passes;
                newPasses[index].setUniform(uniform.name, newValue);

                // dragging merges into one undo step, closed when the widget is released
                Command* passCmd = new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", newPasses);
                cmd = passCmd;
                CommandHandle::get(sceneId)->addCommand(passCmd);
            }
        }
        // a released drag ends its undo step: the next drag (of any row of the chain, all one
        // scene property) is a step of its own
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd){
            cmd->setNoMerge();
            cmd = nullptr;
        }
        if (components > 0)
            drawGlobalLinkButton(uniform.name, components, bindings, setBindings);
        ImGui::PopID();
    }

    drawShaderUniformWarnings(Catalog::getShaderUniformWarnings({&resolved.block}));
}

void editor::InspectorWindow::drawScenePostProcess(SceneProject* sceneProject){
    const std::vector<PostProcessPass> passes = Catalog::getSceneProperty<std::vector<PostProcessPass>>(sceneProject->scene, "post_process");
    uint32_t sceneId = sceneProject->id;

    // the chain is one scene property, so every edit here commits a modified copy
    auto setPasses = [this, sceneId](const std::vector<PostProcessPass>& value){
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", value));
    };

    // resizable: the table's id carries a hash of the pass's members, so a width saved while the
    // shader was still compiling (or before a member was added) never sticks to the new rows
    ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame;

    for (size_t i = 0; i < passes.size(); i++){
        ImGui::PushID((int)i);

        bool enabled = passes[i].enabled;
        if (ImGui::Checkbox("##pp_enabled", &enabled)){
            std::vector<PostProcessPass> newPasses = passes;
            newPasses[i].enabled = enabled;
            setPasses(newPasses);
        }
        ImGui::SameLine();
        ImGui::Text("Pass %zu", i + 1);

        // same compact icon buttons as the shader row, right aligned on the header
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x / 3.0, ImGui::GetStyle().FramePadding.y / 2.0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        float iconsWidth = ImGui::CalcTextSize(ICON_FA_ARROW_UP).x + ImGui::CalcTextSize(ICON_FA_ARROW_DOWN).x +
                ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + ImGui::GetStyle().FramePadding.x * 6.0f +
                ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - iconsWidth));

        ImGui::BeginDisabled(i == 0);
        if (ImGui::Button(ICON_FA_ARROW_UP"##pp_up")){
            std::vector<PostProcessPass> newPasses = passes;
            std::swap(newPasses[i], newPasses[i - 1]);
            setPasses(newPasses);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Move the pass earlier in the chain");

        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == passes.size());
        if (ImGui::Button(ICON_FA_ARROW_DOWN"##pp_down")){
            std::vector<PostProcessPass> newPasses = passes;
            std::swap(newPasses[i], newPasses[i + 1]);
            setPasses(newPasses);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Move the pass later in the chain");

        ImGui::SameLine();
        bool removed = ImGui::Button(ICON_FA_TRASH_CAN"##pp_remove");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove the pass");
        if (removed){
            std::vector<PostProcessPass> newPasses = passes;
            newPasses.erase(newPasses.begin() + i);
            setPasses(newPasses);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        if (removed){
            ImGui::PopID();
            break;
        }

        PostProcessShader resolved = resolvePostProcessShader(passes[i]);

        char passTableId[48];
        std::snprintf(passTableId, sizeof(passTableId), "scene_post_process_pass_table_%016llx",
                      static_cast<unsigned long long>(shaderMembersKey(resolved.block.members, resolved.manifest.get())));
        if (ImGui::BeginTable(passTableId, 2, tableFlags)){
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getPostProcessLabelSize(resolved, passes[i]));
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Shader");
            ImGui::TableSetColumnIndex(1);

            auto setShader = [&](const std::string& value){
                std::vector<PostProcessPass> newPasses = passes;
                newPasses[i].shader = value;
                newPasses[i].uniforms.clear(); // another shader has other members
                // and other textures: keep only the ones the new shader's manifest declares
                // (an old one with a colliding name would be bound silently otherwise)
                std::shared_ptr<const ShaderManifest> manifest = value.empty() ? nullptr : ShaderManifests::get(value);
                auto& textures = newPasses[i].textures;
                textures.erase(std::remove_if(textures.begin(), textures.end(), [&](const std::pair<std::string, Texture>& entry){
                    return !manifest || !manifest->present || !manifest->findTexture(entry.first);
                }), textures.end());
                setPasses(newPasses);
            };
            std::string defaultName = sceneProject->name + " Post " + std::to_string(i + 1);
            auto onFork = [this, sceneId, i, defaultName](){
                shaderForkDialog.open(project, ShaderType::POSTPROCESS, defaultName,
                    [this, sceneId, i](const std::filesystem::path& directory, const std::string& name, bool forkIncludes){
                        SceneProject* currentScene = project->getScene(sceneId);
                        if (!currentScene || !currentScene->scene){
                            Backend::getApp().registerAlert("Error", "The scene is no longer available.");
                            return;
                        }
                        std::vector<PostProcessPass> newPasses = Catalog::getSceneProperty<std::vector<PostProcessPass>>(currentScene->scene, "post_process");
                        if (i >= newPasses.size() || !newPasses[i].shader.empty()){
                            Backend::getApp().registerAlert("Error", "The pass no longer uses the built-in shader.");
                            return;
                        }
                        // the fork writes the files and points the pass at them in one step
                        auto makePropertyCmd = [this, sceneId, i, newPasses](const std::string& forkBase) mutable {
                            newPasses[i].shader = forkBase;
                            return std::make_unique<ScenePropertyCmd<std::vector<PostProcessPass>>>(
                                project, sceneId, "post_process", newPasses);
                        };
                        commitShaderFork(sceneId, std::make_unique<ForkShaderCmd>(
                            project, ShaderType::POSTPROCESS, makePropertyCmd, directory, name, forkIncludes));
                    });
            };

            // the library's post-process shaders (@shader postprocess), a project file, or a fork
            const std::string passId = "post_process_" + std::to_string(i);
            // a fork of the pass's library shader (library:underwater keeps its switch, @role)
            auto onForkLibrary = [this, sceneId, i](const std::string& library){
                std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(library);
                const std::string defaultName = (manifest && !manifest->name.empty()) ? manifest->name : ShaderReference::getLibraryName(library);
                shaderForkDialog.open(project, ShaderType::POSTPROCESS, defaultName,
                    [this, sceneId, i, library](const std::filesystem::path& directory, const std::string& name, bool forkIncludes){
                        SceneProject* currentScene = project->getScene(sceneId);
                        if (!currentScene || !currentScene->scene)
                            return;
                        std::vector<PostProcessPass> newPasses = Catalog::getSceneProperty<std::vector<PostProcessPass>>(currentScene->scene, "post_process");
                        if (i >= newPasses.size() || ShaderReference::getBase(newPasses[i].shader) != library)
                            return;
                        // the fork keeps the pass's values (the same members)
                        auto makePropertyCmd = [this, sceneId, i, newPasses](const std::string& forkBase) mutable {
                            newPasses[i].shader = forkBase;
                            return std::make_unique<ScenePropertyCmd<std::vector<PostProcessPass>>>(project, sceneId, "post_process", newPasses);
                        };
                        commitShaderFork(sceneId, std::make_unique<ForkShaderCmd>(project, ShaderType::POSTPROCESS, makePropertyCmd, directory,
                                                                                 name, forkIncludes, library));
                    }, nullptr, library);
            };
            drawLibraryShaderCombo(passes[i].shader, ShaderType::POSTPROCESS, setShader, "Edit Shader Files##" + passId, nullptr, onForkLibrary);
            drawShaderRowContents(passes[i].shader, setShader, onFork, passId);

            drawPostProcessPassFields(sceneProject, passes, i);

            drawPostProcessUniforms(sceneId, passes, i, resolved);

            ImGui::EndTable();
        }

        ImGui::PopID();
    }

    std::string addLabel = ICON_FA_PLUS " Add Pass##scene_post_process";
    if (ImGui::Button(addLabel.c_str(), ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x), 0))){
        std::vector<PostProcessPass> newPasses = passes;
        newPasses.push_back(PostProcessPass());
        setPasses(newPasses);
    }
}

void editor::InspectorWindow::drawShaderRowContents(const std::string& currentShader,
                                               const std::function<void(const std::string&)>& setShader,
                                               const std::function<void()>& onFork,
                                               const std::string& idSuffix){
    const std::filesystem::path projectPath = project->getProjectPath();

    // The .vert/.frag entry points either share a base name (stored as "shaders/mesh")
    // or live in separate files (stored as "shaders/mesh.vert|shaders/other.frag").
    // a library shader ("library:<name>") lives in the engine: no project file to open
    const bool library = ShaderReference::isLibrary(currentShader);
    // (project-relative files; an assets-relative fork path resolves under the assets directory)
    const Util::CustomShaderPaths paths = library ? Util::CustomShaderPaths() : project->resolveCustomShaderFiles(currentShader);
    const bool hasCustom = !currentShader.empty();
    const bool separate = !library && Util::isSeparateCustomShader(currentShader);
    const bool vertExists = hasCustom && !library && std::filesystem::exists(projectPath / paths.vert);
    const bool fragExists = hasCustom && !library && std::filesystem::exists(projectPath / paths.frag);
    const std::string popupName = "Edit Shader Files##" + idSuffix;

    auto openRelative = [&](const std::string& rel) {
        if (CodeEditor* codeEditor = Backend::getApp().getCodeEditor())
            codeEditor->openFile((projectPath / rel).string(), true);
    };
    // Both .vert and .frag (sharing a base name) are required; store the base path.
    auto useExistingBase = [&](const std::string& base) {
        bool hasVert = std::filesystem::exists(projectPath / (base + ".vert"));
        bool hasFrag = std::filesystem::exists(projectPath / (base + ".frag"));
        if (!hasVert || !hasFrag) {
            Backend::getApp().registerAlert("Error", "A custom shader needs both a .vert and a .frag file with the same base name.");
            return;
        }
        setShader(base);
    };

    if (!hasCustom)
        ImGui::TextDisabled("Built-in");
    else if (library){
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(currentShader);
        if (manifest && !manifest->name.empty())
            ImGui::TextWrapped("%s (%s)", manifest->name.c_str(), currentShader.c_str());
        else
            ImGui::TextWrapped("%s", currentShader.c_str());
        if (manifest && !manifest->description.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", manifest->description.c_str());
    }else if (separate)
        ImGui::TextWrapped("%s, %s", paths.vert.c_str(), paths.frag.c_str());
    else
        ImGui::TextWrapped("%s", currentShader.c_str());

    // Icon row mirrors the ScriptComponent open-file buttons: fork, edit files,
    // open .vert, open .frag, reset to built-in.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x / 3.0, ImGui::GetStyle().FramePadding.y / 2.0));

    ImGui::BeginDisabled(hasCustom);
    if (ImGui::Button((std::string(ICON_FA_CODE_FORK "##fork_") + idSuffix).c_str()) && onFork)
        onFork();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fork the built-in shader into this project");
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_FA_PEN_TO_SQUARE "##edit_") + idSuffix).c_str()))
        ImGui::OpenPopup(popupName.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pick the .vert and .frag files (they can have different names)");

    ImGui::SameLine();
    ImGui::BeginDisabled(!vertExists);
    if (ImGui::Button((std::string(ICON_FA_FILE_CODE "##open_vert_") + idSuffix).c_str()))
        openRelative(paths.vert);
    if (ImGui::IsItemHovered()) {
        if (vertExists)
            ImGui::SetTooltip("Open %s", paths.vert.c_str());
        else if (library)
            ImGui::SetTooltip("A library shader is part of the engine");
        else if (hasCustom)
            ImGui::SetTooltip("Vertex shader file not found");
        else
            ImGui::SetTooltip("Open vertex shader");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!fragExists);
    if (ImGui::Button((std::string(ICON_FA_FILE_LINES "##open_frag_") + idSuffix).c_str()))
        openRelative(paths.frag);
    if (ImGui::IsItemHovered()) {
        if (fragExists)
            ImGui::SetTooltip("Open %s", paths.frag.c_str());
        else if (library)
            ImGui::SetTooltip("A library shader is part of the engine");
        else if (hasCustom)
            ImGui::SetTooltip("Fragment shader file not found");
        else
            ImGui::SetTooltip("Open fragment shader");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasCustom);
    if (ImGui::Button((std::string(ICON_FA_XMARK "##reset_") + idSuffix).c_str()))
        setShader("");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reset to the built-in shader");
    ImGui::EndDisabled();

    ImGui::PopStyleVar();

    drawShaderFilesPopup(popupName, setShader, paths.vert, paths.frag);

    // Drag-drop an existing .vert/.frag from the resources window.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
            if (!receivedStrings.empty() && Util::isShaderFile(receivedStrings[0])) {
                std::error_code ec;
                std::filesystem::path relPath = std::filesystem::relative(receivedStrings[0], projectPath, ec);
                // store the base path without the .vert/.frag extension
                if (!ec && !relPath.empty() && *relPath.begin() != "..") {
                    useExistingBase((relPath.parent_path() / relPath.stem()).generic_string());
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// Popup that lets the user pick the .vert and .frag entry points independently (so they
// can use differently-named files). Applying stores the result via Util::makeCustomShader,
// collapsing to the shared-base shorthand when both files share a <base>.vert/.frag.
// popupName must be unique within the window (several shader rows can share it).
void editor::InspectorWindow::drawShaderFilesPopup(const std::string& popupName, const std::function<void(const std::string&)>& setShader, const std::string& currentVert, const std::string& currentFrag){
    const std::filesystem::path projectPath = project->getProjectPath();

    ImGui::SetNextWindowSizeConstraints(ImVec2(25 * ImGui::GetFontSize(), 0), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopup(popupName.c_str()))
        return;

    static std::string vertEdit;
    static std::string fragEdit;
    if (ImGui::IsWindowAppearing()) {
        vertEdit = currentVert;
        fragEdit = currentFrag;
    }

    ImGui::Text("Edit Shader Files");
    ImGui::Separator();

    float secondColSize = 18 * ImGui::GetFontSize();
    float openBtnWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float clearBtnWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float btnSpacing = ImGui::GetStyle().ItemSpacing.x;

    bool changed = false;

    // Picks a shader file of the required extension into the given slot.
    auto pickSlot = [&](std::string& slotRel, const char* requiredExt) {
        std::filesystem::path startDir = projectPath;
        const std::string nearby = !slotRel.empty() ? slotRel
                                  : (!currentVert.empty() ? currentVert : currentFrag);
        if (!nearby.empty()) {
            const std::filesystem::path parent = (projectPath / nearby).parent_path();
            if (std::filesystem::is_directory(parent))
                startDir = parent;
        }
        std::string selected = FileDialogs::openFileDialog(startDir.string(), FILE_DIALOG_SHADER);
        if (selected.empty())
            return;

        std::string ext = std::filesystem::path(selected).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != requiredExt) {
            Backend::getApp().registerAlert("Error", std::string("Select a ") + requiredExt + " file.");
            return;
        }
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(selected, projectPath, ec);
        if (ec || rel.empty() || *rel.begin() == "..") {
            Backend::getApp().registerAlert("Error", "Shader file must be inside the project directory.");
            return;
        }
        slotRel = rel.generic_string();
        changed = true;
    };

    auto slotRow = [&](const char* label, std::string& slotRel, const char* requiredExt, const char* idSuffix) {
        propertyHeader(label, secondColSize);
        ImGui::SetNextItemWidth(secondColSize - openBtnWidth - clearBtnWidth - btnSpacing * 2.0f);

        char buffer[256];
        strncpy(buffer, slotRel.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::InputText((std::string("##shader_slot_") + idSuffix).c_str(), buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        if (!slotRel.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", slotRel.c_str());

        ImGui::SameLine();
        if (ImGui::Button((std::string(ICON_FA_FOLDER_OPEN "##pick_") + idSuffix).c_str()))
            pickSlot(slotRel, requiredExt);
        ImGui::SameLine();
        ImGui::BeginDisabled(slotRel.empty());
        if (ImGui::Button((std::string(ICON_FA_XMARK "##clear_") + idSuffix).c_str())) {
            slotRel.clear();
            changed = true;
        }
        ImGui::EndDisabled();
    };

    // the popup is its own window, so a fixed table name cannot collide
    ImGui::PushItemWidth(-1);
    ImGui::BeginTable("table_edit_shader_files", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Fragment File"));
    ImGui::TableSetupColumn("Value");
    slotRow("Vertex File", vertEdit, ".vert", "vert");
    slotRow("Fragment File", fragEdit, ".frag", "frag");
    endTable();

    bool bothEmpty = vertEdit.empty() && fragEdit.empty();
    bool bothSet = !vertEdit.empty() && !fragEdit.empty();
    if (!bothEmpty && !bothSet)
        ImGui::TextDisabled("Set both a vertex and a fragment file.");

    // A shader needs both entry points, so a half-filled pair waits for the other slot.
    if (changed && (bothEmpty || bothSet)) {
        std::string newValue = bothEmpty ? std::string() : Util::makeCustomShader(vertEdit, fragEdit);
        std::string currentValue = (currentVert.empty() || currentFrag.empty()) ? std::string() : Util::makeCustomShader(currentVert, currentFrag);
        if (newValue != currentValue) {
            setShader(newValue);
        }
    }

    ImGui::EndPopup();
}

void editor::InspectorWindow::drawSubmeshMaterialRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                      int s, const std::function<void()>& onValueChanged){
    RowSettings settingsFactor;
    settingsFactor.stepSize = 0.01f;
    settingsFactor.secondColSize = 4 * ImGui::GetFontSize();
    settingsFactor.child = true;

    RowSettings settingsMaterial;
    settingsMaterial.child = true;
    settingsMaterial.onValueChanged = onValueChanged;

    settingsFactor.onValueChanged = settingsMaterial.onValueChanged;

    RowSettings settingsAlphaMode = settingsMaterial;
    settingsAlphaMode.enumEntries = &entriesMaterialAlphaMode;

    // with the Sprite Normal Map wand when the Base Texture is an image (both Normal Texture rows)
    RowSettings settingsNormalTexture = normalTextureRowSettings(settingsMaterial, sceneProject, entities, (unsigned int)s);
    settingsNormalTexture.help = "Tangent-space normal map, OpenGL convention (green up). On sprites, tilemaps and polygons it answers "
                                 "Light 2Ds whose Height is above 0; same size and frame layout as the Base Texture.";

    // the shader (built-in Lit, a library shader or a custom file) and its own rows
    beginTable(cpType, getLabelSize("Met. Roug. Texture"), "submeshes_material_shader");
    // one-click setups of the shader, extra pass and state (shader library phase 2)
    drawMaterialPresetRow(cpType, sceneProject, entities, (unsigned int)s, settingsMaterial);
    const bool pbrRows = drawMaterialShaderRows(cpType, sceneProject, entities, (unsigned int)s, settingsMaterial);
    endTable();

    if (pbrRows){
    beginTable(cpType, getLabelSize("Met. Roug. Texture"), "submeshes_material_options");
    propertyRow(RowPropertyType::Color4L, cpType, "submeshes["+std::to_string(s)+"].material.baseColorFactor", "Base Color", sceneProject, entities, settingsMaterial);
    propertyRow(RowPropertyType::Texture, cpType, "submeshes["+std::to_string(s)+"].material.baseColorTexture", "Base Texture", sceneProject, entities,settingsMaterial);
    propertyRow(RowPropertyType::Enum, cpType, "submeshes["+std::to_string(s)+"].material.alphaMode", "Alpha Mode", sceneProject, entities, settingsAlphaMode);
    propertyRow(RowPropertyType::Float_0_1, cpType, "submeshes["+std::to_string(s)+"].material.alphaCutoff", "Alpha Cutoff", sceneProject, entities, settingsFactor);
    propertyRow(RowPropertyType::Float_0_1, cpType, "submeshes["+std::to_string(s)+"].material.foliageTransmission", "Leaf Transmission", sceneProject, entities, settingsFactor);
    propertyRow(RowPropertyType::Float_0_1, cpType, "submeshes["+std::to_string(s)+"].material.metallicFactor", "Metallic Factor", sceneProject, entities, settingsFactor);
    propertyRow(RowPropertyType::Float_0_1, cpType, "submeshes["+std::to_string(s)+"].material.roughnessFactor", "Roughness Factor", sceneProject, entities, settingsFactor);
    propertyRow(RowPropertyType::Texture, cpType, "submeshes["+std::to_string(s)+"].material.metallicRoughnessTexture", "Met. Roug. Texture", sceneProject, entities, settingsMaterial);
    propertyRow(RowPropertyType::Color3L, cpType, "submeshes["+std::to_string(s)+"].material.emissiveFactor", "Emissive Factor", sceneProject, entities, settingsMaterial);
    RowSettings settingsEmission = settingsFactor;
    settingsEmission.help = "Multiplies the Emissive Factor (KHR_materials_emissive_strength): above 1 for emission brighter than the base colour range.";
    propertyRow(RowPropertyType::FloatPositive, cpType, "submeshes["+std::to_string(s)+"].material.emissiveStrength", "Emission Intensity", sceneProject, entities, settingsEmission);
    propertyRow(RowPropertyType::Texture, cpType, "submeshes["+std::to_string(s)+"].material.emissiveTexture", "Emissive Texture", sceneProject, entities, settingsMaterial);
    propertyRow(RowPropertyType::Texture, cpType, "submeshes["+std::to_string(s)+"].material.occlusionTexture", "Occlusion Texture", sceneProject, entities, settingsMaterial);
    propertyRow(RowPropertyType::Texture, cpType, "submeshes["+std::to_string(s)+"].material.normalTexture", "Normal Texture", sceneProject, entities, settingsNormalTexture);

    // Stochastic Tiling of these maps (includes/pbr.glsl), its values while it is on
    const std::string materialPrefix = "submeshes[" + std::to_string(s) + "].material.";
    RowSettings settingsStochastic = settingsMaterial;
    settingsStochastic.help = "Hides the repetition of the material's tiled maps: hexagonal cells each read them at a random offset "
                              "and blend at their borders, every map alike (shadows and reflections too). Up to three reads per "
                              "map where cells meet. Suits organic textures (grass, rock, sand); bricks and tiles lose their grid. "
                              "The textures must repeat. Not on sprites, tilemaps or terrains (their layers have their own).";
    propertyRow(RowPropertyType::Bool, cpType, materialPrefix + "stochasticTiling", "Stochastic Tiling", sceneProject, entities, settingsStochastic);
    const Material& firstMaterial = sceneProject->scene->getComponent<MeshComponent>(entities[0]).submeshes[s].material;
    if (firstMaterial.stochasticTiling){
        RowSettings settingsParam = settingsFactor;
        settingsParam.indent = true;
        settingsParam.help = "Width of one cell in texture repeats (0.25-4). Smaller cells hide the repetition better but cut up large features of the texture.";
        // the ranges MCP takes: 0.25-4, the others 0-1
        settingsParam.floatMin = 0.25f;
        settingsParam.floatMax = 4.0f;
        settingsParam.logarithmic = true;
        propertyRow(RowPropertyType::FloatPositive, cpType, materialPrefix + "stochasticCellSize", "Cell Size", sceneProject, entities, settingsParam);
        settingsParam.floatMin = 0.0f;
        settingsParam.floatMax = 1.0f;
        settingsParam.logarithmic = false;
        settingsParam.help = "How narrow the blend between neighbouring cells is: low blends wide and soft, high keeps more of each cell whole.";
        propertyRow(RowPropertyType::Float_0_1, cpType, materialPrefix + "stochasticBlendSharpness", "Blend Sharpness", sceneProject, entities, settingsParam);
        settingsParam.help = "Keeps the texture's contrast where cells blend, which a plain blend washes out. Too high shows the cell borders. 0 blends linearly.";
        propertyRow(RowPropertyType::Float_0_1, cpType, materialPrefix + "stochasticContrast", "Contrast", sceneProject, entities, settingsParam);
        settingsParam.help = "Turns each cell by a random angle, up to +-180 degrees at 1. Leave at 0 for textures with a direction (planks, stripes, light baked in) "
                             "and for normal maps whose average normal leans.";
        propertyRow(RowPropertyType::Float_0_1, cpType, materialPrefix + "stochasticRotation", "Rotation", sceneProject, entities, settingsParam);
        drawStochasticRotationWarning(cpType, sceneProject, entities, materialPrefix + "stochasticRotation", firstMaterial.stochasticRotation,
                                      firstMaterial.normalTexture, settingsMaterial.onValueChanged);
    }
    endTable();
    drawMaterialAdvancedSurfaceRows(cpType, sceneProject, entities, (unsigned int)s, settingsMaterial);
    }else{
        // a library shader with @require normalmap reads the material's own Normal Texture
        // (the 2D lights' normal map): the one PBR row it keeps
        Scene* materialScene = sceneProject->scene;
        MeshComponent& firstMesh = materialScene->getComponent<MeshComponent>(entities[0]);
        const std::string drawnShader = RenderSystem::getSubmeshShader(firstMesh, firstMesh.submeshes[s],
                                                                       materialScene->getDefaultCustomShader(ShaderType::MESH));
        std::shared_ptr<const ShaderManifest> drawnManifest = drawnShader.empty() ? nullptr : ShaderManifests::get(drawnShader);
        const std::vector<std::string> features = ShaderReference::getEnabledFeatures(drawnShader, firstMesh.submeshes[s].material.shaderFeatures);
        if (Catalog::shaderReadsMaterialNormalMap(drawnManifest.get(), features)){
            beginTable(cpType, getLabelSize("Met. Roug. Texture"), "submeshes_material_normal_map");
            propertyRow(RowPropertyType::Texture, cpType, "submeshes["+std::to_string(s)+"].material.normalTexture", "Normal Texture", sceneProject, entities, settingsNormalTexture);
            endTable();
        }
    }

    separatorText("Surface");
    drawMaterialSurfaceRows(cpType, sceneProject, entities, (unsigned int)s, settingsMaterial, pbrRows);

    separatorText("Extra pass");
    drawMaterialExtraPassRows(cpType, sceneProject, entities, (unsigned int)s, settingsMaterial);
}

void editor::InspectorWindow::drawMeshComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    // a model made of parts draws its parts' meshes: its own (empty) one changes nothing
    std::vector<Entity> parts;
    if (entities.size() == 1) {
        const MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entities[0]);
        if (mesh && mesh->numSubmeshes == 0) parts = sceneProject->scene->getSystem<MeshSystem>()->getModelParts(entities[0]);
    }
    if (parts.empty()) {
        drawMeshComponentFields(cpType, sceneProject, entities);
        return;
    }

    beginTable(cpType, getLabelSize("Cull Below Screen Height"), "mesh_parts");
    propertyHeader("Geometry");
    ImGui::TextWrapped("Geometry is on %zu part%s", parts.size(), parts.size() == 1 ? "" : "s");
    if (ImGui::Button("Select Parts")) {
        project->replaceSelectedEntities(sceneProject->id, parts);
    }
    UIProbe::item("inspector.mesh.selectParts");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Select the GameObjects holding this model's meshes, to edit them together");
    }
    endTable();

    const ImVec2 fieldsTop = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled();
    drawMeshComponentFields(cpType, sceneProject, entities);
    ImGui::EndDisabled();
    const ImVec2 fieldsBottom(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetCursorScreenPos().y);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseHoveringRect(fieldsTop, fieldsBottom)) {
        ImGui::SetTooltip("Edit the parts");
    }
}

void editor::InspectorWindow::drawMeshComponentFields(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Cull Below Screen Height"));

    // Static variables for shape parameters
    static PrimitiveParameters shapeParams;

    // Add New Geometry property row
    propertyHeader("Geometry");
    if (ImGui::Button("Create Primitive")){
        ImGui::OpenPopup("menusettings_shape_geometry");

        updateShapePreview(shapeParams);
    }

    // Geometry creation popup
    ImGui::SetNextWindowSizeConstraints(ImVec2(17 * ImGui::GetFontSize(), 0), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopup("menusettings_shape_geometry")){
        ImGui::Text("Create Primitive");
        ImGui::Separator();

        const char* geometryTypes[] = { "Plane", "Box", "Sphere", "Cylinder", "Capsule", "Torus", "Wall" };

        beginTable(cpType, getLabelSize("Geometry Type"), "geometry_popup");

        // Geometry type selection
        propertyHeader("Geometry Type");

        float secondColSize = 8 * ImGui::GetFontSize();
        bool updatedPreview = false;

        Texture texRender = shapePreviewRender.getTexture();
        Widgets::image(Backend::getImGuiTexture(texRender.getRender()), ImVec2(secondColSize, secondColSize));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##geometry_type", &shapeParams.geometryType, geometryTypes, IM_ARRAYSIZE(geometryTypes))) {
            updatedPreview = true;
        }

        // Show parameters based on selected geometry type
        switch (shapeParams.geometryType) {
            case 0: // Plane
                propertyHeader("Width", secondColSize);
                if (ImGui::DragFloat("##plane_width", &shapeParams.planeWidth, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Depth", secondColSize);
                if (ImGui::DragFloat("##plane_depth", &shapeParams.planeDepth, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Tiles", secondColSize);
                if (ImGui::DragInt("##plane_tiles", (int*)&shapeParams.planeTiles, 1, 1, 100)) {
                    updatedPreview = true;
                }
                break;

            case 1: // Box
                propertyHeader("Width", secondColSize);
                if (ImGui::DragFloat("##box_width", &shapeParams.boxWidth, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Height", secondColSize);
                if (ImGui::DragFloat("##box_height", &shapeParams.boxHeight, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Depth", secondColSize);
                if (ImGui::DragFloat("##box_depth", &shapeParams.boxDepth, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Tiles", secondColSize);
                if (ImGui::DragInt("##box_tiles", (int*)&shapeParams.boxTiles, 1, 1, 100)) {
                    updatedPreview = true;
                }
                break;

            case 2: // Sphere
                propertyHeader("Radius", secondColSize);
                if (ImGui::DragFloat("##sphere_radius", &shapeParams.sphereRadius, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Slices", secondColSize);
                if (ImGui::DragInt("##sphere_slices", (int*)&shapeParams.sphereSlices, 1, 3, 100)) {
                    updatedPreview = true;
                }

                propertyHeader("Stacks", secondColSize);
                if (ImGui::DragInt("##sphere_stacks", (int*)&shapeParams.sphereStacks, 1, 3, 100)) {
                    updatedPreview = true;
                }
                break;

            case 3: // Cylinder
                propertyHeader("Base Radius", secondColSize);
                if (ImGui::DragFloat("##cylinder_base_radius", &shapeParams.cylinderBaseRadius, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Top Radius", secondColSize);
                if (ImGui::DragFloat("##cylinder_top_radius", &shapeParams.cylinderTopRadius, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Height", secondColSize);
                if (ImGui::DragFloat("##cylinder_height", &shapeParams.cylinderHeight, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Slices", secondColSize);
                if (ImGui::DragInt("##cylinder_slices", (int*)&shapeParams.cylinderSlices, 1, 3, 100)) {
                    updatedPreview = true;
                }

                propertyHeader("Stacks", secondColSize);
                if (ImGui::DragInt("##cylinder_stacks", (int*)&shapeParams.cylinderStacks, 1, 1, 100)) {
                    updatedPreview = true;
                }
                break;

            case 4: // Capsule
                propertyHeader("Base Radius", secondColSize);
                if (ImGui::DragFloat("##capsule_base_radius", &shapeParams.capsuleBaseRadius, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Top Radius", secondColSize);
                if (ImGui::DragFloat("##capsule_top_radius", &shapeParams.capsuleTopRadius, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Height", secondColSize);
                if (ImGui::DragFloat("##capsule_height", &shapeParams.capsuleHeight, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Slices", secondColSize);
                if (ImGui::DragInt("##capsule_slices", (int*)&shapeParams.capsuleSlices, 1, 3, 100)) {
                    updatedPreview = true;
                }

                propertyHeader("Stacks", secondColSize);
                if (ImGui::DragInt("##capsule_stacks", (int*)&shapeParams.capsuleStacks, 1, 1, 100)) {
                    updatedPreview = true;
                }
                break;

            case 5: // Torus
                propertyHeader("Radius", secondColSize);
                if (ImGui::DragFloat("##torus_radius", &shapeParams.torusRadius, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Ring Radius", secondColSize);
                if (ImGui::DragFloat("##torus_ring_radius", &shapeParams.torusRingRadius, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Sides", secondColSize);
                if (ImGui::DragInt("##torus_sides", (int*)&shapeParams.torusSides, 1, 3, 100)) {
                    updatedPreview = true;
                }

                propertyHeader("Rings", secondColSize);
                if (ImGui::DragInt("##torus_rings", (int*)&shapeParams.torusRings, 1, 3, 100)) {
                    updatedPreview = true;
                }
                break;

            case 6: // Wall
                propertyHeader("Width", secondColSize);
                if (ImGui::DragFloat("##wall_width", &shapeParams.wallWidth, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Height", secondColSize);
                if (ImGui::DragFloat("##wall_height", &shapeParams.wallHeight, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    updatedPreview = true;
                }

                propertyHeader("Tiles", secondColSize);
                if (ImGui::DragInt("##wall_tiles", (int*)&shapeParams.wallTiles, 1, 1, 100)) {
                    updatedPreview = true;
                }
                break;
        }

        if (updatedPreview){
            updateShapePreview(shapeParams);
        }

        endTable();

        ImGui::Separator();

        // Create geometry button
        if (ImGui::Button("Apply", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            for (Entity& entity : entities) {
                std::shared_ptr<jasperengine::MeshSystem> meshSys = sceneProject->scene->getSystem<MeshSystem>();
                MeshComponent meshComp = sceneProject->scene->getComponent<MeshComponent>(entity);

                updateMeshShape(meshComp, meshSys.get(), shapeParams);

                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new MeshChangeCmd(project, sceneProject->id, entities[0], meshComp));
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    RowSettings lightsSettings;
    lightsSettings.help = "Light this mesh with the scene's lights, 2D lights and image-based lighting. Turn off to draw it unlit, in its base and emissive colors only.";
    propertyRow(RowPropertyType::Bool, cpType, "receiveLights", "Receive Lights", sceneProject, entities, lightsSettings);

    RowSettings iblSettings;
    iblSettings.help = "Light this mesh with image-based lighting, from the scene's Skybox environment and from Reflection Probes. Skybox lighting requires a Skybox with a texture.";
    propertyRow(RowPropertyType::Bool, cpType, "receiveIBL", "Receive IBL", sceneProject, entities, iblSettings);

    propertyRow(RowPropertyType::Bool, cpType, "castShadows", "Cast Shadows", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "receiveShadows", "Receive Shadows", sceneProject, entities);
    RowSettings staticCasterSettings;
    staticCasterSettings.help = "Cache this mesh's shadows with the other static casters: they are redrawn only when one of them changes, "
                                "while dynamic casters move over them. Mark what does not move (level geometry). A static caster that "
                                "does change still shadows correctly, it only costs a redraw of the static layer.";
    propertyRow(RowPropertyType::Bool, cpType, "staticShadowCaster", "Static Shadow Caster", sceneProject, entities, staticCasterSettings);

    RowSettings cullingSettings;
    cullingSettings.enumEntries = &entriesCullingMode;
    cullingSettings.help = "Which faces the submeshes with Face Culling on leave out: the back faces (the default) or the front ones (inverted-hull outlines, insides). A material's cull mode overrides it.";
    propertyRow(RowPropertyType::Enum, cpType, "cullingMode", "Culling Mode", sceneProject, entities, cullingSettings);

    RowSettings lodSettings;
    lodSettings.help = "Let this mesh use simplified detail levels while the scene's Mesh Detail (LOD) is on. Turn off to always keep it at full detail.";
    propertyRow(RowPropertyType::Bool, cpType, "lodEnabled", "Detail Levels", sceneProject, entities, lodSettings);
    if (sceneProject->scene->isMeshLodEnabled() && sceneProject->scene->getComponent<MeshComponent>(entities[0]).lodEnabled) {
        RowSettings lodBiasSettings;
        lodBiasSettings.help = "Scales how far detail is kept: above 1 keeps it longer, below 1 drops it sooner.";
        lodBiasSettings.secondColSize = 6 * ImGui::GetFontSize();
        propertyRow(RowPropertyType::FloatPositive, cpType, "lodBias", "Detail Bias", sceneProject, entities, lodBiasSettings);

        // how a level change shows (spec S54 R9, Unity's LOD Group)
        const MeshComponent& lodMesh = sceneProject->scene->getComponent<MeshComponent>(entities[0]);
        const bool instanced = sceneProject->scene->findComponent<InstancedMeshComponent>(entities[0]) != nullptr;
        RowSettings fadeModeSettings;
        fadeModeSettings.enumEntries = &entriesLodFadeMode;
        fadeModeSettings.help = "Cross Fade blends between detail levels with a short dithered fade instead of switching at once "
                                "(fading objects draw twice for the duration). None switches at once.";
        propertyRow(RowPropertyType::Enum, cpType, "lodFadeMode", "Fade Mode", sceneProject, entities, fadeModeSettings);
        if (lodMesh.lodFadeMode == LodFadeMode::CROSS_FADE){
            if (!instanced){
                RowSettings animateSettings;
                animateSettings.indent = true;
                animateSettings.help = "Fade over the scene's Cross-fade Duration when the level changes (fading objects draw twice for the "
                                       "duration). Off: fade over a band of distance (Fade Transition Width).";
                propertyRow(RowPropertyType::Bool, cpType, "lodAnimateCrossFading", "Animate Cross-fading", sceneProject, entities, animateSettings);
            }
            if (instanced || !lodMesh.lodAnimateCrossFading){
                RowSettings widthSettings;
                widthSettings.indent = true;
                widthSettings.floatMin = 0.0f;
                widthSettings.floatMax = 1.0f;
                widthSettings.help = std::string(instanced ? "Instanced meshes always fade by distance. " : "") +
                                     "The share of each level's distance band over which it fades into the next one: objects in "
                                     "the band draw twice (both levels) while they stay there.";
                propertyRow(RowPropertyType::Float_0_1, cpType, "lodFadeTransitionWidth", "Fade Transition Width", sceneProject, entities, widthSettings);
            }
        }
        RowSettings cullBelowSettings;
        cullBelowSettings.floatMin = 0.0f;
        cullBelowSettings.floatMax = 1.0f;
        cullBelowSettings.format = "%.3f";
        cullBelowSettings.help = "Stop drawing this mesh when it covers less than this share of the view's height (0.02: 2 %). "
                                 "0 never culls it. With Cross Fade it fades out (fading objects draw twice for the duration).";
        propertyRow(RowPropertyType::Float_0_1, cpType, "lodCullBelow", "Cull Below Screen Height", sceneProject, entities, cullBelowSettings);
    }

    RowSettings instancingSettings;
    instancingSettings.help = "Let this mesh draw in one instanced draw with the opaque meshes of the same mesh, material and shader "
                              "while Project Settings > Graphics > GPU Instancing is on (the image stays the same). Turn off to draw it alone.";
    propertyRow(RowPropertyType::Bool, cpType, "allowGpuInstancing", "Allow GPU Instancing", sceneProject, entities, instancingSettings);
    drawSortingRows(cpType, sceneProject, entities);

    RowSettings reflectionProbeSettings;
    reflectionProbeSettings.help = "Draw this mesh into Reflection Probe captures. Turn off to keep a GameObject out of its own reflection.";
    propertyRow(RowPropertyType::Bool, cpType, "renderInReflectionProbes", "Render in Probes", sceneProject, entities, reflectionProbeSettings);

    RowSettings transparencySettings;
    transparencySettings.help = "Just for render ordering";
    propertyRowWithAutoButton(RowPropertyType::Bool, cpType, "transparent", "Transparent", "autoTransparency", "Auto Transparency", sceneProject, entities, transparencySettings);

    endTable();

    // the morph target weights (Record keys them, animation clips 9.R5)
    drawBlendShapes(sceneProject, entities);

    drawCustomShaderRow(cpType, ShaderType::MESH, sceneProject, entities);

    unsigned int numSubmeshes = sceneProject->scene->getComponent<MeshComponent>(entities[0]).numSubmeshes;
    for (Entity& entity : entities){
        numSubmeshes = std::min(numSubmeshes, sceneProject->scene->getComponent<MeshComponent>(entity).numSubmeshes);
    }
    bool hasGeneratedSubmeshes = false;
    for (const Entity& entity : entities) {
        const MeshComponent& checkMesh = sceneProject->scene->getComponent<MeshComponent>(entity);
        for (unsigned int gs = 0; gs < checkMesh.numSubmeshes; gs++) {
            if (checkMesh.submeshes[gs].generated) {
                hasGeneratedSubmeshes = true;
                break;
            }
        }
        if (hasGeneratedSubmeshes) break;
    }

    if (!hasGeneratedSubmeshes && ImGui::Button(ICON_FA_PLUS " Add Submesh", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        for (Entity& entity : entities) {
            MeshComponent meshComp = sceneProject->scene->getComponent<MeshComponent>(entity);
            const unsigned int newSubmeshIndex = meshComp.numSubmeshes;
            meshComp.numSubmeshes++;
            meshComp.submeshes[newSubmeshIndex] = Submesh();
            meshComp.submeshes[newSubmeshIndex].needUpdateTexture = true;

            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new MeshChangeCmd(project, sceneProject->id, entity, meshComp));
        }
    }

    for (int s = 0; s < numSubmeshes; s++){
        ImGui::PushID(s);
        bool submeshGenerated = false;
        for (const Entity& entity : entities) {
            const MeshComponent& checkMesh = sceneProject->scene->getComponent<MeshComponent>(entity);
            if ((unsigned int)s < checkMesh.numSubmeshes && checkMesh.submeshes[s].generated) {
                submeshGenerated = true;
                break;
            }
        }
        std::string submeshLabel = submeshGenerated ? ("Submesh " + std::to_string(s) + " (Internal)") : ("Submesh " + std::to_string(s));
        separatorText(submeshLabel.c_str());

        ImVec2 separatorMin = ImGui::GetItemRectMin();
        ImVec2 separatorMax = ImGui::GetItemRectMax();
        ImVec2 cursorAfterSeparator = ImGui::GetCursorPos();

        {
            float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
            float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
            ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);

            float buttonX = separatorMax.x - deleteButtonSize.x;
            float buttonY = separatorMin.y + (separatorMax.y - separatorMin.y - ImGui::GetFrameHeight()) * 0.5f;

            ImDrawList* separatorDrawList = ImGui::GetWindowDrawList();
            float separatorGap = ImGui::GetStyle().ItemSpacing.x;
            separatorDrawList->AddRectFilled(
                ImVec2(buttonX - separatorGap, separatorMin.y),
                separatorMax,
                ImGui::GetColorU32(ImGuiCol_WindowBg));

            ImGui::SetCursorScreenPos(ImVec2(buttonX, buttonY));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
            if (ImGui::Button((std::string(ICON_FA_TRASH_CAN) + "##delete_submesh_" + std::to_string(s)).c_str(), deleteButtonSize)) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                ImGui::SetCursorPos(cursorAfterSeparator);

                for (Entity& entity : entities) {
                    MeshComponent meshComp = sceneProject->scene->getComponent<MeshComponent>(entity);
                    if ((unsigned int)s >= meshComp.numSubmeshes) {
                        continue;
                    }

                    for (unsigned int j = (unsigned int)s; j + 1 < meshComp.numSubmeshes; j++) {
                        meshComp.submeshes[j] = meshComp.submeshes[j + 1];
                    }

                    if (meshComp.numSubmeshes > 0) {
                        meshComp.numSubmeshes--;
                        meshComp.submeshes[meshComp.numSubmeshes] = Submesh();
                    }

                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                        new MeshChangeCmd(project, sceneProject->id, entity, meshComp));
                }

                ImGui::PopID();
                return;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove submesh");
            }
        }
        ImGui::SetCursorPos(cursorAfterSeparator);
        if (submeshGenerated) ImGui::BeginDisabled(true);

        beginTable(cpType, getLabelSize("Material"), "submeshes_material");

        bool materialOpened = false;
        if (propertyRow(RowPropertyType::Material, cpType, "submeshes["+std::to_string(s)+"].material", "Material", sceneProject, entities)){
            materialOpened = true;
        }
        endTable();

        if (materialOpened){
            drawSubmeshMaterialRows(cpType, sceneProject, entities, s, [this, sceneProject, entities, s]() {
                for (const Entity& entity : entities) {
                    MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
                    if (!mesh || s >= (int)mesh->numSubmeshes) {
                        continue;
                    }

                    Material& material = mesh->submeshes[s].material;
                    if (material.name.empty()) {
                        continue;
                    }

                    std::string relativePath = std::filesystem::path(material.name).lexically_normal().generic_string();
                    markMaterialDirty(sceneProject->id, entity, s, relativePath);
                }
            });
        }

        beginTable(cpType, getLabelSize("Texture Shadow"), "submeshes_settings");
        RowSettings settingsPrimitive;
        settingsPrimitive.enumEntries = &entriesPrimitiveType;

        propertyRow(RowPropertyType::Bool, cpType, "submeshes["+std::to_string(s)+"].faceCulling", "Face Culling", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "submeshes["+std::to_string(s)+"].textureShadow", "Texture Shadow", sceneProject, entities);
        propertyRow(RowPropertyType::Enum, cpType, "submeshes["+std::to_string(s)+"].primitiveType", "Primitive", sceneProject, entities, settingsPrimitive);
        RowSettings settingsTextureRect;
        settingsTextureRect.showColors = false;
        propertyRow(RowPropertyType::Vector4, cpType, "submeshes["+std::to_string(s)+"].textureRect", "Texture Rect", sceneProject, entities, settingsTextureRect);

        endTable();

        if (submeshGenerated) ImGui::EndDisabled();
        ImGui::PopID();
    }
}

void editor::InspectorWindow::drawModelComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Model File"));

    prefabRowProperty = "filename";
    propertyHeader("Model File");

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ModelComponent& model = sceneProject->scene->getComponent<ModelComponent>(entity);

        std::string currentPath = model.filename;
        std::string displayName = currentPath.empty() ? "< Not set >" : std::filesystem::path(currentPath).filename().string();

        ImGui::BeginGroup();

        float thumbSize = ImGui::GetFrameHeight() * 3;
        Texture* thumbTexture = findThumbnail(currentPath);
        if (thumbTexture) {
            ImU32 border_col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
            drawImageWithBorderAndRounding(thumbTexture, ImVec2(thumbSize, thumbSize), ImGui::GetStyle().FrameRounding, border_col);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                Widgets::image(Backend::getImGuiTexture(thumbTexture->getRender()), ImVec2(thumbTexture->getWidth(), thumbTexture->getHeight()));
                ImGui::EndTooltip();
            }
        }

        float availWidth = ImGui::GetContentRegionAvail().x;
        float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x + ImGui::GetStyle().FramePadding.x * 2;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);

        ImGui::BeginChild("modelfilename", ImVec2(availWidth - buttonWidth - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x * 2, ImGui::GetFrameHeight()),
            false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        float textWidth = ImGui::CalcTextSize(displayName.c_str()).x;
        float childAvail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(childAvail - textWidth - 2);
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        if (currentPath.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Text("%s", displayName.c_str());
        if (currentPath.empty())
            ImGui::PopStyleColor();
        ImGui::EndChild();
        if (!currentPath.empty()){
            ImGui::SetItemTooltip("%s", currentPath.c_str());
        }

        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##model_load")) {
            std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_MODEL);
            if (!path.empty()) {
                std::filesystem::path assetsPath = project->getAssetsPath();
                std::filesystem::path filePath = std::filesystem::absolute(path);

                std::error_code ec;
                auto relative = std::filesystem::relative(filePath, assetsPath, ec);
                if (ec || relative.empty() || *relative.begin() == "..") {
                    ImGui::OpenPopup("Model Import Error");
                }else{
                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new ModelLoadCmd(project, sceneProject->id, entity, FileUtils::pathToGenericUtf8(relative)));
                }
            }
        }

        // Unity's None: unloads the model and leaves the Model without a file
        ImGui::SameLine();
        std::string clearReason;
        const bool canClearModel = ModelClearCmd::canClear(project, sceneProject->id, entity, &clearReason);
        ImGui::BeginDisabled(!canClearModel);
        if (ImGui::Button(ICON_FA_XMARK "##model_clear")) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new ModelClearCmd(project, sceneProject->id, entity));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", canClearModel ? "None: unload the model" : clearReason.c_str());
        }

        if (EditorModal::begin("Model Import Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Selected file must be within the assets directory.");
            EditorModal::message("Selected file must be within the assets directory.");
            ImGui::Separator();
            float bw = 120;
            float ww = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((ww - bw) * 0.5f);
            if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndGroup();

        // Drag and drop model files from Resources
        if (sceneProject && sceneProject->playState == ScenePlayState::STOPPED) {
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                    std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
                    if (!receivedStrings.empty()) {
                        const bool insideAssets = project->isInsideAssetsPath(receivedStrings[0]);
                        const std::string droppedRelativePath = droppedAssetPath(project, receivedStrings[0], insideAssets);
                        if (Util::isModelFile(droppedRelativePath)) {
                            if (insideAssets) {
                                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new ModelLoadCmd(project, sceneProject->id, entity, droppedRelativePath));
                            } else {
                                Backend::getApp().registerOutsideAssetsAlert(receivedStrings[0]);
                            }
                            ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

    }else{
        ImGui::TextDisabled("Select a single GameObject");
    }

    propertyRow(RowPropertyType::LocalEntity, cpType, "skeleton", "Skeleton", sceneProject, entities);

    endTable();

    if (entities.size() == 1) drawModelRig(cpType, sceneProject, entities[0]);

    if (entities.size() != 1) {
        separatorText("Animations");
        ImGui::TextDisabled("Select a single GameObject to edit animations");
        return;
    }

    ModelComponent& model = sceneProject->scene->getComponent<ModelComponent>(entities[0]);

    separatorText("Animations");

    beginTable(cpType, getLabelSize("Animation 00"), "model_animations_header");
    propertyHeader("Animations", -1, false, false);
    ImGui::Text("%zu", model.animations.size());
    // a model played by an Animation Player has no entity animations (R15)
    if (drawSummaryAddButton("Add Animation##model_animations_add", 0.0f, model.animationEntities,
                             "The model is played by an Animation Player (no entity animations)")) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (Entity entity : entities) {
            if (ModelComponent* modelComp = sceneProject->scene->findComponent<ModelComponent>(entity)) {
                std::vector<Entity> newAnimations = modelComp->animations;
                newAnimations.push_back(NULL_ENTITY);
                multiCmd->addPropertyCmd<std::vector<Entity>>(project, sceneProject->id, entity, cpType, "animations", newAnimations, nullptr);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    endTable();

    if (model.animations.empty()) {
        drawModelClipRows(cpType, sceneProject, entities[0], {});
        return;
    }

    // the loader's entities keep duplicate glTF names: the rows show the sub-clip names
    const std::vector<std::string> subClipNames = modelSubClipNames(model.filename);

    // the rows standing for a sub-clip carry its clip buttons (Add to Animation Player, Extract)
    const float clipButtonsWidth = modelRowClipButtonsWidth();
    std::vector<std::string> shownInRows;

    const float animationFirstColSize = getLabelSize("Animation 00");
    const float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
    const float deleteButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + clearButtonFramePadding * 2.0f;
    const float rowSpacing = ImGui::GetStyle().ItemSpacing.x;
    // the label column beginTable gives the table (Unity's ~40% label width), so the rows' buttons fit
    const float labelColWidth = std::max(animationFirstColSize, std::min(ImGui::GetContentRegionAvail().x * 0.4f, Theme::dpi(180.0f)));
    const float valueColWidth = std::max(0.0f, ImGui::GetContentRegionAvail().x - labelColWidth - ImGui::GetStyle().CellPadding.x * 4.0f);

    beginTable(cpType, animationFirstColSize, "model_animations");

    for (size_t i = 0; i < model.animations.size(); i++) {
        ImGui::PushID((int)i);

        std::string propId = "animations[" + std::to_string(i) + "]";
        Entity animEntity = model.animations[i];
        std::string animationLabel = "Animation " + std::to_string(i);
        std::string rowSubClip;
        std::string rowEntityName;
        if (animEntity != NULL_ENTITY && sceneProject->scene->isEntityCreated(animEntity)) {
            if (sceneProject->scene->findComponent<AnimationComponent>(animEntity)) {
                rowEntityName = sceneProject->scene->getEntityName(animEntity);
                animationLabel = inspectorclips::modelAnimationRowLabel(rowEntityName, subClipNames, i, animationLabel);
                if (i < subClipNames.size() && animationLabel == subClipNames[i] &&
                    std::find(shownInRows.begin(), shownInRows.end(), animationLabel) == shownInRows.end()) {
                    rowSubClip = animationLabel;
                    shownInRows.push_back(rowSubClip);
                }
            }
        }
        const float rowButtons = rowSubClip.empty() ? 0.0f : clipButtonsWidth;

        RowSettings settingsAnimation;
        settingsAnimation.entityFilter.set(sceneProject->scene->getComponentId<AnimationComponent>());
        settingsAnimation.secondColSize = std::max(0.0f, valueColWidth - deleteButtonWidth - rowSpacing - rowButtons);

        propertyRow(RowPropertyType::LocalEntity, cpType, propId, animationLabel, sceneProject, entities, settingsAnimation);

        if (!rowSubClip.empty() && rowButtons > 0.0f) {
            ImGui::SameLine();
            drawModelRowClipButtons(sceneProject, entities[0], rowSubClip, rowEntityName);
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        const size_t animationIndex = i;
        if (ImGui::Button((std::string(ICON_FA_TRASH_CAN) + "##remove_animation").c_str(), ImVec2(deleteButtonWidth, 0))) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities) {
                if (ModelComponent* modelComp = sceneProject->scene->findComponent<ModelComponent>(entity)) {
                    if (animationIndex < modelComp->animations.size()) {
                        std::vector<Entity> newAnimations = modelComp->animations;
                        newAnimations.erase(newAnimations.begin() + (long int)animationIndex);
                        multiCmd->addPropertyCmd<std::vector<Entity>>(project, sceneProject->id, entity, cpType, "animations", newAnimations, nullptr);
                    }
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove animation");
        }

        ImGui::PopID();
    }

    endTable();

    drawModelClipRows(cpType, sceneProject, entities[0], shownInRows);
}

void editor::InspectorWindow::drawUIComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    // a Button writes the colour and/or the texture from its state (its transition): those rows
    // are read-only, the Button's Normal values are what to edit
    bool colorByButton = !entities.empty();
    bool textureByButton = !entities.empty();
    for (Entity entity : entities){
        const ButtonComponent* button = sceneProject->scene->findComponent<ButtonComponent>(entity);
        colorByButton = colorByButton && button && UISystem::buttonOwnsUIColor(*button);
        textureByButton = textureByButton && button && UISystem::buttonOwnsUITexture(*button);
    }

    beginTable(cpType, getLabelSize("Texture"));
    ImGui::BeginDisabled(colorByButton);
    propertyRow(RowPropertyType::Color4L, cpType, "color", "Color", sceneProject, entities);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(textureByButton);
    propertyRow(RowPropertyType::Texture, cpType, "texture", "Texture", sceneProject, entities);
    ImGui::EndDisabled();
    endTable();
    if (colorByButton || textureByButton){
        const char* note = (colorByButton && textureByButton) ? "Color and Texture are set by the Button's transition: edit its Normal Color and Normal Texture"
                         : colorByButton ? "Color is set by the Button's transition: edit its Normal Color"
                                         : "Texture is set by the Button's transition: edit its Normal Texture";
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", note);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("A Button whose Normal is unset takes these values when it loads; set the Button's Transition to None to edit them yourself");
    }

    drawCustomShaderRow(cpType, ShaderType::UI, sceneProject, entities);

    // what the Canvas Groups above make of the element (ui canvas group spec 7.7)
    if (entities.size() == 1){
        drawCanvasGroupInheritedLine(sceneProject, entities[0]);
    }
}

void editor::InspectorWindow::drawCanvasGroupInheritedLine(SceneProject* sceneProject, Entity entity){
    Scene* scene = sceneProject->scene;
    const CanvasGroupState state = scene->getSystem<UISystem>()->getCanvasGroupState(entity);
    if (state.group == NULL_ENTITY){
        return;
    }

    char alphaText[32];
    snprintf(alphaText, sizeof(alphaText), "%.2f", state.alpha);
    std::string line = std::string("Inherited alpha ") + alphaText;
    if (!state.interactable){
        line += " \xc2\xb7 not interactable";
    }
    if (!state.blocksRaycasts){
        line += " \xc2\xb7 click-through";
    }
    std::string groupName = scene->getEntityName(state.group);
    if (groupName.empty()){
        groupName = "GameObject " + std::to_string(state.group);
    }
    line += " (Canvas Group: " + groupName + ")";

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", line.c_str());
    ImGui::PopStyleColor();
    if (state.group != entity){
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROW_POINTER " Select##canvas_group_select")){
            project->setSelectedSceneForProperties(sceneProject->id);
            project->setSelectedEntity(sceneProject->id, state.group);
        }
        if (ImGui::IsItemHovered()){
            ImGui::SetTooltip("Select the nearest Canvas Group of this element");
        }
    }
}

void editor::InspectorWindow::drawCanvasGroupComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    static std::vector<editor::EnumEntry> entriesRenderMode = {
        { (int)CanvasGroupRenderMode::PER_ELEMENT, "Per Element" },
        { (int)CanvasGroupRenderMode::FLATTEN, "Flatten" }
    };

    RowSettings settingsAlpha;
    settingsAlpha.floatMin = 0.0f;
    settingsAlpha.floatMax = 1.0f;
    settingsAlpha.help = "Multiplied into the alpha of every UI element of this GameObject and its children (nested groups multiply). "
                         "To hide a panel like Unity: alpha 0, Interactable and Blocks Raycasts off (alpha 0 alone still blocks clicks)";

    RowSettings settingsInteractable;
    settingsInteractable.help = "Off: Buttons, Input Fields, Scrollbars and Panels below stop reacting and show their disabled look. "
                                "Pointer callbacks of plain images still fire";

    RowSettings settingsBlocks;
    settingsBlocks.help = "Off: clicks and touches go through the elements below to what is behind them "
                          "(a dragged item keeps its drag; the drop finds the slot behind it)";

    RowSettings settingsIgnore;
    settingsIgnore.help = "The Canvas Groups above this one stop counting for this subtree (unlike Unity, it shows even under a parent at alpha 0)";

    RowSettings settingsRenderMode;
    settingsRenderMode.enumEntries = &entriesRenderMode;
    settingsRenderMode.help = "How overlapping children fade.\n"
                              "Per Element (Unity, no cost): each element is faded, overlaps show through:\n"
                              "   [A][AB][B]   the overlap AB is darker than A and B\n"
                              "Flatten (CSS opacity): the subtree is drawn as one layer, then faded:\n"
                              "   [A][A ][B]   the overlap looks like the front element\n"
                              "Flatten costs, while alpha is between 0 and 1, one offscreen pass per camera (a cleared screen-size\n"
                              "target; flattened siblings share one) and a quad over the group's bounds";

    beginTable(cpType, getLabelSize("Ignore Parent Groups"));
    propertyRow(RowPropertyType::Float_0_1, cpType, "alpha", "Alpha", sceneProject, entities, settingsAlpha);
    propertyRow(RowPropertyType::Bool, cpType, "interactable", "Interactable", sceneProject, entities, settingsInteractable);
    propertyRow(RowPropertyType::Bool, cpType, "blocksRaycasts", "Blocks Raycasts", sceneProject, entities, settingsBlocks);
    propertyRow(RowPropertyType::Bool, cpType, "ignoreParentGroups", "Ignore Parent Groups", sceneProject, entities, settingsIgnore);
    propertyRow(RowPropertyType::Enum, cpType, "renderMode", "Render Mode", sceneProject, entities, settingsRenderMode);
    endTable();

    // HTML UI documents have their own opacity and pointer-events (CSS)
    Scene* scene = sceneProject->scene;
    bool holdsDocument = false;
    auto transforms = scene->getComponentArray<Transform>();
    for (Entity entity : entities){
        if (!transforms->hasEntity(entity)){
            continue;
        }
        const size_t first = transforms->getIndex(entity);
        const size_t last = scene->findBranchLastIndex(entity);
        for (size_t i = first; i <= last && !holdsDocument; i++){
            holdsDocument = scene->findComponent<UIDocumentComponent>(transforms->getEntity(i)) != nullptr;
        }
    }
    if (holdsDocument){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " HTML documents ignore Canvas Groups: use CSS opacity / pointer-events");
        ImGui::PopStyleColor();
    }

    // the groups above this one
    if (entities.size() == 1){
        Entity parent = NULL_ENTITY;
        if (Transform* transform = scene->findComponent<Transform>(entities[0])){
            parent = transform->parent;
        }
        const CanvasGroupComponent& own = scene->getComponent<CanvasGroupComponent>(entities[0]);
        if (parent != NULL_ENTITY && !own.ignoreParentGroups){
            drawCanvasGroupInheritedLine(sceneProject, parent);
        }
    }
}

void editor::InspectorWindow::drawInputFieldComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settings;
    settings.onValueChanged = [sceneProject, entities](){
        for (const Entity& entity : entities){
            if (InputFieldComponent* inputfield = sceneProject->scene->findComponent<InputFieldComponent>(entity)){
                inputfield->needUpdateInputField = true;
            }
        }
    };

    RowSettings settingsFloat;
    settingsFloat.onValueChanged = settings.onValueChanged;

    RowSettings settingsInt;
    settingsInt.stepSize = 1.0f;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();
    settingsInt.onValueChanged = settings.onValueChanged;

    beginTable(cpType, getLabelSize("Selection"));
    propertyRow(RowPropertyType::LocalEntity, cpType, "text", "Text", sceneProject, entities, settings);
    propertyRow(RowPropertyType::LocalEntity, cpType, "selection", "Selection", sceneProject, entities, settings);
    propertyRow(RowPropertyType::LocalEntity, cpType, "cursor", "Cursor", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "disabled", "Disabled", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "password", "Password", sceneProject, entities, settings);
    endTable();

    separatorText("Content");
    beginTable(cpType, getLabelSize("Placeholder"), "inputfield_content");
    propertyRow(RowPropertyType::String, cpType, "placeholder", "Placeholder", sceneProject, entities, settings);
    endTable();

    separatorText("Cursor");
    beginTable(cpType, getLabelSize("Cursor Width"), "inputfield_cursor");
    propertyRow(RowPropertyType::Float, cpType, "cursorBlink", "Blink", sceneProject, entities, settingsFloat);
    propertyRow(RowPropertyType::Float, cpType, "cursorWidth", "Width", sceneProject, entities, settingsFloat);
    propertyRow(RowPropertyType::Color4L, cpType, "cursorColor", "Color", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Color4L, cpType, "selectionColor", "Selection Color", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Color4L, cpType, "placeholderColor", "Placeholder Color", sceneProject, entities, settings);
    endTable();

    separatorText("Selection");
    beginTable(cpType, getLabelSize("Selection Anchor"), "inputfield_selection");
    propertyRow(RowPropertyType::Int, cpType, "cursorIndex", "Cursor Index", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::Int, cpType, "selectionAnchor", "Selection Anchor", sceneProject, entities, settingsInt);
    endTable();

    if (entities.size() == 1){
        InputFieldComponent* inputfield = sceneProject->scene->findComponent<InputFieldComponent>(entities[0]);
        if (inputfield){
            beginTable(cpType, getLabelSize("Password Char"), "inputfield_password_char");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Password Char");
            ImGui::TableSetColumnIndex(1);
            char passwordCharBuffer[2] = { inputfield->passwordChar ? inputfield->passwordChar : '*', '\0' };
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##inputfield_password_char", passwordCharBuffer, sizeof(passwordCharBuffer))){
                inputfield->passwordChar = passwordCharBuffer[0] ? passwordCharBuffer[0] : '*';
                settings.onValueChanged();
            }
            endTable();
        }
    }
}

// SYNC4: upstream's Text rows (ui-scaling-text: Outline width/colour, Shadow enabled/colour/offset/softness/dilate,
// Visible Characters) and the RectTransform "Driven by Safe Area" lock (safe-area) were edits to the hand-written
// Text/RectTransform drawers this branch replaced with reflection rows (UIReflect.cpp, InspectorDefault.cpp): they
// belong there. drawTextEffectComponent is the Text Effect component's own drawer.
void editor::InspectorWindow::drawTextEffectComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    static std::vector<editor::EnumEntry> entriesType = {
        { (int)TextEffectType::None, "None" },
        { (int)TextEffectType::Wave, "Wave" },
        { (int)TextEffectType::Bounce, "Bounce" },
        { (int)TextEffectType::Shake, "Shake" },
        { (int)TextEffectType::Pulse, "Pulse" },
        { (int)TextEffectType::Typewriter, "Typewriter" },
        { (int)TextEffectType::FadeIn, "Fade In" },
        { (int)TextEffectType::Grow, "Grow" },
        { (int)TextEffectType::Unfold, "Unfold" },
        { (int)TextEffectType::Rainbow, "Rainbow" },
        { (int)TextEffectType::Gradient, "Gradient" }
    };
    static std::vector<editor::EnumEntry> entriesMix = {
        { (int)TextEffectMix::Multiply, "Multiply" },
        { (int)TextEffectMix::Add, "Add" },
        { (int)TextEffectMix::Replace, "Replace" }
    };

    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    TextEffectComponent* effect = scene->findComponent<TextEffectComponent>(entity);
    if (!effect) return;

    if (!scene->findComponent<TextComponent>(entity)) {
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Needs a Text on this GameObject");
    }

    // preview in edit mode (TMP Effect runs in the editor), the UI clock
    if (sceneProject->playState == ScenePlayState::STOPPED && entities.size() == 1) {
        if (ImGui::Button(effect->previewInEditor ? ICON_FA_STOP " Stop Preview" : ICON_FA_PLAY " Preview")) {
            effect->previewInEditor = !effect->previewInEditor;
            effect->playing = effect->previewInEditor;
            effect->needRestart = effect->previewInEditor;
            effect->wasActive = false;
            effect->needApply = true;
            sceneProject->needUpdateRender = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Restart")) {
            effect->previewInEditor = true;
            effect->playing = true;
            effect->needRestart = true;
            effect->wasActive = false;
            sceneProject->needUpdateRender = true;
        }
        if (effect->previewInEditor)
            sceneProject->needUpdateRender = true;
    }

    RowSettings settingsInt;
    settingsInt.stepSize = 1.0f;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsLength = settingsInt;
    settingsLength.intMin = -1;
    settingsLength.intMax = 1000000;
    settingsLength.help = "Characters from Start Index; -1 to the end.";
    RowSettings settingsStart = settingsInt;
    settingsStart.intMin = 0;
    settingsStart.intMax = 1000000;
    RowSettings settingsWords;
    settingsWords.help = "Whole words to animate, separated by spaces (TMP ForWords). Overrides the range.";
    RowSettings settingsUnscaled;
    settingsUnscaled.help = "Runs while Engine.timeScale is 0 (pause menus), on the UI clock. Off: game time.";
    RowSettings settingsLoop;
    settingsLoop.help = "One-shot layers (typewriter, fade in, grow, unfold) start again once they all finished.";

    beginTable(cpType, getLabelSize("Unscaled Time"), "text_effect_settings");
    propertyRow(RowPropertyType::Bool, cpType, "autoPlay", "Auto Play", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "loop", "Loop", sceneProject, entities, settingsLoop);
    propertyRow(RowPropertyType::Bool, cpType, "useUnscaledTime", "Unscaled Time", sceneProject, entities, settingsUnscaled);
    propertyRow(RowPropertyType::Int, cpType, "startIndex", "Start Index", sceneProject, entities, settingsStart);
    propertyRow(RowPropertyType::Int, cpType, "length", "Length", sceneProject, entities, settingsLength);
    propertyRow(RowPropertyType::String, cpType, "forWords", "For Words", sceneProject, entities, settingsWords);
    endTable();

    // every layer up to the last one in use, and the next empty one (empty layers between
    // used ones stay shown: MCP and Lua may fill any slot)
    int lastUsed = -1;
    for (int i = 0; i < MAX_TEXT_EFFECT_LAYERS; i++) {
        if (effect->layers[i].type != TextEffectType::None) lastUsed = i;
    }
    for (int i = 0; i < MAX_TEXT_EFFECT_LAYERS && i <= lastUsed + 1; i++) {
        const std::string prefix = "layers[" + std::to_string(i) + "].";
        TextEffectLayer& layer = effect->layers[i];

        ImGui::PushID(i);
        ImGui::SeparatorText(("Layer " + std::to_string(i + 1)).c_str());

        RowSettings settingsType;
        settingsType.enumEntries = &entriesType;
        RowSettings settingsF;
        settingsF.stepSize = 0.01f;
        settingsF.secondColSize = 6 * ImGui::GetFontSize();

        beginTable(cpType, getLabelSize("Chars Per Second"), ("text_effect_layer_" + std::to_string(i)).c_str());
        propertyRow(RowPropertyType::Enum, cpType, prefix + "type", "Type", sceneProject, entities, settingsType);
        const TextEffectType type = layer.type;
        if (type != TextEffectType::None) {
            propertyRow(RowPropertyType::Bool, cpType, prefix + "enabled", "Enabled", sceneProject, entities);

            RowSettings amplitude = settingsF;
            amplitude.help = (type == TextEffectType::Pulse) ? "Scale change (0.15 = 15% bigger and smaller)."
                           : (type == TextEffectType::Grow || type == TextEffectType::Unfold) ? "Share of the change (TMP Effect's Amplitude): 1 = the whole glyph, above overshoots."
                           : "Font sizes (em).";
            if (type == TextEffectType::Wave || type == TextEffectType::Bounce || type == TextEffectType::Shake || type == TextEffectType::Pulse ||
                type == TextEffectType::Grow || type == TextEffectType::Unfold)
                propertyRow(RowPropertyType::Float, cpType, prefix + "amplitude", "Amplitude", sceneProject, entities, amplitude);

            RowSettings duration = settingsF;
            duration.help = TextEffects::isOneShot(type) ? "Seconds the effect takes over the whole text." : "Seconds of one period.";
            if (type != TextEffectType::Shake && !(type == TextEffectType::Typewriter && layer.charactersPerSecond > 0.0f))
                propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "duration", "Duration", sceneProject, entities, duration);

            RowSettings spread = settingsF;
            spread.help = "0: one character after the other; 1: all at once (TMP Character Duration Ratio).";
            if (type == TextEffectType::Bounce || type == TextEffectType::Pulse || type == TextEffectType::FadeIn ||
                type == TextEffectType::Grow || type == TextEffectType::Unfold || type == TextEffectType::Gradient)
                propertyRow(RowPropertyType::Float_0_1, cpType, prefix + "spread", "Spread", sceneProject, entities, spread);

            RowSettings phase = settingsF;
            phase.help = "Radians between two characters (Rainbow: of the hue circle).";
            if (type == TextEffectType::Wave || type == TextEffectType::Rainbow)
                propertyRow(RowPropertyType::Float, cpType, prefix + "phaseStep", "Phase Step", sceneProject, entities, phase);

            if (type == TextEffectType::Shake)
                propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "frequency", "Frequency", sceneProject, entities, settingsF);

            if (type == TextEffectType::Typewriter) {
                RowSettings cps = settingsF;
                cps.help = "Characters a second; 0: the whole text over Duration.";
                propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "charactersPerSecond", "Chars Per Second", sceneProject, entities, cps);
                RowSettings fade = settingsF;
                fade.help = "Seconds each character fades in.";
                propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "fadeTime", "Fade Time", sceneProject, entities, fade);
            }

            if (type == TextEffectType::Rainbow || type == TextEffectType::Gradient) {
                RowSettings mix;
                mix.enumEntries = &entriesMix;
                mix.help = "How the colour combines with the text's: Multiply, Add (TMP Effect Add), Replace.";
                propertyRow(RowPropertyType::Enum, cpType, prefix + "mix", "Mix", sceneProject, entities, mix);
            }
            if (type == TextEffectType::Rainbow) {
                propertyRow(RowPropertyType::Float_0_1, cpType, prefix + "saturation", "Saturation", sceneProject, entities, settingsF);
                propertyRow(RowPropertyType::Float_0_1, cpType, prefix + "lightness", "Lightness", sceneProject, entities, settingsF);
            }
        }
        endTable();

        // the gradient's stops (sRGB), one undo step per edit
        if (type == TextEffectType::Gradient && entities.size() == 1) {
            std::vector<TextEffectGradientStop> stops = layer.gradient;
            bool changed = false;
            bool finished = false;
            int remove = -1;
            for (size_t s = 0; s < stops.size(); s++) {
                ImGui::PushID((int)s);
                // the stop's colour through the Color window (sRGB, as stored)
                ui::ColorFieldOptions stopOptions;
                stopOptions.linearStorage = false;
                stopOptions.label = "Gradient Color";
                stopOptions.width = ImGui::GetFontSize() * 4.0f;
                const ui::EditPhase stopPhase = ui::colorField("##stopColor", stops[s].color, stopOptions);
                if (stopPhase == ui::EditPhase::Changing || stopPhase == ui::EditPhase::Cancelled || stopPhase == ui::EditPhase::Applied) changed = true;
                if (stopPhase == ui::EditPhase::Committed || stopPhase == ui::EditPhase::Cancelled || stopPhase == ui::EditPhase::Applied) finished = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
                if (ImGui::SliderFloat("##stopPosition", &stops[s].position, 0.0f, 1.0f, "at %.2f")) changed = true;
                finished |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_XMARK)) remove = (int)s;
                ImGui::PopID();
            }
            if (remove >= 0) {
                stops.erase(stops.begin() + remove);
                changed = finished = true;
            }
            if (ImGui::SmallButton(ICON_FA_PLUS " Add Color")) {
                TextEffectGradientStop stop;
                stop.position = stops.empty() ? 0.0f : 1.0f;
                stops.push_back(stop);
                changed = finished = true;
            }
            if (changed) {
                cmd = new editor::PropertyCmd<std::vector<TextEffectGradientStop>>(project, sceneProject->id, entity, cpType, prefix + "gradient", stops);
                CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
            }
            if (finished && cmd) {
                cmd->setNoMerge();
                cmd = nullptr;
            }
        }
        ImGui::PopID();
    }
}

void editor::InspectorWindow::drawLayoutGroupComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    std::vector<editor::EnumEntry>& entriesLayoutGroupType = *enumEntries(reflect::enums::kLayoutGroupType);

    RowSettings settingsLayoutGroupType;
    settingsLayoutGroupType.enumEntries = &entriesLayoutGroupType;

    RowSettings settingsCellSize;
    settingsCellSize.stepSize = 1.0f;
    settingsCellSize.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsRect;
    settingsRect.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsLayout;
    settingsLayout.stepSize = 1.0f;
    settingsLayout.secondColSize = 6 * ImGui::GetFontSize();

    unsigned int numBoxes = sceneProject->scene->getComponent<LayoutGroupComponent>(entities[0]).numBoxes;
    for (Entity& entity : entities){
        numBoxes = std::min(numBoxes, sceneProject->scene->getComponent<LayoutGroupComponent>(entity).numBoxes);
    }

    beginTable(cpType, getLabelSize("Num Boxes"));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Type", sceneProject, entities, settingsLayoutGroupType);
    propertyRow(RowPropertyType::Label, cpType, "numBoxes", "Num Boxes", sceneProject, entities, settingsLayout);

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GEAR)){
        ImGui::OpenPopup("menusettings_container_boxes");
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(10 * ImGui::GetFontSize(), 0), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopup("menusettings_container_boxes")){

        if (numBoxes == 0){
            ImGui::TextDisabled("No boxes available");
        }

        for (unsigned int b = 0; b < numBoxes; b++){
            separatorText(("Box " + std::to_string(b + 1)).c_str());

            beginTable(cpType, getLabelSize("Rect Height"), "box_popup_" + std::to_string(b));

            propertyRow(RowPropertyType::Label, cpType, "boxes["+std::to_string(b)+"].layout", "Layout GameObject", sceneProject, entities, settingsLayout);
            propertyRow(RowPropertyType::Bool, cpType, "boxes["+std::to_string(b)+"].expand", "Expand", sceneProject, entities);

            propertyRow(RowPropertyType::Label, cpType, "boxes["+std::to_string(b)+"].rect.x", "Rect X", sceneProject, entities, settingsRect);
            propertyRow(RowPropertyType::Label, cpType, "boxes["+std::to_string(b)+"].rect.y", "Rect Y", sceneProject, entities, settingsRect);
            propertyRow(RowPropertyType::Label, cpType, "boxes["+std::to_string(b)+"].rect.width", "Rect Width", sceneProject, entities, settingsRect);
            propertyRow(RowPropertyType::Label, cpType, "boxes["+std::to_string(b)+"].rect.height", "Rect Height", sceneProject, entities, settingsRect);

            endTable();
        }

        ImGui::EndPopup();
    }
    endTable();

    bool showWrapSettings = false;
    for (const Entity& entity : entities){
        const LayoutGroupComponent& container = sceneProject->scene->getComponent<LayoutGroupComponent>(entity);
        if (container.type == LayoutGroupType::VERTICAL_WRAP || container.type == LayoutGroupType::HORIZONTAL_WRAP){
            showWrapSettings = true;
        }
    }

    if (showWrapSettings) {
        separatorText("Wrap settings");
        beginTable(cpType, getLabelSize("Use All Space"), "wrap_settings");
        propertyRow(RowPropertyType::Bool, cpType, "useAllWrapSpace", "Use All Space", sceneProject, entities);
        propertyRow(RowPropertyType::UInt, cpType, "wrapCellWidth", "Cell Width", sceneProject, entities, settingsCellSize);
        propertyRow(RowPropertyType::UInt, cpType, "wrapCellHeight", "Cell Height", sceneProject, entities, settingsCellSize);
        endTable();
    }

}

void editor::InspectorWindow::drawSafeAreaComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    // the edges that follow the safe area, in one row (Unity's community SafeArea "Conform X/Y",
    // per edge); an edge turned off stays at the parent's edge
    beginTable(cpType, getLabelSize("Include Extra Insets"));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Edges");
    ImGui::SetItemTooltip("The edges of the RectTransform that keep inside Screen.safeArea");
    ImGui::TableSetColumnIndex(1);
    const struct { const char* property; const char* label; } edges[] = {
        {"left", "Left"}, {"top", "Top"}, {"right", "Right"}, {"bottom", "Bottom"}};
    for (int i = 0; i < 4; i++){
        bool all = true, any = false;
        for (const Entity& entity : entities){
            PropertyData prop = Catalog::findProperty(scene, entity, cpType, edges[i].property);
            const bool on = prop.ref && *static_cast<bool*>(prop.ref);
            all = all && on;
            any = any || on;
        }
        bool value = all;
        if (i > 0) ImGui::SameLine();
        if (any && !all) ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Checkbox((std::string(edges[i].label) + "##safearea_" + edges[i].property).c_str(), &value)){
            for (const Entity& entity : entities){
                cmd = new PropertyCmd<bool>(project, sceneProject->id, entity, cpType, edges[i].property, value);
                CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
            }
            if (cmd) cmd->setNoMerge();
        }
        if (any && !all) ImGui::PopStyleColor();
    }
    RowSettings settingsExtra;
    settingsExtra.help = "Screen.setExtraInsets (a banner ad's height) shrinks this panel too";
    propertyRow(RowPropertyType::Bool, cpType, "includeExtraInsets", "Include Extra Insets", sceneProject, entities, settingsExtra);
    endTable();

    bool missingRect = false;
    for (const Entity& entity : entities){
        if (!scene->findComponent<RectTransformComponent>(entity)) missingRect = true;
    }
    if (missingRect){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Needs a RectTransform (a UI element) to drive");
    }

    // what it fits to now, in canvas units (the Game view's simulated device in the editor)
    const Rect safe = Screen::getSafeArea();
    const DeviceProfile* device = Platform::getSimulation().device;
    char line[160];
    std::snprintf(line, sizeof(line), "Safe area: x %g, y %g, %g x %g (%s%s)", std::round(safe.getX()), std::round(safe.getY()),
                  std::round(safe.getWidth()), std::round(safe.getHeight()), device ? "simulated: " : "no device simulated",
                  device ? device->name : "");
    ImGui::TextDisabled("%s", line);
    ImGui::SetItemTooltip("Screen.safeArea, canvas units. Pick a phone in the Game view's Device menu to see it with a notch.");
}

void editor::InspectorWindow::drawImageComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    separatorText("Nine-patch rect");

    if (entities.size() == 1) {
        if (UIComponent* ui = sceneProject->scene->findComponent<UIComponent>(entities[0])){
            Texture* thumbTexture = findThumbnail(ui->texture.getPath());
            if (thumbTexture) {
                drawNinePatchesPreview(sceneProject->scene->getComponent<ImageComponent>(entities[0]), &ui->texture, thumbTexture, ImVec2(THUMBNAIL_SIZE, THUMBNAIL_SIZE));
            }
        }
    }

    RowSettings settingsInt;
    settingsInt.stepSize = 1.0f;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsTexScale;
    settingsTexScale.secondColSize = 6 * ImGui::GetFontSize();
    settingsTexScale.help = "Increase or decrease texture area by a factor";

    beginTable(cpType, getLabelSize("Margin Bottom"), "nine_margin_table");
    propertyRow(RowPropertyType::UInt, cpType, "patchMarginLeft", "Margin Left", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::UInt, cpType, "patchMarginRight", "Margin Right", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::UInt, cpType, "patchMarginTop", "Margin Top", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::UInt, cpType, "patchMarginBottom", "Margin Bottom", sceneProject, entities, settingsInt);
    endTable();

    beginTable(cpType, getLabelSize("Texture Scale"));
    propertyRow(RowPropertyType::Float, cpType, "textureScaleFactor", "Texture Scale", sceneProject, entities, settingsTexScale);
    endTable();
}

void editor::InspectorWindow::drawSheetSpriteSection(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    namespace ase = pixelart::aseprite;
    const SpriteComponent& sprite = sceneProject->scene->getComponent<SpriteComponent>(entity);
    std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(sprite.sheet.path);
    const bool isAseprite = SpriteSheetPool::isAsepritePath(sprite.sheet.path);
    separatorText("Sprite Sheet");
    if (!sheet || sheet->frames.empty()) {
        const bool importing = isAseprite && ase::AsepriteImporter::get().isPending(sprite.sheet.path);
        ImGui::TextColored(Theme::Colors::WarningText, "%s", importing ? "Importing..." : "The sheet has no frames (see the Console).");
        return;
    }
    const int count = sheet->frameCount();
    drawSheetFramePicker(cpType, sceneProject, entity, *sheet);
    ImGui::TextDisabled("%d frames, %d tags, %d slices", count, (int)sheet->tags.size(), (int)sheet->slices.size());

    // a texture sheet: its Slicer (S45 R12); an atlas or a packed source: where it is drawn from
    if (SpriteSheetSources::isTexturePath(sprite.sheet.path)) {
        // and the Sprite Normal Map of its frames, side by side
        const float sheetHalf = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(ICON_FA_GRIP " Slicer Tool", ImVec2(sheetHalf, 0))) openSpriteSlicer(cpType, sceneProject, entity, sprite.sheet.path);
        UIProbe::item("inspector.sprite.slicerTool");
        ImGui::SameLine();
        spritenormalsentry::normalMapButton(sceneProject, entity, ImGui::GetContentRegionAvail().x);
        spritenormalsentry::normalMapHint(sceneProject, entity);
    }
    if (SpriteSheetSources::isPacked(sprite.sheet.path)) {
        ImGui::TextDisabled(ICON_FA_TABLE_CELLS " Drawn from a Sprite Atlas page");
    }
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (isAseprite) {
        if (ImGui::Button(ICON_FA_PEN_TO_SQUARE " Open in Aseprite", ImVec2(half, 0))) {
            ase::openInAsepriteOrFallback(project, {sprite.sheet.path}, true);
        }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(sheet->slices.empty());
    if (ImGui::Button(ICON_FA_VECTOR_SQUARE " Create Colliders from Slices", ImVec2(isAseprite ? -FLT_MIN : half, 0))) {
        std::vector<Command*> commands;
        std::vector<std::string> names;
        pixelart::PixelProblem problem;
        if (!ase::sliceColliderCommands(project, sceneProject->id, entity, commands, names, problem)) {
            Out::warning("Create Colliders from Slices: %s", problem.message.c_str());
        } else if (ase::runAsOneStep(sceneProject->id, std::move(commands))) {
            Out::info("Created %d slice collider(s) on \"%s\"", (int)names.size(), sceneProject->scene->getEntityName(entity).c_str());
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("A Rigidbody 2D collider per slice, following the slice's box on every frame (Is Trigger when its user data says sensor: true)");
    }
    const nlohmann::json& ud = sheet->userData;
    const bool hasProperties = ud.is_object() && ud.contains("properties") && ud["properties"].is_object() && !ud["properties"].empty();
    if (hasProperties) {
        if (ImGui::Button(ICON_FA_LIST_CHECK " Apply Sprite Properties", ImVec2(-FLT_MIN, 0))) {
            CommandHistory* history = CommandHandle::get(sceneProject->id);
            const size_t mark = history->mark();
            std::vector<std::string> applied;
            std::vector<pixelart::PixelProblem> problems;
            const bool ok = ase::applySpriteProperties(project, sceneProject->id, entity, *sheet, [history](Command* command) {
                command->setNoMerge();
                return history->addCommand(command);
            }, applied, problems);
            if (ok) {
                history->groupSince(mark);
            } else {
                while (history->position() > mark && history->canUndo() && history->undo()) {}
            }
            for (const pixelart::PixelProblem& p : problems) Out::warning("Apply Sprite Properties: %s: %s", p.field.c_str(), p.message.c_str());
            if (ok) Out::info("Applied %d sprite propert%s", (int)applied.size(), applied.size() == 1 ? "y" : "ies");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sets the Aseprite file's user properties named <component>.<property> (light2d.intensity...) on this GameObject");
        }
    }
}

void editor::InspectorWindow::drawSpriteComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsInt;
    settingsInt.stepSize = 1.0f;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsTexScale;
    settingsTexScale.secondColSize = 6 * ImGui::GetFontSize();
    settingsTexScale.help = "Increase or decrease texture area by a factor";

    RowSettings settingsPivot;
    settingsPivot.enumEntries = &entriesPivotPreset;

    RowSettings settingsSheet;
    settingsSheet.help = "A texture, an .aseprite file or a .spriteatlas: the Sprite shows one of its frames (an atlas packing it draws it from its page)";
    RowSettings settingsDrawMode;
    settingsDrawMode.enumEntries = &entriesSpriteDrawMode;
    settingsDrawMode.help = "Sliced: a frame with a border as 9 quads at Width x Height, corners unscaled";
    // S41: texels per world unit (0: the project's) and the rotation policy under a Pixel Perfect Camera
    RowSettings settingsPpu;
    settingsPpu.secondColSize = 6 * ImGui::GetFontSize();
    settingsPpu.help = "Texels per world unit. 0 = the Sprite Sheet's, else Project (" + std::to_string(project->getPixelsPerUnit()) + ").";
    RowSettings settingsPixelRotation;
    settingsPixelRotation.enumEntries = enumEntries(reflect::enums::kPixelRotation);
    settingsPixelRotation.help = "Under a Pixel Perfect Camera: the camera's rotation policy, 90 Degree Steps or Rotate On Grid.";

    const bool singleSheetSprite = entities.size() == 1 && !sceneProject->scene->getComponent<SpriteComponent>(entities[0]).sheet.empty();
    beginTable(cpType, getLabelSize("Pixels Per Unit"));
    propertyRow(RowPropertyType::Asset, cpType, "sheet", "Sprite", sceneProject, entities, settingsSheet);
    propertyRow(RowPropertyType::Enum, cpType, "drawMode", "Draw Mode", sceneProject, entities, settingsDrawMode);
    endTable();
    if (singleSheetSprite) drawSpriteNativeSize(cpType, sceneProject, entities[0]);
    beginTable(cpType, getLabelSize("Pixels Per Unit"), "sprite_size");
    propertyRow(RowPropertyType::UInt, cpType, "width", "Width", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::UInt, cpType, "height", "Height", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::Enum, cpType, "pivotPreset", "Pivot", sceneProject, entities, settingsPivot);
    propertyRow(RowPropertyType::Float, cpType, "textureScaleFactor", "Texture Scale", sceneProject, entities, settingsTexScale);
    propertyRowWithAutoButton(RowPropertyType::Bool, cpType, "flipY", "Flip Y", "automaticFlipY", "Automatic Flip Y", sceneProject, entities);
    propertyRow(RowPropertyType::FloatPositive, cpType, "pixelsPerUnit", "Pixels Per Unit", sceneProject, entities, settingsPpu);
    propertyRow(RowPropertyType::Enum, cpType, "pixelRotation", "Pixel Rotation", sceneProject, entities, settingsPixelRotation);
    // S52: the point Custom Axis sorting measures (Sorting Layer and Order in Layer are the Mesh
    // Renderer's rows, drawn with it)
    RowSettings settingsSortPoint;
    settingsSortPoint.enumEntries = enumEntries(reflect::enums::kSpriteSortPoint);
    settingsSortPoint.help = "What Transparency Sort Mode Custom Axis measures: the sprite's bounds' centre or its pivot.";
    propertyRow(RowPropertyType::Enum, cpType, "spriteSortPoint", "Sprite Sort Point", sceneProject, entities, settingsSortPoint);

    endTable();

    // a sheet sprite (S44): its frame by name, and what its .aseprite file brings
    if (entities.size() == 1 && !sceneProject->scene->getComponent<SpriteComponent>(entities[0]).sheet.empty()) {
        drawSheetSpriteSection(cpType, sceneProject, entities[0]);
        return;
    }

    // --- Frames Rect Section ---
    if (entities.size() == 1) {
        SpriteComponent& sprite = sceneProject->scene->getComponent<SpriteComponent>(entities[0]);
        MeshComponent* meshComp = sceneProject->scene->findComponent<MeshComponent>(entities[0]);
        Texture previewTexture;
        if (meshComp) {
            if (meshComp->numSubmeshes > 0) {
                previewTexture = meshComp->submeshes[0].material.baseColorTexture;
            }
        }

        // a legacy sprite drawn from an atlas page shows its source texture's frames (S45 R8)
        if (meshComp && meshComp->numSubmeshes > 0) previewTexture.setPath(MeshSystem::spriteSourceTexture(sprite, *meshComp));
        drawLegacyFramesHeader(cpType, sceneProject, entities[0]);

        // Frame count from numFramesRect
        int activeFrameCount = (int)sprite.numFramesRect;

        // the Slicer Tool (S45 R12: it writes the texture's Sprite Mode and makes this a sheet
        // sprite) and the Sprite Normal Map (this texture with these frames), side by side
        const float spriteFramesHalf = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const std::string slicedTexture = previewTexture.getPath();
        if (ImGui::Button(ICON_FA_GRIP " Slicer Tool", ImVec2(spriteFramesHalf, 0))) {
            openSpriteSlicer(cpType, sceneProject, entities[0], slicedTexture);
        }
        ImGui::SameLine();
        spritenormalsentry::normalMapButton(sceneProject, entities[0], ImGui::GetContentRegionAvail().x);
        spritenormalsentry::normalMapHint(sceneProject, entities[0]);

        beginTable(cpType, getLabelSize("Frames"), "sprite_frames_header");
        propertyHeader("Frames", -1, false, false);
        ImGui::Text("%d", activeFrameCount);
        float spriteFramesArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        if (drawSummaryAddButton(ICON_FA_PLUS " Add Frame##sprite_frames_add", spriteFramesArrowWidth,
                sprite.framesRect.validIndex((int)sprite.numFramesRect), "A Sprite holds at most 65,536 frames")) {
            int freeSlot = (int)sprite.numFramesRect;
            if (sprite.framesRect.validIndex(freeSlot)) {
                editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();
                std::string prefix = "framesRect[" + std::to_string(freeSlot) + "]";
                multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                    prefix + ".name", "frame_" + std::to_string(freeSlot));
                multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                    "numFramesRect", (unsigned int)(freeSlot + 1));
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##toggle_all_frames", spriteFramesExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
            spriteFramesExpanded = !spriteFramesExpanded;
        }
        endTable();

        if (spriteFramesExpanded) {
            // Draw each frame
            RowSettings settingsFrameRect;
            settingsFrameRect.stepSize = 1.0f;
            settingsFrameRect.format = "%.0f";
            settingsFrameRect.showColors = false;

            for (unsigned int i = 0; i < sprite.numFramesRect; i++) {

                ImGui::PushID(i);

                std::string frameLabel = "[" + std::to_string(i) + "] " + sprite.framesRect[i].name;
                std::string frameGroupStr = "frame_" + std::to_string(i);
                std::string prefix = "framesRect[" + std::to_string(i) + "]";

                separatorText(frameLabel.c_str());

                beginTable(cpType, getLabelSize("Frame"), frameGroupStr);

                // Frame row: Preview + Trash button
                propertyHeader("Frame", -1, false, false);

                {
                    float previewSize = ImGui::GetFrameHeight() * 2.2f;
                    float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
                    float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
                    ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
                    ImVec2 arrowButtonSize = ImGui::CalcItemSize(ImVec2(0, 0), ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
                    ImVec2 applyButtonSize = arrowButtonSize;
                    ImVec2 resizeButtonSize = arrowButtonSize;

                    bool hasFramePreview = !previewTexture.empty() && sprite.framesRect[i].rect.getWidth() > 0.0f && sprite.framesRect[i].rect.getHeight() > 0.0f;
                    bool hasValidMeshTextureRect = meshComp && meshComp->numSubmeshes > 0;
                    bool canApplyFrame = hasValidMeshTextureRect && !previewTexture.empty() && previewTexture.getWidth() > 0 && previewTexture.getHeight() > 0;
                    bool canResizeToFrame = sprite.framesRect[i].rect.getWidth() > 0.0f && sprite.framesRect[i].rect.getHeight() > 0.0f;
                    unsigned int frameWidth = canResizeToFrame ? static_cast<unsigned int>(sprite.framesRect[i].rect.getWidth()) : 0;
                    unsigned int frameHeight = canResizeToFrame ? static_cast<unsigned int>(sprite.framesRect[i].rect.getHeight()) : 0;
                    Rect normalizedFrameRect = sprite.framesRect[i].rect;
                    if (canApplyFrame) {
                        normalizedFrameRect = Rect(
                            sprite.framesRect[i].rect.getX() / (float)previewTexture.getWidth(),
                            sprite.framesRect[i].rect.getY() / (float)previewTexture.getHeight(),
                            sprite.framesRect[i].rect.getWidth() / (float)previewTexture.getWidth(),
                            sprite.framesRect[i].rect.getHeight() / (float)previewTexture.getHeight()
                        );
                    }
                    bool isAppliedFrame = false;
                    if (hasValidMeshTextureRect) {
                        const Rect& appliedRect = meshComp->submeshes[0].textureRect;
                        isAppliedFrame = appliedRect.getX() == normalizedFrameRect.getX()
                                && appliedRect.getY() == normalizedFrameRect.getY()
                                && appliedRect.getWidth() == normalizedFrameRect.getWidth()
                                && appliedRect.getHeight() == normalizedFrameRect.getHeight();
                    }
                        bool isSizedToFrame = canResizeToFrame && sprite.width == frameWidth && sprite.height == frameHeight;

                    float trailingWidth = arrowButtonSize.x
                            + ImGui::GetStyle().ItemSpacing.x + applyButtonSize.x
                            + ImGui::GetStyle().ItemSpacing.x + resizeButtonSize.x
                            + ImGui::GetStyle().ItemSpacing.x + deleteButtonSize.x;
                    if (hasFramePreview) {
                        trailingWidth += previewSize + ImGui::GetStyle().ItemSpacing.x;
                    }

                    float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
                    ImGui::SetCursorPosX(targetX);

                    if (hasFramePreview) {
                        drawSpriteFramePreview(&previewTexture, sprite.framesRect[i].rect, ImVec2(previewSize, previewSize), "##frame_preview");
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("%s", frameLabel.c_str());

                            float tooltipMaxSize = Theme::dpi(200.0f);
                            float scale = std::min(tooltipMaxSize / std::max(1.0f, sprite.framesRect[i].rect.getWidth()),
                                                   tooltipMaxSize / std::max(1.0f, sprite.framesRect[i].rect.getHeight()));
                            scale = std::min(scale, 1.0f);
                            ImVec2 tooltipSize(std::max(Theme::dpi(32.0f), sprite.framesRect[i].rect.getWidth() * scale),
                                               std::max(Theme::dpi(32.0f), sprite.framesRect[i].rect.getHeight() * scale));

                            drawSpriteFramePreview(&previewTexture, sprite.framesRect[i].rect, tooltipSize, "##frame_preview_tooltip");
                            ImGui::EndTooltip();
                        }

                        ImGui::SameLine();
                    }

                    if (ImGui::ArrowButton("##toggle_frame", spriteFramesButtonGroups[frameGroupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
                        spriteFramesButtonGroups[frameGroupStr] = !spriteFramesButtonGroups[frameGroupStr];
                    }
                    ImGui::SameLine();

                    bool deleted = false;
                    ImGui::BeginDisabled(!canApplyFrame || isAppliedFrame);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(ICON_FA_CHECK "##apply_frame", applyButtonSize)) {
                        editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();
                        multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], ComponentType::MeshComponent,
                            "submeshes[0].textureRect", Vector4(normalizedFrameRect.getX(), normalizedFrameRect.getY(), normalizedFrameRect.getWidth(), normalizedFrameRect.getHeight()));
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    if (ImGui::IsItemHovered()) {
                        if (!meshComp) {
                            ImGui::SetTooltip("Sprite GameObject has no Mesh Renderer");
                        } else if (meshComp->numSubmeshes == 0) {
                            ImGui::SetTooltip("Sprite mesh has no submeshes");
                        } else if (!canApplyFrame) {
                            ImGui::SetTooltip("Sprite needs a valid texture to apply a frame");
                        } else {
                            ImGui::SetTooltip("Apply frame to sprite");
                        }
                    }
                    ImGui::PopStyleVar();
                    ImGui::EndDisabled();

                    ImGui::SameLine();

                    ImGui::BeginDisabled(!canResizeToFrame || isSizedToFrame);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(ICON_FA_EXPAND "##resize_to_frame", resizeButtonSize)) {
                        editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();
                        multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                            "width", frameWidth);
                        multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                            "height", frameHeight);
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    if (ImGui::IsItemHovered()) {
                        if (!canResizeToFrame) {
                            ImGui::SetTooltip("Frame needs a valid size to resize the sprite");
                        } else if (isSizedToFrame) {
                            ImGui::SetTooltip("Sprite already matches this frame size");
                        } else {
                            ImGui::SetTooltip("Resize sprite to this frame size");
                        }
                    }
                    ImGui::PopStyleVar();
                    ImGui::EndDisabled();

                    ImGui::SameLine();

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(ICON_FA_TRASH_CAN"##delete_frame")) {
                        editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();

                        // Shift frames after deleted one
                        for (unsigned int j = i; j + 1 < sprite.numFramesRect; j++) {
                            std::string srcPrefix = "framesRect[" + std::to_string(j + 1) + "]";
                            std::string dstPrefix = "framesRect[" + std::to_string(j) + "]";
                            multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                                dstPrefix + ".name", sprite.framesRect[j + 1].name);
                            multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], cpType,
                                dstPrefix + ".rect", Vector4(sprite.framesRect[j + 1].rect.getX(), sprite.framesRect[j + 1].rect.getY(),
                                    sprite.framesRect[j + 1].rect.getWidth(), sprite.framesRect[j + 1].rect.getHeight()));
                        }

                        // Clear last slot
                        unsigned int lastIdx = sprite.numFramesRect - 1;
                        std::string lastPrefix = "framesRect[" + std::to_string(lastIdx) + "]";
                        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType, lastPrefix + ".name", std::string(""));
                        multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], cpType, lastPrefix + ".rect", Vector4(0, 0, 0, 0));

                        // Decrement count
                        multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                            "numFramesRect", sprite.numFramesRect - 1);

                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                        deleted = true;
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);

                    // Name and Rect rows
                    if (!deleted && spriteFramesButtonGroups[frameGroupStr]) {
                        propertyRow(RowPropertyType::String, cpType, prefix + ".name", "Name", sceneProject, entities);
                        propertyRow(RowPropertyType::Vector4, cpType, prefix + ".rect", "Rect", sceneProject, entities, settingsFrameRect);
                    }
                }

                endTable();

                ImGui::PopID();
            }
        }
    } else {
        separatorText("Sprite Frames");
        ImGui::TextDisabled("Select a single GameObject to edit sprite frames");
    }
}

void editor::InspectorWindow::drawCameraComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    CameraComponent& camera = scene->getComponent<CameraComponent>(entities[0]);

    float firstColSize = std::max({getLabelSize("Auto Resize"), getLabelSize("Culling Mask"), getLabelSize("Transparency Mode")});

    beginTable(cpType, firstColSize);

    RowSettings enumSettings;
    enumSettings.enumEntries = &entriesCameraType;
    propertyRow(RowPropertyType::Enum, cpType, "type", "Type", sceneProject, entities, enumSettings);

    RowSettings defaultSettings;

    propertyRow(RowPropertyType::LayerMask, cpType, "cullingMask", "Culling Mask", sceneProject, entities, defaultSettings);

    propertyRow(RowPropertyType::Bool, cpType, "useTarget", "Use Target", sceneProject, entities, defaultSettings);

    if (camera.useTarget){
        propertyRow(RowPropertyType::Vector3, cpType, "target", "Target", sceneProject, entities, defaultSettings);
    }
    propertyRow(RowPropertyType::Direction, cpType, "up", "Up", sceneProject, entities, defaultSettings);

    if (camera.type == CameraType::CAMERA_PERSPECTIVE) {
        RowSettings fovSettings;
        fovSettings.format = "%.3f";
        fovSettings.stepSize = 0.01f;
        propertyRow(RowPropertyType::Float, cpType, "yfov", "Y FOV", sceneProject, entities, fovSettings);

        propertyRow(RowPropertyType::Float, cpType, "aspect", "Aspect", sceneProject, entities, defaultSettings);
    }

    if (camera.type == CameraType::CAMERA_ORTHO || camera.type == CameraType::CAMERA_UI) {
        propertyRow(RowPropertyType::Float, cpType, "left", "Left", sceneProject, entities, defaultSettings);
        propertyRow(RowPropertyType::Float, cpType, "right", "Right", sceneProject, entities, defaultSettings);
        propertyRow(RowPropertyType::Float, cpType, "bottom", "Bottom", sceneProject, entities, defaultSettings);
        propertyRow(RowPropertyType::Float, cpType, "top", "Top", sceneProject, entities, defaultSettings);
    }

    propertyRow(RowPropertyType::Float, cpType, "near", "Near", sceneProject, entities, defaultSettings);
    propertyRow(RowPropertyType::Float, cpType, "far", "Far", sceneProject, entities, defaultSettings);

    propertyRow(RowPropertyType::Bool, cpType, "depthTest", "Depth Test", sceneProject, entities, defaultSettings);
    propertyRow(RowPropertyType::Bool, cpType, "autoResize", "Auto Resize", sceneProject, entities, defaultSettings);

    // S57 R4: weighted blended OIT for the alpha-blended materials
    RowSettings transparencySettings;
    transparencySettings.enumEntries = &entriesTransparencyMode;
    transparencySettings.help = "Sorted draws blended renderers back to front, one at a time. Order Independent draws alpha-blended "
                                "materials through weighted blended OIT: one extra pass into RGBA16F+R16F targets and a fullscreen "
                                "resolve; Sorted on devices without half-float targets. Additive and multiply blending stay sorted.";
    propertyRow(RowPropertyType::Enum, cpType, "transparencyMode", "Transparency Mode", sceneProject, entities, transparencySettings);
    if (camera.transparencyMode == TransparencyMode::ORDER_INDEPENDENT){
        RowSettings weightSettings;
        weightSettings.indent = true;
        weightSettings.stepSize = 0.01f;
        weightSettings.format = "%.4f";
        weightSettings.help = "Multiplies the view depths the OIT weight takes: lower it for large scenes, so far layers keep their weight.";
        propertyRow(RowPropertyType::FloatPositive, cpType, "oitWeightScale", "OIT Weight Scale", sceneProject, entities, weightSettings);
    }

    if (entities.size() == 1 && sceneProject->sceneRender && sceneWindow) {
        Entity cameraEntity = entities[0];
        bool previewingThisCamera = sceneProject->sceneRender->isPreviewCameraActive()
            && sceneProject->sceneRender->getPreviewCameraEntity() == cameraEntity;
        bool canPreviewCamera = sceneProject->playState == ScenePlayState::STOPPED
            && scene->findComponent<Transform>(cameraEntity);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Preview");
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginDisabled(!canPreviewCamera);
        if (previewingThisCamera) {
            if (ImGui::Button(ICON_FA_XMARK" Exit")) {
                sceneWindow->stopViewingCamera(sceneProject->id);
            }
        } else {
            if (ImGui::Button(ICON_FA_VIDEO" View")) {
                sceneWindow->viewThroughCamera(sceneProject->id, cameraEntity);
            }
        }
        ImGui::EndDisabled();
        if (!canPreviewCamera && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(sceneProject->playState == ScenePlayState::STOPPED ? "Camera transform unavailable" : "Stop the scene to preview cameras");
        }
    }

    // the scene's post-processing (scene settings) reaches the main camera only
    if (entities.size() == 1 && sceneProject->sceneType != SceneType::SCENE_UI) {
        const bool mainCamera = sceneProject->mainCamera == entities[0];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Post");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", mainCamera ? "Main camera: post-processed" : "Not post-processed");
        ImGui::SetItemTooltip("%s", mainCamera
            ? "The scene's Post-processing settings (Inspector with nothing selected) apply to this camera's image."
            : "Only the scene's main camera is post-processed (Scene settings > Post-processing); other cameras, "
              "render-to-texture ones included, draw without it.");
    }

    endTable();

    // framebuffer settings only matter when the camera renders into a texture
    // (bind a camera to a texture by dragging it onto a texture slot)
    if (camera.renderToTexture){
        RowSettings sizeSettings;
        sizeSettings.stepSize = 1.0f;

        RowSettings filterSettings;
        filterSettings.enumEntries = &entriesTextureFilter;

        separatorText("Render Target");
        beginTable(cpType, getLabelSize("Height"), "camera_framebuffer");
        propertyRow(RowPropertyType::UInt, cpType, "framebufferWidth", "Width", sceneProject, entities, sizeSettings);
        propertyRow(RowPropertyType::UInt, cpType, "framebufferHeight", "Height", sceneProject, entities, sizeSettings);
        propertyRow(RowPropertyType::Enum, cpType, "framebufferFilter", "Filter", sceneProject, entities, filterSettings);
        endTable();
    }
}

void editor::InspectorWindow::drawAudioSnapshotComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    // a Mixer Snapshot action of an Animation (S40 R18, R22): the snapshot picked from the mixer
    Scene* scene = sceneProject->scene;
    beginTable(cpType, getLabelSize("Revert Transition"));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Snapshot");
    ImGui::SetItemTooltip("The mixer's snapshot the action moves to when it starts");
    ImGui::TableSetColumnIndex(1);
    std::string current;
    bool mixed = false;
    for (size_t i = 0; i < entities.size(); i++){
        const AudioSnapshotComponent* snapshot = scene->findComponent<AudioSnapshotComponent>(entities[i]);
        const std::string name = snapshot ? snapshot->snapshot : std::string();
        if (i == 0) current = name;
        else if (name != current) mixed = true;
    }
    const std::vector<std::string> snapshots = AudioMixer::getSnapshotNames();
    const bool known = current.empty() || std::find(snapshots.begin(), snapshots.end(), current) != snapshots.end();
    const std::string preview = mixed ? std::string("-") : current.empty() ? std::string("(none)") : known ? current : current + " (missing)";
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##audiosnapshot_snapshot", preview.c_str())){
        std::vector<std::string> choices = snapshots;
        choices.insert(choices.begin(), std::string());
        for (const std::string& name : choices){
            if (ImGui::Selectable(name.empty() ? "(none)" : name.c_str(), !mixed && name == current)){
                for (const Entity& entity : entities){
                    cmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "snapshot", name);
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                }
                if (cmd) cmd->setNoMerge();
            }
        }
        ImGui::EndCombo();
    }
    RowSettings seconds;
    seconds.help = "The transition time, in seconds";
    propertyRow(RowPropertyType::FloatPositive, cpType, "seconds", "Transition", sceneProject, entities, seconds);
    propertyRow(RowPropertyType::Bool, cpType, "unscaled", "Unscaled Time", sceneProject, entities);
    RowSettings revert;
    revert.help = "On stop, back to the snapshot shown before it started, if the mixer still shows this one";
    propertyRow(RowPropertyType::Bool, cpType, "revert", "Revert On Stop", sceneProject, entities, revert);
    RowSettings revertSeconds;
    revertSeconds.help = "-1: the same as Transition";
    propertyRow(RowPropertyType::Float, cpType, "revertSeconds", "Revert Transition", sceneProject, entities, revertSeconds);
    endTable();
    if (!known && !mixed){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " The mixer has no snapshot '%s'", current.c_str());
    }
    if (!scene->findComponent<ActionComponent>(entities.front())){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Plays as an Action: add an Action and put it on an Animation's track");
    }
}

void editor::InspectorWindow::drawAudioComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    // an Animation's audio action (S40 R22)
    if (sceneProject->scene->findComponent<ActionComponent>(entities.front())){
        ImGui::TextColored(Theme::Colors::SubtleText, ICON_FA_FILM " Timeline audio: plays when its action starts");
    }
    RowSettings compactSettings;
    compactSettings.secondColSize = 7 * ImGui::GetFontSize();

    RowSettings floatSettings = compactSettings;
    floatSettings.format = "%.3f";
    floatSettings.stepSize = 0.01f;

    RowSettings doubleSettings = floatSettings;

    beginTable(cpType, getLabelSize("Audio Clip"));

    if (!sceneProject->scene->getComponent<AudioSourceComponent>(entities.front()).event.empty()) {
        // the event plays: its clip is ignored (S37 R9)
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Audio Clip");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("(played by the event)");
    } else if (entities.size() == 1) {
        Entity entity = entities[0];
        AudioSourceComponent& audio = sceneProject->scene->getComponent<AudioSourceComponent>(entity);

        auto setAudioFilename = [&](const std::filesystem::path& selectedPath) -> bool {
            std::filesystem::path assetsPath = std::filesystem::absolute(project->getAssetsPath()).lexically_normal();
            std::filesystem::path filePath = project->resolveAssetPath(selectedPath);
            filePath = std::filesystem::absolute(filePath).lexically_normal();

            std::error_code errorCode;
            std::filesystem::path relative = std::filesystem::relative(filePath, assetsPath, errorCode);
            std::string relativePath = relative.generic_string();
            if (errorCode || relative.empty() || *relative.begin() == "..") {
                ImGui::OpenPopup("Audio Import Error");
                return false;
            }

            if (!Util::isAudioFile(relativePath)) {
                return false;
            }

            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "filename", relativePath)
            );
            return true;
        };

        std::string currentPath = audio.filename;

        prefabRowProperty = "filename";
        if (propertyHeader("Audio Clip", -1, !currentPath.empty())) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "filename", "")
            );
            currentPath = "";
        }

        std::string displayName = currentPath.empty() ? "< Not set >" : std::filesystem::path(currentPath).filename().string();
        float availWidth = ImGui::GetContentRegionAvail().x;
        float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2;

        ImGui::BeginGroup();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);
        ImGui::BeginChild("audiofilename", ImVec2(availWidth - buttonWidth - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFrameHeight()),
            false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        float textWidth = ImGui::CalcTextSize(displayName.c_str()).x;
        float childAvail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(std::max(0.0f, childAvail - textWidth - 2));
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        if (currentPath.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Text("%s", displayName.c_str());
        if (currentPath.empty())
            ImGui::PopStyleColor();
        ImGui::EndChild();
        if (!currentPath.empty()){
            ImGui::SetItemTooltip("%s", currentPath.c_str());
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##audio_load")) {
            std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_AUDIO);
            if (!path.empty()) {
                setAudioFilename(path);
            }
        }

        ImGui::EndGroup();

        if (sceneProject && sceneProject->playState == ScenePlayState::STOPPED) {
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                    std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
                    if (!receivedStrings.empty()) {
                        std::filesystem::path droppedPath(receivedStrings[0]);
                        std::filesystem::path filePath = project->resolveAssetPath(droppedPath);
                        std::error_code errorCode;
                        std::filesystem::path relative = std::filesystem::relative(std::filesystem::absolute(filePath), std::filesystem::absolute(project->getAssetsPath()), errorCode);
                        if (!errorCode && Util::isAudioFile(relative.generic_string()) && payload->IsDelivery()) {
                            if (setAudioFilename(droppedPath)) {
                                ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                            }
                        }
                    }
                }
                // a built-in clip from the Sound Browser: imported first
                if (const ImGuiPayload* builtin = ImGui::AcceptDragDropPayload(SoundBrowserWindow::BUILTIN_PAYLOAD)) {
                    std::vector<std::string> sounds;
                    std::string dropError;
                    if (SoundDrop::resolve(project, builtin, sounds, dropError) && !sounds.empty()) setAudioFilename(sounds.front());
                    if (!dropError.empty()) Out::warning("Sound drop: %s", dropError.c_str());
                }
                ImGui::EndDragDropTarget();
            }
        }

        if (EditorModal::begin("Audio Import Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Selected file must be an audio file within the project directory.");
            EditorModal::message("Selected file must be an audio file within the project directory.");
            ImGui::Separator();
            float buttonModalWidth = 120;
            float windowWidth = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((windowWidth - buttonModalWidth) * 0.5f);
            if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }else{
        propertyRow(RowPropertyType::Asset, cpType, "filename", "Audio Clip", sceneProject, entities);
    }

    RowSettings stateSettings = compactSettings;
    stateSettings.enumEntries = &entriesAudioState;
    propertyRow(RowPropertyType::Enum, cpType, "state", "State", sceneProject, entities, stateSettings);

    endTable();

    drawAudioEventRows(cpType, sceneProject, entities);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        AudioSourceComponent& audio = sceneProject->scene->getComponent<AudioSourceComponent>(entity);
        bool sceneIsStopped = sceneProject->playState == ScenePlayState::STOPPED;
        SoundPreviewRuntime& ap = soundPreviewRuntime;
        bool isThisPreview = ap.active && ap.sceneId == sceneProject->id && ap.entity == entity;
        // a capture's mix is the Play's alone
        const bool capturing = AudioCapture::isActive();

        if (ap.active && (!isThisPreview || !sceneIsStopped || capturing || ap.filename != audio.filename)) {
            stopSoundPreview(!isThisPreview || ap.filename != audio.filename);
            isThisPreview = false;
        }

        if (isThisPreview) {
            updateSoundPreview(audio);
            isThisPreview = ap.active && ap.sceneId == sceneProject->id && ap.entity == entity;
        }

        ImGui::Separator();
        ImGui::BeginDisabled(!sceneIsStopped || audio.filename.empty() || capturing);

        if (isThisPreview && ap.playing) {
            if (ImGui::Button(ICON_FA_PAUSE "##audio_pause")) {
                pauseSoundPreview();
            }
        } else {
            if (ImGui::Button(ICON_FA_PLAY "##audio_play")) {
                if (!startSoundPreview(sceneProject, entity, audio)) {
                    ImGui::OpenPopup("Audio Preview Error");
                } else {
                    isThisPreview = true;
                }
            }
            if (capturing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Audio preview is off while Play captures audio");
            }
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(!isThisPreview);
        if (ImGui::Button(ICON_FA_STOP "##audio_stop")) {
            stopSoundPreview();
            isThisPreview = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        double previewTime = isThisPreview ? ap.time : 0.0;
        double previewLength = (ap.loaded && ap.sceneId == sceneProject->id && ap.entity == entity && ap.filename == audio.filename) ? ap.length : audio.length;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%.2fs", previewTime);
        ImGui::SameLine();

        float timelineWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        float frameHeight = ImGui::GetFrameHeight();
        float timelineHeight = 6.0f;
        float rounding = timelineHeight * 0.5f;
        ImVec2 timelinePos = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton("##audio_timeline_strip", ImVec2(timelineWidth, frameHeight));
        bool timelineHovered = ImGui::IsItemHovered();
        bool timelineClicked = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);

        if (sceneIsStopped && previewLength > 0.0 && timelineClicked) {
            float mouseX = ImGui::GetIO().MousePos.x;
            float clickFraction = std::clamp((mouseX - timelinePos.x) / timelineWidth, 0.0f, 1.0f);
            double seekTime = clickFraction * previewLength;
            if (!seekSoundPreview(sceneProject, entity, audio, seekTime)) {
                ImGui::OpenPopup("Audio Preview Error");
            } else {
                isThisPreview = true;
            }
        }

        if (timelineHovered && previewLength > 0.0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4 trackColor = style.Colors[ImGuiCol_TextDisabled];
        trackColor.w = 0.22f;
        ImVec4 fillColor = style.Colors[ImGuiCol_PlotHistogram];
        float fraction = (previewLength > 0.0) ? std::clamp(static_cast<float>(previewTime / previewLength), 0.0f, 1.0f) : 0.0f;

        ImVec2 barMin(timelinePos.x, timelinePos.y + (frameHeight - timelineHeight) * 0.5f);
        ImVec2 barMax(timelinePos.x + timelineWidth, barMin.y + timelineHeight);
        drawList->AddRectFilled(barMin, barMax, ImGui::GetColorU32(trackColor), rounding);

        if (fraction > 0.0f) {
            ImVec2 fillMax(barMin.x + timelineWidth * fraction, barMax.y);
            drawList->AddRectFilled(barMin, fillMax, ImGui::GetColorU32(fillColor), rounding);
        }

        ImGui::EndDisabled();

        if (EditorModal::begin("Audio Preview Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", ap.error.c_str());
            EditorModal::message(ap.error);
            ImGui::Separator();
            float buttonModalWidth = 120;
            float windowWidth = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((windowWidth - buttonModalWidth) * 0.5f);
            if (EditorModal::button("OK", "ok", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    separatorText("Playback");

    beginTable(cpType, getLabelSize("Ignore Listener Pause"), "audio_playback_settings");

    // Output: the mixer group (Unity's outputAudioMixerGroup), by name; "" is Master
    {
        const std::vector<std::string> groups = AudioMixer::getGroupNames();
        auto shownGroup = [](const AudioSourceComponent& audio) {
            if (!audio.outputGroup.empty()) {
                const std::string name = AudioMixer::getGroupName(audio.outputGroup);
                if (!name.empty()) return name;
            }
            return audio.outputGroupName;
        };
        const std::string current = shownGroup(sceneProject->scene->getComponent<AudioSourceComponent>(entities[0]));
        bool different = false;
        for (Entity entity : entities) {
            different = different || shownGroup(sceneProject->scene->getComponent<AudioSourceComponent>(entity)) != current;
        }
        const bool missing = !current.empty() && !AudioMixer::hasGroup(current);
        const std::string masterName = groups.empty() ? std::string("Master") : groups[0];

        prefabRowProperty = "outputGroup";
        bool reset = propertyHeader("Output", compactSettings.secondColSize * 1.6f, !current.empty() && !different);
        std::string chosen;
        bool choose = false;
        const std::string preview = different ? std::string("-") : current.empty() ? masterName :
                                    missing ? current + " (not in the mixer)" : current;
        if (ImGui::BeginCombo("##audio_output_group", preview.c_str())) {
            for (size_t i = 0; i < groups.size(); i++) {
                // Master is the empty name: a source keeps playing through it whatever it is called
                const std::string value = i == 0 ? "" : groups[i];
                const bool selected = !different && value == current;
                if (ImGui::Selectable(groups[i].c_str(), selected)) {
                    chosen = value;
                    choose = true;
                }
            }
            ImGui::Separator();
            if (ImGui::Selectable(ICON_FA_SLIDERS " Open Audio Mixer...")) {
                editor::getEditorHost().openAudioMixerWindow();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", AudioMixer::hasAsset() ? ("The mixer group it plays through (" + AudioMixer::getPath() + ")").c_str()
                                                           : "The mixer group it plays through. The project has no mixer yet: Window > Audio > Audio Mixer.");
        if (reset) {
            chosen = "";
            choose = true;
        }
        if (choose && (different || chosen != current)) {
            auto* multi = new MultiPropertyCmd();
            for (Entity entity : entities) {
                multi->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "outputGroup", chosen);
            }
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(multi);
        }
    }

    propertyRow(RowPropertyType::DoublePositive, cpType, "volume", "Volume", sceneProject, entities, doubleSettings);
    RowSettings pitchSettings = floatSettings;
    pitchSettings.help = "Playback speed: 2 is an octave up and twice as fast (Unity's Pitch; Lua pitch or speed)";
    propertyRow(RowPropertyType::FloatPositive, cpType, "speed", "Pitch", sceneProject, entities, pitchSettings);

    RowSettings panSettings = floatSettings;
    panSettings.help = "-1 left, 0 center, 1 right";
    propertyRow(RowPropertyType::Float, cpType, "pan", "Pan", sceneProject, entities, panSettings);

    propertyRow(RowPropertyType::Bool, cpType, "looping", "Looping", sceneProject, entities, compactSettings);
    propertyRow(RowPropertyType::DoublePositive, cpType, "loopingPoint", "Loop Point", sceneProject, entities, doubleSettings);
    propertyRow(RowPropertyType::Bool, cpType, "protectVoice", "Protect Voice", sceneProject, entities, compactSettings);
    propertyRow(RowPropertyType::Bool, cpType, "enableClocked", "Clocked", sceneProject, entities, compactSettings);
    propertyRow(RowPropertyType::Bool, cpType, "inaudibleBehaviorMustTick", "Inaudible Must Tick", sceneProject, entities, compactSettings);
    propertyRow(RowPropertyType::Bool, cpType, "inaudibleBehaviorKill", "Inaudible Kill", sceneProject, entities, compactSettings);

    RowSettings prioritySettings = compactSettings;
    prioritySettings.stepSize = 1.0f;
    prioritySettings.intMin = 0;
    prioritySettings.intMax = 256;
    prioritySettings.help = "0 is the most important, 256 the least (Unity). Past the voice limit (Project Settings > Audio) the least important voice is stopped.";
    propertyRow(RowPropertyType::Int, cpType, "priority", "Priority", sceneProject, entities, prioritySettings);

    RowSettings voicesSettings = compactSettings;
    voicesSettings.stepSize = 1.0f;
    voicesSettings.intMin = 1;
    voicesSettings.intMax = 64;
    voicesSettings.help = "Voices of this source at once (the clip and its PlayOneShot voices); past it the oldest one-shot is stopped.";
    propertyRow(RowPropertyType::Int, cpType, "maxVoices", "Max Voices", sceneProject, entities, voicesSettings);

    RowSettings listenerSettings = compactSettings;
    listenerSettings.help = "Keeps playing while AudioListener.pause is set (the sounds of a pause menu)";
    propertyRow(RowPropertyType::Bool, cpType, "ignoreListenerPause", "Ignore Listener Pause", sceneProject, entities, listenerSettings);

    RowSettings loadTypeSettings;
    loadTypeSettings.enumEntries = &entriesAudioLoadType;
    loadTypeSettings.help = "Auto streams files of 1 MiB or more (music) and decodes smaller ones (effects, no latency)";
    propertyRow(RowPropertyType::Enum, cpType, "loadType", "Load Type", sceneProject, entities, loadTypeSettings);
    if (entities.size() == 1) {
        const AudioSourceComponent& audio = sceneProject->scene->getComponent<AudioSourceComponent>(entities[0]);
        if (!audio.filename.empty()) {
            const uint64_t bytes = SoundPool::fileSize(audio.filename);
            const AudioLoadType resolved = SoundPool::resolveLoadType(audio.filename, audio.loadType);
            const char* names[] = {"Auto", "Decompress On Load", "Compressed In Memory", "Streaming"};
            char text[160];
            snprintf(text, sizeof(text), "%s%s, file %.2f MB", audio.loadType == AudioLoadType::Auto ? "Auto: " : "",
                     names[(int)resolved], bytes / (1024.0 * 1024.0));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", text);
        }
    }

    bool show3DSettings = false;
    for (Entity entity : entities){
        Signature signature = sceneProject->scene->getSignature(entity);
        show3DSettings = show3DSettings || signature.test(sceneProject->scene->getComponentId<Transform>());
    }

    endTable();

    if (show3DSettings){
        separatorText("3D Audio Source Settings");

        beginTable(cpType, getLabelSize("Max Distance"), "audio_3d_settings");

        RowSettings attenuationSettings;
        attenuationSettings.enumEntries = &entriesAudioAttenuation;
        propertyRow(RowPropertyType::FloatPositive, cpType, "minDistance", "Min Distance", sceneProject, entities, floatSettings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxDistance", "Max Distance", sceneProject, entities, floatSettings);
        propertyRow(RowPropertyType::Enum, cpType, "attenuationModel", "Attenuation", sceneProject, entities, attenuationSettings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "attenuationRolloffFactor", "Rolloff", sceneProject, entities, floatSettings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "dopplerFactor", "Doppler", sceneProject, entities, floatSettings);

        endTable();
    }

}

void editor::InspectorWindow::drawTilemapComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsInt;
    settingsInt.stepSize = 1.0f;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsTexScale;
    settingsTexScale.secondColSize = 6 * ImGui::GetFontSize();
    settingsTexScale.help = "Increase or decrease texture area by a factor";

    RowSettings settingsPpu;
    settingsPpu.secondColSize = 6 * ImGui::GetFontSize();
    settingsPpu.help = "Texels per world unit. 0 = Project (" + std::to_string(project->getPixelsPerUnit()) + ").";

    beginTable(cpType, getLabelSize("Pixels Per Unit"));
    propertyRow(RowPropertyType::UInt, cpType, "width", "Width", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::UInt, cpType, "height", "Height", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::Float, cpType, "textureScaleFactor", "Texture Scale", sceneProject, entities, settingsTexScale);
    propertyRowWithAutoButton(RowPropertyType::Bool, cpType, "flipY", "Flip Y", "automaticFlipY", "Automatic Flip Y", sceneProject, entities);
    propertyRow(RowPropertyType::FloatPositive, cpType, "pixelsPerUnit", "Pixels Per Unit", sceneProject, entities, settingsPpu);
    propertyRow(RowPropertyType::UInt, cpType, "reserveTiles", "Reserve Tiles", sceneProject, entities, settingsInt);
    endTable();

    if (entities.size() == 1) {
        TilemapComponent& tilemap = sceneProject->scene->getComponent<TilemapComponent>(entities[0]);
        MeshComponent* meshComp = sceneProject->scene->findComponent<MeshComponent>(entities[0]);

        // Build submesh list for tileset slicer
        std::vector<TextureSlicerToolDialog::SubmeshInfo> submeshInfos;
        if (meshComp) {
            for (unsigned int s = 0; s < meshComp->numSubmeshes; s++) {
                TextureSlicerToolDialog::SubmeshInfo info;
                info.name = "Submesh " + std::to_string(s);
                info.texture = meshComp->submeshes[s].material.baseColorTexture;
                submeshInfos.push_back(info);
            }
        }

        // --- Tile Rects Section ---
        separatorText("Tile Rects");

        int activeTileRectCount = (int)tilemap.numTilesRect;

        // Slicer tool button
        if (ImGui::Button(ICON_FA_TABLE_CELLS " Slicer Tool", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            textureSlicerToolDialog.openTileset(
                submeshInfos,
                static_cast<int>(tilemap.width),
                static_cast<int>(tilemap.height),
                [this, sceneProject, entities, cpType](const TextureSlicerToolDialog::SliceResult& result) {
                    editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();

                    TilemapComponent& tilemap = sceneProject->scene->getComponent<TilemapComponent>(entities[0]);

                    // Collect existing rects from other submeshes (keep them)
                    struct KeptRect {
                        std::string name;
                        int submeshId;
                        Vector4 rect;
                    };
                    std::vector<KeptRect> kept;
                    for (unsigned int i = 0; i < tilemap.numTilesRect; i++) {
                        if (tilemap.tilesRect[i].submeshId != result.submeshId) {
                            KeptRect kr;
                            kr.name = tilemap.tilesRect[i].name;
                            kr.submeshId = tilemap.tilesRect[i].submeshId;
                            kr.rect = Vector4(tilemap.tilesRect[i].rect.getX(), tilemap.tilesRect[i].rect.getY(),
                                              tilemap.tilesRect[i].rect.getWidth(), tilemap.tilesRect[i].rect.getHeight());
                            kept.push_back(kr);
                        }
                    }

                    unsigned int newTotal = (unsigned int)(kept.size() + result.rects.size());
                    unsigned int writeIndex = 0;

                    // Write kept rects (from other submeshes)
                    for (size_t i = 0; i < kept.size(); i++, writeIndex++) {
                        std::string prefix = "tilesRect[" + std::to_string(writeIndex) + "]";
                        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".name", kept[i].name);
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".submeshId", kept[i].submeshId);
                        multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".rect", kept[i].rect);
                    }

                    // Write new rects from slicer result
                    for (size_t i = 0; i < result.rects.size(); i++, writeIndex++) {
                        std::string prefix = "tilesRect[" + std::to_string(writeIndex) + "]";
                        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".name", result.rects[i].name);
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".submeshId", result.submeshId);
                        multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".rect", Vector4(result.rects[i].rect.getX(), result.rects[i].rect.getY(),
                                result.rects[i].rect.getWidth(), result.rects[i].rect.getHeight()));
                    }

                    // Clear leftover slots beyond new total
                    for (unsigned int i = newTotal; i < tilemap.numTilesRect; i++) {
                        std::string prefix = "tilesRect[" + std::to_string(i) + "]";
                        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".name", std::string(""));
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".submeshId", -1);
                        multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], cpType,
                            prefix + ".rect", Vector4(0, 0, 0, 0));
                    }

                    // Update numTilesRect
                    multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                        "numTilesRect", newTotal);

                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }
            );
        }

        ImGui::TextDisabled(ICON_FA_GRIP_VERTICAL " Drag rect preview to Scene to place tile");

        beginTable(cpType, getLabelSize("Tile Rects"), "tilemap_rects_header");
        propertyHeader("Tile Rects", -1, false, false);
        ImGui::Text("%d", activeTileRectCount);
        float tileRectsArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        if (drawSummaryAddButton(ICON_FA_PLUS " Add Tile Rect##tile_rects_add", tileRectsArrowWidth,
                tilemap.tilesRect.validIndex((int)tilemap.numTilesRect), "A Tilemap holds at most 65,536 tile rects")) {
            int freeSlot = (int)tilemap.numTilesRect;
            if (tilemap.tilesRect.validIndex(freeSlot)) {
                editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();
                std::string prefix = "tilesRect[" + std::to_string(freeSlot) + "]";
                multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                    prefix + ".name", "rect_" + std::to_string(freeSlot));
                multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                    prefix + ".submeshId", 0);
                multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                    "numTilesRect", (unsigned int)(freeSlot + 1));
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##toggle_all_rects", tilemapRectsExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
            tilemapRectsExpanded = !tilemapRectsExpanded;
        }
        endTable();

        if (tilemapRectsExpanded) {
            // Draw each tile rect
            RowSettings settingsRect;
            settingsRect.stepSize = 1.0f;
            settingsRect.format = "%.0f";
            settingsRect.showColors = false;

            for (unsigned int i = 0; i < tilemap.numTilesRect; i++) {
                ImGui::PushID(i);

                std::string label = "[" + std::to_string(i) + "] " + tilemap.tilesRect[i].name;
                std::string groupStr = "tilerect_" + std::to_string(i);
                std::string prefix = "tilesRect[" + std::to_string(i) + "]";

                separatorText(label.c_str());

                beginTable(cpType, getLabelSize("Name"), groupStr);

                propertyHeader("Rect", -1, false, false);
                {
                    float previewSize = ImGui::GetFrameHeight() * 2.2f;
                    float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
                    float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
                    ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
                    ImVec2 arrowButtonSize = ImGui::CalcItemSize(ImVec2(0, 0), ImGui::GetFrameHeight(), ImGui::GetFrameHeight());

                    int submeshId = tilemap.tilesRect[i].submeshId;
                    Texture* rectTex = nullptr;
                    if (meshComp && submeshId >= 0 && (unsigned int)submeshId < meshComp->numSubmeshes) {
                        rectTex = &meshComp->submeshes[submeshId].material.baseColorTexture;
                        if (rectTex->empty()) rectTex = nullptr;
                    }

                    bool hasFramePreview = rectTex && tilemap.tilesRect[i].rect.getWidth() > 0.0f && tilemap.tilesRect[i].rect.getHeight() > 0.0f;

                    float trailingWidth = arrowButtonSize.x
                            + ImGui::GetStyle().ItemSpacing.x + arrowButtonSize.x
                            + ImGui::GetStyle().ItemSpacing.x + deleteButtonSize.x;
                    if (hasFramePreview) {
                        trailingWidth += previewSize + ImGui::GetStyle().ItemSpacing.x;
                    }

                    float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
                    ImGui::SetCursorPosX(targetX);

                    if (hasFramePreview) {
                        drawSpriteFramePreview(rectTex, tilemap.tilesRect[i].rect, ImVec2(previewSize, previewSize), "##rect_preview");
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                            TileRectPayload payload{(uint32_t)sceneProject->id, entities[0], (int)i};
                            ImGui::SetDragDropPayload("tile_rect", &payload, sizeof(payload));
                            drawSpriteFramePreview(rectTex, tilemap.tilesRect[i].rect, Theme::dpi(ImVec2(48.0f, 48.0f)), "##rect_drag_preview");
                            ImGui::EndDragDropSource();
                        } else if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("%s", label.c_str());

                            float tooltipMaxSize = Theme::dpi(200.0f);
                            float scale = std::min(tooltipMaxSize / std::max(1.0f, tilemap.tilesRect[i].rect.getWidth()),
                                                   tooltipMaxSize / std::max(1.0f, tilemap.tilesRect[i].rect.getHeight()));
                            scale = std::min(scale, 1.0f);
                            ImVec2 tooltipSize(std::max(Theme::dpi(32.0f), tilemap.tilesRect[i].rect.getWidth() * scale),
                                               std::max(Theme::dpi(32.0f), tilemap.tilesRect[i].rect.getHeight() * scale));

                            drawSpriteFramePreview(rectTex, tilemap.tilesRect[i].rect, tooltipSize, "##rect_preview_tooltip");
                            ImGui::EndTooltip();
                        }
                        ImGui::SameLine();
                    }

                    if (ImGui::ArrowButton("##toggle_rect", tilemapRectsButtonGroups[groupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
                        tilemapRectsButtonGroups[groupStr] = !tilemapRectsButtonGroups[groupStr];
                    }
                    ImGui::SameLine();

                    {
                    ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_SQUARE_PLUS);
                    ImVec2 centerPad((arrowButtonSize.x - iconSize.x) * 0.5f, (arrowButtonSize.y - iconSize.y) * 0.5f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, centerPad);
                    }
                    ImGui::BeginDisabled(tilemap.numTiles >= MAX_TILEMAP_RENDERED_TILES);
                    const bool addTile = ImGui::Button(ICON_FA_SQUARE_PLUS "##add_tile_from_rect");
                    ImGui::EndDisabled();
                    if (tilemap.numTiles >= MAX_TILEMAP_RENDERED_TILES && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("A Tilemap draws at most %d tiles", MAX_TILEMAP_RENDERED_TILES);
                    }
                    if (addTile) {
                        int freeSlot = (int)tilemap.numTiles;
                        if (freeSlot < MAX_TILEMAP_RENDERED_TILES) {
                            editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();
                            std::string tilePrefix = "tiles[" + std::to_string(freeSlot) + "]";
                            multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                                tilePrefix + ".name", tilemap.tilesRect[i].name);
                            multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                                tilePrefix + ".rectId", (int)i);
                            multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType,
                                tilePrefix + ".width", tilemap.tilesRect[i].rect.getWidth());
                            multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType,
                                tilePrefix + ".height", tilemap.tilesRect[i].rect.getHeight());
                            multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                                "numTiles", (unsigned int)(freeSlot + 1));
                            multiCmd->setNoMerge();
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                        }
                    }
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Create tile from this rect");
                    }
                    ImGui::SameLine();

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(ICON_FA_TRASH_CAN "##delete_rect", deleteButtonSize)) {
                        editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();
                        // Shift subsequent rects down
                        for (unsigned int j = i; j < tilemap.numTilesRect - 1; j++) {
                            std::string dstPrefix = "tilesRect[" + std::to_string(j) + "]";
                            multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                                dstPrefix + ".name", tilemap.tilesRect[j + 1].name);
                            multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                                dstPrefix + ".submeshId", tilemap.tilesRect[j + 1].submeshId);
                            multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], cpType,
                                dstPrefix + ".rect", Vector4(tilemap.tilesRect[j + 1].rect.getX(), tilemap.tilesRect[j + 1].rect.getY(),
                                    tilemap.tilesRect[j + 1].rect.getWidth(), tilemap.tilesRect[j + 1].rect.getHeight()));
                        }
                        // Clear last slot
                        unsigned int lastIdx = tilemap.numTilesRect - 1;
                        std::string lastPrefix = "tilesRect[" + std::to_string(lastIdx) + "]";
                        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                            lastPrefix + ".name", std::string(""));
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                            lastPrefix + ".submeshId", -1);
                        multiCmd->addPropertyCmd<Vector4>(project, sceneProject->id, entities[0], cpType,
                            lastPrefix + ".rect", Vector4(0, 0, 0, 0));
                        for (unsigned int j = 0; j < tilemap.numTiles; j++) {
                            int rectId = tilemap.tiles[j].rectId;
                            if (rectId == (int)i) {
                                rectId = -1;
                            } else if (rectId > (int)i) {
                                rectId--;
                            }

                            if (rectId != tilemap.tiles[j].rectId) {
                                multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType,
                                    "tiles[" + std::to_string(j) + "].rectId", rectId);
                            }
                        }
                        multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                            "numTilesRect", (unsigned int)(tilemap.numTilesRect - 1));
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                        endTable();
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor(2);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                }

                if (tilemapRectsButtonGroups[groupStr]) {
                    propertyRow(RowPropertyType::String, cpType, prefix + ".name", "Name", sceneProject, entities);
                    propertyRow(RowPropertyType::Vector4, cpType, prefix + ".rect", "Rect", sceneProject, entities, settingsRect);

                    // Submesh selector combo
                    {
                        unsigned int numSubmeshes = meshComp ? meshComp->numSubmeshes : 1;
                        int currentSubmeshId = tilemap.tilesRect[i].submeshId;

                        propertyHeader("Submesh");
                        ImGui::SetNextItemWidth(-1);

                        std::string comboLabel = "##submeshId_" + std::to_string(i);
                        std::string previewStr = (currentSubmeshId >= 0 && (unsigned int)currentSubmeshId < numSubmeshes)
                            ? "Submesh " + std::to_string(currentSubmeshId)
                            : "None";

                        if (ImGui::BeginCombo(comboLabel.c_str(), previewStr.c_str())) {
                            for (unsigned int s = 0; s < numSubmeshes; s++) {
                                bool isSelected = ((int)s == currentSubmeshId);
                                std::string itemLabel = "Submesh " + std::to_string(s);
                                if (ImGui::Selectable(itemLabel.c_str(), isSelected)) {
                                    if ((int)s != currentSubmeshId) {
                                        editor::PropertyCmd<int>* propCmd = new editor::PropertyCmd<int>(
                                            project, sceneProject->id, entities[0], cpType,
                                            prefix + ".submeshId", (int)s);
                                        propCmd->setNoMerge();
                                        CommandHandle::get(project->getSelectedSceneId())->addCommand(propCmd);
                                    }
                                }
                                if (isSelected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    }
                }

                endTable();
                ImGui::PopID();
            }
        }

        // --- Tiles Section ---
        separatorText("Tiles");

        int activeTileCount = (int)tilemap.numTiles;

        beginTable(cpType, getLabelSize("Tiles"), "tilemap_tiles_header");
        propertyHeader("Tiles", -1, false, false);
        ImGui::Text("%d", activeTileCount);
        float tilesArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        if (drawSummaryAddButton(ICON_FA_PLUS " Add Tile##tilemap_tiles_add", tilesArrowWidth,
                tilemap.numTiles < MAX_TILEMAP_RENDERED_TILES, "A Tilemap draws at most 16,383 tiles")) {
            int freeSlot = (int)tilemap.numTiles;
            if (freeSlot < MAX_TILEMAP_RENDERED_TILES) {
                float defaultWidth = 100.0f;
                float defaultHeight = 100.0f;
                if (tilemap.numTilesRect > 0 && tilemap.tilesRect[0].rect.getWidth() > 0.0f && tilemap.tilesRect[0].rect.getHeight() > 0.0f) {
                    defaultWidth = tilemap.tilesRect[0].rect.getWidth();
                    defaultHeight = tilemap.tilesRect[0].rect.getHeight();
                }
                editor::MultiPropertyCmd* multiCmd = new editor::MultiPropertyCmd();
                std::string prefix = "tiles[" + std::to_string(freeSlot) + "]";
                multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entities[0], cpType,
                    prefix + ".name", "tile_" + std::to_string(freeSlot));
                multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType,
                    prefix + ".width", defaultWidth);
                multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType,
                    prefix + ".height", defaultHeight);
                multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, entities[0], cpType,
                    "numTiles", (unsigned int)(freeSlot + 1));
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##toggle_all_tiles", tilemapTilesExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
            tilemapTilesExpanded = !tilemapTilesExpanded;
        }
        endTable();

        if (tilemapTilesExpanded) {
            RowSettings settingsTileFloat;
            settingsTileFloat.stepSize = 1.0f;

            for (unsigned int i = 0; i < tilemap.numTiles; i++) {
                ImGui::PushID(1000 + i);

                std::string label = "[" + std::to_string(i) + "] " + tilemap.tiles[i].name;
                std::string groupStr = "tile_" + std::to_string(i);
                std::string prefix = "tiles[" + std::to_string(i) + "]";

                bool isTileSelected = sceneProject->sceneRender &&
                    sceneProject->sceneRender->getSelectedTileIndex() == (int)i &&
                    sceneProject->sceneRender->getSelectedTileEntity() == entities[0];

                // sub-selected in the scene viewport: same marking as occluder points
                if (isTileSelected) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
                    separatorText((std::string(ICON_FA_CARET_RIGHT) + " " + label).c_str());
                    ImGui::PopStyleColor();
                } else {
                    separatorText(label.c_str());
                }
                if (lastHeaderRowClicked()) {
                    syncSubSelection(sceneProject, entities[0], (int)i, -1);
                }

                beginTable(cpType, getLabelSize("Position"), groupStr);

                propertyHeader("Tile", -1, false, false);
                {
                    float previewSize = ImGui::GetFrameHeight() * 2.2f;
                    float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
                    float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
                    ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
                    ImVec2 arrowButtonSize = ImGui::CalcItemSize(ImVec2(0, 0), ImGui::GetFrameHeight(), ImGui::GetFrameHeight());

                    Texture* tileTex = nullptr;
                    Rect tileRect;
                    if (tilemap.tiles[i].rectId >= 0 && tilemap.tiles[i].rectId < (int)tilemap.numTilesRect) {
                        const auto& rectData = tilemap.tilesRect[tilemap.tiles[i].rectId];
                        tileRect = rectData.rect;
                        if (meshComp && rectData.submeshId >= 0 && (unsigned int)rectData.submeshId < meshComp->numSubmeshes) {
                            tileTex = &meshComp->submeshes[rectData.submeshId].material.baseColorTexture;
                            if (tileTex->empty()) tileTex = nullptr;
                        }
                    }

                    bool hasFramePreview = tileTex && tileRect.getWidth() > 0.0f && tileRect.getHeight() > 0.0f;

                    float trailingWidth = arrowButtonSize.x
                            + ImGui::GetStyle().ItemSpacing.x + deleteButtonSize.x;
                    if (hasFramePreview) {
                        trailingWidth += previewSize + ImGui::GetStyle().ItemSpacing.x;
                    }

                    float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
                    ImGui::SetCursorPosX(targetX);

                    if (hasFramePreview) {
                        drawSpriteFramePreview(tileTex, tileRect, ImVec2(previewSize, previewSize), "##tile_preview");
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("%s", label.c_str());

                            float tooltipMaxSize = Theme::dpi(200.0f);
                            float scale = std::min(tooltipMaxSize / std::max(1.0f, tileRect.getWidth()),
                                                   tooltipMaxSize / std::max(1.0f, tileRect.getHeight()));
                            scale = std::min(scale, 1.0f);
                            ImVec2 tooltipSize(std::max(Theme::dpi(32.0f), tileRect.getWidth() * scale),
                                               std::max(Theme::dpi(32.0f), tileRect.getHeight() * scale));

                            drawSpriteFramePreview(tileTex, tileRect, tooltipSize, "##tile_preview_tooltip");
                            ImGui::EndTooltip();
                        }
                        ImGui::SameLine();
                    }

                    if (ImGui::ArrowButton("##toggle_tile", tilemapTilesButtonGroups[groupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
                        tilemapTilesButtonGroups[groupStr] = !tilemapTilesButtonGroups[groupStr];
                    }
                    ImGui::SameLine();

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(ICON_FA_TRASH_CAN, deleteButtonSize)) {
                        Command* deleteCmd = ProjectUtils::buildDeleteTileCmd(project, sceneProject->id, entities[0], i);
                        if (deleteCmd) {
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(deleteCmd);
                        }
                        endTable();
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor(2);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                }

                if (tilemapTilesButtonGroups[groupStr]) {
                    propertyRow(RowPropertyType::String, cpType, prefix + ".name", "Name", sceneProject, entities);
                    propertyRow(RowPropertyType::Int, cpType, prefix + ".rectId", "Rect ID", sceneProject, entities, settingsInt);
                    propertyRow(RowPropertyType::Vector2, cpType, prefix + ".position", "Position", sceneProject, entities, settingsTileFloat);
                    propertyRow(RowPropertyType::Float, cpType, prefix + ".width", "Width", sceneProject, entities, settingsTileFloat);
                    propertyRow(RowPropertyType::Float, cpType, prefix + ".height", "Height", sceneProject, entities, settingsTileFloat);
                }

                endTable();
                ImGui::PopID();
            }
        }
    }
}

void editor::InspectorWindow::drawTerrainComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(entities[0]);

    RowSettings textureSettings;
    textureSettings.secondColSize = -1;

    if (entities.size() == 1){
        if (ImGui::Button(ICON_FA_MOUNTAIN " Edit Terrain", ImVec2(-1, 0))){
            if (TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow()){
                terrainEditWindow->openForEntity(entities[0], sceneProject->id);
            }
        }
    }

    separatorText("Maps");
    beginTable(cpType, getLabelSize("Height Map"), "terrain_maps");
    propertyRow(RowPropertyType::Texture, cpType, "heightMap", "Height Map", sceneProject, entities, textureSettings);
    endTable();

    // The Terrain window owns adding and removing these, so here they are only listed
    if (!terrain.blendMaps.empty()){
        separatorText("Blend Maps");
        beginTable(cpType, getLabelSize("Blend Map 1"), "terrain_blendmaps");
        for (size_t i = 0; i < terrain.blendMaps.size(); i++){
            const std::string property = "blendMaps[" + std::to_string(i) + "]";
            const std::string label = "Blend Map " + std::to_string(i + 1);
            propertyRow(RowPropertyType::Texture, cpType, property, label, sceneProject, entities, textureSettings);
        }
        endTable();
    }

    if (!terrain.surfaceLayers.empty()){
        RowSettings settingsLayerFloat;
        settingsLayerFloat.secondColSize = 6 * ImGui::GetFontSize();

        separatorText("Layers");
        for (size_t i = 0; i < terrain.surfaceLayers.size(); i++){
            const std::string property = "surfaceLayers[" + std::to_string(i) + "]";
            const std::string label = "Layer " + std::to_string(i + 1);
            if (!ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)){
                continue;
            }
            ImGui::PushID((int)i);
            beginTable(cpType, getLabelSize("Occlusion Strength"), "terrain_layer");
            propertyRow(RowPropertyType::Bool, cpType, property + ".pbr", "Enable PBR", sceneProject, entities, textureSettings);
            propertyRow(RowPropertyType::Texture, cpType, property + ".colorTexture", "Color", sceneProject, entities, textureSettings);
            // A color layer blends its texture as it is; the rest belongs to PBR layers
            if (terrain.surfaceLayers[i].pbr){
                propertyRow(RowPropertyType::Color4L, cpType, property + ".colorFactor", "Tint", sceneProject, entities, textureSettings);
                propertyRow(RowPropertyType::Texture, cpType, property + ".normalTexture", "Normal", sceneProject, entities, textureSettings);
                propertyRow(RowPropertyType::Float, cpType, property + ".normalStrength", "Normal Strength", sceneProject, entities, settingsLayerFloat);
                propertyRow(RowPropertyType::Texture, cpType, property + ".roughnessTexture", "Roughness", sceneProject, entities, textureSettings);
                propertyRow(RowPropertyType::Float_0_1, cpType, property + ".roughnessFactor", "Roughness Factor", sceneProject, entities, settingsLayerFloat);
                propertyRow(RowPropertyType::Texture, cpType, property + ".metallicTexture", "Metallic", sceneProject, entities, textureSettings);
                propertyRow(RowPropertyType::Float_0_1, cpType, property + ".metallicFactor", "Metallic Factor", sceneProject, entities, settingsLayerFloat);
                propertyRow(RowPropertyType::Texture, cpType, property + ".occlusionTexture", "Occlusion", sceneProject, entities, textureSettings);
                propertyRow(RowPropertyType::Float_0_1, cpType, property + ".occlusionStrength", "Occlusion Strength", sceneProject, entities, settingsLayerFloat);
                propertyRow(RowPropertyType::Texture, cpType, property + ".heightTexture", "Height", sceneProject, entities, textureSettings);
            }
            // Tiling rides on every layer, PBR or not
            propertyRow(RowPropertyType::Vector2, cpType, property + ".uvScale", "UV Scale", sceneProject, entities, settingsLayerFloat);
            propertyRow(RowPropertyType::Vector2, cpType, property + ".uvOffset", "UV Offset", sceneProject, entities, settingsLayerFloat);
            RowSettings settingsStochastic = textureSettings;
            settingsStochastic.help = "Hides the repetition of this layer's tiled maps: hexagonal cells each read them at a random offset "
                                      "and blend at their borders, every map of the layer alike. Up to three reads per map where cells "
                                      "meet. Suits organic textures (grass, rock, sand); bricks and tiles lose their grid.";
            propertyRow(RowPropertyType::Bool, cpType, property + ".stochasticTiling", "Stochastic Tiling", sceneProject, entities, settingsStochastic);
            if (terrain.surfaceLayers[i].stochasticTiling){
                RowSettings settingsParam = settingsLayerFloat;
                settingsParam.indent = true;
                settingsParam.stepSize = 0.01f;
                settingsParam.help = "Width of one cell in texture repeats (0.25-4). Smaller cells hide the repetition better but cut up large features of the texture.";
                // the ranges of the Terrain window and set_terrain_maps: 0.25-4, the others 0-1
                settingsParam.floatMin = 0.25f;
                settingsParam.floatMax = 4.0f;
                settingsParam.logarithmic = true;
                propertyRow(RowPropertyType::FloatPositive, cpType, property + ".stochasticCellSize", "Cell Size", sceneProject, entities, settingsParam);
                settingsParam.floatMin = 0.0f;
                settingsParam.floatMax = 1.0f;
                settingsParam.logarithmic = false;
                settingsParam.help = "How narrow the blend between neighbouring cells is: low blends wide and soft, high keeps more of each cell whole.";
                propertyRow(RowPropertyType::Float_0_1, cpType, property + ".stochasticBlendSharpness", "Blend Sharpness", sceneProject, entities, settingsParam);
                settingsParam.help = "Keeps the texture's contrast where cells blend, which a plain blend washes out. Too high shows the cell borders. 0 blends linearly.";
                propertyRow(RowPropertyType::Float_0_1, cpType, property + ".stochasticContrast", "Contrast", sceneProject, entities, settingsParam);
                settingsParam.help = "Turns each cell by a random angle, up to +-180 degrees at 1. Leave at 0 for textures with a direction (planks, stripes, light baked in).";
                propertyRow(RowPropertyType::Float_0_1, cpType, property + ".stochasticRotation", "Rotation", sceneProject, entities, settingsParam);
                // (a layer's normal map is read by its PBR surface only)
                drawStochasticRotationWarning(cpType, sceneProject, entities, property + ".stochasticRotation", terrain.surfaceLayers[i].stochasticRotation,
                                              terrain.surfaceLayers[i].pbr ? terrain.surfaceLayers[i].normalTexture : Texture(), nullptr);
            }
            // anti-repetition of the flat projection (the side planes stay planar)
            RowSettings settingsHex = settingsLayerFloat;
            settingsHex.help = "Samples the layer's flat projection on three randomly rotated copies on a hex grid, which hides the tiling "
                               "a repeated texture shows from a distance (three taps per map instead of the near and far tiling).";
            propertyRow(RowPropertyType::Bool, cpType, property + ".hexTiling", "Hex Tiling", sceneProject, entities, settingsHex);
            if (terrain.surfaceLayers[i].hexTiling){
                settingsHex.help = "How far a tile may turn: 1 a full turn, 0 none (copies only shifted).";
                propertyRow(RowPropertyType::Float_0_1, cpType, property + ".hexRotation", "Rotation", sceneProject, entities, settingsHex);
            }
            endTable();
            ImGui::PopID();
            ImGui::TreePop();
        }
    }

    RowSettings settingsFloat;
    settingsFloat.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsResolution = settingsFloat;
    settingsResolution.stepSize = 1.0f;
    settingsResolution.format = "%.0f";

    // The engine rounds the resolution to a multiple of 4 and writes it back (see
    // createOrUpdateTerrain), so drag by 4 to not fight the value the row is rebound to.
    RowSettings settingsGridResolution = settingsResolution;
    settingsGridResolution.stepSize = 4.0f;

    RowSettings settingsInt;
    settingsInt.stepSize = 1.0f;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    // Bound the quadtree so it can never request an impossible node allocation
    // (see the MAX_TERRAIN_NODES guard in MeshSystem). rootGridSize^2 must fit in
    // MAX_TERRAINGRID (16 -> max 4); Levels is capped so even at the largest root
    // grid the node count stays within the engine budget, so any value set here
    // always renders instead of tripping the engine's rejection.
    RowSettings settingsRootGrid = settingsInt;
    settingsRootGrid.intMin = 1;
    settingsRootGrid.intMax = 4;

    RowSettings settingsLevels = settingsInt;
    settingsLevels.intMin = 1;
    settingsLevels.intMax = 9;

    separatorText("Mesh Resolution");
    beginTable(cpType, getLabelSize("Root Grid Size"), "terrain_shape");
    propertyRow(RowPropertyType::FloatPositive, cpType, "terrainSize", "Size", sceneProject, entities, settingsFloat);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxHeight", "Max Height", sceneProject, entities, settingsFloat);
    propertyRow(RowPropertyType::FloatPositive, cpType, "resolution", "Resolution", sceneProject, entities, settingsGridResolution);
    propertyRow(RowPropertyType::Int, cpType, "rootGridSize", "Root Grid Size", sceneProject, entities, settingsRootGrid);
    propertyRow(RowPropertyType::Int, cpType, "levels", "Levels", sceneProject, entities, settingsLevels);
    propertyRow(RowPropertyType::Vector2, cpType, "offset", "Offset", sceneProject, entities);
    endTable();

    separatorText("Texturing");
    beginTable(cpType, getLabelSize("Detail Tiles"), "terrain_tiling");
    propertyRow(RowPropertyType::FloatPositive, cpType, "textureBaseTiles", "Base Tiles", sceneProject, entities, settingsResolution);
    propertyRow(RowPropertyType::FloatPositive, cpType, "textureDetailTiles", "Detail Tiles", sceneProject, entities, settingsResolution);
    endTable();

    separatorText("Ranges");
    beginTable(cpType, getLabelSize("Auto Ranges"), "terrain_ranges_auto");
    propertyRow(RowPropertyType::Bool, cpType, "autoSetRanges", "Auto Ranges", sceneProject, entities);
    endTable();

    if (!terrain.autoSetRanges) {
        drawTrackValues<TerrainComponent, float>(cpType, sceneProject, entities, RowPropertyType::FloatPositive, 0.0f, "terrain", &TerrainComponent::ranges, "ranges");
    }
}

void editor::InspectorWindow::drawLightIntensityRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                    const RowSettings& settingsFloat){
    Scene* scene = sceneProject->scene;
    const LightComponent& light = scene->getComponent<LightComponent>(entities[0]);
    // the unit as it is drawn: one the type cannot take reads as its neighbour (lux and candela,
    // the same number), LightPhotometry::validUnit
    const LightUnit unit = LightPhotometry::validUnit(light.intensityUnit, light.type);
    bool sameUnit = true;
    for (Entity entity : entities){
        const LightComponent& other = scene->getComponent<LightComponent>(entity);
        sameUnit = sameUnit && LightPhotometry::validUnit(other.intensityUnit, other.type) == unit && other.type == light.type;
    }

    RowSettings settings = settingsFloat;
    if (unit != LightUnit::ENGINE)
        settings.help = std::string("In ") + LightPhotometry::getUnitName(unit) + ", drawn through the scene's Physical Light Exposure (EV100, Lighting settings): " +
                        "a 100000 lx sun at EV100 15 is about 2.5 engine units." + (unit == LightUnit::LUMEN ? " A spot's lumens spread over the whole sphere, so its cone does not change its brightness." : "");
    propertyRow(RowPropertyType::Float, cpType, "intensity", "Intensity", sceneProject, entities, settings);

    // the unit, among those of the type; a change keeps the light as bright (its value converted)
    propertyHeader("Unit", -1, unit != LightUnit::ENGINE, false);
    std::vector<LightUnit> units = {LightUnit::ENGINE};
    if (light.type == LightType::DIRECTIONAL){
        units.push_back(LightUnit::LUX);
    }else{
        units.push_back(LightUnit::CANDELA);
        units.push_back(LightUnit::LUMEN);
    }
    ImGui::SetNextItemWidth(settingsFloat.secondColSize > 0 ? settingsFloat.secondColSize : 6 * ImGui::GetFontSize());
    if (ImGui::BeginCombo("##light_unit", sameUnit ? LightPhotometry::getUnitName(unit) : "---")){
        for (LightUnit option : units){
            if (ImGui::Selectable(LightPhotometry::getUnitName(option), sameUnit && option == unit)){
                const float ev100 = scene->getPhysicalLightEV100();
                auto* multi = new MultiPropertyCmd();
                for (Entity entity : entities){
                    LightComponent converted = scene->getComponent<LightComponent>(entity);
                    LightPhotometry::setIntensityUnit(converted, option, true, ev100);
                    multi->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, "intensityUnit", (int)converted.intensityUnit);
                    multi->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, "intensity", converted.intensity);
                }
                CommandHandle::get(sceneProject->id)->addCommandNoMerge(multi);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); helpMarker("Engine: the engine's own intensity, drawn as it is. Lux (directional), Candela and Lumen (point, spot): "
                                  "physical units, converted on the CPU through the scene's Physical Light Exposure. Changing the unit keeps "
                                  "the light as bright.");
}

void editor::InspectorWindow::drawLightTemperatureRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    Scene* scene = sceneProject->scene;
    const LightComponent& light = scene->getComponent<LightComponent>(entities[0]);

    RowSettings settingsUse;
    settingsUse.help = "Colour the light as a black body of the temperature below (Kelvin); its Color then filters that colour.";
    propertyRow(RowPropertyType::Bool, cpType, "useColorTemperature", "Use Color Temperature", sceneProject, entities, settingsUse);
    if (!light.useColorTemperature)
        return;

    RowSettings settingsKelvin;
    settingsKelvin.stepSize = 10.0f;
    settingsKelvin.format = "%.0f K";
    settingsKelvin.secondColSize = 6 * ImGui::GetFontSize();
    propertyRow(RowPropertyType::FloatPositive, cpType, "colorTemperature", "Temperature", sceneProject, entities, settingsKelvin);

    // the black body gradient, 1000 to 20000 K, the light's own marked
    propertyHeader("", -1, false, false);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    const float height = ImGui::GetFrameHeight() * 0.6f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const int segments = 48;
    auto colorAt = [](float kelvin){
        const Vector3 linear = LightPhotometry::kelvinToLinearRGB(kelvin);
        const Vector3 srgb = Color::linearTosRGB(linear);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(srgb.x, srgb.y, srgb.z, 1.0f));
    };
    auto kelvinAt = [](float t){
        return LightPhotometry::MIN_TEMPERATURE + t * (LightPhotometry::MAX_TEMPERATURE - LightPhotometry::MIN_TEMPERATURE);
    };
    for (int i = 0; i < segments; i++){
        const float x0 = origin.x + width * (float)i / segments;
        const float x1 = origin.x + width * (float)(i + 1) / segments;
        const ImU32 left = colorAt(kelvinAt((float)i / segments));
        const ImU32 right = colorAt(kelvinAt((float)(i + 1) / segments));
        drawList->AddRectFilledMultiColor(ImVec2(x0, origin.y), ImVec2(x1, origin.y + height), left, right, right, left);
    }
    const float t = std::clamp((light.colorTemperature - LightPhotometry::MIN_TEMPERATURE) /
                               (LightPhotometry::MAX_TEMPERATURE - LightPhotometry::MIN_TEMPERATURE), 0.0f, 1.0f);
    const float markerX = origin.x + width * t;
    drawList->AddRectFilled(ImVec2(markerX - 1.5f, origin.y - 2.0f), ImVec2(markerX + 1.5f, origin.y + height + 2.0f), IM_COL32(20, 20, 20, 255));
    ImGui::Dummy(ImVec2(width, height));

    // Unity's presets
    struct KelvinPreset { const char* name; float kelvin; };
    static const KelvinPreset presets[] = {
        {"Candle", 1900.0f}, {"Tungsten", 2700.0f}, {"Halogen", 3200.0f},
        {"Daylight", 5600.0f}, {"Overcast", 6500.0f}, {"Blue Sky", 10000.0f}
    };
    propertyHeader("Presets", -1, false, false);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##kelvin_presets", "Choose...")){
        for (const KelvinPreset& preset : presets){
            if (ImGui::Selectable((std::string(preset.name) + " (" + std::to_string((int)preset.kelvin) + " K)").c_str())){
                auto* multi = new MultiPropertyCmd();
                for (Entity entity : entities)
                    multi->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, "colorTemperature", preset.kelvin);
                CommandHandle::get(sceneProject->id)->addCommandNoMerge(multi);
            }
        }
        ImGui::EndCombo();
    }
}

void editor::InspectorWindow::drawPhysicalLightExposureRow(SceneProject* sceneProject){
    drawScenePropertyRow<float>(sceneProject, "physical_light_ev100", "Physical Light Exposure", ScenePropertyInputType::DragFloat, 5 * ImGui::GetFontSize(), -6.0f, 24.0f,
        "The camera exposure (EV100) lights in physical units (lux, candela, lumen) are drawn at: it affects only them. "
        "Lights in Engine units are drawn as they are. Sunny 15, Overcast 12, Interior 7, Night 2.");
    // Sunny 16 and its neighbours
    struct ExposurePreset { const char* name; float ev100; };
    static const ExposurePreset presets[] = {{"Sunny", 15.0f}, {"Overcast", 12.0f}, {"Interior", 7.0f}, {"Night", 2.0f}};
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    for (const ExposurePreset& preset : presets){
        if (ImGui::SmallButton((std::string(preset.name) + "##ev100").c_str())){
            Command* command = new ScenePropertyCmd<float>(project, sceneProject->id, "physical_light_ev100", preset.ev100);
            CommandHandle::get(sceneProject->id)->addCommandNoMerge(command);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("EV100 %.0f", preset.ev100);
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

void editor::InspectorWindow::drawLightComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    LightComponent& light = sceneProject->scene->getComponent<LightComponent>(entities[0]);

    RowSettings settingsFloat;
    settingsFloat.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsCone;
    settingsCone.secondColSize = 6 * ImGui::GetFontSize();
    settingsCone.format = "%.1f";

    RowSettings settingsLightType;
    settingsLightType.enumEntries = &entriesLightType;

    beginTable(cpType, std::max(getLabelSize("Volumetric Intensity"), getLabelSize("Culling Mask")));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Type", sceneProject, entities, settingsLightType);
    drawLightIntensityRows(cpType, sceneProject, entities, settingsFloat);
    propertyRow(RowPropertyType::Float, cpType, "range", "Range", sceneProject, entities, settingsFloat);
    // with a colour temperature, the colour filters the black body's (Unity's Filter)
    propertyRow(RowPropertyType::Color3L, cpType, "color", light.useColorTemperature ? "Filter" : "Color", sceneProject, entities);
    drawLightTemperatureRows(cpType, sceneProject, entities);
    if (light.type != LightType::POINT){
        propertyRow(RowPropertyType::Direction, cpType, "direction", "Direction", sceneProject, entities);
    }
    if (light.type != LightType::DIRECTIONAL){
        RowSettings settingsVolumetric = settingsFloat;
        settingsVolumetric.help = "How much of this light the volumetric fog scatters (High atmosphere quality, up to 4 point or spot lights, "
                                  "no shadows). 0: none.";
        propertyRow(RowPropertyType::FloatPositive, cpType, "volumetricIntensity", "Volumetric Intensity", sceneProject, entities, settingsVolumetric);
    }
    if (light.type == LightType::SPOT){
        RowSettings settingsMask;
        settingsMask.allowCameraTexture = false;
        settingsMask.allowTextureSettings = false;
        settingsMask.help = "Optional image projected by the spotlight. Alpha controls intensity when transparency is present; otherwise luminance is used. Empty uses the default circular cone.";
        propertyRow(RowPropertyType::Texture, cpType, "spotMask", "Mask", sceneProject, entities, settingsMask);
        if (light.spotMask.empty()){
            propertyRow(RowPropertyType::HalfCone, cpType, "innerConeCos", "Inner Cone", sceneProject, entities, settingsCone);
            propertyRow(RowPropertyType::HalfCone, cpType, "outerConeCos", "Outer Cone", sceneProject, entities, settingsCone);
        }else{
            propertyRow(RowPropertyType::HalfCone, cpType, "outerConeCos", "Angle Cone", sceneProject, entities, settingsCone);
        }
    }
    RowSettings settingsCulling;
    settingsCulling.help = "Layers this light lights. GameObjects on other layers get no light and cast no shadow from it.";
    propertyRow(RowPropertyType::LayerMask, cpType, "cullingMask", "Culling Mask", sceneProject, entities, settingsCulling);
    RowSettings settingsShading;
    settingsShading.enumEntries = &entriesLightShading;
    settingsShading.help = "Where this light is evaluated on materials whose Lighting is Mixed: Per Pixel (specular, shadows, spot masks) "
                           "or Per Vertex (diffuse only, cheapest). Both leaves it to the material (per pixel under Mixed). "
                           "Materials lit Per Pixel or Per Vertex evaluate every light their own way.";
    propertyRow(RowPropertyType::Enum, cpType, "shading", "Shading", sceneProject, entities, settingsShading);
    endTable();

    drawLightShadows(cpType, sceneProject, entities);
}

void editor::InspectorWindow::drawShadowCaster2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    ShadowCaster2DComponent& occluder = sceneProject->scene->getComponent<ShadowCaster2DComponent>(entities[0]);

    RowSettings settingsShape;
    settingsShape.enumEntries = &entriesShadowCaster2DShape;
    settingsShape.help = "Auto Quad derives the outline from this GameObject's mesh bounds; Polygon uses the point list below (local space).";

    beginTable(cpType, getLabelSize("Enabled"));
    propertyRow(RowPropertyType::Enum, cpType, "shape", "Outline", sceneProject, entities, settingsShape);
    if (occluder.shape == ShadowCaster2DShape::POLYGON){
        propertyRow(RowPropertyType::Bool, cpType, "closed", "Closed", sceneProject, entities);
    }
    propertyRow(RowPropertyType::Bool, cpType, "enabled", "Enabled", sceneProject, entities);
    endTable();

    if (occluder.shape == ShadowCaster2DShape::POLYGON && entities.size() == 1){
        Entity entity = entities[0];

        Command* pointsCmd = nullptr;
        auto setPoints = [&](const std::vector<Vector2>& newPoints, bool merge){
            editor::PropertyCmd<std::vector<Vector2>>* cmd = new editor::PropertyCmd<std::vector<Vector2>>(project, sceneProject->id, entity, cpType, "points", newPoints);
            if (!merge){
                cmd->setNoMerge();
            }
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
            pointsCmd = cmd;
        };

        separatorText("Points");

        // point sub-selected in the scene viewport (click on a handle)
        int selectedPoint = -1;
        if (sceneProject->sceneRender && sceneProject->sceneRender->getSelectedOccluderPointEntity() == entity){
            selectedPoint = sceneProject->sceneRender->getSelectedOccluderPointIndex();
        }

        beginTable(cpType, getLabelSize(ICON_FA_CARET_RIGHT " Point 000"), "shadowcaster2d_points_table");
        for (size_t i = 0; i < occluder.points.size(); i++){
            bool pointSelected = ((int)i == selectedPoint);

            ImGui::PushID(static_cast<int>(i));
            if (pointSelected){
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
                propertyHeader(std::string(ICON_FA_CARET_RIGHT) + " Point " + std::to_string(i), -1, false, false);
                ImGui::PopStyleColor();
            }else{
                propertyHeader("Point " + std::to_string(i), -1, false, false);
            }

            float deleteButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float value[2] = {occluder.points[i].x, occluder.points[i].y};
            ImGui::SetNextItemWidth(std::max(0.0f, ImGui::GetContentRegionAvail().x - deleteButtonWidth - ImGui::GetStyle().ItemSpacing.x));
            if (ImGui::DragFloat2("##shadowcaster2d_point", value, 1.0f, 0.0f, 0.0f, "%.2f")){
                std::vector<Vector2> newPoints = occluder.points;
                newPoints[i] = Vector2(value[0], value[1]);
                setPoints(newPoints, true);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()){
                if (pointsCmd){
                    pointsCmd->setNoMerge();
                    pointsCmd = nullptr;
                }
            }

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ImGui::Button(ICON_FA_TRASH_CAN "##shadowcaster2d_point_delete")){
                std::vector<Vector2> newPoints = occluder.points;
                newPoints.erase(newPoints.begin() + static_cast<std::ptrdiff_t>(i));
                if (sceneProject->sceneRender && (int)i <= selectedPoint){
                    sceneProject->sceneRender->clearOccluderPointSelection();
                }
                setPoints(newPoints, false);
                ImGui::PopStyleColor(2);
                ImGui::PopID();
                break;
            }
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        endTable();

        if (ImGui::Button(ICON_FA_PLUS " Add Point##shadowcaster2d", ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x), 0))){
            std::vector<Vector2> newPoints = occluder.points;
            if (newPoints.size() >= 2){
                // continue the outline: offset from the last edge so the new point is visible
                Vector2 last = newPoints[newPoints.size() - 1];
                Vector2 prev = newPoints[newPoints.size() - 2];
                Vector2 dir = last - prev;
                newPoints.push_back(last + dir * 0.5f);
            }else if (newPoints.size() == 1){
                newPoints.push_back(newPoints[0] + Vector2(50.0f, 0.0f));
            }else{
                newPoints.push_back(Vector2(0.0f, 0.0f));
            }
            setPoints(newPoints, false);
        }
    }
}

void editor::InspectorWindow::drawCapsuleShadowCasterComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    CapsuleShadowCasterComponent& caster = scene->getComponent<CapsuleShadowCasterComponent>(entities[0]);

    RowSettings settingsCast;
    settingsCast.enumEntries = &entriesCapsuleCastShadows;
    settingsCast.help = "On: the meshes also cast their shadow-map shadows (their own Cast Shadows decides). Capsules Only: the meshes "
                        "leave the shadow maps, so a moving character keeps the static shadow cache valid.";

    beginTable(cpType, getLabelSize("Cast Shadows"));
    propertyRow(RowPropertyType::Enum, cpType, "castShadows", "Cast Shadows", sceneProject, entities, settingsCast);
    endTable();

    if (!scene->isCapsuleShadowsEnabled()){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("The scene's Capsule Shadows are off (Inspector with nothing selected > Shadows): these capsules cast nothing.");
        ImGui::PopStyleColor();
    }

    if (entities.size() != 1)
        return;
    Entity entity = entities[0];
    const int maxCapsules = CapsuleShadowCasterComponent::MAX_CAPSULES;

    auto setCapsules = [&](const std::vector<CapsuleShadowShape>& capsules){
        auto* capsulesCmd = new PropertyCmd<std::vector<CapsuleShadowShape>>(project, sceneProject->id, entity, cpType, "capsules", capsules);
        capsulesCmd->setNoMerge();
        CommandHandle::get(sceneProject->id)->addCommand(capsulesCmd);
    };

    separatorText("Capsules");

    // the capsule whose handle is sub-selected in the Scene view
    int selectedCapsule = -1;
    if (sceneProject->sceneRender && sceneProject->sceneRender->getSelectedCapsuleHandleEntity() == entity &&
        sceneProject->sceneRender->getSelectedCapsuleHandleIndex() >= 0){
        selectedCapsule = sceneProject->sceneRender->getSelectedCapsuleHandleIndex() / SceneRender::CAPSULE_HANDLES;
    }

    RowSettings settingsBone;
    settingsBone.help = "The name of the bone (a GameObject under this one) the capsule follows. Empty: this GameObject.";
    RowSettings settingsSize;
    settingsSize.stepSize = 0.01f;
    settingsSize.format = "%.3f";
    RowSettings settingsRadius = settingsSize;
    settingsRadius.help = "Metres, scaled by the bone's scale.";
    RowSettings settingsLength = settingsSize;
    settingsLength.help = "Metres between the centres of the two caps, scaled by the bone's scale.";
    RowSettings settingsOffset = settingsSize;
    settingsOffset.help = "The capsule's centre in the bone's space.";
    RowSettings settingsDirection;
    settingsDirection.enumEntries = &entriesCapsuleDirection;
    settingsDirection.help = "The bone axis the capsule lies along.";

    for (size_t i = 0; i < caster.capsules.size(); i++){
        ImGui::PushID((int)i);
        const std::string prefix = "capsules[" + std::to_string(i) + "]";
        const std::string title = (selectedCapsule == (int)i ? std::string(ICON_FA_CARET_RIGHT " ") : std::string()) +
                                  "Capsule " + std::to_string(i + 1) +
                                  (caster.capsules[i].bone.empty() ? std::string() : " (" + caster.capsules[i].bone + ")");

        if (selectedCapsule == (int)i)
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
        ImGui::SeparatorText(title.c_str());
        if (selectedCapsule == (int)i)
            ImGui::PopStyleColor();

        const ImGuiStyle& style = ImGui::GetStyle();
        const float iconWidth = ImGui::GetFrameHeight();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - iconWidth));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        const bool remove = ImGui::Button(ICON_FA_TRASH_CAN "##capsule_remove", ImVec2(iconWidth, 0));
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Remove capsule");
        if (remove){
            std::vector<CapsuleShadowShape> capsules = caster.capsules;
            capsules.erase(capsules.begin() + (std::ptrdiff_t)i);
            if (sceneProject->sceneRender && selectedCapsule >= (int)i)
                sceneProject->sceneRender->clearCapsuleHandleSelection();
            setCapsules(capsules);
            ImGui::PopID();
            break;
        }

        beginTable(cpType, getLabelSize("Direction"), "capsule_shape");
        propertyRow(RowPropertyType::String, cpType, prefix + ".bone", "Bone", sceneProject, entities, settingsBone);
        propertyRow(RowPropertyType::FloatPositive, cpType, prefix + ".radius", "Radius", sceneProject, entities, settingsRadius);
        propertyRow(RowPropertyType::FloatPositive, cpType, prefix + ".length", "Length", sceneProject, entities, settingsLength);
        propertyRow(RowPropertyType::Vector3, cpType, prefix + ".offset", "Offset", sceneProject, entities, settingsOffset);
        propertyRow(RowPropertyType::Enum, cpType, prefix + ".direction", "Direction", sceneProject, entities, settingsDirection);
        endTable();
        ImGui::PopID();
    }

    const float buttonWidth = std::max(0.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    ImGui::BeginDisabled((int)caster.capsules.size() >= maxCapsules);
    if (ImGui::Button(ICON_FA_PLUS " Add Capsule##capsuleshadowcaster", ImVec2(buttonWidth, 0))){
        std::vector<CapsuleShadowShape> capsules = caster.capsules;
        capsules.push_back(CapsuleShadowShape());
        setCapsules(capsules);
    }
    ImGui::EndDisabled();
    if ((int)caster.capsules.size() >= maxCapsules)
        ImGui::SetItemTooltip("A caster has at most %d capsules.", maxCapsules);
    ImGui::SameLine();

    // Fit To Skeleton: the capsules of the humanoid bones under this GameObject, in world space
    // (the fit reads the rest pose from the bones' current transforms)
    if (ImGui::Button(ICON_FA_PERSON " Fit To Skeleton##capsuleshadowcaster", ImVec2(buttonWidth, 0))){
        std::vector<CapsuleShadows::Bone> bones;
        auto transforms = scene->getComponentArray<Transform>();
        if (scene->findComponent<Transform>(entity)){
            // the GameObject's branch is contiguous in the Transforms, parents before children
            const size_t first = transforms->getIndex(entity);
            const size_t last = scene->findBranchLastIndex(entity);
            std::map<Entity, int> indexOf;
            for (size_t t = first; t <= last && t < transforms->size(); t++){
                const Entity bone = transforms->getEntity(t);
                const Transform& transform = transforms->getComponentFromIndex(t);
                CapsuleShadows::Bone entry;
                entry.name = scene->getEntityName(bone);
                auto parent = indexOf.find(transform.parent);
                entry.parent = (t == first || parent == indexOf.end()) ? -1 : parent->second;
                entry.position = transform.worldPosition;
                entry.rotation = transform.worldRotation;
                entry.scale = transform.worldScale;
                indexOf[bone] = (int)bones.size();
                bones.push_back(entry);
            }
        }
        std::vector<CapsuleShadowShape> capsules;
        std::string error;
        if (CapsuleShadows::fitToSkeleton(bones, capsules, &error)){
            capsuleFitError.clear();
            capsuleFitErrorEntity = NULL_ENTITY;
            if (sceneProject->sceneRender)
                sceneProject->sceneRender->clearCapsuleHandleSelection();
            setCapsules(capsules);
        }else{
            capsuleFitError = "Fit To Skeleton: " + error + ".";
            capsuleFitErrorEntity = entity;
        }
    }
    ImGui::SetItemTooltip("Replaces the capsules with eight on the humanoid bones under this GameObject (head, spine, legs and feet), "
                          "sized by the body's height.");

    if (capsuleFitErrorEntity == entity && !capsuleFitError.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", capsuleFitError.c_str());
        ImGui::PopStyleColor();
    }
}

bool editor::InspectorWindow::beginFoldout(const char* label){
    const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (open && layoutRecording){
        layoutGroups.emplace_back(label);
    }
    return open;
}

void editor::InspectorWindow::endFoldout(){
    ImGui::TreePop();
    if (layoutRecording && !layoutGroups.empty()){
        layoutGroups.pop_back();
    }
}

void editor::InspectorWindow::separatorText(const char* label){
    ImGui::SeparatorText(label);
    if (layoutRecording){
        if (layoutGroups.empty()){
            layoutGroups.emplace_back(label);
        }else{
            layoutGroups.back() = label;
        }
    }
}

void editor::InspectorWindow::beginLayoutComponent(ComponentType cpType){
    layoutRecording = true;
    layoutComponent = cpType;
    layoutGroups.clear();
}

void editor::InspectorWindow::endLayoutComponent(){
    layoutRecording = false;
    layoutGroups.clear();
}

const editor::InspectorLayout& editor::InspectorWindow::getLastLayout() const{
    return layoutLast;
}

const char* editor::InspectorWindow::rowPropertyTypeName(RowPropertyType type){
    switch (type){
        case RowPropertyType::Label: return "Label";
        case RowPropertyType::Bool: return "Bool";
        case RowPropertyType::String: return "String";
        case RowPropertyType::MultilineString: return "MultilineString";
        case RowPropertyType::Float: return "Float";
        case RowPropertyType::Double: return "Double";
        case RowPropertyType::FloatPositive: return "FloatPositive";
        case RowPropertyType::DoublePositive: return "DoublePositive";
        case RowPropertyType::Float_0_1: return "Float_0_1";
        case RowPropertyType::Vector2: return "Vector2";
        case RowPropertyType::Vector3: return "Vector3";
        case RowPropertyType::Vector4: return "Vector4";
        case RowPropertyType::Quat: return "Quat";
        case RowPropertyType::Color3L: return "Color3L";
        case RowPropertyType::Color4L: return "Color4L";
        case RowPropertyType::Int: return "Int";
        case RowPropertyType::UInt: return "UInt";
        case RowPropertyType::Material: return "Material";
        case RowPropertyType::Texture: return "Texture";
        case RowPropertyType::TextureCube: return "TextureCube";
        case RowPropertyType::HalfCone: return "HalfCone";
        case RowPropertyType::UIntSlider: return "UIntSlider";
        case RowPropertyType::IntSlider: return "IntSlider";
        case RowPropertyType::Direction: return "Direction";
        case RowPropertyType::Enum: return "Enum";
        case RowPropertyType::Ease: return "Ease";
        case RowPropertyType::Custom: return "Custom";
        case RowPropertyType::LocalEntity: return "LocalEntity";
        case RowPropertyType::ExternalEntity: return "ExternalEntity";
        case RowPropertyType::Font: return "Font";
        case RowPropertyType::Asset: return "Asset";
        case RowPropertyType::LayerMask: return "LayerMask";
        case RowPropertyType::Curve: return "Curve";
        case RowPropertyType::Gradient: return "Gradient";
    }
    return "Custom";
}

namespace {

    bool floatsDiffer(const float* a, const float* b, size_t lanes){
        for (size_t i = 0; i < lanes; i++){
            if (std::fabs(a[i] - b[i]) > 1e-4f){
                return true;
            }
        }
        return false;
    }

    // Two values of one property differ the way the Inspector shows a mixed value
    bool propertyValueDiffers(const editor::PropertyData& a, const editor::PropertyData& b){
        using editor::PropertyType;
        if (!a.ref || !b.ref || a.type != b.type){
            return a.type != b.type;
        }
        switch (a.type){
            case PropertyType::Bool: return *static_cast<bool*>(a.ref) != *static_cast<bool*>(b.ref);
            case PropertyType::Int:
            case PropertyType::Enum: return *static_cast<int*>(a.ref) != *static_cast<int*>(b.ref);
            case PropertyType::UInt:
            case PropertyType::Entity: return *static_cast<unsigned int*>(a.ref) != *static_cast<unsigned int*>(b.ref);
            case PropertyType::Float: return floatsDiffer(static_cast<float*>(a.ref), static_cast<float*>(b.ref), 1);
            case PropertyType::Double: return std::fabs(*static_cast<double*>(a.ref) - *static_cast<double*>(b.ref)) > 1e-4;
            case PropertyType::Vector2: return floatsDiffer(static_cast<float*>(a.ref), static_cast<float*>(b.ref), 2);
            case PropertyType::Vector3: return floatsDiffer(static_cast<float*>(a.ref), static_cast<float*>(b.ref), 3);
            case PropertyType::Vector4: return floatsDiffer(static_cast<float*>(a.ref), static_cast<float*>(b.ref), 4);
            case PropertyType::Quat: return floatsDiffer(&static_cast<Quaternion*>(a.ref)->w, &static_cast<Quaternion*>(b.ref)->w, 4);
            case PropertyType::String: return *static_cast<std::string*>(a.ref) != *static_cast<std::string*>(b.ref);
            case PropertyType::Asset: return static_cast<AssetRef*>(a.ref)->path != static_cast<AssetRef*>(b.ref)->path;
            case PropertyType::Shader: return static_cast<ShaderRef*>(a.ref)->path != static_cast<ShaderRef*>(b.ref)->path;
            case PropertyType::Curve: return *static_cast<Curve*>(a.ref) != *static_cast<Curve*>(b.ref);
            case PropertyType::Gradient: return *static_cast<Gradient*>(a.ref) != *static_cast<Gradient*>(b.ref);
            case PropertyType::Texture: return *static_cast<Texture*>(a.ref) != *static_cast<Texture*>(b.ref);
            case PropertyType::Font: return *static_cast<FontArray*>(a.ref) != *static_cast<FontArray*>(b.ref);
            case PropertyType::Ease: return *static_cast<Ease*>(a.ref) != *static_cast<Ease*>(b.ref);
            case PropertyType::Material: return *static_cast<Material*>(a.ref) != *static_cast<Material*>(b.ref);
            case PropertyType::EntityReference:
                return static_cast<EntityReference*>(a.ref)->entity != static_cast<EntityReference*>(b.ref)->entity ||
                       static_cast<EntityReference*>(a.ref)->sceneId != static_cast<EntityReference*>(b.ref)->sceneId;
            case PropertyType::Custom: return false;
        }
        return false;
    }

}

bool editor::InspectorWindow::propertyValuesDiffer(SceneProject* sceneProject, ComponentType cpType, const std::string& id, const std::vector<Entity>& entities){
    if (!sceneProject || !sceneProject->scene || entities.size() < 2){
        return false;
    }
    // a field of the component's table: compared as the default Inspector compares (lane by lane,
    // arrays by length then elements); a property outside it (a script's) by its value
    if (const reflect::ComponentDesc* desc = reflect::find(cpType)){
        if (reflect::findField(*desc, id)){
            std::vector<void*> comps;
            for (Entity entity : entities){
                comps.push_back(desc->find(sceneProject->scene, entity));
            }
            return reflect::mixedValues(*desc, comps, id).any;
        }
    }
    const PropertyData first = Catalog::findProperty(sceneProject->scene, entities[0], cpType, id);
    for (size_t i = 1; i < entities.size(); i++){
        if (propertyValueDiffers(first, Catalog::findProperty(sceneProject->scene, entities[i], cpType, id))){
            return true;
        }
    }
    return false;
}

void editor::InspectorWindow::recordLayoutRow(RowPropertyType type, ComponentType cpType, const std::string& id, const std::string& label,
                                              SceneProject* sceneProject, const std::vector<Entity>& entities, bool visible){
    if (!layoutRecording){
        return;
    }
    InspectorLayoutRow row;
    row.component = cpType;
    row.path = id;
    row.label = label;
    row.kind = rowPropertyTypeName(type);
    row.group = layoutGroups.empty() ? std::string() : layoutGroups.back();
    row.mixed = propertyValuesDiffer(sceneProject, cpType, id, entities);
    row.readOnly = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    row.visible = visible;
    layoutDrawing.rows.push_back(std::move(row));
}

void editor::InspectorWindow::drawAtmospherePresetsMenu(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                        const std::vector<AtmospherePreset>& presets, const char* current){
    const std::string popup = "atmosphere_presets_" + std::to_string((int)cpType);
    const std::string button = std::string(current ? current : "Presets") + " " ICON_FA_CARET_DOWN "##" + popup;
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    if (ImGui::Button(button.c_str()))
        ImGui::OpenPopup(popup.c_str());
    if (ImGui::BeginPopup(popup.c_str())){
        for (const AtmospherePreset& preset : presets){
            if (ImGui::MenuItem(preset.name.c_str())){
                std::string error;
                if (Command* cmd = AtmospherePresetCmd::build(project, sceneProject->id, entities, cpType, preset.id, error)){
                    CommandHandle::get(sceneProject->id)->addCommandNoMerge(cmd);
                }else if (!error.empty()){
                    Out::warning("%s", error.c_str());
                }
            }
            if (!preset.description.empty())
                ImGui::SetItemTooltip("%s", preset.description.c_str());
        }
        ImGui::EndPopup();
    }
}

void editor::InspectorWindow::drawFogComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    FogComponent& fog = sceneProject->scene->getComponent<FogComponent>(entities[0]);
    const bool height = fog.type == FogType::HEIGHT;

    RowSettings settingsFogType;
    settingsFogType.enumEntries = &entriesFogType;
    settingsFogType.help = "Linear, Exponential, Exponential Squared: fog by distance. Height: physically based fog, dense up to the Base Height and "
                           "thinning above it, lit by the sun, with an optional volumetric part.";

    RowSettings settingsFloat;
    settingsFloat.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsDensity = settingsFloat;
    settingsDensity.stepSize = 0.001f;
    settingsDensity.format = "%.4f";

    const float labelSize = getLabelSize("Directional Inscattering");

    if (height)
        drawAtmospherePresetsMenu(cpType, sceneProject, entities, AtmospherePresets::getFogPresets(), nullptr);

    beginTable(cpType, labelSize);
    propertyRow(RowPropertyType::Enum, cpType, "type", "Type", sceneProject, entities, settingsFogType);
    if (!height){
        propertyRow(RowPropertyType::Color3L, cpType, "color", "Color", sceneProject, entities);
        if (fog.type == FogType::LINEAR) {
            propertyRow(RowPropertyType::Float, cpType, "linearStart", "Linear Start", sceneProject, entities, settingsFloat);
            propertyRow(RowPropertyType::Float, cpType, "linearEnd", "Linear End", sceneProject, entities, settingsFloat);
        } else {
            propertyRow(RowPropertyType::Float, cpType, "density", "Density", sceneProject, entities, settingsDensity);
        }
        // the volumetric fog is a part of the height fog
        ImGui::BeginDisabled();
        RowSettings settingsVolumetric;
        settingsVolumetric.help = "Volumetric fog needs a Fog of Type Height.";
        propertyRow(RowPropertyType::Bool, cpType, "volumetric", "Volumetric", sceneProject, entities, settingsVolumetric);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Volumetric fog needs a Fog of Type Height.");
        endTable();
        return;
    }
    endTable();

    const AtmosphereInfo& info = sceneProject->scene->getSystem<RenderSystem>()->getAtmosphereInfo(0);

    if (beginFoldout("Height Fog")){
        RowSettings settingsColorMode;
        settingsColorMode.enumEntries = &entriesFogColorMode;
        settingsColorMode.help = "Constant Color: the fog takes its Color. Sky Color: the colour of the sky at the horizon (a cube map Skybox, or a "
                                 "sky shader with a horizonColor property).";
        RowSettings settingsAttenuation = settingsFloat;
        settingsAttenuation.help = "The distance (m) light travels in the fog below the Base Height before 63% of it is absorbed. Smaller: denser fog.";
        RowSettings settingsBase = settingsFloat;
        settingsBase.help = "The fog is uniform below this height (m) and thins above it.";
        RowSettings settingsMaximum = settingsFloat;
        settingsMaximum.help = "The height (m) where the fog has thinned to a thousandth of its density.";
        RowSettings settingsStart = settingsFloat;
        settingsStart.help = "No fog nearer the camera than this (m).";
        RowSettings settingsMaxDistance = settingsFloat;
        settingsMaxDistance.help = "No more fog past this distance (m) from the camera.";
        RowSettings settingsOpacity = settingsFloat;
        settingsOpacity.help = "The most the fog hides of what is behind it (0..1).";

        beginTable(cpType, labelSize, "height_fog");
        propertyRow(RowPropertyType::Enum, cpType, "colorMode", "Color Mode", sceneProject, entities, settingsColorMode);
        propertyRow(RowPropertyType::Color3L, cpType, "color", "Color", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "attenuationDistance", "Attenuation Distance", sceneProject, entities, settingsAttenuation);
        propertyRow(RowPropertyType::Float, cpType, "baseHeight", "Base Height", sceneProject, entities, settingsBase);
        propertyRow(RowPropertyType::Float, cpType, "maximumHeight", "Maximum Height", sceneProject, entities, settingsMaximum);
        propertyRow(RowPropertyType::FloatPositive, cpType, "startDistance", "Start Distance", sceneProject, entities, settingsStart);
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxFogDistance", "Max Fog Distance", sceneProject, entities, settingsMaxDistance);
        propertyRow(RowPropertyType::Float_0_1, cpType, "maxOpacity", "Max Opacity", sceneProject, entities, settingsOpacity);
        endTable();
        if (fog.colorMode == FogColorMode::SKY && info.horizonSource == "fog_color"){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("The sky has no horizon colour (it is not a cube map and its shader declares no horizonColor): the fog uses its Color.");
        }
        endFoldout();
    }

    if (beginFoldout("Directional Inscattering")){
        RowSettings settingsIntensity = settingsFloat;
        settingsIntensity.help = "How much of the main directional light the fog scatters towards the camera.";
        RowSettings settingsAnisotropy = settingsFloat;
        settingsAnisotropy.help = "-1 scatters the light back towards the sun, 0 evenly, 1 forward (a glow around the sun).";
        beginTable(cpType, labelSize, "fog_inscattering");
        propertyRow(RowPropertyType::FloatPositive, cpType, "directionalInscattering", "Intensity", sceneProject, entities, settingsIntensity);
        propertyRow(RowPropertyType::Float, cpType, "anisotropy", "Anisotropy", sceneProject, entities, settingsAnisotropy);
        endTable();
        endFoldout();
    }

    if (beginFoldout("Sky")){
        RowSettings settingsFill;
        settingsFill.help = "The sky takes the fog's colour near the horizon, so the fogged ground meets it without a seam.";
        RowSettings settingsAngle = settingsFloat;
        settingsAngle.help = "How far above the horizon (degrees) the sky fill reaches.";
        beginTable(cpType, labelSize, "fog_sky");
        propertyRow(RowPropertyType::Bool, cpType, "skyFill", "Sky Fill", sceneProject, entities, settingsFill);
        ImGui::BeginDisabled(!fog.skyFill);
        propertyRow(RowPropertyType::FloatPositive, cpType, "skyFillAngle", "Sky Fill Angle", sceneProject, entities, settingsAngle);
        ImGui::EndDisabled();
        endTable();
        endFoldout();
    }

    if (beginFoldout("Volumetric Fog")){
        RowSettings settingsEnabled;
        settingsEnabled.help = "Raymarches the fog near the camera, with the sun's shadows (light shafts), Local Volumetric Fog volumes and a density "
                               "noise. The main camera only; off at Atmosphere Quality Off.";
        RowSettings settingsDistance = settingsFloat;
        settingsDistance.help = "How far from the camera (m) the fog is raymarched; the analytic fog continues beyond.";
        RowSettings settingsShafts;
        settingsShafts.help = "The main directional light's shadows darken the fog behind objects (light shafts). Needs its shadows on.";
        RowSettings settingsNoise = settingsFloat;
        settingsNoise.help = "How much a 3D noise varies the fog's density (0..1), fading out towards the Distance.";
        RowSettings settingsNoiseScale = settingsFloat;
        settingsNoiseScale.help = "The size (m) of the noise's features.";
        RowSettings settingsWind;
        settingsWind.help = "The speed (m/s) the noise drifts at.";
        RowSettings settingsQuality;
        settingsQuality.enumEntries = &entriesAtmosphereQualityOverride;
        settingsQuality.help = "Caps this fog's tier below the project's Atmosphere Quality (never raises it).";

        beginTable(cpType, labelSize, "fog_volumetric");
        propertyRow(RowPropertyType::Bool, cpType, "volumetric", "Enabled", sceneProject, entities, settingsEnabled);
        ImGui::BeginDisabled(!fog.volumetric);
        propertyRow(RowPropertyType::FloatPositive, cpType, "volumetricDistance", "Distance", sceneProject, entities, settingsDistance);
        propertyRow(RowPropertyType::Bool, cpType, "lightShafts", "Light Shafts", sceneProject, entities, settingsShafts);
        propertyRow(RowPropertyType::Float_0_1, cpType, "noiseIntensity", "Noise Intensity", sceneProject, entities, settingsNoise);
        propertyRow(RowPropertyType::FloatPositive, cpType, "noiseScale", "Noise Scale", sceneProject, entities, settingsNoiseScale);
        propertyRow(RowPropertyType::Vector3, cpType, "wind", "Wind", sceneProject, entities, settingsWind);
        propertyRow(RowPropertyType::Enum, cpType, "qualityOverride", "Quality Override", sceneProject, entities, settingsQuality);
        ImGui::EndDisabled();
        endTable();
        if (fog.volumetric){
            std::string running;
            if (info.volumetricActive){
                running = std::string("Running: ") + AtmosphereTiers::getName(info.quality) + " \xC2\xB7 " + std::to_string(info.fogWidth) + "\xC3\x97" +
                          std::to_string(info.fogHeight) + " \xC2\xB7 " + std::to_string(info.fogSteps) + " steps \xC2\xB7 " +
                          (info.floatTargets ? "RGBA16F" : "RGBA8");
                if (info.forcedDepthPrePass)
                    running += " \xC2\xB7 depth pre-pass";
            }else{
                running = std::string("Not running (Atmosphere Quality ") + AtmosphereTiers::getName(info.quality) + ")";
            }
            ImGui::TextDisabled("%s", running.c_str());
        }
        endFoldout();
    }
}

void editor::InspectorWindow::drawVolumetricCloudsComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    VolumetricCloudsComponent& clouds = sceneProject->scene->getComponent<VolumetricCloudsComponent>(entities[0]);

    RowSettings settingsFloat;
    settingsFloat.secondColSize = 6 * ImGui::GetFontSize();
    const float labelSize = getLabelSize("Ambient Light Dimmer");

    const std::vector<AtmospherePreset>& presets = AtmospherePresets::getCloudsPresets();
    const int presetIndex = std::clamp((int)clouds.preset, 0, (int)presets.size() - 1);
    drawAtmospherePresetsMenu(cpType, sceneProject, entities, presets, presets[presetIndex].name.c_str());

    RowSettings settingsRendering;
    settingsRendering.enumEntries = &entriesCloudsRendering;
    settingsRendering.help = "Sky Map: a map of the sky refreshed over a few frames, seen by every camera, behind all geometry. Screen: marched per "
                             "pixel at quarter resolution, in front of far geometry, the main camera only. Automatic: Sky Map at Low and Medium "
                             "Atmosphere Quality, Screen at High.";
    RowSettings settingsQuality;
    settingsQuality.enumEntries = &entriesAtmosphereQualityOverride;
    settingsQuality.help = "Caps the clouds' tier below the project's Atmosphere Quality (never raises it).";

    beginTable(cpType, labelSize);
    propertyRow(RowPropertyType::Enum, cpType, "rendering", "Rendering", sceneProject, entities, settingsRendering);
    propertyRow(RowPropertyType::Enum, cpType, "qualityOverride", "Quality Override", sceneProject, entities, settingsQuality);
    endTable();

    if (beginFoldout("Shape")){
        RowSettings settingsCoverage = settingsFloat;
        settingsCoverage.help = "The share of the sky the clouds cover (0..1).";
        RowSettings settingsDensity = settingsFloat;
        settingsDensity.help = "How dense the clouds are (0..1).";
        RowSettings settingsShapeScale = settingsFloat;
        settingsShapeScale.help = "The tiling of the cloud shapes: higher makes smaller clouds.";
        RowSettings settingsShapeFactor = settingsFloat;
        settingsShapeFactor.help = "How much the shapes carve the covered area: 0 flat sheets, 1 rounded heaps.";
        RowSettings settingsErosion = settingsFloat;
        settingsErosion.help = "How much the detail noise erodes the clouds' edges (0..1). Not at Low quality.";
        RowSettings settingsErosionScale = settingsFloat;
        settingsErosionScale.help = "The tiling of the detail noise.";
        beginTable(cpType, labelSize, "clouds_shape");
        propertyRow(RowPropertyType::Float_0_1, cpType, "coverage", "Coverage", sceneProject, entities, settingsCoverage);
        propertyRow(RowPropertyType::Float_0_1, cpType, "density", "Density", sceneProject, entities, settingsDensity);
        propertyRow(RowPropertyType::FloatPositive, cpType, "shapeScale", "Shape Scale", sceneProject, entities, settingsShapeScale);
        propertyRow(RowPropertyType::Float_0_1, cpType, "shapeFactor", "Shape Factor", sceneProject, entities, settingsShapeFactor);
        propertyRow(RowPropertyType::Float_0_1, cpType, "erosionFactor", "Erosion Factor", sceneProject, entities, settingsErosion);
        propertyRow(RowPropertyType::FloatPositive, cpType, "erosionScale", "Erosion Scale", sceneProject, entities, settingsErosionScale);
        endTable();
        endFoldout();
    }

    if (beginFoldout("Layer")){
        RowSettings settingsBottom = settingsFloat;
        settingsBottom.help = "The height (m) of the clouds' base.";
        RowSettings settingsRange = settingsFloat;
        settingsRange.help = "The clouds' thickness (m) above their base.";
        RowSettings settingsCurvature = settingsFloat;
        settingsCurvature.help = "How much the layer bends down with the earth's curvature towards the horizon (0 flat, 1 the earth's).";
        RowSettings settingsMaxDistance = settingsFloat;
        settingsMaxDistance.help = "Clouds farther than this (km) are not drawn.";
        beginTable(cpType, labelSize, "clouds_layer");
        propertyRow(RowPropertyType::Float, cpType, "bottomAltitude", "Bottom Altitude", sceneProject, entities, settingsBottom);
        propertyRow(RowPropertyType::FloatPositive, cpType, "altitudeRange", "Altitude Range", sceneProject, entities, settingsRange);
        propertyRow(RowPropertyType::Float_0_1, cpType, "earthCurvature", "Earth Curvature", sceneProject, entities, settingsCurvature);
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxDistance", "Max Distance", sceneProject, entities, settingsMaxDistance);
        endTable();
        endFoldout();
    }

    if (beginFoldout("Weather")){
        RowSettings settingsMap;
        settingsMap.allowCameraTexture = false;
        settingsMap.allowTextureSettings = false;
        settingsMap.help = "Where the clouds are (red: coverage) and their kind (green: 0 flat stratus .. 1 towering cumulonimbus), tiled over the "
                           "Weather Map Scale. Empty: a built-in map.";
        RowSettings settingsScale = settingsFloat;
        settingsScale.help = "The distance (km) the weather map spans before it repeats.";
        RowSettings settingsDirection = settingsFloat;
        settingsDirection.help = "The direction the wind blows towards, in degrees about the up axis (0 = +Z).";
        RowSettings settingsSpeed = settingsFloat;
        settingsSpeed.help = "The wind's speed (m/s).";
        beginTable(cpType, labelSize, "clouds_weather");
        propertyRow(RowPropertyType::Texture, cpType, "weatherMap", "Weather Map", sceneProject, entities, settingsMap);
        propertyRow(RowPropertyType::FloatPositive, cpType, "weatherMapScale", "Weather Map Scale", sceneProject, entities, settingsScale);
        propertyRow(RowPropertyType::Float, cpType, "windDirection", "Wind Direction", sceneProject, entities, settingsDirection);
        propertyRow(RowPropertyType::FloatPositive, cpType, "windSpeed", "Wind Speed", sceneProject, entities, settingsSpeed);
        endTable();
        endFoldout();
    }

    if (beginFoldout("Lighting")){
        RowSettings settingsSun = settingsFloat;
        settingsSun.help = "Scales the main directional light on the clouds.";
        RowSettings settingsAmbient = settingsFloat;
        settingsAmbient.help = "Scales the scene's ambient light on the clouds.";
        beginTable(cpType, labelSize, "clouds_lighting");
        propertyRow(RowPropertyType::FloatPositive, cpType, "sunLightDimmer", "Sun Light Dimmer", sceneProject, entities, settingsSun);
        propertyRow(RowPropertyType::FloatPositive, cpType, "ambientLightDimmer", "Ambient Light Dimmer", sceneProject, entities, settingsAmbient);
        endTable();
        endFoldout();
    }

    // the first clouds of the scene are drawn (as the Skybox)
    auto all = sceneProject->scene->getComponentArray<VolumetricCloudsComponent>();
    if (all->size() > 1 && all->getEntity(0) != entities[0]){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("A scene draws its first Volumetric Clouds only: these are not drawn.");
    }
}

void editor::InspectorWindow::drawUIDocumentComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    UIDocumentComponent& document = sceneProject->scene->getComponent<UIDocumentComponent>(entities[0]);
    UIDocumentSystem* documents = sceneProject->scene->getSystem<UIDocumentSystem>().get();

    RowSettings sourceSettings;
    sourceSettings.help = "HTML file of the document, relative to the assets folder (e.g. ui/menu.html).";

    RowSettings sortSettings;
    sortSettings.help = "Documents with a higher sort order draw on top of the scene's other documents.";

    RowSettings interactiveSettings;
    interactiveSettings.help = "Receives pointer, keyboard and text input. Off: the game gets every event.";

    RowSettings modeSettings;
    modeSettings.enumEntries = &entriesUIDocumentRenderMode;
    modeSettings.help = "Screen: drawn over the scene. Texture: rendered into a texture that a material, Mesh or Image shows.";

    RowSettings sizeSettings;
    sizeSettings.intMin = 1;
    sizeSettings.intMax = 8192;
    sizeSettings.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings surfaceSettings;
    surfaceSettings.help = "Mesh whose surface receives the pointer (hit test on its UVs). None: this GameObject's own mesh.";

    RowSettings ratioSettings;
    ratioSettings.secondColSize = 6 * ImGui::GetFontSize();
    ratioSettings.stepSize = 0.05f;
    // ImGui prints a format without a conversion as it is
    ratioSettings.format = (document.pixelRatio == 0.0f) ? "Auto" : "%.2f";
    ratioSettings.help = "Document pixels per CSS pixel. 0 = Auto: the canvas-to-screen scale (1 in Texture mode).";

    beginTable(cpType, getLabelSize("Texture Height"));

    if (entities.size() == 1) {
        Entity entity = entities[0];

        auto setDocumentFilename = [&](const std::filesystem::path& selectedPath) -> bool {
            std::filesystem::path assetsPath = std::filesystem::absolute(project->getAssetsPath()).lexically_normal();
            std::filesystem::path filePath = std::filesystem::absolute(project->resolveAssetPath(selectedPath)).lexically_normal();

            std::error_code errorCode;
            std::filesystem::path relative = std::filesystem::relative(filePath, assetsPath, errorCode);
            const std::string relativePath = relative.generic_string();
            if (errorCode || relative.empty() || *relative.begin() == "..") {
                ImGui::OpenPopup("HTML Document Error");
                return false;
            }
            if (!Util::isHtmlFile(relativePath)) {
                return false;
            }

            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "filename", relativePath)
            );
            return true;
        };

        const std::string currentPath = document.filename;

        prefabRowProperty = "filename";
        if (propertyHeader("Source", -1, !currentPath.empty())) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "filename", "")
            );
        }

        const std::string displayName = currentPath.empty() ? "< Not set >" : std::filesystem::path(currentPath).filename().string();
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2;

        ImGui::BeginGroup();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);
        ImGui::BeginChild("uidocumentfilename", ImVec2(availWidth - buttonWidth - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFrameHeight()),
            false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float textWidth = ImGui::CalcTextSize(displayName.c_str()).x;
        const float childAvail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(std::max(0.0f, childAvail - textWidth - 2));
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        if (currentPath.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Text("%s", displayName.c_str());
        if (currentPath.empty())
            ImGui::PopStyleColor();
        ImGui::EndChild();
        if (!currentPath.empty()){
            ImGui::SetItemTooltip("%s", currentPath.c_str());
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##uidocument_load")) {
            std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_HTML);
            if (!path.empty()) {
                setDocumentFilename(path);
            }
        }

        ImGui::EndGroup();

        if (sceneProject->playState == ScenePlayState::STOPPED) {
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                    std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
                    if (!receivedStrings.empty()) {
                        std::filesystem::path droppedPath(receivedStrings[0]);
                        std::filesystem::path filePath = project->resolveAssetPath(droppedPath);
                        std::error_code errorCode;
                        std::filesystem::path relative = std::filesystem::relative(std::filesystem::absolute(filePath), std::filesystem::absolute(project->getAssetsPath()), errorCode);
                        if (!errorCode && Util::isHtmlFile(relative.generic_string()) && payload->IsDelivery()) {
                            if (setDocumentFilename(droppedPath)) {
                                ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                            }
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        if (EditorModal::begin("HTML Document Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Selected file must be an .html, .htm or .rml file within the assets directory.");
            EditorModal::message("Selected file must be an .html, .htm or .rml file within the assets directory.");
            ImGui::Separator();
            const float buttonModalWidth = 120;
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - buttonModalWidth) * 0.5f);
            if (EditorModal::button("OK", "ok", ImVec2(buttonModalWidth, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Open in the code editor, Reload keeping the document's state
        propertyHeader("");
        ImGui::BeginDisabled(currentPath.empty());
        if (ImGui::Button(ICON_FA_FILE_CODE " Open##uidocument_open")) {
            if (CodeEditor* codeEditor = Backend::getApp().getCodeEditor())
                codeEditor->openFile((project->getAssetsPath() / currentPath).string(), true);
        }
        ImGui::SetItemTooltip("Edit the document in the code editor");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE " Reload##uidocument_reload")) {
            if (documents) documents->reloadDocument(entity, true);
            sceneProject->needUpdateRender = true;
        }
        ImGui::SetItemTooltip("Patch the document with its file: elements, Lua handles, listeners, form values, scroll and focus stay "
                              "(saved files reload by themselves; the component menu's Reload (Rebuild) loads it again)");
        ImGui::EndDisabled();
    }else{
        propertyRow(RowPropertyType::Asset, cpType, "filename", "Source", sceneProject, entities, sourceSettings);
    }

    propertyRow(RowPropertyType::Int, cpType, "sortOrder", "Sort Order", sceneProject, entities, sortSettings);
    propertyRow(RowPropertyType::Bool, cpType, "interactive", "Interactive", sceneProject, entities, interactiveSettings);
    propertyRow(RowPropertyType::Enum, cpType, "renderMode", "Render Mode", sceneProject, entities, modeSettings);
    if (document.renderMode == UIDocumentRenderMode::TEXTURE) {
        propertyRow(RowPropertyType::Int, cpType, "textureWidth", "Texture Width", sceneProject, entities, sizeSettings);
        propertyRow(RowPropertyType::Int, cpType, "textureHeight", "Texture Height", sceneProject, entities, sizeSettings);
        propertyRow(RowPropertyType::LocalEntity, cpType, "inputSurface", "Input Surface", sceneProject, entities, surfaceSettings);
    }
    propertyRow(RowPropertyType::FloatPositive, cpType, "pixelRatio", "Pixel Ratio", sceneProject, entities, ratioSettings);

    endTable();

    if (entities.size() != 1 || !documents) {
        return;
    }
    const Entity entity = entities[0];

    // the RmlUi debugger is global: showing it here hides it in the scene that had it
    const bool debuggerShown = documents->isDebuggerVisible();
    if (ImGui::Button(debuggerShown ? ICON_FA_BUG " Hide HTML UI Debugger##uidocument_debugger" : ICON_FA_BUG " Show HTML UI Debugger##uidocument_debugger")) {
        documents->setDebuggerVisible(!debuggerShown);
        sceneProject->needUpdateRender = true;
    }
    ImGui::SetItemTooltip("RmlUi's element inspector and log, drawn over this scene's documents");

    // remote URLs (imports) and the compat diagnostics of the document
    htmlui::HtmlUiImportPanel::draw(project, sceneProject, entity, document.filename, documents);

    // what RmlUi said while loading this document (and since), with the file and line it gave
    const std::vector<HtmlUIMessage>& messages = documents->getDocumentMessages(entity);
    std::string header = "Messages";
    if (!messages.empty()) header += " (" + std::to_string(messages.size()) + ")";
    separatorText(header.c_str());
    if (messages.empty()) {
        ImGui::TextDisabled(document.filename.empty() ? "No document" : (documents->isDocumentLoaded(entity) ? "No warnings or errors" : "Not loaded yet"));
        return;
    }

    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float height = std::min((float)messages.size(), 8.0f) * lineHeight + ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::BeginChild("##uidocument_messages", ImVec2(0, height), ImGuiChildFlags_Borders);
    for (const HtmlUIMessage& message : messages) {
        const bool error = message.level == HtmlUIMessageLevel::Error;
        ImGui::PushStyleColor(ImGuiCol_Text, error ? Theme::Colors::ErrorText : Theme::Colors::WarningText);
        ImGui::TextUnformatted(error ? ICON_FA_CIRCLE_EXCLAMATION : ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", message.text.c_str());
        if (!message.file.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s:%d", message.file.c_str(), message.line);
        }
    }
    ImGui::EndChild();
}

void editor::InspectorWindow::drawPoseAnimatorComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    PoseAnimatorComponent& animator = sceneProject->scene->getComponent<PoseAnimatorComponent>(entities[0]);

    RowSettings animationSettings;
    animationSettings.help = "Pose animation file (.poseanim), relative to the assets folder.";

    RowSettings autoplaySettings;
    autoplaySettings.help = "Plays when the game starts. Off: a script calls play() or goToPose().";

    RowSettings speedSettings;
    speedSettings.secondColSize = 6 * ImGui::GetFontSize();
    speedSettings.help = "Playback speed: 1 = authored timing, negative plays backwards.";

    RowSettings weightSettings;
    weightSettings.secondColSize = 6 * ImGui::GetFontSize();
    weightSettings.help = "Blend over the Model's animation clips (or its rest pose): 0 = the clips only, 1 = the pose animation only.";

    beginTable(cpType, getLabelSize("Animation"));

    if (entities.size() == 1) {
        Entity entity = entities[0];

        auto setAnimationPath = [&](const std::filesystem::path& selectedPath) -> bool {
            std::filesystem::path assetsPath = std::filesystem::absolute(project->getAssetsPath()).lexically_normal();
            std::filesystem::path filePath = std::filesystem::absolute(project->resolveAssetPath(selectedPath)).lexically_normal();

            std::error_code errorCode;
            std::filesystem::path relative = std::filesystem::relative(filePath, assetsPath, errorCode);
            const std::string relativePath = relative.generic_string();
            if (errorCode || relative.empty() || *relative.begin() == ".." || !Util::isPoseAnimationFile(relativePath)) {
                ImGui::OpenPopup("Pose Animation Error");
                return false;
            }

            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "animation", relativePath)
            );
            return true;
        };

        const std::string currentPath = animator.animation;

        prefabRowProperty = "animation";
        if (propertyHeader("Animation", -1, !currentPath.empty())) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "animation", "")
            );
        }

        const std::string displayName = currentPath.empty() ? "< Not set >" : std::filesystem::path(currentPath).filename().string();
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2;

        ImGui::BeginGroup();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);
        ImGui::BeginChild("poseanimatorfile", ImVec2(availWidth - buttonWidth - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFrameHeight()),
            false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float textWidth = ImGui::CalcTextSize(displayName.c_str()).x;
        const float childAvail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(std::max(0.0f, childAvail - textWidth - 2));
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        if (currentPath.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Text("%s", displayName.c_str());
        if (currentPath.empty())
            ImGui::PopStyleColor();
        ImGui::EndChild();
        if (!currentPath.empty()){
            ImGui::SetItemTooltip("%s", currentPath.c_str());
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##poseanimator_load")) {
            std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_POSE_ANIMATION);
            if (!path.empty()) {
                setAnimationPath(path);
            }
        }

        ImGui::EndGroup();

        if (sceneProject->playState == ScenePlayState::STOPPED) {
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                    std::vector<std::string> receivedStrings = editor::Util::getStringsFromPayload(payload);
                    if (!receivedStrings.empty() && Util::isPoseAnimationFile(receivedStrings[0]) && payload->IsDelivery()) {
                        if (setAnimationPath(std::filesystem::path(receivedStrings[0]))) {
                            ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        if (EditorModal::begin("Pose Animation Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Selected file must be a .poseanim file within the assets directory.");
            EditorModal::message("Selected file must be a .poseanim file within the assets directory.");
            ImGui::Separator();
            const float buttonModalWidth = 120;
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - buttonModalWidth) * 0.5f);
            if (EditorModal::button("OK", "ok", ImVec2(buttonModalWidth, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }else{
        propertyRow(RowPropertyType::String, cpType, "animation", "Animation", sceneProject, entities, animationSettings);
    }

    propertyRow(RowPropertyType::Bool, cpType, "autoplay", "Autoplay", sceneProject, entities, autoplaySettings);
    propertyRow(RowPropertyType::Float, cpType, "speed", "Speed", sceneProject, entities, speedSettings);
    propertyRow(RowPropertyType::Float_0_1, cpType, "weight", "Weight", sceneProject, entities, weightSettings);

    // Bake (pose baker) and Open (Pose Animation window): shown once the editor tasks hook them in
    const PoseInspectorHooks& hooks = poseInspectorHooks();
    if (entities.size() == 1 && (hooks.bake || hooks.open)) {
        const Entity entity = entities[0];
        const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
        propertyHeader("");
        ImGui::BeginDisabled(animator.animation.empty());
        if (hooks.open) {
            if (ImGui::Button(ICON_FA_PERSON_RUNNING " Open##poseanimator_open")) {
                hooks.open(project, sceneProject->id, entity, animator.animation);
            }
            ImGui::SetItemTooltip("Edit the pose animation in the Pose Animation window");
            if (hooks.bake) ImGui::SameLine();
        }
        if (hooks.bake) {
            // as the Pose Animation window's Bake: to a shared Animation Clip (primary) or into the scene
            ImGui::BeginDisabled(!stopped);
            if (ImGui::Button(ICON_FA_FILM " Bake " ICON_FA_CARET_DOWN "##poseanimator_bake")) {
                ImGui::OpenPopup("##poseanimator_bake_menu");
            }
            UIProbe::item("poseanimator.bake");
            ImGui::SetItemTooltip("Bake the pose animation to an Animation Clip (.anim, shared) or into the scene (one undo step)");
            if (ImGui::BeginPopup("##poseanimator_bake_menu")) {
                if (hooks.bakeToClip) {
                    if (ImGui::MenuItem(ICON_FA_FILM " Bake to Animation Clip")) {
                        hooks.bakeToClip(project, sceneProject->id, entity, animator.animation);
                    }
                    UIProbe::item("poseanimator.bakeMenu.clip");
                    ImGui::SetItemTooltip("Writes <name>.anim next to the pose animation (baking again updates it) and lists it in the\n"
                                          "Model's Animation Player: one clip any character with the same bones can play.");
                }
                if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT " Bake into Scene")) {
                    hooks.bake(project, sceneProject->id, entity, animator.animation);
                }
                UIProbe::item("poseanimator.bakeMenu.scene");
                ImGui::SetItemTooltip("Bake to an Animation of this Model in the scene (baking again updates it).\n"
                                      "Plays with Model:playAnimation(name) and in the Animation window.");
                VatBakeEntry vatEntry;
                vatEntry.kind = VatBakeEntry::Kind::PoseAnimator;
                vatEntry.sceneId = sceneProject->id;
                vatEntry.entity = entity;
                vatEntry.asset = animator.animation;
                vatbakeentry::menuItem(project, vatEntry, stopped, "poseanimator.bakeMenu.vat");
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
    }

    endTable();

    // what the animator does while playing
    if (entities.size() == 1 && sceneProject->playState != ScenePlayState::STOPPED) {
        const pose::PoseAnimatorRuntime* runtime = animator.runtime.get();
        if (runtime && runtime->failed) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", runtime->error.c_str());
        }else if (runtime && runtime->isLoaded()) {
            ImGui::TextDisabled("%s  %.2f / %.2f s", animator.playing ? "Playing" : (runtime->transitionActive && !runtime->transitionArrived ? "Go to pose" : "Paused"),
                                animator.time, runtime->length);
        }else if (!animator.animation.empty()) {
            ImGui::TextDisabled("Loading...");
        }
    }
}

void editor::InspectorWindow::drawReflectionProbeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    ReflectionProbeComponent& probe = sceneProject->scene->getComponent<ReflectionProbeComponent>(entities[0]);

    RowSettings modeSettings;
    modeSettings.enumEntries = &entriesReflectionProbeMode;
    RowSettings updateSettings;
    updateSettings.enumEntries = &entriesReflectionProbeUpdateMode;
    RowSettings resolutionSettings;
    resolutionSettings.sliderValues = &probeResolutionValues;
    RowSettings floatSettings;
    floatSettings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings boxOffsetSettings;
    boxOffsetSettings.help = "Moves the influence box in local space; the cubemap is still captured at the GameObject's origin. Large offsets can increase box-projection distortion near the volume edges.";
    RowSettings blendSettings = floatSettings;
    blendSettings.help = "Runtime blending is limited to the influence box's smallest world-space half-extent. The authored value is preserved so later box or scale changes can restore it.";

    beginTable(cpType, getLabelSize("Update Interval"), "reflection_probe_main");
    propertyRow(RowPropertyType::Enum, cpType, "mode", "Mode", sceneProject, entities, modeSettings);
    if (probe.mode == ReflectionProbeMode::DYNAMIC){
        propertyRow(RowPropertyType::Enum, cpType, "updateMode", "Update", sceneProject, entities, updateSettings);
        if (probe.updateMode == ReflectionProbeUpdateMode::INTERVAL){
            propertyRow(RowPropertyType::FloatPositive, cpType, "updateInterval", "Update Interval", sceneProject, entities, floatSettings);
        }
    }else{
        propertyRow(RowPropertyType::TextureCube, cpType, "texture", "Cubemap", sceneProject, entities);
    }
    propertyRow(RowPropertyType::FloatPositive, cpType, "intensity", "Intensity", sceneProject, entities, floatSettings);
    propertyRow(RowPropertyType::Int, cpType, "priority", "Priority", sceneProject, entities);
    endTable();

    separatorText("Influence");
    beginTable(cpType, getLabelSize("Blend Distance"), "reflection_probe_influence");
    propertyRow(RowPropertyType::Vector3, cpType, "boxOffset", "Box Offset", sceneProject, entities, boxOffsetSettings);
    propertyRow(RowPropertyType::Vector3, cpType, "boxSize", "Box Size", sceneProject, entities);
    propertyRow(RowPropertyType::FloatPositive, cpType, "blendDistance", "Blend Distance", sceneProject, entities, blendSettings);
    endTable();

    if (entities.size() == 1){
        Transform* transform = sceneProject->scene->findComponent<Transform>(entities[0]);
        if (transform){
            Vector3 half(
                std::fabs(probe.boxSize.x * transform->worldScale.x) * 0.5f,
                std::fabs(probe.boxSize.y * transform->worldScale.y) * 0.5f,
                std::fabs(probe.boxSize.z * transform->worldScale.z) * 0.5f);
            float maxBlendDistance = std::max(0.0f, std::min(half.x, std::min(half.y, half.z)));
            if (probe.blendDistance > maxBlendDistance + 0.0001f){
                ImGui::TextDisabled("Effective blend distance: %.2f (limited by box size)", maxBlendDistance);
            }
        }
    }

    separatorText("Capture");
    beginTable(cpType, std::max(getLabelSize("Resolution"), getLabelSize("Culling Mask")), "reflection_probe_capture");
    propertyRow(RowPropertyType::UIntSlider, cpType, "resolution", "Resolution", sceneProject, entities, resolutionSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "nearClip", "Near", sceneProject, entities, floatSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "farClip", "Far", sceneProject, entities, floatSettings);
    propertyRow(RowPropertyType::Bool, cpType, "includeSky", "Include Skybox", sceneProject, entities);
    propertyRow(RowPropertyType::LayerMask, cpType, "cullingMask", "Culling Mask", sceneProject, entities);
    endTable();

    if (entities.size() == 1 && ImGui::Button(ICON_FA_ARROWS_ROTATE " Refresh Probe", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
        probe.needUpdate = true;
        probe.captureRevision++;
    }
}

// The value of a script property of type Prefab (Unity's GameObject field holding a prefab):
// the prefab's name, a drop target for a .prefab from the Project window, and a button back
// to None. Kept by GUID (the path is a hint), so it follows the prefab when it is renamed or
// moved.
static void drawPrefabScriptProperty(editor::Project* project, editor::SceneProject* sceneProject, const std::vector<Entity>& entities,
        editor::ComponentType cpType, const std::string& propertyId,
        const std::function<void()>& onValueChanged, editor::Command*& cmd, bool& finishProperty) {
    using namespace editor;

    std::vector<PrefabAsset*> values;
    for (Entity entity : entities) {
        PropertyData data = Catalog::findProperty(sceneProject->scene, entity, cpType, propertyId);
        if (data.ref) {
            values.push_back(static_cast<PrefabAsset*>(data.ref));
        }
    }
    if (values.empty()) {
        return;
    }
    bool mixed = false;
    for (const PrefabAsset* value : values) {
        if (value->getGuid() != values[0]->getGuid() || value->getPath() != values[0]->getPath()) {
            mixed = true;
        }
    }
    const PrefabAsset& value = *values[0];

    std::string label;
    std::string tooltip;
    if (mixed) {
        label = "-";
    } else if (value.isEmpty()) {
        label = "None (Prefab)";
    } else {
        fs::path resolved = project->resolvePrefabPath(value.getGuid(), value.getPath());
        if (resolved.empty()) {
            label = "Missing (" + value.getName() + ")";
            tooltip = "No prefab has this GUID: " + value.getGuid() + "\nLast path: " + value.getPath();
        } else {
            label = resolved.stem().string();
            tooltip = resolved.generic_string();
        }
    }

    auto assign = [&](const PrefabAsset& newValue) {
        for (Entity entity : entities) {
            cmd = new PropertyCmd<PrefabAsset>(project, sceneProject->id, entity, cpType, propertyId, newValue, onValueChanged);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        finishProperty = true;
    };

    const float clearWidth = ImGui::GetFrameHeight();
    const float fieldWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - clearWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushID(propertyId.c_str());
    ImGui::Button((std::string(ICON_FA_CUBE " ") + label + "##prefab_field").c_str(), ImVec2(fieldWidth, 0));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip.empty() ? "Drag a prefab here from the Project window" : tooltip.c_str());
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty()) {
                fs::path relative = project->normalizeToProjectRelative(dropped[0]);
                if (!relative.empty() && Util::isPrefabFile(relative.string())) {
                    assign(PrefabAsset(project->getPrefabGuid(relative), relative.generic_string()));
                    ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                } else {
                    Out::warning("Only a prefab (.prefab) of this project can go in a Prefab property");
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!mixed && value.isEmpty());
    if (ImGui::Button(ICON_FA_XMARK "##prefab_clear", ImVec2(clearWidth, 0))) {
        assign(PrefabAsset());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Set to None");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

// The value of a script property of type Asset (S04): the asset's name, a drop target for a
// file of the property's asset type, and a button back to None. Kept by GUID (the path is a
// hint); a missing asset shows red with its last path, and stays until it is cleared (R28).
static void drawAssetScriptProperty(editor::Project* project, editor::SceneProject* sceneProject, const std::vector<Entity>& entities,
        editor::ComponentType cpType, const std::string& propertyId, const std::string& assetType,
        const std::function<void()>& onValueChanged, editor::Command*& cmd, bool& finishProperty) {
    using namespace editor;

    std::vector<AssetRef*> values;
    for (Entity entity : entities) {
        PropertyData data = Catalog::findProperty(sceneProject->scene, entity, cpType, propertyId);
        if (data.ref && data.type == PropertyType::Asset) {
            values.push_back(static_cast<AssetRef*>(data.ref));
        }
    }
    if (values.empty()) {
        return;
    }
    bool mixed = false;
    for (const AssetRef* value : values) {
        mixed |= *value != *values[0];
    }
    const AssetRef& value = *values[0];
    const AssetType expected = assetTypeFromName(assetType);
    const std::string typeLabel = assetType.empty() ? std::string("Asset") : assetType;

    std::string label;
    std::string tooltip;
    bool missing = false;
    if (mixed) {
        label = "-";
    } else if (!value.isSet()) {
        label = "None (" + typeLabel + ")";
    } else {
        const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(value.guid, value.path, expected, true);
        if (!found.found()) {
            missing = true;
            label = "Missing (" + fs::path(value.path).filename().string() + ")";
            tooltip = "Missing (GUID " + value.guid.str() + ")\nLast path: " + value.path;
        } else {
            label = fs::path(found.path).filename().string();
            tooltip = found.path + "\nGUID " + found.guid.str();
        }
    }

    auto assign = [&](const AssetRef& newValue) {
        for (Entity entity : entities) {
            cmd = new PropertyCmd<AssetRef>(project, sceneProject->id, entity, cpType, propertyId, newValue, onValueChanged);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        finishProperty = true;
    };

    const float clearWidth = ImGui::GetFrameHeight();
    const float fieldWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - clearWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushID(propertyId.c_str());
    if (missing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
    }
    ImGui::Button((std::string(ICON_FA_FILE " ") + label + "##asset_field").c_str(), ImVec2(fieldWidth, 0));
    if (missing) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip.empty() ? ("Drag a " + typeLabel + " here from the Project window").c_str() : tooltip.c_str());
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty()) {
                const std::string relative = project->normalizeToProjectRelative(dropped[0]).generic_string();
                const AssetDatabase::Resolution found = project->getAssetDatabase().lookupPath(relative);
                if (found.found() && (expected == AssetType::Unknown || found.type == expected)) {
                    assign(AssetRef(relative, found.guid));
                    ImGui::SetWindowFocus(InspectorWindow::WINDOW_NAME);
                } else {
                    Out::warning("Only a %s asset of this project can go in this property", typeLabel.c_str());
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!mixed && !value.isSet());
    if (ImGui::Button(ICON_FA_XMARK "##asset_clear", ImVec2(clearWidth, 0))) {
        assign(AssetRef());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Set to None");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void editor::InspectorWindow::drawScriptComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    if (entities.empty()) return;

    ScriptComponent& firstScriptComp = sceneProject->scene->getComponent<ScriptComponent>(entities[0]);

    // Identify common scripts by index
    std::vector<size_t> commonIndices;
    for (size_t i = 0; i < firstScriptComp.scripts.size(); i++) {
        const ScriptEntry& refScript = firstScriptComp.scripts[i];
        bool isCommon = true;
        for (size_t e = 1; e < entities.size(); e++) {
            ScriptComponent& otherScriptComp = sceneProject->scene->getComponent<ScriptComponent>(entities[e]);
            if (i >= otherScriptComp.scripts.size()) {
                isCommon = false;
                break;
            }
            const ScriptEntry& otherScript = otherScriptComp.scripts[i];
            // headerPath is where the properties come from, so entries not sharing it differ
            if (refScript.type != otherScript.type || 
                refScript.className != otherScript.className || 
                refScript.path != otherScript.path ||
                refScript.headerPath != otherScript.headerPath) {
                isCommon = false;
                break;
            }
        }
        if (isCommon) {
            commonIndices.push_back(i);
        }
    }

    beginTable(cpType, getLabelSize("Scripts"), "script_component_header");
    prefabRowProperty = "scripts";
    propertyHeader("Scripts", -1, false, false);
    ImGui::Text("%zu", firstScriptComp.scripts.size());
    const bool playing = project->isAnyScenePlaying();
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Script##script_add", 0.0f, !playing, "Stop Play to add scripts")) {
        ImGui::OpenPopup("script_add_menu");
    }
    UIProbe::item("inspector.addScript");
    drawAddScriptPopup(sceneProject, entities);
    endTable();

    if (commonIndices.empty()) {
        if (entities.size() > 1) {
            ImGui::TextDisabled("Select a single GameObject to view script details");
        }
        return;
    }

    bool removedScriptThisFrame = false;

    for (size_t scriptIdx : commonIndices) {
        if (removedScriptThisFrame) break; // Avoid using invalidated references after removal

        ScriptEntry& script = firstScriptComp.scripts[scriptIdx];

        ImGui::PushID(static_cast<int>(scriptIdx));

        std::string scriptLabel = script.className.empty() ? "Unnamed Script" : script.className;
        std::string typeLabel;
        if (script.type == ScriptType::CPP) {
            typeLabel = " [C++]";
        } else if (script.type == ScriptType::LUA) {
            typeLabel = " [Lua]";
        }

        const float indentation = 10.0f;

        // Indent to show scripts are nested inside ScriptComponent
        ImGui::Indent(indentation);

        // Custom styling for script headers
        ImGui::PushStyleColor(ImGuiCol_Header, Theme::Colors::NestedHeader);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Colors::NestedHeaderHovered);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::Colors::NestedHeaderActive);

        // Add icon to distinguish from component headers
        std::string headerText = ICON_FA_FILE_CODE " " + scriptLabel + typeLabel;

        // AllowOverlap so the trailing trash button can receive clicks
        bool headerOpen = ImGui::CollapsingHeader(
            headerText.c_str(),
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

        const ImVec2 headerMin = ImGui::GetItemRectMin();
        const ImVec2 headerMax = ImGui::GetItemRectMax();
        const ImVec2 cursorAfterHeader = ImGui::GetCursorPos();

        auto removeScriptAtIndex = [&]() {
            for (const Entity& entity : entities) {
                ScriptComponent& sc = sceneProject->scene->getComponent<ScriptComponent>(entity);
                std::vector<ScriptEntry> newScripts = sc.scripts;
                if (scriptIdx < newScripts.size()) {
                    newScripts.erase(newScripts.begin() + scriptIdx);
                    project->updateScriptProperties(sceneProject, entity, newScripts);
                    cmd = new PropertyCmd<std::vector<ScriptEntry>>(project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", newScripts);
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                }
            }
            if (cmd) cmd->setNoMerge();
            removedScriptThisFrame = true;
        };

        // Context menu must run while the header is still the last item
        if (ImGui::BeginPopupContextItem(("script_options_menu_" + std::to_string(scriptIdx)).c_str())) {
            ImGui::TextDisabled("Script options");
            ImGui::Separator();

            bool canMoveUp = scriptIdx > 0;
            if (ImGui::MenuItem(ICON_FA_ARROW_UP " Move Up", nullptr, false, canMoveUp)) {
                for (const Entity& entity : entities) {
                    ScriptComponent& sc = sceneProject->scene->getComponent<ScriptComponent>(entity);
                    std::vector<ScriptEntry> newScripts = sc.scripts;
                    if (scriptIdx < newScripts.size() && scriptIdx > 0) {
                        std::swap(newScripts[scriptIdx], newScripts[scriptIdx - 1]);

                        project->updateScriptProperties(sceneProject, entity, newScripts);

                        cmd = new PropertyCmd<std::vector<ScriptEntry>>(project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", newScripts);
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    }
                }
                if (cmd) cmd->setNoMerge();
            }

            bool canMoveDown = scriptIdx < firstScriptComp.scripts.size() - 1;
            if (ImGui::MenuItem(ICON_FA_ARROW_DOWN " Move Down", nullptr, false, canMoveDown)) {
                for (const Entity& entity : entities) {
                    ScriptComponent& sc = sceneProject->scene->getComponent<ScriptComponent>(entity);
                    std::vector<ScriptEntry> newScripts = sc.scripts;
                    if (scriptIdx < newScripts.size() - 1) {
                        std::swap(newScripts[scriptIdx], newScripts[scriptIdx + 1]);

                        project->updateScriptProperties(sceneProject, entity, newScripts);

                        cmd = new PropertyCmd<std::vector<ScriptEntry>>(project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", newScripts);
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    }
                }
                if (cmd) cmd->setNoMerge();
            }

            if (ImGui::MenuItem(ICON_FA_TRASH " Remove")) {
                removeScriptAtIndex();
            }

            ImGui::EndPopup();
        }

        // Trash button overlaid on the right of the collapsing header
        if (!removedScriptThisFrame) {
            const float framePadX = ImGui::GetStyle().FramePadding.x / 4.0f;
            const ImVec2 deleteButtonSize(ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + framePadX * 2.0f, 0.0f);

            ImGui::SetCursorScreenPos(ImVec2(
                headerMax.x - deleteButtonSize.x,
                headerMin.y + (headerMax.y - headerMin.y - ImGui::GetFrameHeight()) * 0.5f));

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(framePadX, ImGui::GetStyle().FramePadding.y));
            const bool removeClicked = ImGui::Button(ICON_FA_TRASH_CAN "##delete_script", deleteButtonSize);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove script");
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            if (removeClicked) {
                removeScriptAtIndex();
            }
        }

        if (headerOpen && !removedScriptThisFrame) {
            ImGui::SetCursorPos(cursorAfterHeader);
            ImGui::Unindent(indentation); // Unindent for content

            beginTable(cpType, getLabelSize("Script", false), "script_" + std::to_string(scriptIdx));

            // Path and enabled status
            propertyHeader("Script");

            // Enabled Checkbox Logic
            bool allEnabled = true;
            bool anyEnabled = false;
            for (const Entity& e : entities) {
                bool en = sceneProject->scene->getComponent<ScriptComponent>(e).scripts[scriptIdx].enabled;
                if (en) anyEnabled = true;
                else allEnabled = false;
            }

            bool enabled = allEnabled;
            bool mixed = anyEnabled && !allEnabled;

            if (mixed) ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::Checkbox(("##script_enabled_" + std::to_string(scriptIdx)).c_str(), &enabled);
            UIProbe::item("inspector.scriptEnabled." + std::to_string(scriptIdx));
            if (mixed) ImGui::PopStyleColor();

            if (ImGui::IsItemDeactivatedAfterEdit()) {
                // after this frame's drawing: in Play it runs onEnable / onDisable, which may move
                // the components `script` and the loop below point into (deferredCommands)
                const std::string propName = "scripts[" + std::to_string(scriptIdx) + "].enabled";
                const uint32_t commandSceneId = project->getSelectedSceneId();
                const uint32_t stateSceneId = sceneProject->id;
                const std::vector<Entity> targets = entities;
                deferredCommands.push_back([this, propName, commandSceneId, stateSceneId, targets, enabled]() {
                    if (!project->getScene(stateSceneId)) return;
                    Command* last = nullptr;
                    for (const Entity& entity : targets) {
                        last = new PropertyCmd<bool>(project, stateSceneId, entity, ComponentType::ScriptComponent, propName, enabled);
                        CommandHandle::get(commandSceneId)->addCommand(last);
                    }
                    if (last) last->setNoMerge();
                });
            }

            ImGui::SameLine();
            ImGui::TextUnformatted(script.className.c_str());
            if (script.path.empty() && script.headerPath.empty()) {
                // an entry saved without a file (Add Script made them once): it runs nothing
                ImGui::SameLine();
                ImGui::TextColored(Theme::Colors::WarningText, "No file: pick one or remove the entry");
            }

            // in Play, where the script is in its lifecycle (as get_runtime_entity reports it)
            if (entities.size() == 1 && ScriptRuntime::hasStarted(sceneProject->scene)) {
                const ScriptEntry& live = sceneProject->scene->getComponent<ScriptComponent>(entities[0]).scripts[scriptIdx];
                const ScriptState* state = ScriptRuntime::getState(live.instance);
                const char* status = nullptr;
                const char* help = nullptr;
                if (!live.instance) {
                    status = "not running";
                    help = "No instance: the script failed to load (see the Console) or its C++ class is not built.";
                } else if (state && state->destroyed) {
                    status = "destroyed";
                    help = "onDestroy ran.";
                } else if (!state || !state->initialized) {
                    status = "not initialized";
                    help = "The GameObject has not been active yet: init runs when it is first activated.";
                } else if (state->started) {
                    status = state->enabledLatch ? "started" : "started, inactive";
                    help = state->enabledLatch ? "init, onEnable and start ran." : "start ran; the script is disabled or its GameObject inactive now (onDisable ran).";
                } else if (state->enabledLatch) {
                    status = "waiting to start";
                    help = "onEnable ran; start runs at the beginning of the next frame.";
                } else {
                    status = "initialized";
                    help = "init ran; onEnable and start wait until the script is enabled and its GameObject active.";
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", status);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", help);
                }
            }

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x / 3.0, ImGui::GetStyle().FramePadding.y / 2.0));

            if (ImGui::Button((ICON_FA_PEN_TO_SQUARE "##edit_name_" + std::to_string(scriptIdx)).c_str())) {
                ImGui::OpenPopup(("Edit Script##" + std::to_string(scriptIdx)).c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(script.type == ScriptType::LUA ? "Edit script details: source file"
                                                                 : "Edit script details: class name, header and source files");
            }

            ImGui::PopStyleVar();

            ImGui::SetNextWindowSizeConstraints(ImVec2(23 * ImGui::GetFontSize(), 0), ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::BeginPopup(("Edit Script##" + std::to_string(scriptIdx)).c_str())) {
                ImGui::Text("Edit Script Details");
                ImGui::Separator();

                static char nameBuffer[128];
                static char sourceBuffer[256];
                static char headerBuffer[256];

                std::filesystem::path projectPath = project->getProjectPath();
                std::filesystem::path srcPath(script.path);
                std::filesystem::path hdrPath(script.headerPath);
                if (ImGui::IsWindowAppearing()) {
                    strncpy(nameBuffer, script.className.c_str(), sizeof(nameBuffer) - 1);
                    nameBuffer[sizeof(nameBuffer) - 1] = '\0';

                    strncpy(sourceBuffer, srcPath.filename().string().c_str(), sizeof(sourceBuffer) - 1);
                    sourceBuffer[sizeof(sourceBuffer) - 1] = '\0';

                    strncpy(headerBuffer, hdrPath.filename().string().c_str(), sizeof(headerBuffer) - 1);
                    headerBuffer[sizeof(headerBuffer) - 1] = '\0';
                }

                bool changed = false;
                float secondColSize = 15 * ImGui::GetFontSize();

                beginTable(cpType, getLabelSize("Source Path"), "edit_script_details");

                if (script.type == ScriptType::LUA) {
                    // a Lua script's name is its file name: it follows the Source File
                    propertyHeader("Module", secondColSize);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::InputText("##new_name", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_ReadOnly);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("The module name is the file name: pick another Source File to change it");
                    }
                } else {
                    propertyHeader("Class Name", secondColSize);
                    if (ImGui::InputText("##new_name", nameBuffer, sizeof(nameBuffer),
                            ImGuiInputTextFlags_CallbackCharFilter,
                            ScriptCreateDialog::classNameCharFilter)) {
                        // Edit happening
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        changed = true;
                    }
                }

                float openBtnWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                float clearBtnWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                float btnSpacing = ImGui::GetStyle().ItemSpacing.x;

                // Header Path
                if (script.type != ScriptType::LUA) {
                    propertyHeader("Header File", secondColSize);
                    ImGui::SetNextItemWidth(secondColSize - openBtnWidth - clearBtnWidth - btnSpacing * 2.0f);

                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::InputText("##new_header", headerBuffer, sizeof(headerBuffer), ImGuiInputTextFlags_ReadOnly);
                    ImGui::PopStyleColor();
                    if (!hdrPath.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", hdrPath.string().c_str());
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##header_btn")) {
                        fs::path startDirectory = projectPath;
                        if (!script.headerPath.empty()) {
                            fs::path fullHdrPath(script.headerPath);
                            if (fullHdrPath.is_relative()) fullHdrPath = projectPath / fullHdrPath;
                            startDirectory = fullHdrPath.parent_path();
                        }
                        std::string selected = FileDialogs::openFileDialog(
                            startDirectory.string(), FILE_DIALOG_SCRIPT);
                        if (!selected.empty()) {
                            std::filesystem::path p(selected);
                            if (!Util::isHeaderFile(p.string())) {
                                Backend::getApp().registerAlert("Error", "Select a C++ header file.");
                            } else if (!Util::isInsidePath(p, projectPath)) {
                                Backend::getApp().registerAlert("Error", "File must be inside project directory.");
                            } else {
                                std::error_code ec;
                                std::filesystem::path rel = std::filesystem::relative(p, projectPath, ec);
                                if (!ec) {
                                    hdrPath = rel;
                                    strncpy(headerBuffer, rel.filename().string().c_str(), sizeof(headerBuffer) - 1);
                                    headerBuffer[sizeof(headerBuffer) - 1] = '\0';
                                    changed = true;
                                } else {
                                    Backend::getApp().registerAlert("Error", "File must be inside project directory.");
                                }
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(hdrPath.empty());
                    if (ImGui::Button(ICON_FA_XMARK "##header_clear")) {
                        hdrPath = "";
                        headerBuffer[0] = '\0';
                        changed = true;
                    }
                    ImGui::EndDisabled();
                }

                // Source Path
                propertyHeader("Source File", secondColSize);
                ImGui::SetNextItemWidth(secondColSize - openBtnWidth - clearBtnWidth - btnSpacing * 2.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::InputText("##new_source", sourceBuffer, sizeof(sourceBuffer), ImGuiInputTextFlags_ReadOnly);
                ImGui::PopStyleColor();
                if (!srcPath.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", srcPath.string().c_str());
                }

                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FOLDER_OPEN "##source_btn")) {
                    const fs::path sourceRoot = script.type == ScriptType::LUA
                        ? project->getLuaPath() : projectPath;
                    fs::path startDirectory = sourceRoot;
                    if (!script.path.empty()) {
                        fs::path fullSrcPath = script.type == ScriptType::LUA
                            ? project->resolveLuaPath(script.path) : projectPath / fs::path(script.path);
                        startDirectory = fullSrcPath.parent_path();
                    }
                    std::string selected = FileDialogs::openFileDialog(
                        startDirectory.string(), FILE_DIALOG_SCRIPT);
                    if (!selected.empty()) {
                        std::filesystem::path p(selected);
                        const bool validType = script.type == ScriptType::LUA
                            ? Util::isLuaFile(p.string())
                            : Util::isSourceFile(p.string());
                        if (!validType) {
                            Backend::getApp().registerAlert("Error", script.type == ScriptType::LUA
                                ? "Select a Lua script file."
                                : "Select a C++ source file.");
                        } else {
                            // Lua sources are stored relative to the Lua root ("lua://")
                            if (!Util::isInsidePath(p, sourceRoot)) {
                                Backend::getApp().registerAlert("Error", script.type == ScriptType::LUA
                                    ? "File must be inside the Lua directory."
                                    : "File must be inside project directory.");
                            } else {
                                std::error_code ec;
                                std::filesystem::path rel = std::filesystem::relative(p, sourceRoot, ec);
                                if (!ec) {
                                    srcPath = rel;
                                    strncpy(sourceBuffer, rel.filename().string().c_str(), sizeof(sourceBuffer) - 1);
                                    sourceBuffer[sizeof(sourceBuffer) - 1] = '\0';
                                    changed = true;
                                } else {
                                    Backend::getApp().registerAlert("Error", "File must be inside project directory.");
                                }
                            }
                        }
                    }
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(srcPath.empty());
                if (ImGui::Button(ICON_FA_XMARK "##source_clear")) {
                    srcPath = "";
                    sourceBuffer[0] = '\0';
                    changed = true;
                }
                ImGui::EndDisabled();

                endTable();

                if (changed) {
                    std::string newName = nameBuffer;
                    std::string newSource = FileUtils::pathToGenericUtf8(srcPath);
                    std::string newHeader = FileUtils::pathToGenericUtf8(hdrPath);

                    // Empty class name falls back to header (or source) filename stem
                    if (newName.empty()) {
                        newName = !hdrPath.empty() ? hdrPath.stem().string() : srcPath.stem().string();
                    }
                    // a Lua module is named by its file (unchanged when the file is cleared)
                    if (script.type == ScriptType::LUA && !srcPath.empty()) {
                        newName = srcPath.stem().string();
                        strncpy(nameBuffer, newName.c_str(), sizeof(nameBuffer) - 1);
                        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
                    }

                    // Normalize to a valid C++ identifier - the same rule the Create
                    // Script dialog uses - since className is emitted verbatim as a
                    // class token by the code generator. Reflect it back in the field.
                    if (!newName.empty() && script.type != ScriptType::LUA) {
                        newName = Factory::toIdentifier(newName);
                        strncpy(nameBuffer, newName.c_str(), sizeof(nameBuffer) - 1);
                        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
                    }

                    for (const Entity& entity : entities) {
                        ScriptComponent& sc = sceneProject->scene->getComponent<ScriptComponent>(entity);
                        std::vector<ScriptEntry> newScripts = sc.scripts;
                        if (scriptIdx < newScripts.size()) {
                            newScripts[scriptIdx].className = newName;
                            newScripts[scriptIdx].path = newSource;
                            if (script.type != ScriptType::LUA) {
                                newScripts[scriptIdx].headerPath = newHeader;
                                if (newHeader.empty()) {
                                    newScripts[scriptIdx].properties.clear();
                                }
                            } else {
                                if (newSource.empty()) {
                                    newScripts[scriptIdx].properties.clear();
                                }
                            }

                            const ScriptEntry& updated = newScripts[scriptIdx];
                            bool hasAnyPath = !updated.path.empty() || !updated.headerPath.empty();
                            if (hasAnyPath) {
                                project->updateScriptProperties(sceneProject, entity, newScripts);
                            }

                            cmd = new PropertyCmd<std::vector<ScriptEntry>>(project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", newScripts);
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                        }
                    }
                    if (cmd) cmd->setNoMerge();
                }

                ImGui::EndPopup();
            }

            ImGui::SameLine();

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x / 3.0, ImGui::GetStyle().FramePadding.y / 2.0));

            // Resolve header path (if present)
            std::filesystem::path projectPath = project->getProjectPath();
            std::filesystem::path hdrPath = script.headerPath;
            bool hasHeader = !script.headerPath.empty();
            if (hasHeader) {
                if (hdrPath.is_relative()) hdrPath = projectPath / hdrPath;
            }
            bool hdrExists = hasHeader && std::filesystem::exists(hdrPath);
            ImGui::BeginDisabled(!hdrExists);
            if (ImGui::Button((ICON_FA_FILE_LINES "##open_hdr_" + std::to_string(scriptIdx)).c_str())) {
                Backend::getApp().getCodeEditor()->openFile(hdrPath.string(), true);
            }
            if (ImGui::IsItemHovered()) {
                if (hdrExists){
                    ImGui::SetTooltip("Header: %s", script.headerPath.c_str());
                } else if (hasHeader) {
                    ImGui::SetTooltip("Header file not found");
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine();

            // Resolve source path: Lua sources are stored relative to the Lua root
            std::filesystem::path srcPath = script.type == ScriptType::LUA
                ? project->resolveLuaPath(script.path) : std::filesystem::path(script.path);
            if (srcPath.is_relative()) srcPath = projectPath / srcPath;
            bool srcExists = !script.path.empty() && std::filesystem::exists(srcPath);
            ImGui::BeginDisabled(!srcExists);
            if (ImGui::Button((ICON_FA_FILE_CODE "##open_src_" + std::to_string(scriptIdx)).c_str())) {
                Backend::getApp().getCodeEditor()->openFile(srcPath.string(), true);
            }
            if (ImGui::IsItemHovered()) {
                if (srcExists){
                    ImGui::SetTooltip("Source: %s", script.path.c_str());
                } else if (!script.path.empty()) {
                    ImGui::SetTooltip("Source file not found");
                }
            }
            ImGui::EndDisabled();

            ImGui::PopStyleVar();

            // during Play: what the script started and is still running (coroutines, timers)
            if (entities.size() == 1 && script.instance) {
                // the owner: a Lua instance's ref, a C++ script's ScriptBase
                const void* owner = script.type == ScriptType::CPP && script.cppScript ? static_cast<const void*>(script.cppScript) : script.instance;
                const size_t coroutines = ScriptScheduler::coroutineCountOf(owner);
                const size_t timers = ScriptScheduler::timerCountOf(owner);
                if (coroutines > 0 || timers > 0) {
                    propertyHeader("Running");
                    ImGui::TextDisabled("Coroutines: %zu, Timers: %zu", coroutines, timers);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Started by this script (self:startCoroutine, self:invoke): coroutines stop when the "
                                          "GameObject is deactivated, timers when it is destroyed");
                    }
                }
            }

            endTable();

            // Display script properties if available
            if (!script.properties.empty()) {
                separatorText("Properties");

                ImGui::BeginDisabled(!enabled && !mixed);

                // Compute dynamic first column width based on longest property label
                float maxPropLabelSize = 0;
                for (const ScriptProperty& prop : script.properties) {
                    std::string displayName = prop.displayName.empty() ? prop.name : prop.displayName;
                    float size = getLabelSize(displayName);
                    if (size > maxPropLabelSize)
                        maxPropLabelSize = size;
                }

                beginTable(cpType, maxPropLabelSize, "script_properties_" + std::to_string(scriptIdx));

                for (size_t propIdx = 0; propIdx < script.properties.size(); propIdx++) {
                    ScriptProperty& prop = script.properties[propIdx];

                    std::string propertyId = "scripts[" + std::to_string(scriptIdx) + "]." + prop.name;
                    std::string displayName = prop.displayName.empty() ? prop.name : prop.displayName;

                    RowPropertyType propType = scriptPropertyTypeToRowPropertyType(prop.type);

                    RowSettings propSettings;
                    propSettings.onValueChanged = [sceneProject, entities, scriptIdx, propName = prop.name]() {
                        for (const Entity& e : entities) {
                            ScriptComponent& sc = sceneProject->scene->getComponent<ScriptComponent>(e);
                            if (scriptIdx < sc.scripts.size()) {
                                ScriptEntry& se = sc.scripts[scriptIdx];
                                for (ScriptProperty& sp : se.properties) {
                                    if (sp.name == propName) {
                                        sp.syncToMember();
                                        break;
                                    }
                                }
                            }
                        }
                    };

                    if (prop.type == ScriptPropertyType::Prefab) {
                        // the label as the other rows draw it (a prefab instance's overrides shown)
                        prefabRowProperty = propertyId;
                        propertyHeader(displayName, -1, false, false);
                        prefabRowProperty.clear();
                        drawPrefabScriptProperty(project, sceneProject, entities, cpType, propertyId, propSettings.onValueChanged, cmd, finishProperty);
                        continue;
                    }
                    if (prop.type == ScriptPropertyType::Asset) {
                        prefabRowProperty = propertyId;
                        propertyHeader(displayName, -1, false, false);
                        prefabRowProperty.clear();
                        drawAssetScriptProperty(project, sceneProject, entities, cpType, propertyId, prop.assetType, propSettings.onValueChanged, cmd, finishProperty);
                        continue;
                    }

                    propertyRow(propType, cpType, propertyId, displayName, sceneProject, entities, propSettings);
                }

                endTable();

                ImGui::EndDisabled();
            }

            ImGui::Indent(indentation); // Re-indent after content
        }

        ImGui::PopStyleColor(3);
        ImGui::Unindent(indentation);

        ImGui::PopID();
    }
}

void editor::InspectorWindow::drawSkyboxComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Default sky"), "default_sky");

    bool allDefault = true;
    for (auto& entity : entities){
        PropertyData prop = Catalog::getProperty(sceneProject->scene, entity, cpType, "texture");
        Texture* tex = static_cast<Texture*>(prop.ref);
        if (tex->getId() != "editor:resources:default_sky") {
            allDefault = false;
            break;
        }
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Default sky");
    ImGui::TableNextColumn();

    if (allDefault) ImGui::BeginDisabled();
    if (ImGui::Button("Apply")){
        for (auto& entity : entities){
            Texture newTex;

            ProjectUtils::setDefaultSkyTexture(newTex);

            cmd = new PropertyCmd<Texture>(project, sceneProject->id, entity, cpType, "texture", newTex, nullptr);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
    }
    if (allDefault) ImGui::EndDisabled();

    endTable();

    drawCustomShaderRow(cpType, ShaderType::SKYBOX, sceneProject, entities);

    separatorText("Skybox Settings");

    beginTable(cpType, getLabelSize("Rotation"), "sky_settings");

    RowSettings visibleSettings;
    visibleSettings.help = "When disabled, the sky is not drawn but still provides image-based lighting to meshes.";
    propertyRow(RowPropertyType::Bool, cpType, "visible", "Visible", sceneProject, entities, visibleSettings);
    propertyRow(RowPropertyType::TextureCube, cpType, "texture", "Texture", sceneProject, entities);
    propertyRow(RowPropertyType::Color4L, cpType, "color", "Color", sceneProject, entities);
    propertyRow(RowPropertyType::Float, cpType, "rotation", "Rotation", sceneProject, entities);

    endTable();
}

// (upstream's drawParticleSystemLegacy rows of ui-scaling-text, Unscaled Time and the "UI particles" note, are S20's
// drawParticleSystemComponent's, InspectorParticles.cpp)
void editor::InspectorWindow::startActionPreview(Entity entity, Scene* scene, SceneProject* sceneProject) {
    if (actionPreviewing) return;

    ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
    if (!actionComp) return;

    // One pose owner per root (animation clips 9.R7): an Animation window or Pose window preview
    // of the entities the preview writes stops (and restores) before the snapshot is taken
    releaseActionPreviewOwners();
    actionPreviewOwners = inspectorclips::acquirePreviewOwners(sceneProject->id, scene, entity, [this]() { stopActionPreviewIfActive(); });
    actionComp = scene->findComponent<ActionComponent>(entity);
    if (!actionComp) {
        releaseActionPreviewOwners();
        return;
    }

    actionPreviewSnapshot.clear();
    actionPreviewEntity = entity;
    actionPreviewSceneId = sceneProject->id;

    // Collect all entities that will be affected by the preview
    std::unordered_set<Entity> collected;

    std::function<void(Entity)> collectEntities = [&](Entity e) {
        if (e == NULL_ENTITY || !scene->isEntityCreated(e) || !collected.insert(e).second) return;

        // Save this entity's state
        actionPreviewSnapshot.add(sceneProject->id, scene, e);

        // If entity has an ActionComponent with a target, save the target too
        if (ActionComponent* ac = scene->findComponent<ActionComponent>(e)) {
            if (ac->target != NULL_ENTITY && scene->isEntityCreated(ac->target)) {
                collectEntities(ac->target);
            }
        }

        // If entity has an AnimationComponent, collect child action entities and their targets
        if (AnimationComponent* anim = scene->findComponent<AnimationComponent>(e)) {
            for (const ActionFrame& frame : anim->actions) {
                if (frame.action != NULL_ENTITY && scene->isEntityCreated(frame.action)) {
                    collectEntities(frame.action);
                }
            }
        }
    };

    collectEntities(entity);

    actionComp->state = ActionState::Stopped;
    actionComp->timecount = 0;
    actionComp->stopTrigger = false;
    actionComp->pauseTrigger = false;
    actionComp->startTrigger = true;

    actionPreviewing = true;
}

void editor::InspectorWindow::releaseActionPreviewOwners() {
    // moved out first: a token is released once, also when a release runs a stop
    const std::vector<int> tokens = std::move(actionPreviewOwners);
    actionPreviewOwners.clear();
    for (int token : tokens) {
        PreviewOwner::release(token);
    }
}

void editor::InspectorWindow::stopActionPreviewIfActive() {
    if (!actionPreviewing) {
        releaseActionPreviewOwners();
        return;
    }

    SceneProject* previewSceneProject = project ? project->getScene(actionPreviewSceneId) : nullptr;
    if (previewSceneProject && previewSceneProject->scene) {
        stopActionPreview(previewSceneProject->scene, previewSceneProject);
        return;
    }

    actionPreviewSnapshot.clear();
    actionPreviewing = false;
    actionPreviewPlaying = false;
    actionPreviewEntity = NULL_ENTITY;
    actionPreviewSceneId = 0;
    releaseActionPreviewOwners();
}

void editor::InspectorWindow::stopActionPreview(Scene* scene, SceneProject* sceneProject) {
    if (!actionPreviewing) return;

    // the authored state back, with what commands wrote during the preview
    actionPreviewSnapshot.restore(scene);

    actionPreviewSnapshot.clear();
    actionPreviewing = false;
    actionPreviewPlaying = false;
    actionPreviewEntity = NULL_ENTITY;
    actionPreviewSceneId = 0;
    releaseActionPreviewOwners();
    if (sceneProject) {
        sceneProject->needUpdateRender = true;
    }
}

template<typename Component, typename ValueType>
void editor::InspectorWindow::drawTrackValues(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities, RowPropertyType rowType, const ValueType& defaultNewValue, const char* idPrefix, std::vector<ValueType> Component::*memberPtr, const char* propertyName,
                                         const std::function<void(MultiPropertyCmd*, Entity, Component*, size_t)>& onRemoveValue,
                                         const std::function<void(MultiPropertyCmd*, Entity, Component*)>& onAddValue){
    Component& comp = sceneProject->scene->getComponent<Component>(entities[0]);

    float firstColSize = getLabelSize("Value 000");
    beginTable(cpType, firstColSize, std::string(idPrefix) + "_values_header");

    RowSettings settingsValue;
    settingsValue.secondColSize = -1;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Values");
    ImGui::TableSetColumnIndex(1);

    ImGui::Text("%zu", (comp.*memberPtr).size());
    float trackValuesArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    std::string addValueLabel = std::string("Add Value##add_") + idPrefix + "_values";
    if (drawSummaryAddButton(addValueLabel, trackValuesArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (Entity entity : entities){
            if (Component* trackComp = sceneProject->scene->findComponent<Component>(entity)){
                std::vector<ValueType> newValues = trackComp->*memberPtr;
                ValueType newValue = defaultNewValue;
                if (!newValues.empty()){
                    newValue = newValues.back();
                }
                newValues.push_back(newValue);
                multiCmd->addPropertyCmd<std::vector<ValueType>>(project, sceneProject->id, entity, cpType, propertyName, newValues);
                if (onAddValue){
                    onAddValue(multiCmd, entity, trackComp);
                }
            }
        }

        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }

    ImGui::SameLine();
    std::string arrowId = std::string("##toggle_") + idPrefix + "_values";
    if (ImGui::ArrowButton(arrowId.c_str(), trackValuesExpanded[idPrefix] ? ImGuiDir_Up : ImGuiDir_Down)){
        trackValuesExpanded[idPrefix] = !trackValuesExpanded[idPrefix];
    }

    bool removedValue = false;

    if (trackValuesExpanded[idPrefix]){
    float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
    float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
    ImVec2 inputSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
    if (inputSize.x < 100.0f){
        inputSize.x = 100.0f;
    }

    for (size_t i = 0; i < (comp.*memberPtr).size(); i++){
        std::string valueId = std::string(propertyName) + "[" + std::to_string(i) + "]";
        std::string valueLabel = "Value " + std::to_string(i);

        RowSettings settingsEntry = settingsValue;
        settingsEntry.secondColSize = inputSize.x;
        propertyRow(rowType, cpType, valueId, valueLabel, sceneProject, entities, settingsEntry);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        std::string removeId = std::string(ICON_FA_TRASH_CAN) + "##remove_" + idPrefix + "_value_" + std::to_string(i);
        if (ImGui::Button(removeId.c_str())){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities){
                if (Component* trackComp = sceneProject->scene->findComponent<Component>(entity)){
                    if (i < (trackComp->*memberPtr).size()){
                        std::vector<ValueType> newValues = trackComp->*memberPtr;
                        newValues.erase(newValues.begin() + (long int)i);
                        multiCmd->addPropertyCmd<std::vector<ValueType>>(project, sceneProject->id, entity, cpType, propertyName, newValues);
                        if (onRemoveValue){
                            onRemoveValue(multiCmd, entity, trackComp, i);
                        }
                    }
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

            removedValue = true;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            break;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove value");
        }
    }

    } // trackValuesExpanded

    if (removedValue){
        endTable();
        return;
    }

    endTable();
}

// the track drawers (InspectorAnimation.cpp) use these
template void editor::InspectorWindow::drawTrackValues<KeyframeTracksComponent, float>(ComponentType, SceneProject*, std::vector<Entity>, RowPropertyType,
    const float&, const char*, std::vector<float> KeyframeTracksComponent::*, const char*,
    const std::function<void(MultiPropertyCmd*, Entity, KeyframeTracksComponent*, size_t)>&, const std::function<void(MultiPropertyCmd*, Entity, KeyframeTracksComponent*)>&);
template void editor::InspectorWindow::drawTrackValues<TranslateTracksComponent, Vector3>(ComponentType, SceneProject*, std::vector<Entity>, RowPropertyType,
    const Vector3&, const char*, std::vector<Vector3> TranslateTracksComponent::*, const char*,
    const std::function<void(MultiPropertyCmd*, Entity, TranslateTracksComponent*, size_t)>&, const std::function<void(MultiPropertyCmd*, Entity, TranslateTracksComponent*)>&);
template void editor::InspectorWindow::drawTrackValues<RotateTracksComponent, Quaternion>(ComponentType, SceneProject*, std::vector<Entity>, RowPropertyType,
    const Quaternion&, const char*, std::vector<Quaternion> RotateTracksComponent::*, const char*,
    const std::function<void(MultiPropertyCmd*, Entity, RotateTracksComponent*, size_t)>&, const std::function<void(MultiPropertyCmd*, Entity, RotateTracksComponent*)>&);
template void editor::InspectorWindow::drawTrackValues<ScaleTracksComponent, Vector3>(ComponentType, SceneProject*, std::vector<Entity>, RowPropertyType,
    const Vector3&, const char*, std::vector<Vector3> ScaleTracksComponent::*, const char*,
    const std::function<void(MultiPropertyCmd*, Entity, ScaleTracksComponent*, size_t)>&, const std::function<void(MultiPropertyCmd*, Entity, ScaleTracksComponent*)>&);

void editor::InspectorWindow::revealAssetInProject(const std::filesystem::path& absolutePath){
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        projectWindow->revealAsset(absolutePath);
    }
}

// ---------------------------------------------------------------- Lattice (spec §9.1)

void editor::InspectorWindow::runDeferredCommands(){
    std::vector<std::function<void()>> commands;
    commands.swap(deferredCommands);
    for (const std::function<void()>& command : commands) {
        command();
    }
}

void editor::InspectorWindow::show(){
    runDeferredCommands();
    if (materialAsset && !materialAsset->path.empty() && ImGui::GetFrameCount() > materialAssetFrame + 1) {
        leaveMaterialAsset();
    }

    // Flush any debounced material file writes
    flushDirtyMaterials(ImGui::GetIO().DeltaTime);

    trackInspectedObject();
    closeIdleVideoPreview();

    // fluid previews keep their Scene view redrawing; they and the Paint Dye brush end with
    // what they work on (FluidEditorTools)
    FluidEditorTools::update(project);
    // UI tests: what the last frame named (UIProbe, JASPER_UI_PROBE only)
    UIProbe::flush();

    if (!windowOpen) {
        return;
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    bool wasOpen = windowOpen;

    if (!ImGui::Begin(InspectorWindow::WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        if (wasOpen && !windowOpen) {
            setOpen(false);
        }
        return;
    }
    if (nameFocusFrames > 0) nameFocusFrames--;
    if (UIProbe::enabled()) {
        // what shows of the window, wherever it is scrolled
        const ImRect inner = ImGui::GetCurrentWindow()->InnerClipRect;
        UIProbe::rect("inspector", inner.Min.x, inner.Min.y, inner.Max.x, inner.Max.y);
        // and how far it is scrolled (UI tests scroll it with the mouse wheel)
        UIProbe::scroll("inspector");
    }
    const bool openAddComponent = std::exchange(addComponentRequested, false);

    // this frame's rows replace the last frame's when the window is done
    layoutDrawing = InspectorLayout();
    layoutDrawing.frame = ImGui::GetFrameCount();
    struct LayoutFinish{
        InspectorWindow* window;
        ~LayoutFinish(){
            window->layoutRecording = false;
            window->layoutLast = std::move(window->layoutDrawing);
        }
    } layoutFinish{this};
    // texture files selected in the Project window: their import settings (S14 R22), read only
    // while someone else holds their lock (S29 R28)
    if (!project->getInspectedAssets().empty()) {
        if (!textureImporter) textureImporter = std::make_unique<TextureImporterView>(project);
        if (!modelImporter) modelImporter = std::make_unique<ModelImporterView>(project);
        const bool assetsLocked = drawVcsLockBanner(project->getInspectedAssets());
        if (assetsLocked) ImGui::BeginDisabled(true);
        // model files: their import settings (S53 R16), the Animation tab their clip table
        if (modelImporter->draw(project->getInspectedAssets(), [this](const std::string& relative) {
                drawModelClipsAsset(project->getAssetsPath() / FileUtils::pathFromUtf8(relative), relative);
            })) {
            if (assetsLocked) ImGui::EndDisabled();
            ImGui::End();
            return;
        }
        // one image that is a generated sprite normal map or the source of one: upstream's asset
        // view of it (its recipe, stale line, Regenerate), above the import settings
        const std::vector<std::filesystem::path>& inspectedFiles = project->getInspectedAssets();
        const std::filesystem::path inspectedImage = inspectedFiles.size() == 1
            ? (inspectedFiles[0].is_absolute() ? inspectedFiles[0] : project->getProjectPath() / inspectedFiles[0]).lexically_normal()
            : std::filesystem::path();
        if (!inspectedImage.empty() && Util::isImageFile(inspectedImage.string()) && drawSpriteNormalsAsset(inspectedImage)) {
            ImGui::Separator();
        }
        // a PNG a Procedural Material baked: where from, and whether the graph changed since (S90 R16)
        if (!inspectedImage.empty() && Util::isImageFile(inspectedImage.string())) {
            if (const std::optional<procmat::BakeStatus> bake = procmat::cachedBakeStatus(project->getProjectPath(), inspectedImage)) {
                ImGui::TextWrapped("Baked from %s: %s, %d px, seed %u, %s (%s).", bake->recipe.graphPath.c_str(), bake->recipe.output.c_str(),
                                   bake->recipe.size, bake->recipe.seed, bake->recipe.tier.c_str(), bake->recipe.device.c_str());
                if (bake->state != procmat::BakeState::Fresh) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
                    ImGui::TextWrapped("Stale: %s. Bake it again.", bake->reason.c_str());
                    ImGui::PopStyleColor();
                    UIProbe::rect("inspector.procmatStale", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                  ImGui::GetItemRectMax().y);
                }
                ImGui::Separator();
            }
        }
        bool drewAssets = textureImporter->draw(project->getInspectedAssets());
        // .aseprite files: the Aseprite Importer (S44 R30)
        if (!drewAssets) {
            if (!asepriteImporter) asepriteImporter = std::make_unique<AsepriteImporterView>(project);
            drewAssets = asepriteImporter->draw(project->getInspectedAssets());
        }
        if (!drewAssets) {
            // S45: a .spriteatlas
            if (!spriteAtlasInspector) spriteAtlasInspector = std::make_shared<SpriteAtlasInspector>(project);
            drewAssets = spriteAtlasInspector->draw(project->getInspectedAssets());
        }
        if (assetsLocked) ImGui::EndDisabled();
        if (drewAssets) {
            ImGui::End();
            return;
        }
    }
    // a state, transition or layer clicked in the Animator window: drawn instead of the scene
    // selection until the selection or the inspected asset changes (S21 R23)
    if (animatorEditorHooks().drawSelection && animatorEditorHooks().drawSelection()) {
        stopSoundPreview();
        stopActionPreviewIfActive();
        ImGui::End();
        return;
    }
    // and a node clicked in a graph window (S90 R26): its parameters
    if (graphEditorHooks().drawSelection && graphEditorHooks().drawSelection()) {
        stopSoundPreview();
        stopActionPreviewIfActive();
        ImGui::End();
        return;
    }
    // the inspected .anim (or model clips) the Project window's last click chose: drawn instead of
    // the scene selection, never touching a scene (R30)
    if (drawInspectedAsset()) {
        stopSoundPreview();
        stopActionPreviewIfActive();
        stopVertexAnimatorPreview();
        // a .shake's Test preview belongs to the inspected asset
        syncCameraShakePreview(project->getSelectedScene(), {});
        ImGui::End();
        return;
    }

    uint32_t propertiesSceneId = project->getSelectedSceneForProperties();
    SceneProject* sceneProject = project->getScene(propertiesSceneId);
    if (!sceneProject) {
        sceneProject = project->getSelectedScene();
    }
    if (!sceneProject) {
        stopTransientPreviews();
        ImGui::End();
        return;
    }

    std::vector<Entity> entities = project->getSelectedEntities(sceneProject->id);
    layoutDrawing.sceneId = sceneProject->id;
    layoutDrawing.entities = entities;

    // S29 R28: the open document's lock; another user's makes the Inspector read only
    bool documentLocked = false;
    {
        std::filesystem::path document = sceneProject->filepath;
        if (const PrefabStage* stage = project->getPrefabStage(sceneProject->id)) document = stage->assetPath;
        if (!document.empty()) {
            if (document.is_relative()) document = project->getProjectPath() / document;
            documentLocked = drawVcsLockBanner({document});
        }
    }

    if (updateInspectedParticleAsset(sceneProject, entities)) {
        stopTransientPreviews();
        drawParticleAssetInspector();
        ImGui::End();
        if (wasOpen && !windowOpen) {
            setOpen(false);
        }
        return;
    }

    const bool previewSelectionMatches = actionPreviewing
            && sceneProject->id == actionPreviewSceneId
            && sceneProject->playState == ScenePlayState::STOPPED
            && entities.size() == 1
            && entities[0] == actionPreviewEntity;
    // the Vertex Animator preview belongs to the shown GameObject too
    syncVertexAnimatorPreview(sceneProject, entities);
    // and the Camera Shaker's Test preview
    syncCameraShakePreview(sceneProject, entities);
    if (actionPreviewing && !previewSelectionMatches) {
        stopActionPreviewIfActive();
    }

    std::vector<ComponentType> components;
    Scene* scene = sceneProject->scene;

    if (entities.size() > 0){

        // to change component view order, need change ComponentType
        std::string names;
        bool isFirstEntity = true;
        for (Entity& entity : entities){
            std::vector<ComponentType> newComponents = Catalog::findComponents(scene, entity);

            if (!std::is_sorted(newComponents.begin(), newComponents.end())) {
                std::sort(newComponents.begin(), newComponents.end());
            }

            if (isFirstEntity) {
                components = newComponents;
                isFirstEntity = false;
            } else {
                std::vector<ComponentType> intersection;
                intersection.reserve(std::min(components.size(), newComponents.size()));

                std::set_intersection(
                    components.begin(), components.end(),
                    newComponents.begin(), newComponents.end(),
                    std::back_inserter(intersection));

                components = std::move(intersection);

                names += ", ";
            }

            names += scene->getEntityName(entity);
        }

        // Stop destroys what the running scene made, so while playing only authored entities are editable
        const std::vector<Entity>& authored = sceneProject->entities;
        const bool isRuntimeSelection = sceneProject->playState != ScenePlayState::STOPPED &&
            std::any_of(entities.begin(), entities.end(), [&authored](Entity entity) {
                return std::find(authored.begin(), authored.end(), entity) == authored.end();
            });

        drawGameObjectHeader(sceneProject, entities, names, isRuntimeSelection || documentLocked);

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));

        // GameObjects of prefab instances (their roots included: the real root is the prefab's
        // root GameObject) among the selection: their edits are overrides (Unity)
        bool isPrefab = false;
        for (Entity entity : entities) {
            PrefabObjectRef ref;
            if (project->findPrefabObject(sceneProject->id, entity, ref)) {
                isPrefab = true;
                break;
            }
        }

        for (ComponentType& cpType : components){

            // Context menu and body disabled for a runtime selection, or a document locked by someone else
            const bool compReadOnly = isRuntimeSelection || documentLocked;
            bool headerOpen = drawComponentHeader(sceneProject, entities, cpType, isPrefab, compReadOnly);

            if (headerOpen){
                beginPrefabComponent(sceneProject, entities, cpType, compReadOnly);
                // Disable non-script components while playing
                if (compReadOnly) ImGui::BeginDisabled(true);
                beginLayoutComponent(cpType);

                // its own drawer, or the rows of its reflection table (InspectorDefault.cpp)
                if (cpType == ComponentType::VideoPlayerComponent){
                    // its playback bar (Play) is no edit: it works on a read-only player too
                    if (compReadOnly) ImGui::EndDisabled();
                    drawVideoPlayerComponent(cpType, sceneProject, entities, compReadOnly);
                    if (compReadOnly) ImGui::BeginDisabled(true);
                }else if (cpType == ComponentType::TextEffectComponent){
                    // upstream's (SYNC#4) component drawers
                    drawTextEffectComponent(cpType, sceneProject, entities);
                }else if (cpType == ComponentType::SafeAreaComponent){
                    drawSafeAreaComponent(cpType, sceneProject, entities);
                }else if (cpType == ComponentType::CameraShakerComponent){
                    drawCameraShakerComponent(cpType, sceneProject, entities);
                }else if (cpType == ComponentType::CameraFollowComponent){
                    drawCameraFollowComponent(cpType, sceneProject, entities);
                }else if (cpType == ComponentType::FluidSimulationComponent){
                    // its preview bar is no edit: it works on a read-only (play-created) fluid too
                    if (compReadOnly) ImGui::EndDisabled();
                    drawFluidSimulationComponent(cpType, sceneProject, entities, compReadOnly);
                    if (compReadOnly) ImGui::BeginDisabled(true);
                }else if (ComponentDrawer drawer = componentDrawer(cpType)){
                    (this->*drawer)(cpType, sceneProject, entities);
                }else{
                    // (its cost per frame is measured by test_reflection.py, S08 R19)
                    JE_PROFILE_SCOPE("Inspector Default Component");
                    drawDefaultComponent(cpType, sceneProject, entities);
                }

                endLayoutComponent();
                if (compReadOnly) ImGui::EndDisabled();
                endPrefabComponent();
            }
        }

        if (isPrefab) {
            drawRemovedPrefabComponents(sceneProject, entities, isRuntimeSelection);
        }

        drawAddComponentButton(sceneProject, entities, isRuntimeSelection, openAddComponent);

    }else{
        // Scene settings: same header shape as a GameObject (icon, full width name)
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_DISPLAY);
        ImGui::SetItemTooltip("Scene settings, shown while no GameObject is selected");
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(-1);
        static char nameBuffer[128];
        strncpy(nameBuffer, sceneProject->name.c_str(), sizeof(nameBuffer) - 1);
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';

        bool isMainScene = (sceneProject->id == project->getSelectedSceneId());
        ImGui::BeginDisabled(!isMainScene);
        if (nameFocusFrames > 0 && isMainScene) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##input_scene_name", nameBuffer, IM_ARRAYSIZE(nameBuffer));
        if (ImGui::IsItemActive()) nameFocusFrames = 0;
        ImGui::EndDisabled();

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (isMainScene && nameBuffer[0] != '\0' && strcmp(nameBuffer, sceneProject->name.c_str()) != 0) {
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new SceneNameCmd(project, sceneProject->id, nameBuffer));
            }
        }
        ImGui::Separator();

        if (sceneProject->scene) {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame;
            const float shadowQualityLabelColWidth = getLabelSize("Filter Quality") + ImGui::GetStyle().CellPadding.x * 2.0f;
            const float shadowQualityComboWidth = ImGui::CalcTextSize("Medium").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;

            if (ImGui::BeginTable("scene_settings_table", 2, tableFlags)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Background"));
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                drawScenePropertyRow<Vector4>(sceneProject, "background_color", "Background", ScenePropertyInputType::ColorRGBA);
                drawScenePropertyRow<LightState>(sceneProject, "light_state", "Lights", ScenePropertyInputType::Combo, 5 * ImGui::GetFontSize());

                ImGui::EndTable();
            }

            if (sceneProject->sceneType != SceneType::SCENE_3D) {
                ImGui::SeparatorText("Ambient Light (2D)");

                if (ImGui::BeginTable("scene_ambient2d_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Intensity"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<Vector3>(sceneProject, "ambient_light_2d_color", "Color", ScenePropertyInputType::ColorRGB);
                    drawScenePropertyRow<float>(sceneProject, "ambient_light_2d_intensity", "Intensity", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 1.0f);

                    ImGui::EndTable();
                }

                ImGui::SeparatorText("2D Shadows");

                if (ImGui::BeginTable("scene_shadows2d_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, shadowQualityLabelColWidth);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<ShadowQuality>(sceneProject, "shadows_2d_quality", "Filter Quality", ScenePropertyInputType::Combo, shadowQualityComboWidth, 0.0f, 1.0f,
                        "Smoothness of 2D light shadow edges. The penumbra width is set per light (Shadow Softness):\n"
                        "- None: 1 sample, no filtering, hard edges (softness is ignored)\n"
                        "- Low: 5 samples\n"
                        "- Medium: 9 samples\n"
                        "- High: 13 samples, smooth even with very wide penumbras\n"
                        "Higher values cost more per lit pixel. Raise it if wide penumbras show banding.");

                    ImGui::EndTable();
                }
            }

            LightState currentLightState = jasperengine::editor::Catalog::getSceneProperty<LightState>(sceneProject->scene, "light_state");
            if (currentLightState != LightState::OFF) {
                ImGui::SeparatorText("Global Illumination");

                if (ImGui::BeginTable("scene_globalillum_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Intensity"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<Vector3>(sceneProject, "global_illumination_color", "Color", ScenePropertyInputType::ColorRGB);
                    drawScenePropertyRow<float>(sceneProject, "global_illumination_intensity", "Intensity", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 1.0f);

                    ImGui::EndTable();
                }

                ImGui::SeparatorText("Lights");

                if (ImGui::BeginTable("scene_lights_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Max Lights Per Object"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<int>(sceneProject, "max_lights_per_object", "Max Lights Per Object", ScenePropertyInputType::DragInt, -1.0f, 1.0f, 16.0f,
                        "How many lights each object is lit by (1-16). The scene can have any number of lights: each object "
                        "takes the ones that reach it and give it the most light, and the others reaching it are folded into "
                        "its ambient light (spherical harmonics), so they still brighten it without popping. More costs more "
                        "per lit pixel; 4, 8 and 16 are the shader tiers.");
                    drawScenePropertyRow<LitQuality>(sceneProject, "lit_quality", "Lit Quality", ScenePropertyInputType::Combo, 7 * ImGui::GetFontSize(), 0.0f, 1.0f,
                        "How the built-in Lit shades (and forks of it):\n"
                        "- Standard: as it always has\n"
                        "- High: specular anti-aliasing (no shimmering highlights on dense normal maps or fine curves) and the "
                        "energy rough metals lose under direct lights given back. A few percent more per lit pixel.\n"
                        "New scenes are High; a scene saved before Lit Quality existed loads as Standard.");
                    drawPhysicalLightExposureRow(sceneProject);

                    ImGui::EndTable();
                }

                ImGui::SeparatorText("Shadows");
                drawSceneShadows(sceneProject);

                ImGui::SeparatorText("Mesh Detail (LOD)");

                if (ImGui::BeginTable("scene_lod_settings_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Cross-fade Duration"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<bool>(sceneProject, "mesh_lod_enabled", "Enabled", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
                        "Draw simplified versions of meshes when their detail no longer shows on screen. Off: every mesh draws at full detail and no levels are built.");
                    if (jasperengine::editor::Catalog::getSceneProperty<bool>(sceneProject->scene, "mesh_lod_enabled")) {
                        drawScenePropertyRow<float>(sceneProject, "mesh_lod_threshold", "Threshold", ScenePropertyInputType::SliderFloat, -1.0f, 0.25f, 8.0f,
                            "Screen pixels of geometric error a simplified mesh may show. Lower keeps more detail.");
                        drawScenePropertyRow<bool>(sceneProject, "mesh_lod_cross_fade", "Cross Fade", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
                            "Blend between detail levels with a short dithered fade instead of switching at once: fading objects draw twice for the duration. "
                            "Off: no fade shaders are built or shipped.");
                        if (jasperengine::editor::Catalog::getSceneProperty<bool>(sceneProject->scene, "mesh_lod_cross_fade")) {
                            drawScenePropertyRow<float>(sceneProject, "mesh_lod_cross_fade_duration", "Cross-fade Duration", ScenePropertyInputType::SliderFloat, -1.0f, 0.05f, 2.0f,
                                "Seconds a level change fades over (Unity's LODGroup.crossFadeAnimationDuration): fading objects draw twice for the duration.");
                        }
                    }

                    ImGui::EndTable();
                }

                ImGui::SeparatorText("Depth Prepass");

                if (ImGui::BeginTable("scene_prepass_settings_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Enabled"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<bool>(sceneProject, "depth_prepass_enabled", "Enabled", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
                        "Write depth before the opaque color pass so each pixel is shaded once. Pays off with heavy overdraw (dense foliage); costs an extra geometry pass.");

                    ImGui::EndTable();
                }

                ImGui::SeparatorText("Scene Color");

                if (ImGui::BeginTable("scene_color_settings_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("In Use"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    const float sceneColorScaleComboWidth = ImGui::CalcTextSize("Half").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
                    drawScenePropertyRow<SceneColorScale>(sceneProject, "scene_color_scale", "Scale", ScenePropertyInputType::Combo, sceneColorScaleComboWidth, 0.0f, 1.0f,
                        "Resolution of the copy of the opaque scene and sky that shaders reading the scene colour (glass, heat haze, refraction) sample, taken once per camera pass that draws such a shader. "
                        "Half (default): a quarter of the pixels to copy and keep, slightly blurred (each texel averages 2x2 pixels), right for distortion and on mobile. "
                        "Full: sharp, for clear glass seen up close, at four times the copy's bandwidth and memory.");

                    // what the last frame made for the draws reading the scene (depth too: no table of its own)
                    const uint32_t sceneInputsInUse = jasperengine::editor::Catalog::getSceneInputsInUse(sceneProject->scene);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("In Use");
                    ImGui::TableSetColumnIndex(1);
                    if (sceneInputsInUse) {
                        const std::string label = jasperengine::editor::Catalog::getSceneInputsLabel(sceneInputsInUse);
                        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_GAUGE_HIGH " %c%s", (char)std::toupper((unsigned char)label[0]), label.c_str() + 1);
                    } else {
                        ImGui::TextDisabled("None");
                    }
                    if (ImGui::IsItemHovered()) {
                        const std::string help = sceneInputsInUse
                            ? "What the last frame made for the shaders reading the scene " + jasperengine::editor::Catalog::getSceneInputsLabel(sceneInputsInUse) +
                              " in the cameras and views drawing one: " + jasperengine::editor::Catalog::getSceneInputsCost(sceneInputsInUse, true) + "."
                            : std::string("What the last frame made for shaders reading the scene depth or colour: nothing, no visible draw reads them, so they cost nothing.");
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                        ImGui::TextUnformatted(help.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }

                    ImGui::EndTable();
                }

                drawScenePlanarReflections(sceneProject, tableFlags);
                drawSceneWaterWakes(sceneProject, tableFlags);

                ImGui::SeparatorText("Ambient Occlusion (SSAO)");

                if (ImGui::BeginTable("scene_ssao_settings_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Intensity"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<bool>(sceneProject, "ssao_enabled", "Enabled", ScenePropertyInputType::Checkbox);

                    if (jasperengine::editor::Catalog::getSceneProperty<bool>(sceneProject->scene, "ssao_enabled")) {
                        drawScenePropertyRow<float>(sceneProject, "ssao_radius", "Radius", ScenePropertyInputType::SliderFloat, -1.0f, 0.05f, 2.0f);
                        drawScenePropertyRow<float>(sceneProject, "ssao_intensity", "Intensity", ScenePropertyInputType::SliderFloat, -1.0f, 0.1f, 4.0f);
                        drawScenePropertyRow<float>(sceneProject, "ssao_bias", "Bias", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 0.1f);
                        drawScenePropertyRow<bool>(sceneProject, "ssao_debug", "Debug View", ScenePropertyInputType::Checkbox);
                    }

                    ImGui::EndTable();
                }

                ImGui::SeparatorText("Reflections (SSR)");

                if (ImGui::BeginTable("scene_ssr_settings_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Max Distance"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    drawScenePropertyRow<bool>(sceneProject, "ssr_enabled", "Enabled", ScenePropertyInputType::Checkbox);

                    if (jasperengine::editor::Catalog::getSceneProperty<bool>(sceneProject->scene, "ssr_enabled")) {
                        drawScenePropertyRow<float>(sceneProject, "ssr_max_distance", "Max Distance", ScenePropertyInputType::SliderFloat, -1.0f, 1.0f, 50.0f);
                        drawScenePropertyRow<float>(sceneProject, "ssr_thickness", "Thickness", ScenePropertyInputType::SliderFloat, -1.0f, 0.05f, 4.0f);
                        drawScenePropertyRow<float>(sceneProject, "ssr_intensity", "Intensity", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 2.0f);
                        drawScenePropertyRow<float>(sceneProject, "ssr_blur", "Glossy Blur", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 1.0f);

                        // Debug View: visualize the SSR G-buffer channels (centralized in the composite)
                        {
                            const char* ssrDebugNames[] = { "Off", "Reflection", "Normal", "Roughness", "Metallic", "Albedo",
                                                            "Environment Specular", "Reflection Source", "Occlusion" };
                            int debugMode = jasperengine::editor::Catalog::getSceneProperty<int>(sceneProject->scene, "ssr_debug_mode");
                            int debugModeDefault = jasperengine::editor::Catalog::getScenePropertyDefault<int>("ssr_debug_mode");
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("Debug View");
                            if (debugMode != debugModeDefault) {
                                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
                                ImGui::SameLine();
                                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                                if (ImGui::Button(ICON_FA_ROTATE_LEFT"##reset_ssr_debug_mode")) {
                                    Command* resetCmd = new ScenePropertyCmd<int>(project, sceneProject->id, "ssr_debug_mode", debugModeDefault);
                                    CommandHandle::get(sceneProject->id)->addCommandNoMerge(resetCmd);
                                }
                                ImGui::PopStyleColor(2);
                                ImGui::PopStyleVar(3);
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetNextItemWidth(8 * ImGui::GetFontSize());
                            if (ImGui::Combo("##ssr_debug_mode", &debugMode, ssrDebugNames, IM_ARRAYSIZE(ssrDebugNames))) {
                                Command* cmd = new ScenePropertyCmd<int>(project, sceneProject->id, "ssr_debug_mode", debugMode);
                                CommandHandle::get(sceneProject->id)->addCommand(cmd);
                            }
                            ImGui::SetItemTooltip("Reflection Source colours what SSR swaps against: blue the sky, red/green/yellow/cyan "
                                                  "a reflection probe's slot, orange a probe without a slot (no SSR), grey the ambient, "
                                                  "magenta added only, black none. Occlusion shows what the swap is weighed by.");
                        }
                    }

                    ImGui::EndTable();
                }
            }

            if (sceneProject->sceneType != SceneType::SCENE_UI) {
                ImGui::SeparatorText("Physics");

                if (ImGui::BeginTable("scene_physics_table", 2, tableFlags)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Interpolate"));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    if (sceneProject->sceneType == SceneType::SCENE_3D) {
                        drawScenePropertyRow<Vector3>(sceneProject, "physics_gravity_3d", "Gravity", ScenePropertyInputType::DragFloat, -1.0f, 0.0f, 1.0f,
                            "Gravity applied to 3D physics bodies (meters/s^2).");
                    } else {
                        drawScenePropertyRow<Vector2>(sceneProject, "physics_gravity_2d", "Gravity", ScenePropertyInputType::DragFloat, -1.0f, 0.0f, 1.0f,
                            "Gravity applied to 2D physics bodies (meters/s^2).");
                    }
                    drawScenePropertyRow<bool>(sceneProject, "physics_interpolation", "Interpolate", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
                        "Draws bodies between their last two fixed steps, so they move smoothly at any frame rate (a frame behind the simulation by up to one step). Bodies moved by a script or an animation keep the pose they were given.");
                    if (sceneProject->sceneType != SceneType::SCENE_3D) {
                        drawScenePropertyRow<int>(sceneProject, "physics_2d_sub_steps", "Sub Steps", ScenePropertyInputType::DragInt, 6 * ImGui::GetFontSize(), 1.0f, 16.0f,
                            "Box2D solver sub-steps of each fixed step (1-16, default 4). More sub-steps make joint chains, stacks and stiff springs steadier, at a cost.");
                    }

                    ImGui::EndTable();
                }
            }

            // the UI's own space: a UI scene's native UI and every scene's screen HTML documents
            ImGui::SeparatorText("Canvas Scaler");

            if (ImGui::BeginTable("scene_canvas_scaler_table", 2, tableFlags)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Reference Resolution"));
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                using jasperengine::editor::SceneCanvasScaler;
                const SceneCanvasScaler scaler = jasperengine::editor::Catalog::getSceneProperty<SceneCanvasScaler>(sceneProject->scene, "canvas_scaler");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Use Project Default");
                ImGui::SetItemTooltip("Project Settings > Graphics > UI Scale applies. Off: this scene's own Canvas Scaler.");
                ImGui::TableSetColumnIndex(1);
                bool useDefault = scaler.useProjectDefault;
                if (ImGui::Checkbox("##canvas_scaler_default", &useDefault)) {
                    SceneCanvasScaler changed = scaler;
                    changed.useProjectDefault = useDefault;
                    // the scene's own settings start from the project's
                    if (!useDefault && scaler.settings == CanvasScalerSettings())
                        changed.settings = project->getUIScale();
                    Command* cmd = new ScenePropertyCmd<SceneCanvasScaler>(project, sceneProject->id, "canvas_scaler", changed);
                    CommandHandle::get(sceneProject->id)->addCommandNoMerge(cmd);
                }
                if (useDefault) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", canvasScalerModeLabel(project->getUIScale().mode));
                } else {
                    CanvasScalerSettings edited = scaler.settings;
                    bool editFinished = false;
                    if (drawCanvasScalerFields("##scene_canvas_scaler", edited, &editFinished)) {
                        SceneCanvasScaler changed = scaler;
                        changed.settings = edited;
                        cmd = new ScenePropertyCmd<SceneCanvasScaler>(project, sceneProject->id, "canvas_scaler", changed);
                        CommandHandle::get(sceneProject->id)->addCommand(cmd);
                    }
                    // a drag is one undo step
                    if (editFinished && cmd) {
                        cmd->setNoMerge();
                        cmd = nullptr;
                    }
                }

                const CanvasScalerSettings& effective = useDefault ? project->getUIScale() : scaler.settings;
                if (effective.mode != UIScaleMode::FollowGameCanvas) {
                    float viewWidth = 0.0f, viewHeight = 0.0f, dpi = 0.0f;
                    project->getEditViewSize(viewWidth, viewHeight, dpi);
                    int gameCanvasWidth = 0, gameCanvasHeight = 0;
                    Rect viewRect;
                    Engine::computeViewLayout((int)viewWidth, (int)viewHeight, (int)project->getCanvasWidth(), (int)project->getCanvasHeight(),
                                              project->getScalingMode(), gameCanvasWidth, gameCanvasHeight, viewRect);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("%s", canvasScalerPreview(effective, viewRect.getWidth(), viewRect.getHeight(),
                                                                  (float)gameCanvasWidth, (float)gameCanvasHeight, dpi).c_str());
                    ImGui::SetItemTooltip("For the Game view's current resolution; the Scene view lays the UI out at this size.");
                }

                ImGui::EndTable();
            }

            ImGui::SeparatorText("Fixed Resolution");
            // R4: a Pixel Perfect Camera on the main camera drives the target instead
            if (const std::string note = pixelart::fixedResolutionNote(sceneProject); !note.empty())
                ImGui::TextDisabled("%s", note.c_str());

            if (ImGui::BeginTable("scene_fixed_resolution_table", 2, tableFlags)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Enabled"));
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                drawScenePropertyRow<bool>(sceneProject, "fixed_resolution_enabled", "Enabled", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
                    "Renders the main camera at a fixed resolution and upscales it to the view.\n"
                    "Applies only in play mode and when this scene is the main scene; layer\n"
                    "scenes stay at native resolution on top. Letterbox/crop still follows the\n"
                    "project canvas and scaling mode.");

                if (jasperengine::editor::Catalog::getSceneProperty<bool>(sceneProject->scene, "fixed_resolution_enabled")) {
                    drawScenePropertyRow<int>(sceneProject, "fixed_resolution_width", "Width", ScenePropertyInputType::DragInt, 6 * ImGui::GetFontSize(), 1.0f, 8192.0f);
                    drawScenePropertyRow<int>(sceneProject, "fixed_resolution_height", "Height", ScenePropertyInputType::DragInt, 6 * ImGui::GetFontSize(), 1.0f, 8192.0f);
                    drawScenePropertyRow<TextureFilter>(sceneProject, "fixed_resolution_filter", "Filter", ScenePropertyInputType::Combo, 7 * ImGui::GetFontSize(), 0.0f, 1.0f,
                        "How the low-resolution image is sampled when upscaled:\n"
                        "- Nearest: crisp pixels (pixel-art look)\n"
                        "- Linear: smooth interpolation");
                }

                ImGui::EndTable();
            }

            // The engine's post-processing of the main camera (effects, presets, cost), then the
            // ordered user passes run after the scene color (and SSR) pass: each one a library
            // post-process shader or a fork whose uniforms become rows here.
            if (sceneProject->sceneType != SceneType::SCENE_UI) {
                ImGui::SeparatorText("Post-processing");
                drawScenePostProcessing(sceneProject);
                ImGui::Spacing();
                ImGui::SeparatorText("Custom Passes");
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("Fullscreen shaders of the library or your own, run in order after the scene is drawn (before "
                                    "the uber pass): each costs one more pass at the scene's resolution.");
                ImGui::PopTextWrapPos();
                drawScenePostProcess(sceneProject);
            }

            // Scene default shaders: used by components of each type whose customShader is
            // empty (priority: component shader > scene default > built-in).
            ImGui::SeparatorText("Default Shaders");

            // each type in a table of its own, with the properties of a set default under it:
            // the scene's values, inherited by the objects without a shader of their own
            if (sceneProject->sceneType != SceneType::SCENE_UI) {
                drawSceneDefaultShader(sceneProject, ShaderType::MESH, "default_mesh_shader", "Mesh");
                if (sceneProject->sceneType == SceneType::SCENE_3D)
                    drawSceneDefaultShader(sceneProject, ShaderType::SKYBOX, "default_sky_shader", "Skybox");
            }
            drawSceneDefaultShader(sceneProject, ShaderType::UI, "default_ui_shader", "UI");
            if (sceneProject->sceneType != SceneType::SCENE_UI) {
                drawSceneDefaultShader(sceneProject, ShaderType::POINTS, "default_points_shader", "Points");
                drawSceneDefaultShader(sceneProject, ShaderType::LINES, "default_lines_shader", "Lines");
            }
        }

        thumbnailTextures.clear();
    }

    // Clean up unused material renders
    for (auto it = materialRenders.begin(); it != materialRenders.end(); ) {
        if (usedPreviewIds.find(it->first) == usedPreviewIds.end()) {
            if (!Engine::isSceneRunning(it->second.getScene())){
                it = materialRenders.erase(it);
            } else {
                // Its one-shot render is still queued (a frame that drew nothing, e.g. a
                // minimized window): drop it on a later frame. Without advancing here the
                // loop never ended and froze the editor.
                ++it;
            }
        } else {
            ++it;
        }
    }

    // Clean up unused direction renders
    for (auto it = directionRenders.begin(); it != directionRenders.end(); ) {
        if (usedPreviewIds.find(it->first) == usedPreviewIds.end()) {
            if (!Engine::isSceneRunning(it->second.getScene())){
                it = directionRenders.erase(it);
            } else {
                // Its one-shot render is still queued (a frame that drew nothing, e.g. a
                // minimized window): drop it on a later frame. Without advancing here the
                // loop never ended and froze the editor.
                ++it;
            }
        } else {
            ++it;
        }
    }
    usedPreviewIds.clear();

    scriptCreateDialog.show();
    shaderForkDialog.show();
    RagdollWizardDialog::instance().show();
    textureSlicerToolDialog.show();

    ImGui::End();

    if (wasOpen && !windowOpen) {
        setOpen(false);
    }
}

// the rows InspectorShadows.cpp draws
template void editor::InspectorWindow::drawScenePropertyRow<float>(SceneProject*, const std::string&, const char*, ScenePropertyInputType, float, float, float, const std::string&);
template void editor::InspectorWindow::drawScenePropertyRow<bool>(SceneProject*, const std::string&, const char*, ScenePropertyInputType, float, float, float, const std::string&);
template void editor::InspectorWindow::drawScenePropertyRow<ShadowQuality>(SceneProject*, const std::string&, const char*, ScenePropertyInputType, float, float, float, const std::string&);
