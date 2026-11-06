// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The default Inspector (docs/specs/S08-reflection.md R10-R13): a component's rows drawn from its
// reflection table through propertyRow, so multi-edit, prefab overrides and undo come with every
// row. Fields are drawn in uiOrder (table order among equals), the ungrouped ones first, then each
// group under its heading; Hidden fields and the enable checkbox (the header's) are left out; a
// field whose visibleIf is false for every selected GameObject is not drawn, one whose enabledIf
// is false for any is drawn disabled. A named drawer gives a field its widget ("label",
// "direction", "po2Slider"...). Components the table cannot draw keep their own drawers
// (componentDrawer).

#include "InspectorWindow.h"

#include "Catalog.h"
#include "Theme.h"
#include "component/FogComponent.h"
#include "component/ModelComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "command/CommandHandle.h"
#include "command/type/ReflectedValueCmd.h"
#include "reflect/Enums.h"
#include "reflect/Reflect.h"
#include "subsystem/RenderSystem.h"
#include "external/IconsFontAwesome6.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

std::vector<int> editor::InspectorWindow::powerOfTwoSizes = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384 };

namespace {

    struct NamedDrawer {
        const char* name;
        RowPropertyType type;
    };

    // the widgets a field asks for by name (FieldDesc::drawer)
    constexpr NamedDrawer kNamedDrawers[] = {
        {"label", RowPropertyType::Label},
        {"multiline", RowPropertyType::MultilineString},
        {"direction", RowPropertyType::Direction},
        {"halfCone", RowPropertyType::HalfCone},
        {"po2Slider", RowPropertyType::UIntSlider},
        {"textureCube", RowPropertyType::TextureCube},
        {"modelEntity", RowPropertyType::LocalEntity},
        {"curve", RowPropertyType::Curve},
        {"gradient", RowPropertyType::Gradient},
    };

    bool drawsColor(const reflect::FieldDesc& field) {
        return field.flags & (reflect::ColorLinear | reflect::ColorSRGB);
    }

    // The row of a field: its named drawer's, else its kind's (a hard range of 0..1 a 0..1 slider, a
    // non-negative one a positive field, a colour a colour picker)
    bool rowType(const reflect::FieldDesc& field, RowPropertyType& type) {
        if (field.drawer) {
            for (const NamedDrawer& drawer : kNamedDrawers) {
                if (std::string_view(drawer.name) == field.drawer) {
                    type = drawer.type;
                    return true;
                }
            }
            return false;
        }
        const bool zeroToOne = field.min == 0.0f && field.max == 1.0f;
        const bool positive = field.min == 0.0f && !std::isfinite(field.max);
        switch (field.kind) {
            case reflect::Kind::Bool: type = RowPropertyType::Bool; return true;
            case reflect::Kind::String: type = RowPropertyType::String; return true;
            case reflect::Kind::Float:
                type = zeroToOne ? RowPropertyType::Float_0_1 : (positive ? RowPropertyType::FloatPositive : RowPropertyType::Float);
                return true;
            case reflect::Kind::Double: type = positive ? RowPropertyType::DoublePositive : RowPropertyType::Double; return true;
            case reflect::Kind::Int: type = RowPropertyType::Int; return true;
            case reflect::Kind::UInt: type = RowPropertyType::UInt; return true;
            case reflect::Kind::Vector2: type = RowPropertyType::Vector2; return true;
            case reflect::Kind::Vector3: type = drawsColor(field) ? RowPropertyType::Color3L : RowPropertyType::Vector3; return true;
            case reflect::Kind::Vector4: type = drawsColor(field) ? RowPropertyType::Color4L : RowPropertyType::Vector4; return true;
            case reflect::Kind::Quat: type = RowPropertyType::Quat; return true;
            case reflect::Kind::Enum: type = RowPropertyType::Enum; return true;
            case reflect::Kind::Ease: type = RowPropertyType::Ease; return true;
            case reflect::Kind::Entity: type = RowPropertyType::LocalEntity; return true;
            case reflect::Kind::Texture: type = RowPropertyType::Texture; return true;
            case reflect::Kind::Font: type = RowPropertyType::Font; return true;
            case reflect::Kind::AssetPath: type = RowPropertyType::Asset; return true;
            case reflect::Kind::LayerMask: type = RowPropertyType::LayerMask; return true;
            case reflect::Kind::Material: type = RowPropertyType::Material; return true;
            default: return false;
        }
    }

    struct Row {
        const reflect::FieldDesc* field;
        RowPropertyType type;
        size_t index;
    };

    struct Section {
        const char* group;   // null: the component's own table
        int order;
        size_t index;
        std::vector<Row> rows;
    };

}

std::vector<EnumEntry>* editor::InspectorWindow::enumEntries(const reflect::EnumDesc& desc, uint64_t mask) {
    // the values a dropdown offers, by their labels, in the table's Inspector order
    static std::mutex mutex;
    static std::map<std::pair<const reflect::EnumDesc*, uint64_t>, std::vector<EnumEntry>> tables;
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<EnumEntry>& entries = tables[{&desc, mask}];
    if (entries.empty()) {
        for (const reflect::EnumItem* item : reflect::enums::offered(desc, mask)) {
            entries.push_back({item->value, item->label});
        }
    }
    return &entries;
}

namespace {

    // a struct drawn as a foldout of its members, an array as a foldout of its elements
    bool complexField(const reflect::FieldDesc& field) {
        if (field.drawer) return false;
        if (field.kind == reflect::Kind::Struct) return field.structDesc != nullptr;
        return field.kind == reflect::Kind::Array && field.arrayOps && (field.structDesc || field.elementOps);
    }

    // a field's row settings: its tooltip (with unit and range), step, format, integer range,
    // dropdown entries and slider values
    RowSettings fieldSettings(const reflect::FieldDesc& field, RowPropertyType type, const RowSettings& base) {
        RowSettings settings = base;
        settings.help = reflect::tooltip(field);
        if (field.step > 0.0f) settings.stepSize = field.step;
        if (field.format) settings.format = field.format;
        // what a number typed into it is in ("25cm" on a Meters field, S23)
        switch (field.unit) {
            case reflect::Unit::Meters: settings.unit = NumericUnit::Meters; break;
            case reflect::Unit::Degrees: settings.unit = NumericUnit::Degrees; break;
            case reflect::Unit::Seconds: settings.unit = NumericUnit::Seconds; break;
            case reflect::Unit::Pixels: settings.unit = NumericUnit::Pixels; break;
            default: break;
        }
        if (field.kind == reflect::Kind::Int && std::isfinite(field.min) && std::isfinite(field.max)) {
            settings.intMin = static_cast<int>(field.min);
            settings.intMax = static_cast<int>(field.max);
        }
        if (field.enumDesc) settings.enumEntries = InspectorWindow::enumEntries(*field.enumDesc, field.enumMask);
        if (type == RowPropertyType::UIntSlider) settings.sliderValues = &InspectorWindow::powerOfTwoSizes;
        settings.colorHdr = (field.flags & reflect::Hdr) != 0;
        settings.colorSrgb = (field.flags & reflect::ColorSRGB) != 0;
        if (type == RowPropertyType::Curve && std::isfinite(field.uiMin) && std::isfinite(field.uiMax) && field.uiMin < field.uiMax) {
            settings.curveValueMin = field.uiMin;
            settings.curveValueMax = field.uiMax;
        }
        return settings;
    }

    // the row of an array's element: its field's widget for the element's kind
    bool elementRowType(const reflect::FieldDesc& field, RowPropertyType& type) {
        reflect::FieldDesc element = field;
        element.kind = field.elementOps->kind;
        element.ops = field.elementOps;
        return rowType(element, type);
    }

}

void editor::InspectorWindow::drawDefaultComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    const reflect::ComponentDesc* desc = reflect::find(cpType);
    if (!desc || entities.empty()) return;
    Scene* scene = sceneProject->scene;

    std::vector<void*> comps;
    for (Entity entity : entities) {
        if (void* comp = desc->find(scene, entity)) comps.push_back(comp);
    }
    if (comps.empty()) return;

    // the sections in order: the ungrouped rows, then each group from its first row's place;
    // visibleIf and enabledIf read the first selected GameObject's, as the hand drawers did
    std::vector<Section> sections;
    sections.push_back({nullptr, 0, 0, {}});
    for (size_t i = 0; i < desc->body.fields.size(); i++) {
        const reflect::FieldDesc& field = desc->body.fields[i];
        if (field.flags & (reflect::Hidden | reflect::Derived)) continue;
        if (desc->hasEnabled && std::string_view(field.name) == "enabled") continue;
        if ((field.flags & reflect::NoMultiEdit) && entities.size() > 1) continue;
        RowPropertyType type = RowPropertyType::Custom;
        if (!complexField(field) && !rowType(field, type)) continue;
        if (field.visibleIf && !field.visibleIf(comps.front())) continue;
        Section* section = nullptr;
        for (Section& candidate : sections) {
            if ((candidate.group == nullptr) == (field.group == nullptr) &&
                    (!field.group || std::string_view(candidate.group) == field.group)) {
                section = &candidate;
                break;
            }
        }
        if (!section) {
            sections.push_back({field.group, field.uiOrder, i, {}});
            section = &sections.back();
        }
        if (field.uiOrder < section->order || (field.uiOrder == section->order && i < section->index)) {
            section->order = field.uiOrder;
            section->index = i;
        }
        section->rows.push_back({&field, type, i});
    }
    std::stable_sort(sections.begin() + 1, sections.end(), [](const Section& a, const Section& b) {
        return a.order != b.order ? a.order < b.order : a.index < b.index;
    });

    // an edit marks the components for their own update (a widget redraws itself)
    RowSettings base;
    if (desc->edited) {
        auto edited = desc->edited;
        base.onValueChanged = [scene, entities, edited]() {
            for (Entity entity : entities) edited(scene, entity);
        };
    }

    for (Section& section : sections) {
        if (section.rows.empty()) continue;
        std::stable_sort(section.rows.begin(), section.rows.end(), [](const Row& a, const Row& b) {
            return a.field->uiOrder != b.field->uiOrder ? a.field->uiOrder < b.field->uiOrder : a.index < b.index;
        });
        if (section.group) separatorText(section.group);
        // the rows in a table, then the structs and arrays in foldouts of their own
        float labelSize = 0.0f;
        bool rows = false;
        for (const Row& row : section.rows) {
            if (complexField(*row.field)) continue;
            labelSize = std::max(labelSize, getLabelSize(reflect::label(*row.field)));
            rows = true;
        }
        if (rows) {
            beginTable(cpType, labelSize, section.group ? std::string(desc->key) + "_" + section.group : std::string());
            for (const Row& row : section.rows) {
                const reflect::FieldDesc& field = *row.field;
                if (complexField(field)) continue;
                RowSettings settings = fieldSettings(field, row.type, base);
                if (field.drawer && std::string_view(field.drawer) == "modelEntity") {
                    settings.entityFilter.set(scene->getComponentId<ModelComponent>());
                }
                const bool enabled = !(field.flags & reflect::ReadOnly) && (!field.enabledIf || field.enabledIf(comps.front()));
                if (!enabled) ImGui::BeginDisabled();
                propertyRow(row.type, cpType, field.name, reflect::label(field), sceneProject, entities, settings);
                if (!enabled) ImGui::EndDisabled();
            }
            endTable();
        }
        for (const Row& row : section.rows) {
            if (!complexField(*row.field)) continue;
            if (row.field->kind == reflect::Kind::Struct) drawDefaultStruct(cpType, sceneProject, entities, *row.field, row.field->name, base);
            else drawDefaultArray(cpType, sceneProject, entities, *row.field, row.field->name, base);
        }
    }

    drawDefaultComponentNotes(cpType, sceneProject, entities);
}

void editor::InspectorWindow::drawDefaultStruct(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                const reflect::FieldDesc& field, const std::string& path, const RowSettings& base) {
    // a collapsible sub-section of its members (R8)
    if (!beginFoldout((reflect::label(field) + "##" + path).c_str())) return;
    float labelSize = 0.0f;
    for (const reflect::FieldDesc& member : field.structDesc->fields) labelSize = std::max(labelSize, getLabelSize(reflect::label(member)));
    beginTable(cpType, labelSize, path);
    for (const reflect::FieldDesc& member : field.structDesc->fields) {
        RowPropertyType type;
        if ((member.flags & (reflect::Hidden | reflect::Derived)) || complexField(member) || !rowType(member, type)) continue;
        if (member.flags & reflect::ReadOnly) ImGui::BeginDisabled();
        propertyRow(type, cpType, path + "." + member.name, reflect::label(member), sceneProject, entities, fieldSettings(member, type, base));
        if (member.flags & reflect::ReadOnly) ImGui::EndDisabled();
    }
    endTable();
    endFoldout();
}

void editor::InspectorWindow::drawDefaultArray(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                               const reflect::FieldDesc& field, const std::string& path, const RowSettings& base) {
    // a size heading, a row per element with move and remove buttons, and an add button (R8);
    // arrays of different lengths across the selection are mixed: their size and order read-only
    const reflect::ComponentDesc& desc = *reflect::find(cpType);
    std::vector<void*> comps;
    for (Entity entity : entities) comps.push_back(desc.find(sceneProject->scene, entity));
    const reflect::Mixed mixed = reflect::mixedValues(desc, comps, path);
    const reflect::FieldRef first = reflect::resolve(desc, comps.front(), path);
    if (!first) return;
    const size_t count = reflect::elementCount(field, first.owner, first.ref);
    const std::string title = reflect::label(field) + " (" + (mixed.sizeDiffers ? std::string("\xE2\x80\x94") : std::to_string(count)) + ")##" + path;
    if (!beginFoldout(title.c_str())) return;
    // a counted or fixed-size array keeps its length (its owner counts it)
    const bool resizable = !field.count && std::string_view(field.ops->typeName) != "array";
    const bool editable = !mixed.sizeDiffers && !(field.flags & reflect::ReadOnly) && resizable;
    const uint32_t sceneId = sceneProject->id;
    Command* command = nullptr;
    if (!mixed.sizeDiffers) {
        RowPropertyType elementType = RowPropertyType::Custom;
        const bool scalarElements = !field.structDesc && elementRowType(field, elementType);
        beginTable(cpType, getLabelSize("Element 00"), path);
        for (size_t i = 0; i < count; i++) {
            const std::string elementPath = path + "[" + std::to_string(i) + "]";
            const std::string elementLabel = "Element " + std::to_string(i);
            if (scalarElements) {
                propertyRow(elementType, cpType, elementPath, elementLabel, sceneProject, entities, fieldSettings(field, elementType, base));
            } else if (field.structDesc) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(elementLabel.c_str());
                for (const reflect::FieldDesc& member : field.structDesc->fields) {
                    RowPropertyType type;
                    if ((member.flags & (reflect::Hidden | reflect::Derived)) || complexField(member) || !rowType(member, type)) continue;
                    RowSettings settings = fieldSettings(member, type, base);
                    settings.child = true;
                    propertyRow(type, cpType, elementPath + "." + member.name, reflect::label(member), sceneProject, entities, settings);
                }
            }
            if (!editable) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(elementPath.c_str());
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton(ICON_FA_ARROW_UP)) command = ReflectedValueCmd::move(project, sceneId, entities, cpType, path, i, i - 1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 >= count);
            if (ImGui::SmallButton(ICON_FA_ARROW_DOWN)) command = ReflectedValueCmd::move(project, sceneId, entities, cpType, path, i, i + 1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK)) command = ReflectedValueCmd::remove(project, sceneId, entities, cpType, path, i);
            ImGui::PopID();
        }
        endTable();
    }
    ImGui::BeginDisabled(!editable);
    if (ImGui::SmallButton((std::string(ICON_FA_PLUS " Add##") + path).c_str())) {
        // a default element (a default-constructed one of the element's type)
        const reflect::TypeOps* elementOps = field.elementOps;
        command = ReflectedValueCmd::insert(project, sceneId, entities, cpType, path, count, reflect::Value(elementOps));
    }
    ImGui::EndDisabled();
    endFoldout();
    if (command) {
        if (base.onValueChanged) static_cast<ReflectedValueCmd*>(command)->setOnValueChanged(base.onValueChanged);
        CommandHandle::get(sceneId)->addCommand(command);
    }
}

void editor::InspectorWindow::drawDefaultComponentNotes(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>&) {
    if (cpType == ComponentType::LocalVolumetricFogComponent) {
        // it shows through the scene's volumetric fog only
        FogComponent* fog = sceneProject->scene->findComponentFromIndex<FogComponent>(0);
        if (!fog || fog->type != FogType::HEIGHT || !fog->volumetric) {
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("Local Volumetric Fog shows through volumetric fog only: add a Fog of Type Height and turn its Volumetric Fog on.");
        }
    }
}

editor::InspectorWindow::ComponentDrawer editor::InspectorWindow::componentDrawer(ComponentType cpType) {
    // the components whose Inspector is more than their table's rows (previews, buttons, rows
    // that change their label with the state, tables of elements); the others draw by default
    switch (cpType) {
        case ComponentType::Transform: return &InspectorWindow::drawTransform;
        case ComponentType::MeshComponent: return &InspectorWindow::drawMeshComponent;
        case ComponentType::ModelComponent: return &InspectorWindow::drawModelComponent;
        case ComponentType::UIComponent: return &InspectorWindow::drawUIComponent;
        case ComponentType::InputFieldComponent: return &InspectorWindow::drawInputFieldComponent;
        case ComponentType::LayoutGroupComponent: return &InspectorWindow::drawLayoutGroupComponent;
        case ComponentType::ImageComponent: return &InspectorWindow::drawImageComponent;
        case ComponentType::SpriteComponent: return &InspectorWindow::drawSpriteComponent;
        case ComponentType::TilemapComponent: return &InspectorWindow::drawTilemapComponent;
        case ComponentType::TerrainComponent: return &InspectorWindow::drawTerrainComponent;
        case ComponentType::LightComponent: return &InspectorWindow::drawLightComponent;
        case ComponentType::ShadowCaster2DComponent: return &InspectorWindow::drawShadowCaster2DComponent;
        case ComponentType::CapsuleShadowCasterComponent: return &InspectorWindow::drawCapsuleShadowCasterComponent;
        case ComponentType::FogComponent: return &InspectorWindow::drawFogComponent;
        case ComponentType::VolumetricCloudsComponent: return &InspectorWindow::drawVolumetricCloudsComponent;
        case ComponentType::ReflectionProbeComponent: return &InspectorWindow::drawReflectionProbeComponent;
        case ComponentType::UIDocumentComponent: return &InspectorWindow::drawUIDocumentComponent;
        case ComponentType::CameraComponent: return &InspectorWindow::drawCameraComponent;
        case ComponentType::AudioSourceComponent: return &InspectorWindow::drawAudioComponent;
        case ComponentType::AudioSnapshotComponent: return &InspectorWindow::drawAudioSnapshotComponent;
        case ComponentType::SkyboxComponent: return &InspectorWindow::drawSkyboxComponent;
        case ComponentType::ParticleSystemComponent: return &InspectorWindow::drawParticleSystemComponent;
        case ComponentType::ForceFieldComponent: return &InspectorWindow::drawForceFieldComponent;
        case ComponentType::LineRendererComponent: return &InspectorWindow::drawLineRendererComponent;
        case ComponentType::TrailRendererComponent: return &InspectorWindow::drawTrailRendererComponent;
        case ComponentType::PolygonComponent: return &InspectorWindow::drawPolygonComponent;
        case ComponentType::MeshPolygonComponent: return &InspectorWindow::drawMeshPolygonComponent;
        case ComponentType::PointsComponent: return &InspectorWindow::drawPointsComponent;
        case ComponentType::InstancedMeshComponent: return &InspectorWindow::drawInstancedMeshComponent;
        case ComponentType::ScriptComponent: return &InspectorWindow::drawScriptComponent;
        case ComponentType::Rigidbody2DComponent: return &InspectorWindow::drawRigidbody2DComponent;
        case ComponentType::RigidbodyComponent: return &InspectorWindow::drawRigidbodyComponent;
        case ComponentType::Joint2DComponent: return &InspectorWindow::drawJoint2DComponent;
        case ComponentType::CharacterController2DComponent: return &InspectorWindow::drawCharacterController2DComponent;
        case ComponentType::CharacterControllerComponent: return &InspectorWindow::drawCharacterControllerComponent;
        case ComponentType::WaterVolumeComponent: return &InspectorWindow::drawWaterVolumeComponent;
        case ComponentType::BuoyancyComponent: return &InspectorWindow::drawBuoyancyComponent;
        case ComponentType::Joint3DComponent: return &InspectorWindow::drawJoint3DComponent;
        case ComponentType::ActionComponent: return &InspectorWindow::drawActionComponent;
        case ComponentType::PositionTweenComponent: return &InspectorWindow::drawPositionTweenComponent;
        case ComponentType::RotationTweenComponent: return &InspectorWindow::drawRotationTweenComponent;
        case ComponentType::ScaleTweenComponent: return &InspectorWindow::drawScaleTweenComponent;
        case ComponentType::ColorTweenComponent: return &InspectorWindow::drawColorTweenComponent;
        case ComponentType::AlphaTweenComponent: return &InspectorWindow::drawAlphaTweenComponent;
        case ComponentType::SpriteAnimationComponent: return &InspectorWindow::drawSpriteAnimationComponent;
        case ComponentType::AnimationComponent: return &InspectorWindow::drawAnimationComponent;
        case ComponentType::KeyframeTracksComponent: return &InspectorWindow::drawKeyframeTracksComponent;
        case ComponentType::TranslateTracksComponent: return &InspectorWindow::drawTranslateTracksComponent;
        case ComponentType::RotateTracksComponent: return &InspectorWindow::drawRotateTracksComponent;
        case ComponentType::ScaleTracksComponent: return &InspectorWindow::drawScaleTracksComponent;
        case ComponentType::MorphTracksComponent: return &InspectorWindow::drawMorphTracksComponent;
        case ComponentType::LatticeComponent: return &InspectorWindow::drawLatticeComponent;
        case ComponentType::LatticeModifierComponent: return &InspectorWindow::drawLatticeModifierComponent;
        case ComponentType::LatticeFollowerComponent: return &InspectorWindow::drawLatticeFollowerComponent;
        case ComponentType::LatticeTracksComponent: return &InspectorWindow::drawLatticeTracksComponent;
        case ComponentType::PoseAnimatorComponent: return &InspectorWindow::drawPoseAnimatorComponent;
        case ComponentType::AnimationPlayerComponent: return &InspectorWindow::drawAnimationPlayerComponent;
        // navigation (InspectorNavigation.cpp)
        case ComponentType::NavMeshSurfaceComponent: return &InspectorWindow::drawNavMeshSurfaceComponent;
        case ComponentType::NavMeshAgentComponent: return &InspectorWindow::drawNavMeshAgentComponent;
        case ComponentType::NavMeshObstacleComponent: return &InspectorWindow::drawNavMeshObstacleComponent;
        case ComponentType::NavMeshLinkComponent: return &InspectorWindow::drawNavMeshLinkComponent;
        case ComponentType::NavMeshModifierComponent: return &InspectorWindow::drawNavMeshModifierComponent;
        case ComponentType::NavGridComponent: return &InspectorWindow::drawNavGridComponent;
        // the knot table synced with the Scene view, Edit Spline and Draw, the terrain bake, the
        // generators' warnings, Bake to GameObjects (InspectorSplines.cpp)
        case ComponentType::SplineComponent: return &InspectorWindow::drawSplineComponent;
        case ComponentType::SplineExtrudeComponent: return &InspectorWindow::drawSplineExtrudeComponent;
        case ComponentType::SplineInstantiateComponent: return &InspectorWindow::drawSplineInstantiateComponent;
        case ComponentType::SplineAnimateComponent: return &InspectorWindow::drawSplineAnimateComponent;
        case ComponentType::AnimatorComponent: return &InspectorWindow::drawAnimatorComponent;
        case ComponentType::VertexAnimatorComponent: return &InspectorWindow::drawVertexAnimatorComponent;
        // upstream's (InspectorUpstream.cpp)
        case ComponentType::OnScreenStickComponent: return &InspectorWindow::drawOnScreenStickComponent;
        case ComponentType::OnScreenButtonComponent: return &InspectorWindow::drawOnScreenButtonComponent;
        case ComponentType::TouchAreaComponent: return &InspectorWindow::drawTouchAreaComponent;
        case ComponentType::RagdollComponent: return &InspectorWindow::drawRagdollComponent;
        case ComponentType::CanvasGroupComponent: return &InspectorWindow::drawCanvasGroupComponent;
        // pixel art (InspectorPixelArt.cpp)
        case ComponentType::PixelPerfectCameraComponent: return &InspectorWindow::drawPixelPerfectCameraComponent;
        default: return nullptr;
    }
}
