// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the scene, prefab, project and material decoders know (docs/data-safety.md,
// docs/specs/S03-data-safety.md R24): the keys each component decoder reads, the root keys of
// each document kind, the keys of the nested structures checked inside components, and the
// name table of every enum read from YAML. A key or value outside these tables is reported by
// the load report and preserved (StreamPreserved.h) instead of being dropped silently.
//
// The components' keys and enum names come from their reflection tables (editor/reflect,
// docs/specs/S08-reflection.md): a field added to a table is known here. The rest are hand-kept:
// tools/check_stream_keys.py checks that every key a decoder reads is listed, StreamSchemaTest
// that every key an encoder writes is. A spec that adds a serialized key outside the component
// tables adds it here too (docs/data-safety.md).

#include "Catalog.h"
#include "reflect/Reflect.h"
#include "Project.h"
#include "Generator.h"
#include "window/TerrainEditWindow.h"
#include "render/shadow/ShadowTiers.h"
#include "render/shadow/ShadowTypes.h"

#include "yaml-cpp/yaml.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "component/AudioSourceComponent.h"
#include "navigation/NavCrowd.h"
#include "navigation/GridPathfinder.h"
#include "navigation/NavGridMap.h"
#include "particles/LegacyParticleSystem.h"

namespace jasperengine{
    enum class ActionState;
    enum class AnchorPreset;
    enum class BuoyancyColumns;
    enum class CloudsPreset;
    enum class Collider2DType;
    enum class Collider3DType;
    enum class FluidOutput;
    enum class FogColorMode;
    enum class FogType;
    enum class FogVolumeFalloff;
    enum class FogVolumeShape;
    enum class ForceFieldKind;
    enum class ForceFieldShapeType;
    enum class Joint2DType;
    enum class Joint3DType;
    enum class LatticeInterpolation;
    enum class LatticeJiggleAnchor;
    enum class LatticeMaskChannel;
    enum class LatticeOutside;
    enum class LatticeStage;
    enum class LayoutGroupType;
    enum class LightShading;
    enum class LightType;
    enum class LightUnit;
    enum class NavAgentDriver;
    enum class NavCollectObjects;
    enum class NavGridSource;
    enum class NavModifierInclusion;
    enum class NavObstacleShape;
    enum class NavUseGeometry;
    enum class ProgressbarType;
    enum class ScrollbarType;
    enum class ShadowCaster2DShape;
    enum class UIDocumentRenderMode;
    enum class WaterVolumeWaves;
}

namespace jasperengine::editor {

    // The YAML names of an enum: the name each value is written as, and older or alternative
    // spellings still read (aliases). An unknown name reads as the fallback.
    template<typename E>
    class EnumTable {
    public:
        struct Name {
            E value;
            const char* name;
        };
        using Normalizer = std::string (*)(std::string_view);

        EnumTable(const char* typeName, std::vector<Name> names, E fallback, std::vector<Name> aliases = {}, Normalizer normalize = nullptr):
                typeName_(typeName), names_(std::move(names)), aliases_(std::move(aliases)), fallback_(fallback), normalize_(normalize) {
            for (const Name& name : names_)
                nameList_.emplace_back(name.name);
        }

        // The names of a component enum's table (its YAML tokens and their aliases)
        explicit EnumTable(const reflect::EnumDesc& desc):
                typeName_(desc.cppType), fallback_(static_cast<E>(desc.fallback)), normalize_(desc.normalize) {
            for (const reflect::EnumItem& item : desc.items) {
                names_.push_back({static_cast<E>(item.value), item.token});
                for (const char* alias : item.aliases)
                    aliases_.push_back({static_cast<E>(item.value), alias});
            }
            for (const Name& name : names_)
                nameList_.emplace_back(name.name);
        }

        const char* typeName() const { return typeName_; }
        E fallback() const { return fallback_; }
        // The written names, in table order (the candidates of "did you mean")
        const std::vector<std::string>& names() const { return nameList_; }

        // The written name of a value; an unlisted value writes as the fallback
        std::string toString(E value) const {
            for (const Name& name : names_) {
                if (name.value == value)
                    return name.name;
            }
            for (const Name& name : names_) {
                if (name.value == fallback_)
                    return name.name;
            }
            return names_.empty() ? std::string() : std::string(names_.front().name);
        }

        bool find(const std::string& text, E& value) const {
            const std::string key = normalize_ ? normalize_(text) : text;
            for (const Name& name : names_) {
                if (key == name.name) {
                    value = name.value;
                    return true;
                }
            }
            for (const Name& name : aliases_) {
                if (key == name.name) {
                    value = name.value;
                    return true;
                }
            }
            return false;
        }

        E fromString(const std::string& text) const {
            E value = fallback_;
            return find(text, value) ? value : fallback_;
        }

    private:
        const char* typeName_;
        std::vector<Name> names_;
        std::vector<Name> aliases_;
        E fallback_;
        Normalizer normalize_;
        std::vector<std::string> nameList_;
    };

    // Root maps whose keys are checked
    enum class RootKind {
        Scene,          // a .scene file
        SceneSettings,  // its `scene:` block
        Entity,         // an entity node (scenes, prefabs, clipboard)
        Project,        // project.yaml
        Material,       // a .material file
        PrefabHeader    // a .prefab file
    };

    // Structures below a component's top level whose keys are checked (an unknown one there
    // is reported as dropped: preservation stops at a component's top level)
    enum class NestedKind {
        Submesh,
        Material,
        MaterialPass,
        Stencil,
        Texture
    };

    class StreamSchema {
    public:
        // The top-level keys a component's decoder reads
        static const std::vector<std::string>& knownFieldKeys(ComponentType type);
        static const std::vector<std::string>& knownRootKeys(RootKind kind);
        static const std::vector<std::string>& knownNestedKeys(NestedKind kind);
        static bool isKnownFieldKey(ComponentType type, const std::string& key);
        static bool isKnownRootKey(RootKind kind, const std::string& key);
        static bool isKnownNestedKey(NestedKind kind, const std::string& key);
        static const char* nestedKindName(NestedKind kind);

        // Every component name a scene can hold (Catalog::getComponentName without suffix)
        static const std::vector<std::string>& componentNames();
        // ComponentType of a component key, false for a name this editor does not know
        static bool findComponentType(const std::string& name, ComponentType& type);

        // Name tables of every enum the decoders read
        static const EnumTable<ActionState>& actionState();
        static const EnumTable<AnchorPreset>& anchorPreset();
        static const EnumTable<AttributeDataType>& attributeDataType();
        static const EnumTable<AttributeType>& attributeType();
        static const EnumTable<AudioSourceAttenuation>& audioSourceAttenuation();
        static const EnumTable<AudioSourceState>& audioSourceState();
        static const EnumTable<BodyType>& bodyType();
        static const EnumTable<BufferType>& bufferType();
        static const EnumTable<BufferUsage>& bufferUsage();
        static const EnumTable<CameraType>& cameraType();
        static const EnumTable<Collider2DType>& collider2DType();
        static const EnumTable<Collider3DType>& collider3DType();
        static const EnumTable<CullingMode>& cullingMode();
        static const EnumTable<EaseType>& easeType();
        static const EnumTable<FluidOutput>& fluidOutput();
        static const EnumTable<AtmosphereQuality>& atmosphereQuality();
        static const EnumTable<AtmosphereQualityOverride>& atmosphereQualityOverride();
        static const EnumTable<CloudsPreset>& cloudsPreset();
        static const EnumTable<CloudsRendering>& cloudsRendering();
        static const EnumTable<FogColorMode>& fogColorMode();
        static const EnumTable<FogType>& fogType();
        static const EnumTable<FogVolumeFalloff>& fogVolumeFalloff();
        static const EnumTable<FogVolumeShape>& fogVolumeShape();
        static const EnumTable<WaterVolumeWaves>& waterVolumeWaves();
        // navigation (S19)
        static const EnumTable<NavCollectObjects>& navCollectObjects();
        static const EnumTable<NavUseGeometry>& navUseGeometry();
        static const EnumTable<nav::NavPlane>& navPlane();
        static const EnumTable<nav::AvoidanceQuality>& navAvoidanceQuality();
        static const EnumTable<NavAgentDriver>& navAgentDriver();
        static const EnumTable<NavObstacleShape>& navObstacleShape();
        static const EnumTable<NavModifierInclusion>& navModifierInclusion();
        static const EnumTable<nav::GridLayout>& navGridLayout();
        static const EnumTable<nav::GridMovement>& navGridMovement();
        static const EnumTable<NavGridSource>& navGridSource();
        static const EnumTable<BuoyancyColumns>& buoyancyColumns();
        static const EnumTable<ForceFieldShapeType>& forceFieldShape();
        static const EnumTable<ForceFieldKind>& forceFieldKind();
        static const EnumTable<Joint2DType>& joint2DType();
        static const EnumTable<Joint3DType>& joint3DType();
        static const EnumTable<LayoutGroupType>& layoutGroupType();
        static const EnumTable<LatticeInterpolation>& latticeInterpolation();
        static const EnumTable<LatticeJiggleAnchor>& latticeJiggleAnchor();
        static const EnumTable<LatticeMaskChannel>& latticeMaskChannel();
        static const EnumTable<LatticeOutside>& latticeOutside();
        static const EnumTable<LatticeStage>& latticeStage();
        static const EnumTable<LightShading>& lightShading();
        static const EnumTable<LightState>& lightState();
        static const EnumTable<LightType>& lightType();
        static const EnumTable<LightUnit>& lightUnit();
        static const EnumTable<LitQuality>& litQuality();
        static const EnumTable<PbrTier>& pbrTier();
        static const EnumTable<MaterialAlphaMode>& materialAlphaMode();
        static const EnumTable<MaterialBlendMode>& materialBlendMode();
        static const EnumTable<MaterialCullMode>& materialCullMode();
        static const EnumTable<MaterialDepthTest>& materialDepthTest();
        static const EnumTable<MaterialDepthWrite>& materialDepthWrite();
        static const EnumTable<MaterialLighting>& materialLighting();
        static const EnumTable<MaterialPassOrder>& materialPassOrder();
        static const EnumTable<MaterialStencilCompare>& materialStencilCompare();
        static const EnumTable<MaterialStencilOp>& materialStencilOp();
        static const EnumTable<ParticleEmitterShape>& particleEmitterShape();
        static const EnumTable<PivotPreset>& pivotPreset();
        static const EnumTable<PrimitiveType>& primitiveType();
        static const EnumTable<ProgressbarType>& progressbarType();
        static const EnumTable<RigidbodyMotionQuality>& rigidbodyMotionQuality();
        static const EnumTable<Scaling>& scalingMode();
        static const EnumTable<SceneColorScale>& sceneColorScale();
        static const EnumTable<SceneType>& sceneType();
        static const EnumTable<ScriptPropertyType>& scriptPropertyType();
        static const EnumTable<ScriptType>& scriptType();
        static const EnumTable<ScrollbarType>& scrollbarType();
        static const EnumTable<ShadowCaster2DShape>& shadowCaster2DShape();
        static const EnumTable<ShadowQuality>& shadowQuality();
        static const EnumTable<ShadowUpdateMode>& shadowUpdateMode();
        static const EnumTable<CascadeSplitMode>& cascadeSplitMode();
        static const EnumTable<ShadowTier>& shadowTier();
        static const EnumTable<TerrainBrushFalloff>& terrainBrushFalloff();
        static const EnumTable<TerrainBrushMode>& terrainBrushMode();
        static const EnumTable<TerrainBrushShape>& terrainBrushShape();
        static const EnumTable<TextureAlphaBorder>& textureAlphaBorder();
        static const EnumTable<TextureFilter>& textureFilter();
        static const EnumTable<TextureStrategy>& textureStrategy();
        static const EnumTable<TextureWrap>& textureWrap();
        static const EnumTable<UIDocumentRenderMode>& uiDocumentRenderMode();
        static const EnumTable<UIEventState>& uiEventState();
        static const EnumTable<WindingOrder>& windingOrder();
        static const EnumTable<WindowMode>& windowMode();
    };

    namespace StreamLoad {
        // An enum value the table does not name: reported, and preserved when it sits at a
        // component's top level (StreamPreserved.cpp)
        void unknownEnumValue(const YAML::Node& value, const char* typeName, const std::vector<std::string>& names);
    }

    // The value of an enum node by its table; the table's fallback, reported and preserved
    // (StreamLoad::unknownEnumValue), when the text is not one of its names or not a scalar
    template<typename E>
    E decodeEnumValue(const YAML::Node& value, const EnumTable<E>& table) {
        E result = table.fallback();
        if (value && value.IsScalar() && table.find(value.Scalar(), result))
            return result;
        StreamLoad::unknownEnumValue(value, table.typeName(), table.names());
        return table.fallback();
    }

    // decodeEnumValue of node[key]; the fallback without a report when the key is absent
    template<typename E>
    E decodeEnum(const YAML::Node& node, const char* key, const EnumTable<E>& table) {
        const YAML::Node value = node[key];
        if (!value)
            return table.fallback();
        return decodeEnumValue(value, table);
    }

}
