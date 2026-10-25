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

void editor::InspectorWindow::drawDefaultComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    const reflect::ComponentDesc* desc = reflect::find(cpType);
    if (!desc || entities.empty()) return;
    Scene* scene = sceneProject->scene;

    std::vector<void*> comps;
    for (Entity entity : entities) {
        if (void* comp = desc->find(scene, entity)) comps.push_back(comp);
    }
    if (comps.empty()) return;

    // the sections in order: the ungrouped rows, then each group from its first row's place
    std::vector<Section> sections;
    sections.push_back({nullptr, 0, 0, {}});
    for (size_t i = 0; i < desc->body.fields.size(); i++) {
        const reflect::FieldDesc& field = desc->body.fields[i];
        if (field.flags & (reflect::Hidden | reflect::Derived)) continue;
        if (desc->hasEnabled && std::string_view(field.name) == "enabled") continue;
        if ((field.flags & reflect::NoMultiEdit) && entities.size() > 1) continue;
        RowPropertyType type;
        if (!rowType(field, type)) continue;
        if (field.visibleIf && std::none_of(comps.begin(), comps.end(), [&field](void* comp) { return field.visibleIf(comp); })) continue;
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
        float labelSize = 0.0f;
        for (const Row& row : section.rows) labelSize = std::max(labelSize, getLabelSize(reflect::label(*row.field)));
        if (section.group) separatorText(section.group);
        beginTable(cpType, labelSize, section.group ? std::string(desc->key) + "_" + section.group : std::string());
        for (const Row& row : section.rows) {
            const reflect::FieldDesc& field = *row.field;
            RowSettings settings = base;
            settings.help = reflect::tooltip(field);
            if (field.step > 0.0f) settings.stepSize = field.step;
            if (field.format) settings.format = field.format;
            if (field.kind == reflect::Kind::Int && reflect::hasRange(field) && std::isfinite(field.min) && std::isfinite(field.max)) {
                settings.intMin = static_cast<int>(field.min);
                settings.intMax = static_cast<int>(field.max);
            }
            if (field.enumDesc) settings.enumEntries = enumEntries(*field.enumDesc, field.enumMask);
            if (row.type == RowPropertyType::UIntSlider) settings.sliderValues = &powerOfTwoSizes;
            if (field.drawer && std::string_view(field.drawer) == "modelEntity") {
                settings.entityFilter.set(scene->getComponentId<ModelComponent>());
            }
            const bool enabled = !(field.flags & reflect::ReadOnly) &&
                (!field.enabledIf || std::all_of(comps.begin(), comps.end(), [&field](void* comp) { return field.enabledIf(comp); }));
            if (!enabled) ImGui::BeginDisabled();
            propertyRow(row.type, cpType, field.name, reflect::label(field), sceneProject, entities, settings);
            if (!enabled) ImGui::EndDisabled();
        }
        endTable();
    }

    drawDefaultComponentNotes(cpType, sceneProject, entities);
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
        case ComponentType::FogComponent: return &InspectorWindow::drawFogComponent;
        case ComponentType::VolumetricCloudsComponent: return &InspectorWindow::drawVolumetricCloudsComponent;
        case ComponentType::ReflectionProbeComponent: return &InspectorWindow::drawReflectionProbeComponent;
        case ComponentType::UIDocumentComponent: return &InspectorWindow::drawUIDocumentComponent;
        case ComponentType::CameraComponent: return &InspectorWindow::drawCameraComponent;
        case ComponentType::AudioSourceComponent: return &InspectorWindow::drawAudioComponent;
        case ComponentType::SkyboxComponent: return &InspectorWindow::drawSkyboxComponent;
        case ComponentType::ParticleSystemComponent: return &InspectorWindow::drawParticleSystemComponent;
        case ComponentType::LineRendererComponent: return &InspectorWindow::drawLineRendererComponent;
        case ComponentType::PolygonComponent: return &InspectorWindow::drawPolygonComponent;
        case ComponentType::MeshPolygonComponent: return &InspectorWindow::drawMeshPolygonComponent;
        case ComponentType::PointsComponent: return &InspectorWindow::drawPointsComponent;
        case ComponentType::InstancedMeshComponent: return &InspectorWindow::drawInstancedMeshComponent;
        case ComponentType::ScriptComponent: return &InspectorWindow::drawScriptComponent;
        case ComponentType::Rigidbody2DComponent: return &InspectorWindow::drawRigidbody2DComponent;
        case ComponentType::RigidbodyComponent: return &InspectorWindow::drawRigidbodyComponent;
        case ComponentType::Joint2DComponent: return &InspectorWindow::drawJoint2DComponent;
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
        default: return nullptr;
    }
}
