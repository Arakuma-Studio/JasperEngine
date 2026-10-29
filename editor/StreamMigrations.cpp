// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StreamMigrations.h"
#include "Stream.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "particles/LegacyParticleConverter.h"
#include "pool/ShaderPool.h"
#include "util/YamlNumbers.h"

#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // Scene 0 -> 1: the first versioned scene format is today's shape; only the version is new
    void sceneV0ToV1(YAML::Node&, MigrationLog&){
    }

    void projectV0ToV1(YAML::Node&, MigrationLog&){
    }

    void materialV0ToV1(YAML::Node&, MigrationLog&){
    }

    // Every light component below node (scenes nest entities in children; prefabs list their
    // members): a shadowBias saved in NDC depth units (default 0.002) means a bias of
    // shadowBias shadow texels now, so it is reset to the new default of 1.0. The old units
    // cannot be converted: their size depended on each cascade's depth range.
    int resetLightShadowBias(YAML::Node node){
        int reset = 0;
        if (node.IsMap()){
            for (auto it = node.begin(); it != node.end(); ++it){
                YAML::Node value = it->second;
                if (it->first.IsScalar() && it->first.Scalar() == "light" && value.IsMap() && value["type"] && value["shadowBias"]){
                    bool alreadyDefault = false;
                    try {
                        alreadyDefault = value["shadowBias"].as<float>() == 1.0f;
                    } catch (const YAML::Exception&) {
                    }
                    if (!alreadyDefault){
                        value["shadowBias"] = 1.0f;
                        reset++;
                    }
                }else{
                    reset += resetLightShadowBias(value);
                }
            }
        }else if (node.IsSequence()){
            for (YAML::Node child : node){
                reset += resetLightShadowBias(child);
            }
        }
        return reset;
    }

    void logShadowBiasReset(int reset, MigrationLog& log){
        if (reset > 0){
            log.info("Shadow bias is in shadow texels now: " + std::to_string(reset) +
                     (reset == 1 ? " light's" : " lights'") + " Bias reset to 1 (docs/rendering/shadows.md)");
        }
    }

    // The scene's mesh variant keys without bit 5 when they were written before the bit meant
    // anything: it was the PCF filter ('Pcf') until July 2026. Since the 64-bit keys (keyVersion
    // 2) it is upstream's Stochastic Tiling ('Sto'), kept; S10's High shadow filters ('Shq') are
    // MESH bit 33. (editor::Stream::decodeShaderKey strips it from version 1 keys as well.)
    void clearRetiredMeshBit(YAML::Node keys){
        if (!keys || !keys.IsSequence())
            return;
        const uint64_t bit = uint64_t(1) << 5;
        for (YAML::Node entry : keys){
            try {
                if (entry.IsMap() && entry["type"] && entry["type"].IsScalar() && entry["type"].Scalar() == "mesh" && entry["properties"]){
                    if (entry["keyVersion"] && entry["keyVersion"].as<int>() >= 2)
                        continue;
                    const uint64_t properties = entry["properties"].as<uint64_t>();
                    if (properties & bit)
                        entry["properties"] = properties & ~bit;
                }
                // a packed key (scenes saved before forks were named by path): the decoder
                // reads it as a version 1 key, without bit 5
            } catch (const YAML::Exception&) {
                // not a number: left for the decoder to report
            }
        }
    }

    // Scene 1 -> 2 (S10 shadows): light shadow biases in texels; the old default quality Low
    // becomes Auto (the platform's tier); the retired mesh variant bit 5 cleared
    void sceneV1ToV2(YAML::Node& root, MigrationLog& log){
        clearRetiredMeshBit(root["shaderKeys"]);
        YAML::Node settings = root["scene"];
        if (settings && settings.IsMap() && settings["shadowQuality"] && settings["shadowQuality"].IsScalar() &&
                settings["shadowQuality"].Scalar() == "low"){
            settings["shadowQuality"] = "auto";
            log.info("Shadow quality Low (the old default) is Auto now: the platform's quality tier");
        }
        logShadowBiasReset(resetLightShadowBias(root["entities"]), log);
    }

    // Prefab 3 -> 4 (S10 shadows): its lights' shadow biases in texels
    void prefabV3ToV4(YAML::Node& root, MigrationLog& log){
        logShadowBiasReset(resetLightShadowBias(root["members"]), log);
    }

    // ---------------------------------------------------------------- particle systems (S20)

    // what an old system's target (a Points or an instanced mesh GameObject) gives the Renderer
    particles::LegacyTarget legacyTargetOf(const YAML::Node& components){
        particles::LegacyTarget target;
        if (components && components["points"]){
            const PointsComponent points = editor::Stream::decodePointsComponent(components["points"]);
            target.kind = particles::LegacyTarget::Kind::Points;
            target.texture = points.texture.getPath();
            if (!points.texture.getPathGuid().isNull())
                target.textureGuid = points.texture.getPathGuid().str();
            for (unsigned int i = 0; i < points.numFramesRect; i++){
                const Rect& r = points.framesRect[i].rect;
                particles::SpriteRect rect;
                rect.x = r.getX(); rect.y = r.getY(); rect.width = r.getWidth(); rect.height = r.getHeight();
                target.frames.push_back(rect);
            }
            target.shader = points.customShader.path;
            target.shaderFeatures = points.shaderFeatures;
            for (const auto& u : points.shaderUniforms){
                particles::ShaderProperty p;
                p.name = u.first;
                p.value = u.second;
                p.components = 4;
                target.shaderProperties.push_back(p);
            }
            for (const auto& t : points.shaderTextures){
                particles::ShaderTexture texture;
                texture.name = t.first;
                texture.path = t.second.getPath();
                if (!t.second.getPathGuid().isNull())
                    texture.guid = t.second.getPathGuid().str();
                target.shaderTextures.push_back(texture);
            }
            switch (points.blendMode){
                case MaterialBlendMode::AUTO: target.blend = -1; break;
                case MaterialBlendMode::NONE: target.blendOpaque = true; break;
                case MaterialBlendMode::PREMULTIPLIED: target.blend = (int)particles::ParticleBlend::Premultiplied; break;
                case MaterialBlendMode::ADDITIVE: target.blend = (int)particles::ParticleBlend::Additive; break;
                case MaterialBlendMode::MULTIPLY: target.blend = (int)particles::ParticleBlend::Multiply; break;
                default: target.blend = (int)particles::ParticleBlend::Alpha; break;
            }
            target.depthWrite = points.depthWrite == MaterialDepthWrite::AUTO ? -1 : (points.depthWrite == MaterialDepthWrite::ENABLED ? 1 : 0);
            target.transparent = points.transparent;
        }else if (components && (components["instancedmesh"] || components["mesh"])){
            target.kind = particles::LegacyTarget::Kind::Mesh;
        }
        return target;
    }

    bool isEmptySequence(const YAML::Node& node){
        return !node || !node.IsSequence() || node.size() == 0;
    }

    // The old particle systems of a document's entities (a scene's tree, a prefab's members):
    // each becomes an inline effect (LegacyParticleConverter, docs/specs/S20-particles.md D9) on
    // its action's target, which renders it; the action stays as its controller (D1). A target
    // Points or InstancedMesh with nothing of its own is removed; an instanced mesh's
    // MeshComponent stays as the particles' mesh, disabled so it does not draw itself.
    void migrateParticleSystems(YAML::Node entities, MigrationLog& log){
        std::vector<YAML::Node> all;
        std::function<void(YAML::Node)> collect = [&](YAML::Node list){
            if (!list || !list.IsSequence())
                return;
            for (YAML::Node entity : list){
                if (!entity.IsMap())
                    continue;
                all.push_back(entity);
                collect(entity["children"]);
                collect(entity["members"]);
            }
        };
        collect(entities);
        std::map<uint64_t, YAML::Node> byId;
        std::map<uint64_t, int> targeted;
        for (YAML::Node& entity : all){
            if (entity["entity"])
                byId[editor::YamlNumbers::as<uint64_t>(entity["entity"])] = entity;
            YAML::Node components = entity["components"];
            if (components && components["action"] && components["action"]["target"])
                targeted[editor::YamlNumbers::as<uint64_t>(components["action"]["target"])]++;
        }
        auto nameOf = [](const YAML::Node& entity){
            return entity["name"] ? entity["name"].as<std::string>() : std::string("GameObject");
        };

        for (YAML::Node& entity : all){
            YAML::Node components = entity["components"];
            if (!components || !components["particlesystem"])
                continue;
            YAML::Node old = components["particlesystem"];
            if (old["inlineEffect"] || old["effect"])
                continue;   // already the new form

            const LegacyParticleSystem legacy = editor::Stream::decodeLegacyParticleSystem(old);
            const bool enabled = old["enabled"] ? editor::YamlNumbers::as<bool>(old["enabled"]) : true;
            YAML::Node action = components["action"];
            uint64_t targetId = 0;
            if (action && action["target"])
                targetId = editor::YamlNumbers::as<uint64_t>(action["target"]);
            auto targetIt = byId.find(targetId);
            YAML::Node host = targetIt != byId.end() ? targetIt->second : entity;
            YAML::Node hostComponents = host["components"];

            std::vector<std::string> notes;
            const particles::LegacyTarget target = legacyTargetOf(hostComponents);
            ParticleSystemComponent converted;
            converted.enabled = enabled;
            converted.inlineEffect = particles::LegacyParticleConverter::convert(legacy, target, &notes);
            // the action plays it (its start, stop and pause), as it did
            converted.playOnAwake = !action;
            converted.playbackSpeed = action && action["speed"] ? editor::YamlNumbers::as<float>(action["speed"]) : 1.0f;

            components.remove("particlesystem");
            hostComponents["particlesystem"] = editor::Stream::encodeParticleSystemComponent(converted);
            // a system emits from its GameObject's Transform: an action without a target had none
            if (!hostComponents["transform"]){
                YAML::Node transform;
                transform["position"] = YAML::Load("[0, 0, 0]");
                transform["rotation"] = YAML::Load("[1, 0, 0, 0]");
                transform["scale"] = YAML::Load("[1, 1, 1]");
                transform["visible"] = true;
                hostComponents["transform"] = transform;
                notes.push_back("it had no Transform: one at the origin was added");
            }

            // what the target held for the old system's particles
            const bool onlyTarget = targetIt != byId.end() && targeted[targetId] <= 1;
            if (hostComponents["points"]){
                if (onlyTarget && isEmptySequence(hostComponents["points"]["points"]))
                    hostComponents.remove("points");
                else
                    notes.push_back("its Points component holds points of its own (or another system targets it): kept");
            }
            if (hostComponents["instancedmesh"]){
                if (onlyTarget && isEmptySequence(hostComponents["instancedmesh"]["instances"]))
                    hostComponents.remove("instancedmesh");
                else
                    notes.push_back("its InstancedMesh component holds instances of its own (or another system targets it): kept");
                if (hostComponents["mesh"])
                    hostComponents["mesh"]["enabled"] = false;
            }
            std::string line = "Particle System of " + nameOf(entity) + ": converted to an inline effect";
            if (!host.is(entity))
                line += " on its target " + nameOf(host);
            for (const std::string& note : notes) line += "; " + note;
            log.info(line);
        }
    }

    // Scene 2 -> 3 (S20 particles): the old particle systems become effects on their targets
    void sceneV2ToV3(YAML::Node& root, MigrationLog& log){
        migrateParticleSystems(root["entities"], log);
    }

    // Prefab 4 -> 5 (S20 particles): the same for its members
    void prefabV4ToV5(YAML::Node& root, MigrationLog& log){
        migrateParticleSystems(root["members"], log);
    }

    // ---------------------------------------------------------------- derived transform values (S29 R20)

    // the values the transform pass computes on load, written until scene 4 / prefab 6
    const char* const kDerivedTransformKeys[] = {"worldPosition", "worldRotation", "worldScale", "localMatrix", "modelMatrix", "normalMatrix"};

    // Every transform map below node: GameObjects (children, prefab members), prefab instance
    // overrides and added GameObjects alike
    int dropDerivedTransformKeys(YAML::Node node){
        int dropped = 0;
        if (node.IsMap()){
            for (auto it = node.begin(); it != node.end(); ++it){
                YAML::Node value = it->second;
                if (it->first.IsScalar() && it->first.Scalar() == "transform" && value.IsMap()){
                    for (const char* key : kDerivedTransformKeys){
                        if (value[key]){
                            value.remove(key);
                            dropped++;
                        }
                    }
                }else{
                    dropped += dropDerivedTransformKeys(value);
                }
            }
        }else if (node.IsSequence()){
            for (YAML::Node child : node){
                dropped += dropDerivedTransformKeys(child);
            }
        }
        return dropped;
    }

    // Scene 3 -> 4: the transforms' derived values are no longer written (version control diffs
    // and merges see only what was authored); loading computes them as it always did
    void sceneV3ToV4(YAML::Node& root, MigrationLog&){
        dropDerivedTransformKeys(root["entities"]);
    }

    // Prefab 5 -> 6: the same for its members (and the instances nested in them)
    void prefabV5ToV6(YAML::Node& root, MigrationLog&){
        dropDerivedTransformKeys(root["members"]);
    }

}

const char* StreamMigrations::versionKey(DocumentKind kind){
    return kind == DocumentKind::Prefab ? "version" : "formatVersion";
}

int StreamMigrations::currentVersion(DocumentKind kind){
    switch (kind){
        case DocumentKind::Scene: return 4;
        case DocumentKind::Project: return 1;
        case DocumentKind::Material: return 1;
        // Prefab::FILE_VERSION
        case DocumentKind::Prefab: return 6;
    }
    return 0;
}

int StreamMigrations::absentVersion(DocumentKind kind){
    return kind == DocumentKind::Prefab ? 1 : 0;
}

const char* StreamMigrations::kindName(DocumentKind kind){
    switch (kind){
        case DocumentKind::Scene: return "scene";
        case DocumentKind::Project: return "project";
        case DocumentKind::Material: return "material";
        case DocumentKind::Prefab: return "prefab";
    }
    return "document";
}

int StreamMigrations::readVersion(DocumentKind kind, const YAML::Node& root){
    if (!root || !root.IsMap())
        return absentVersion(kind);
    const YAML::Node value = root[versionKey(kind)];
    if (!value || !value.IsScalar())
        return absentVersion(kind);
    int version = 0;
    if (!YAML::convert<int>::decode(value, version) || version < 0)
        return absentVersion(kind);
    return version;
}

const std::vector<Migration>& StreamMigrations::table(DocumentKind kind){
    static const std::vector<Migration> scene = {
        {0, 1, "stamp the scene format version", sceneV0ToV1},
        {1, 2, "shadow bias in shadow texels, shadow quality Low to Auto", sceneV1ToV2},
        {2, 3, "particle systems to effects on their targets", sceneV2ToV3},
        {3, 4, "derived transform values no longer written", sceneV3ToV4},
    };
    static const std::vector<Migration> project = {
        {0, 1, "stamp the project format version", projectV0ToV1},
    };
    static const std::vector<Migration> material = {
        {0, 1, "stamp the material format version", materialV0ToV1},
    };
    // Versions 1 and 2 are upgraded in place by Prefab::loadRegistry (needsUpgrade)
    static const std::vector<Migration> prefab = {
        {3, 4, "shadow bias in shadow texels", prefabV3ToV4},
        {4, 5, "particle systems to effects on their targets", prefabV4ToV5},
        {5, 6, "derived transform values no longer written", prefabV5ToV6},
    };

    switch (kind){
        case DocumentKind::Scene: return scene;
        case DocumentKind::Project: return project;
        case DocumentKind::Material: return material;
        case DocumentKind::Prefab: return prefab;
    }
    return scene;
}

MigrationResult StreamMigrations::migrate(DocumentKind kind, YAML::Node& root){
    MigrationResult result;
    result.versionRead = readVersion(kind, root);
    result.versionCurrent = currentVersion(kind);
    if (!root || !root.IsMap() || result.newer() || result.versionRead == result.versionCurrent)
        return result;

    int version = result.versionRead;
    for (const Migration& migration : table(kind)){
        if (migration.from != version)
            continue;
        migration.apply(root, result.log);
        result.applied.push_back(migration.name);
        version = migration.to;
    }
    if (result.migrated())
        stampVersion(kind, root, version);
    return result;
}

void StreamMigrations::stampVersion(DocumentKind kind, YAML::Node& root, int version){
    const char* key = versionKey(kind);
    YAML::Node ordered(YAML::NodeType::Map);
    ordered[key] = version;
    for (auto it = root.begin(); it != root.end(); ++it){
        if (it->first.IsScalar() && it->first.Scalar() == key)
            continue;
        ordered[it->first] = it->second;
    }
    root = ordered;
}

void StreamMigrations::stampCurrentVersion(DocumentKind kind, YAML::Node& root){
    stampVersion(kind, root, currentVersion(kind));
}

std::string StreamMigrations::newerFormatMessage(DocumentKind kind, int versionRead){
    return std::string("Written by a newer JasperEngine (") + kindName(kind) + " format " + std::to_string(versionRead) +
            "; this editor writes " + std::to_string(currentVersion(kind)) + ").";
}
