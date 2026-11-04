// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Catalog::getChangedUpdateFlags against what it is defined to answer (docs/specs/S71-mesh-update-flags.md):
// the flags of every property in both components' property lists whose value differs. The oracle
// below builds those two lists, as the function itself did until S71; every property of every
// component is changed in turn and the two must agree, with fixed cases as a check of the oracle.

#include "support/EditorTestSupport.h"

#include "Catalog.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <ctime>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace changed_update_flags_test {

    uint64_t oracle(ComponentType compType, void* oldComp, void* newComp) {
        uint64_t flags = 0;

        auto oldProps = Catalog::getProperties(compType, oldComp);
        auto newProps = Catalog::getProperties(compType, newComp);

        for (auto& [name, newProp] : newProps) {
            auto it = oldProps.find(name);
            if (it == oldProps.end()) continue;

            PropertyData& oldProp = it->second;
            if (!oldProp.ref || !newProp.ref) continue;

            if (oldProp.type != newProp.type) {
                flags |= newProp.updateFlags;
                continue;
            }

            bool changed = false;
            switch (newProp.type) {
                case PropertyType::Bool:
                    changed = *static_cast<bool*>(oldProp.ref) != *static_cast<bool*>(newProp.ref);
                    break;
                case PropertyType::Float:
                    changed = *static_cast<float*>(oldProp.ref) != *static_cast<float*>(newProp.ref);
                    break;
                case PropertyType::Double:
                    changed = *static_cast<double*>(oldProp.ref) != *static_cast<double*>(newProp.ref);
                    break;
                case PropertyType::Int:
                    changed = *static_cast<int*>(oldProp.ref) != *static_cast<int*>(newProp.ref);
                    break;
                case PropertyType::UInt:
                    changed = *static_cast<unsigned int*>(oldProp.ref) != *static_cast<unsigned int*>(newProp.ref);
                    break;
                case PropertyType::String:
                    changed = *static_cast<std::string*>(oldProp.ref) != *static_cast<std::string*>(newProp.ref);
                    break;
                case PropertyType::Asset:
                    changed = *static_cast<AssetRef*>(oldProp.ref) != *static_cast<AssetRef*>(newProp.ref);
                    break;
                case PropertyType::Shader:
                    changed = *static_cast<ShaderRef*>(oldProp.ref) != *static_cast<ShaderRef*>(newProp.ref);
                    break;
                case PropertyType::Curve:
                    changed = *static_cast<Curve*>(oldProp.ref) != *static_cast<Curve*>(newProp.ref);
                    break;
                case PropertyType::Font:
                    changed = *static_cast<FontArray*>(oldProp.ref) != *static_cast<FontArray*>(newProp.ref);
                    break;
                case PropertyType::Vector2:
                    changed = *static_cast<Vector2*>(oldProp.ref) != *static_cast<Vector2*>(newProp.ref);
                    break;
                case PropertyType::Vector3:
                    changed = *static_cast<Vector3*>(oldProp.ref) != *static_cast<Vector3*>(newProp.ref);
                    break;
                case PropertyType::Vector4:
                    changed = *static_cast<Vector4*>(oldProp.ref) != *static_cast<Vector4*>(newProp.ref);
                    break;
                case PropertyType::Quat:
                    changed = *static_cast<Quaternion*>(oldProp.ref) != *static_cast<Quaternion*>(newProp.ref);
                    break;
                case PropertyType::Enum:
                    changed = *static_cast<int*>(oldProp.ref) != *static_cast<int*>(newProp.ref);
                    break;
                case PropertyType::Ease:
                    changed = *static_cast<Ease*>(oldProp.ref) != *static_cast<Ease*>(newProp.ref);
                    break;
                case PropertyType::Texture:
                    changed = *static_cast<Texture*>(oldProp.ref) != *static_cast<Texture*>(newProp.ref);
                    break;
                case PropertyType::Material:
                    changed = *static_cast<Material*>(oldProp.ref) != *static_cast<Material*>(newProp.ref);
                    break;
                case PropertyType::Entity:
                    changed = *static_cast<Entity*>(oldProp.ref) != *static_cast<Entity*>(newProp.ref);
                    break;
                case PropertyType::EntityReference:
                    changed = static_cast<EntityReference*>(oldProp.ref)->entity != static_cast<EntityReference*>(newProp.ref)->entity ||
                              static_cast<EntityReference*>(oldProp.ref)->sceneId != static_cast<EntityReference*>(newProp.ref)->sceneId;
                    break;
                case PropertyType::Gradient:
                    changed = *static_cast<Gradient*>(oldProp.ref) != *static_cast<Gradient*>(newProp.ref);
                    break;
                case PropertyType::Custom:
                    changed = true;
                    break;
            }

            if (changed) {
                flags |= newProp.updateFlags;
            }
        }

        return flags;
    }

    // another value of a property (a Custom one always counts as changed; a byte, as an Enum may be
    // narrower than the int it is compared as)
    void changeValue(const PropertyData& property) {
        void* ref = property.ref;
        if (!ref) return;
        switch (property.type) {
            case PropertyType::Bool: { bool& v = *static_cast<bool*>(ref); v = !v; break; }
            case PropertyType::Float: *static_cast<float*>(ref) += 1.0f; break;
            case PropertyType::Double: *static_cast<double*>(ref) += 1.0; break;
            case PropertyType::Int: *static_cast<int*>(ref) += 1; break;
            case PropertyType::UInt: *static_cast<unsigned int*>(ref) += 1; break;
            case PropertyType::String: *static_cast<std::string*>(ref) += "x"; break;
            case PropertyType::Asset: *static_cast<AssetRef*>(ref) = AssetRef(static_cast<AssetRef*>(ref)->path + "x"); break;
            case PropertyType::Shader: *static_cast<ShaderRef*>(ref) = ShaderRef(static_cast<ShaderRef*>(ref)->path + "x"); break;
            case PropertyType::Curve: static_cast<Curve*>(ref)->keys.emplace_back(); break;
            case PropertyType::Font: (*static_cast<FontArray*>(ref))[0] = AssetRef((*static_cast<FontArray*>(ref))[0].path + "x"); break;
            case PropertyType::Vector2: static_cast<Vector2*>(ref)->x += 1.0f; break;
            case PropertyType::Vector3: static_cast<Vector3*>(ref)->x += 1.0f; break;
            case PropertyType::Vector4: static_cast<Vector4*>(ref)->x += 1.0f; break;
            case PropertyType::Quat: static_cast<Quaternion*>(ref)->x += 1.0f; break;
            case PropertyType::Enum: *static_cast<unsigned char*>(ref) ^= 1; break;
            case PropertyType::Ease: *static_cast<Ease*>(ref) = static_cast<Ease*>(ref)->getType() == EaseType::LINEAR ? EaseType::QUAD_IN : EaseType::LINEAR; break;
            case PropertyType::Texture: static_cast<Texture*>(ref)->setPath(static_cast<Texture*>(ref)->getId() + "x.png"); break;
            case PropertyType::Material: static_cast<Material*>(ref)->metallicFactor += 1.0f; break;
            case PropertyType::Entity: *static_cast<Entity*>(ref) += 1; break;
            case PropertyType::EntityReference: static_cast<EntityReference*>(ref)->entity += 1; break;
            case PropertyType::Gradient: static_cast<Gradient*>(ref)->colorKeys.emplace_back(); break;
            case PropertyType::Custom: break;
        }
    }

    // every array that can grow has elements, down to the arrays in those elements
    void populate(const reflect::StructDesc& desc, void* owner) {
        for (const reflect::FieldDesc& field : desc.fields) {
            if (field.flags & (reflect::NotListed | reflect::Derived)) continue;
            // an out-of-line value stays unset: the sweep makes it
            if (field.peek) continue;
            void* ref = field.ref(owner);
            if (field.kind == reflect::Kind::Struct && field.structDesc) {
                populate(*field.structDesc, ref);
            } else if (field.kind == reflect::Kind::Array && field.arrayOps) {
                if (!field.count && field.arrayOps->resize && field.arrayOps->size(ref) == 0) field.arrayOps->resize(ref, 3);
                if (field.structDesc) {
                    for (size_t i = 0; i < reflect::elementCount(field, owner, ref); i++) {
                        populate(*field.structDesc, field.arrayOps->at(ref, i));
                    }
                }
            }
        }
    }

    // the fixed arrays that keep their count apart, and the extra properties
    void populateCounted(ComponentType type, void* comp) {
        if (type == ComponentType::SpriteComponent) {
            static_cast<SpriteComponent*>(comp)->numFramesRect = 3;
        } else if (type == ComponentType::TilemapComponent) {
            static_cast<TilemapComponent*>(comp)->numTilesRect = 3;
            static_cast<TilemapComponent*>(comp)->numTiles = 3;
        } else if (type == ComponentType::ModelComponent) {
            ModelComponent& model = *static_cast<ModelComponent*>(comp);
            model.bonesIdMapping[1] = 11;
            model.bonesNameMapping["Hips"] = 12;
            model.meshNodesMapping[2] = 13;
            model.nodesIdMapping[3] = 14;
        } else if (type == ComponentType::ScriptComponent) {
            ScriptComponent& script = *static_cast<ScriptComponent*>(comp);
            script.scripts.resize(2);
            for (ScriptEntry& entry : script.scripts) {
                ScriptProperty number;
                number.name = "speed";
                number.type = ScriptPropertyType::Float;
                number.value = 1.0f;
                number.defaultValue = 0.0f;
                ScriptProperty target;
                target.name = "target";
                target.type = ScriptPropertyType::EntityReference;
                target.value = EntityReference{NULL_ENTITY, 0};
                target.defaultValue = EntityReference{NULL_ENTITY, 0};
                entry.properties = {number, target};
            }
        }
    }

    void populateAll(const reflect::ComponentDesc& desc, void* comp) {
        populate(desc.body, comp);
        populateCounted(desc.type, comp);
    }

    // the same answer, each way round. The new answer first: the oracle's lists make what they read
    void expectAgree(ComponentType type, void* a, void* b, const std::string& what) {
        const uint64_t forward = Catalog::getChangedUpdateFlags(type, a, b);
        const uint64_t backward = Catalog::getChangedUpdateFlags(type, b, a);
        EXPECT_EQ(forward, oracle(type, a, b)) << what;
        EXPECT_EQ(backward, oracle(type, b, a)) << what << " (the other way)";
    }

    // the slots list the same properties: the first stands for them (RandomPairs reaches the others)
    bool swept(const std::string& name) {
        return name.compare(0, 10, "submeshes[") != 0 || name.compare(0, 13, "submeshes[0].") == 0;
    }

    // a Mesh with this many submesh slots, whatever a default one holds
    void setSlots(MeshComponent& mesh, size_t slots) {
        for (const reflect::FieldDesc& field : reflect::find(ComponentType::MeshComponent)->body.fields) {
            if (std::string(field.name) == "submeshes") field.arrayOps->resize(field.ref(&mesh), slots);
        }
    }

    void sweepEveryProperty(const reflect::ComponentDesc& desc, const reflect::Value& base) {
        reflect::Value names(base);
        std::map<std::string, PropertyData> properties;
        reflect::listProperties(desc, names.get(), properties);
        for (const auto& [name, unused] : properties) {
            if (!swept(name)) continue;
            reflect::Value a(base);
            reflect::Value b(base);
            changeValue(reflect::property(desc, b.get(), name));
            // the extra properties a path does not name: from the list
            std::map<std::string, PropertyData> changed;
            if (!reflect::property(desc, b.get(), name).ref) {
                reflect::listProperties(desc, b.get(), changed);
                changeValue(changed[name]);
            }
            // copies: an out-of-line value equal to its default is unset in them
            reflect::Value left(a);
            reflect::Value right(b);
            expectAgree(desc.type, left.get(), right.get(), std::string(desc.cppName) + "." + name);
        }
    }

    constexpr uint64_t kMeshAlways = UpdateFlags_Mesh_Reload | UpdateFlags_Mesh_Texture | UpdateFlags_Shader_Uniforms;

    double cpuSeconds() {
        return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
    }

    // CPU microseconds of a call of each, averaged
    template<typename F>
    double microsecondsPerCall(int calls, F call) {
        const double start = cpuSeconds();
        for (int i = 0; i < calls; i++) call();
        return (cpuSeconds() - start) * 1e6 / calls;
    }

}

using namespace changed_update_flags_test;

TEST(ChangedUpdateFlags, EveryPropertyOfADefaultComponent) {
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        sweepEveryProperty(*desc, reflect::Value(desc->body.ops));
    }
}

TEST(ChangedUpdateFlags, EveryPropertyWithItsListsFilled) {
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        reflect::Value base(desc->body.ops);
        populateAll(*desc, base.get());
        sweepEveryProperty(*desc, base);
    }
}

TEST(ChangedUpdateFlags, ListsOfDifferentLengthsCompareTheElementsBothHave) {
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        reflect::Value empty(desc->body.ops);
        reflect::Value filled(desc->body.ops);
        populateAll(*desc, filled.get());
        expectAgree(desc->type, empty.get(), filled.get(), std::string(desc->cppName) + " empty against filled");
    }
}

TEST(ChangedUpdateFlags, NaNDiffersFromItself) {
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        reflect::Value a(desc->body.ops);
        reflect::Value b(desc->body.ops);
        for (void* comp : {a.get(), b.get()}) {
            std::map<std::string, PropertyData> properties;
            reflect::listProperties(*desc, comp, properties);
            for (const auto& [name, property] : properties) {
                if (property.type == PropertyType::Float && property.ref) *static_cast<float*>(property.ref) = std::numeric_limits<float>::quiet_NaN();
            }
        }
        expectAgree(desc->type, a.get(), b.get(), std::string(desc->cppName) + " every float NaN");
    }
}

TEST(ChangedUpdateFlags, RandomPairs) {
    std::mt19937 random(51);
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        for (int round = 0; round < 30; round++) {
            reflect::Value a(desc->body.ops);
            reflect::Value b(desc->body.ops);
            if (round % 2) populateAll(*desc, a.get());
            if (round % 3 == 0) populateAll(*desc, b.get());
            for (reflect::Value* side : {&a, &b}) {
                std::map<std::string, PropertyData> properties;
                reflect::listProperties(*desc, side->get(), properties);
                const int changes = static_cast<int>(random() % 5);
                for (int change = 0; change < changes && !properties.empty(); change++) {
                    auto it = properties.begin();
                    std::advance(it, random() % properties.size());
                    changeValue(it->second);
                }
            }
            reflect::Value left(a);
            reflect::Value right(b);
            expectAgree(desc->type, left.get(), right.get(), std::string(desc->cppName) + " round " + std::to_string(round));
        }
    }
}

TEST(ChangedUpdateFlags, MeshSlotCounts) {
    const reflect::ComponentDesc* desc = reflect::find(ComponentType::MeshComponent);
    ASSERT_NE(desc, nullptr);
    for (const auto& [left, right] : std::vector<std::pair<size_t, size_t>>{{0, 0}, {0, 16}, {1, 16}, {16, 20}, {20, 20}}) {
        MeshComponent a;
        MeshComponent b;
        setSlots(a, left);
        setSlots(b, right);
        if (right > 17) b.submeshes[17].material.shader = ShaderRef("past the first sixteen");
        expectAgree(ComponentType::MeshComponent, &a, &b, "slots " + std::to_string(left) + " and " + std::to_string(right));
    }
}

// A script property can change type between the two: its flags, its value not read
TEST(ChangedUpdateFlags, AScriptPropertyOfAnotherType) {
    ScriptComponent a;
    populateCounted(ComponentType::ScriptComponent, &a);
    ScriptComponent b = a;
    b.scripts[1].properties[0].type = ScriptPropertyType::String;
    b.scripts[1].properties[0].value = std::string("fast");
    b.scripts[1].properties[0].defaultValue = std::string();
    expectAgree(ComponentType::ScriptComponent, &a, &b, "speed a float and a string");
}

// What the function answered before S71 (taken with the oracle, which agrees below)
TEST(ChangedUpdateFlags, FixedCases) {
    struct Case {
        const char* name;
        void (*change)(MeshComponent&);
        uint64_t expected;
    };
    const Case cases[] = {
        {"nothing", [](MeshComponent&) {}, kMeshAlways},
        {"enabled, no flags", [](MeshComponent& mesh) { mesh.enabled = !mesh.enabled; }, kMeshAlways},
        {"castShadows, what is always set", [](MeshComponent& mesh) { mesh.castShadows = !mesh.castShadows; }, kMeshAlways},
        {"customShader", [](MeshComponent& mesh) { mesh.customShader = ShaderRef("a"); }, kMeshAlways | UpdateFlags_Shader_Reload},
        {"customDepthShader", [](MeshComponent& mesh) { mesh.customDepthShader = ShaderRef("a"); }, kMeshAlways | UpdateFlags_Shader_Reload},
        {"renderInReflectionProbes", [](MeshComponent& mesh) { mesh.renderInReflectionProbes = !mesh.renderInReflectionProbes; },
         kMeshAlways | UpdateFlags_Reflection_Probe_Recapture_All},
        {"the last slot's shader", [](MeshComponent& mesh) { mesh.submeshes[15].material.shader = ShaderRef("a"); }, kMeshAlways | UpdateFlags_Shader_Reload},
        {"a slot's base color", [](MeshComponent& mesh) { mesh.submeshes[3].material.baseColorFactor.x = 0.5f; }, kMeshAlways},
        {"a slot's extra pass", [](MeshComponent& mesh) { mesh.submeshes[3].material.extraPass.edit().shader = ShaderRef("a"); }, kMeshAlways},
        {"no slots to compare", [](MeshComponent& mesh) { setSlots(mesh, 0); }, UpdateFlags_Shader_Uniforms},
    };
    for (const Case& fixed : cases) {
        MeshComponent a;
        MeshComponent b;
        setSlots(a, 16);
        setSlots(b, 16);
        fixed.change(b);
        EXPECT_EQ(Catalog::getChangedUpdateFlags(ComponentType::MeshComponent, &a, &b), fixed.expected) << fixed.name;
        EXPECT_EQ(oracle(ComponentType::MeshComponent, &a, &b), fixed.expected) << fixed.name;
    }

    MeshComponent none;
    Transform still;
    Transform moved;
    EXPECT_EQ(Catalog::getChangedUpdateFlags(ComponentType::Transform, &still, &moved), UpdateFlags_None);
    moved.position.x += 1.0f;
    EXPECT_EQ(Catalog::getChangedUpdateFlags(ComponentType::Transform, &still, &moved), UpdateFlags_Transform | UpdateFlags_Layout_Anchors);

    // the list of scripts is a Custom property
    ScriptComponent script;
    ScriptComponent sameScript;
    EXPECT_EQ(Catalog::getChangedUpdateFlags(ComponentType::ScriptComponent, &script, &sameScript), UpdateFlags_Script_Enabled);

    EXPECT_EQ(Catalog::getChangedUpdateFlags(ComponentType::MeshComponent, nullptr, &none), UpdateFlags_None);
    EXPECT_EQ(Catalog::getChangedUpdateFlags(ComponentType::MeshComponent, &none, nullptr), UpdateFlags_None);
}

TEST(ChangedUpdateFlags, ComparingMakesNothing) {
    MeshComponent a;
    MeshComponent b;
    setSlots(a, 16);
    setSlots(b, 16);
    Catalog::getChangedUpdateFlags(ComponentType::MeshComponent, &a, &b);
    for (const MeshComponent* mesh : {&a, &b}) {
        for (size_t slot = 0; slot < mesh->submeshes.stored(); slot++) {
            EXPECT_FALSE(mesh->submeshes[slot].material.extraPass.isSet()) << slot;
            EXPECT_FALSE(mesh->submeshes[slot].material.pbr.isSet()) << slot;
        }
    }
}

// A ratio stays checked anywhere (CPU time, both in this process); the budget where
// JASPER_TEST_PERF_SLACK says the machine is quiet enough for one
TEST(ChangedUpdateFlagsBench, AMeshCostsMicroseconds) {
    MeshComponent a;
    MeshComponent b;
    setSlots(a, 16);
    setSlots(b, 16);
    uint64_t sink = 0;
    const double now = microsecondsPerCall(2000, [&]() { sink |= Catalog::getChangedUpdateFlags(ComponentType::MeshComponent, &a, &b); });
    const double lists = microsecondsPerCall(50, [&]() { sink |= oracle(ComponentType::MeshComponent, &a, &b); });
    std::printf("[ bench ] a Mesh's update flags: %.2f us a call, %.2f us with its property lists (budget 50)\n", now, lists);
    EXPECT_NE(sink, 0u);
    EXPECT_LT(now * 20.0, lists);
    if (const double slack = test::perfSlack(); slack > 0.0) {
        EXPECT_LE(now, 50.0 * slack);
    }
}

// The other components on Stop's path: those that run update flags, with their lists filled
TEST(ChangedUpdateFlagsBench, EveryComponentStaysUnderTheBudget) {
    const double slack = test::perfSlack();
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        if (desc->decodeApply != reflect::DecodeApply::UpdateFlags) continue;
        reflect::Value a(desc->body.ops);
        populateAll(*desc, a.get());
        reflect::Value b(a);
        reflect::Value oracleA(a);
        reflect::Value oracleB(a);
        uint64_t sink = 0;
        const double now = microsecondsPerCall(500, [&]() { sink |= Catalog::getChangedUpdateFlags(desc->type, a.get(), b.get()); });
        const double lists = microsecondsPerCall(50, [&]() { sink |= oracle(desc->type, oracleA.get(), oracleB.get()); });
        std::printf("[ bench ] %-32s %8.2f us a call, %8.2f us with its property lists\n", desc->cppName, now, lists);
        if (slack > 0.0) {
            EXPECT_LE(now, 50.0 * slack) << desc->cppName;
        }
        (void)sink;
    }
}
