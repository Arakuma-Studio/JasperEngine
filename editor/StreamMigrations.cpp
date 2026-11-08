// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StreamMigrations.h"
#include "Stream.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "particles/LegacyParticleConverter.h"
#include "pool/ShaderPool.h"
#include "stream/CurveCodec.h"
#include "stream/UpstreamParticles.h"
#include "util/YamlNumbers.h"

#include <algorithm>
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

    // a system emits from its GameObject's Transform: one at the origin for a host without
    void ensureTransform(YAML::Node hostComponents, std::vector<std::string>& notes){
        if (!hostComponents["transform"]){
            YAML::Node transform;
            transform["position"] = YAML::Load("[0, 0, 0]");
            transform["rotation"] = YAML::Load("[1, 0, 0, 0]");
            transform["scale"] = YAML::Load("[1, 1, 1]");
            transform["visible"] = true;
            hostComponents["transform"] = transform;
            notes.push_back("it had no Transform: one at the origin was added");
        }
    }

    // origin/main's version 2 system (its Unity modules) as an S20 component
    // (stream/UpstreamParticles.h); `notes` gets what did not carry over
    ParticleSystemComponent convertVersion2(const YAML::Node& old, const YAML::Node& action, const upstreamparticles::Target& target,
                                            bool& drawsTargetMesh, std::vector<std::string>& notes){
        upstreamparticles::Result result = upstreamparticles::convert(old, target);
        ParticleSystemComponent converted;
        converted.enabled = old["enabled"] ? editor::YamlNumbers::as<bool>(old["enabled"]) : true;
        converted.inlineEffect = result.effect;
        converted.playOnAwake = result.playOnAwake;
        converted.seed = result.seed;
        // the action's speed scaled the simulation there
        converted.playbackSpeed = action && action["speed"] ? editor::YamlNumbers::as<float>(action["speed"]) : 1.0f;
        drawsTargetMesh = result.drawsTargetMesh;
        notes.insert(notes.end(), result.notes.begin(), result.notes.end());
        return converted;
    }

    // the GameObjects a prefab instance block adds (its own and its nested instances')
    void collectInstanceAdded(const YAML::Node& block, const std::function<void(YAML::Node)>& collect){
        if (!block || !block.IsMap())
            return;
        collect(block["addedGameObjects"]);
        if (const YAML::Node nested = block["nested"]; nested && nested.IsSequence()){
            for (const YAML::Node& entry : nested)
                collectInstanceAdded(entry, collect);
        }
    }

    // The old particle systems of a document's entities (a scene's tree, a prefab's members,
    // the GameObjects prefab instances add): each becomes an inline effect on its action's
    // target, which renders it; the action stays as its controller (D1). The systems before
    // S20 go through LegacyParticleConverter (docs/specs/S20-particles.md D9); origin/main's
    // version 2 systems through stream/UpstreamParticles (S20, Upstream absorption), their
    // target's quad material moving into the Renderer module. A target Points or
    // InstancedMesh with nothing of its own is removed; a MeshComponent the particles draw as
    // their mesh stays, disabled so it does not draw itself.
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
                collectInstanceAdded(entity["prefabInstance"], collect);
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

            const bool version2 = upstreamparticles::isVersion2(old);
            YAML::Node action = components["action"];
            uint64_t targetId = 0;
            if (action && action["target"])
                targetId = editor::YamlNumbers::as<uint64_t>(action["target"]);
            auto targetIt = byId.find(targetId);
            YAML::Node host = targetIt != byId.end() ? targetIt->second : entity;
            YAML::Node hostComponents = host["components"];

            std::vector<std::string> notes;
            ParticleSystemComponent converted;
            bool drawsTargetMesh = false;
            upstreamparticles::Target v2Target;
            if (version2){
                v2Target = upstreamparticles::targetOf(hostComponents);
                converted = convertVersion2(old, action, v2Target, drawsTargetMesh, notes);
            }else{
                const LegacyParticleSystem legacy = editor::Stream::decodeLegacyParticleSystem(old);
                const particles::LegacyTarget target = legacyTargetOf(hostComponents);
                converted.enabled = old["enabled"] ? editor::YamlNumbers::as<bool>(old["enabled"]) : true;
                converted.inlineEffect = particles::LegacyParticleConverter::convert(legacy, target, &notes);
                // the action plays it (its start, stop and pause), as it did
                converted.playOnAwake = !action;
                converted.playbackSpeed = action && action["speed"] ? editor::YamlNumbers::as<float>(action["speed"]) : 1.0f;
                // origin/main's (ui-scaling-text) Unscaled Time of an old system (its UI particles):
                // the GameObject's own, as it was the component's
                converted.useUnscaledTime = old["useUnscaledTime"] && editor::YamlNumbers::as<bool>(old["useUnscaledTime"]);
            }

            components.remove("particlesystem");
            hostComponents["particlesystem"] = editor::Stream::encodeParticleSystemComponent(converted);
            ensureTransform(hostComponents, notes);

            // what the target held for the old system's particles
            // (origin/main's systems target themselves; in a prefab instance the action names the
            // prefab's id, which is not this GameObject's: its own id counts then)
            const uint64_t hostId = host["entity"] ? editor::YamlNumbers::as<uint64_t>(host["entity"]) : 0;
            const bool onlyTarget = (targetIt != byId.end() && targeted[targetId] <= 1) || (version2 && host.is(entity) && targeted[hostId] <= 1);
            if (hostComponents["points"]){
                if (onlyTarget && isEmptySequence(hostComponents["points"]["points"]))
                    hostComponents.remove("points");
                else
                    notes.push_back("its Points component holds points of its own (or another system targets it): kept");
            }
            const bool instanced = (bool)hostComponents["instancedmesh"];
            if (instanced){
                if (onlyTarget && isEmptySequence(hostComponents["instancedmesh"]["instances"]))
                    hostComponents.remove("instancedmesh");
                else
                    notes.push_back("its InstancedMesh component holds instances of its own (or another system targets it): kept");
            }
            if (hostComponents["mesh"] && (instanced || (version2 && v2Target.kind == upstreamparticles::Target::Kind::Mesh))){
                if (version2 && !drawsTargetMesh && onlyTarget && !hostComponents["sprite"]){
                    // the particles' quad: its material is the Renderer module's now (a sprite's
                    // mesh stays, disabled: the sprite frames are its)
                    hostComponents.remove("mesh");
                }else{
                    hostComponents["mesh"]["enabled"] = false;
                }
            }
            std::string line = "Particle System of " + nameOf(entity) + (version2 ? " (origin/main's version 2)" : "") + ": converted to an inline effect";
            if (!host.is(entity))
                line += " on its target " + nameOf(host);
            for (const std::string& note : notes) line += "; " + note;
            log.info(line);
        }
    }

    // origin/main's names of a version 2 system's overridden properties as S20's: the modules
    // are one document (inlineEffect), Play On Awake and the seed the component's own
    std::vector<std::string> overriddenParticleProperties(const YAML::Node& names){
        std::vector<std::string> out;
        auto add = [&](const std::string& name){
            if (std::find(out.begin(), out.end(), name) == out.end())
                out.push_back(name);
        };
        auto map = [&](const std::string& name){
            if (name == "enabled") add("enabled");
            else if (name == "main.playOnAwake") add("playOnAwake");
            else if (name == "main.autoRandomSeed" || name == "main.randomSeed") add("seed");
            else add("inlineEffect");
        };
        if (names.IsSequence()){
            for (const YAML::Node& n : names)
                if (n.IsScalar()) map(n.Scalar());
        }else if (names.IsScalar()){
            map(names.Scalar());
        }
        return out;
    }

    // Prefab instances' overrides of a version 2 system (their values are the whole component)
    // in S20's form; the instance's quad material, when it overrides it, goes into the Renderer
    int migrateParticleOverrides(YAML::Node node, MigrationLog& log){
        int converted = 0;
        if (node.IsMap()){
            for (auto it = node.begin(); it != node.end(); ++it){
                YAML::Node value = it->second;
                if (it->first.IsScalar() && it->first.Scalar() == "overrides" && value.IsSequence()){
                    for (YAML::Node entry : value){
                        if (!entry.IsMap())
                            continue;
                        bool changed = false;
                        std::vector<std::string> notes;
                        // the overridden values (an instance's non-root members carry them here; its
                        // root's are the GameObject's own components, converted with it)
                        YAML::Node components = entry["components"];
                        if (components && components.IsMap() && upstreamparticles::isVersion2(components["particlesystem"])){
                            upstreamparticles::Target target = upstreamparticles::targetOf(components);
                            if (target.kind == upstreamparticles::Target::Kind::None){
                                // the instance does not override the quad: origin/main's default particle material
                                target.kind = upstreamparticles::Target::Kind::Mesh;
                                target.material.shader.path = "library:unlit";
                                target.material.shaderFeatures = {"PARTICLE_TEXTURE"};
                                target.material.blendMode = MaterialBlendMode::ALPHA;
                                target.castShadows = false;
                                target.receiveShadows = false;
                                notes.push_back("the instance does not override its quad's material: the default particle look (check its Renderer module)");
                            }
                            bool drawsTargetMesh = false;
                            const ParticleSystemComponent c = convertVersion2(components["particlesystem"], components["action"], target, drawsTargetMesh, notes);
                            components["particlesystem"] = editor::Stream::encodeParticleSystemComponent(c);
                            changed = true;
                        }
                        // the overridden property names: origin/main's module paths ("main.startSpeed")
                        YAML::Node properties = entry["properties"];
                        if (properties && properties.IsMap() && properties["particlesystem"]){
                            const YAML::Node names = properties["particlesystem"];
                            bool upstreamNames = false;
                            auto isUpstreamName = [](const std::string& n){ return n.find('.') != std::string::npos || n == "version" || n == "useUnscaledTime"; };
                            if (names.IsSequence()){
                                for (const YAML::Node& n : names) upstreamNames = upstreamNames || (n.IsScalar() && isUpstreamName(n.Scalar()));
                            }else if (names.IsScalar()){
                                upstreamNames = isUpstreamName(names.Scalar());
                            }
                            if (upstreamNames){
                                YAML::Node rewritten(YAML::NodeType::Sequence);
                                for (const std::string& name : overriddenParticleProperties(names))
                                    rewritten.push_back(name);
                                properties["particlesystem"] = rewritten;
                                changed = true;
                            }
                        }
                        if (!changed)
                            continue;
                        std::string line = "Prefab instance override of a version 2 Particle System: converted to an inline effect";
                        for (const std::string& note : notes) line += "; " + note;
                        log.info(line);
                        converted++;
                    }
                }else{
                    converted += migrateParticleOverrides(value, log);
                }
            }
        }else if (node.IsSequence()){
            for (YAML::Node child : node)
                converted += migrateParticleOverrides(child, log);
        }
        return converted;
    }

    // Scene 2 -> 3 (S20 particles): the old particle systems become effects on their targets
    void sceneV2ToV3(YAML::Node& root, MigrationLog& log){
        migrateParticleSystems(root["entities"], log);
        migrateParticleOverrides(root["entities"], log);
    }

    // Prefab 4 -> 5 (S20 particles): the same for its members
    void prefabV4ToV5(YAML::Node& root, MigrationLog& log){
        migrateParticleSystems(root["members"], log);
        migrateParticleOverrides(root["members"], log);
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

    // Scene 4 -> 5 (S18 splines): the Spline, Spline Extrude, Spline Instantiate and Spline
    // Animate components. An older scene has none of them, so it reads unchanged (identity); the
    // version tells an older editor that it cannot read them, whose unknown-data preservation
    // keeps them through its saves.
    void sceneV4ToV5(YAML::Node& root, MigrationLog& log){
        (void)root;
        (void)log;
    }


    // ---------------------------------------------------------------- Animation Player controllers

    std::string scalarOf(const YAML::Node& node){
        return node && node.IsScalar() ? node.Scalar() : std::string();
    }

    bool hasName(const YAML::Node& list, const std::string& name){
        if (!list || !list.IsSequence()) return false;
        for (const YAML::Node& item : list){
            if (item.IsScalar() && item.Scalar() == name) return true;
        }
        return false;
    }

    void removeName(YAML::Node list, const std::string& name){
        if (!list || !list.IsSequence()) return;
        YAML::Node kept(YAML::NodeType::Sequence);
        for (const YAML::Node& item : list){
            if (!(item.IsScalar() && item.Scalar() == name)) kept.push_back(item);
        }
        list = kept;
    }

    // the Animator an upstream controller player becomes: its controller, the player's checkbox,
    // and no retargeting (upstream's controllers never retargeted)
    YAML::Node migratedAnimator(const std::string& controller, const std::string& guid, bool enabled){
        YAML::Node animator(YAML::NodeType::Map);
        animator["enabled"] = enabled;
        animator["controller"] = controller;
        if (!guid.empty()) animator["controllerGuid"] = guid;
        animator["retarget"] = "off";
        return animator;
    }

    bool enabledOf(const YAML::Node& player){
        const std::string text = scalarOf(player["enabled"]);
        return !(text == "false" || text == "0");
    }

    // an entity's (or prefab member's) components: an animationplayer with upstream's controller
    // becomes an Animator playing it beside the player, which is disabled (its clips and layers
    // were unused while the controller owned the states; kept as they were, playing nothing)
    int moveEntityController(YAML::Node components, MigrationLog& log){
        YAML::Node player = components["animationplayer"];
        if (!player || !player.IsMap() || !player["controller"]) return 0;
        const std::string controller = scalarOf(player["controller"]);
        const std::string guid = scalarOf(player["controllerGuid"]);
        player.remove("controller");
        player.remove("controllerGuid");
        if (controller.empty()) return 0;
        if (components["animator"]){
            log.info("An Animation Player's controller " + controller + " was dropped: its GameObject has an Animator with its own");
            return 0;
        }
        components["animator"] = migratedAnimator(controller, guid, enabledOf(player));
        player["enabled"] = false;
        return 1;
    }

    // a prefab instance's override of a player's controller: an added Animator (its values the
    // controller) and the player's enabled overridden off; cleared (""): the Animator the base
    // got from its own migration removed and the player enabled
    int moveOverrideController(YAML::Node entry){
        YAML::Node properties = entry["properties"];
        if (!properties || !properties.IsMap()) return 0;
        YAML::Node names = properties["animationplayer"];
        if (!hasName(names, "controller") && !hasName(names, "controllerGuid")) return 0;
        YAML::Node components = entry["components"];
        YAML::Node player = components && components.IsMap() ? components["animationplayer"] : YAML::Node();
        const std::string controller = player && player.IsMap() ? scalarOf(player["controller"]) : std::string();
        const std::string guid = player && player.IsMap() ? scalarOf(player["controllerGuid"]) : std::string();
        const bool enabled = player && player.IsMap() ? enabledOf(player) : true;
        removeName(names, "controller");
        removeName(names, "controllerGuid");
        if (player && player.IsMap()){
            player.remove("controller");
            player.remove("controllerGuid");
        }
        if (!components || !components.IsMap()){
            entry["components"] = YAML::Node(YAML::NodeType::Map);
            components = entry["components"];
        }
        if (!components["animationplayer"] || !components["animationplayer"].IsMap()) components["animationplayer"] = YAML::Node(YAML::NodeType::Map);
        if (!hasName(names, "enabled")){
            if (!names || !names.IsSequence()){
                properties["animationplayer"] = YAML::Node(YAML::NodeType::Sequence);
                names = properties["animationplayer"];
            }
            names.push_back("enabled");
        }
        if (controller.empty()){
            components["animationplayer"]["enabled"] = enabled;
            if (!hasName(entry["removedComponents"], "animator")){
                if (!entry["removedComponents"]) entry["removedComponents"] = YAML::Node(YAML::NodeType::Sequence);
                entry["removedComponents"].push_back("animator");
            }
            return 1;
        }
        components["animationplayer"]["enabled"] = false;
        components["animator"] = migratedAnimator(controller, guid, enabled);
        if (!hasName(entry["addedComponents"], "animator")){
            if (!entry["addedComponents"]) entry["addedComponents"] = YAML::Node(YAML::NodeType::Sequence);
            entry["addedComponents"].push_back("animator");
        }
        return 1;
    }

    int moveAnimationPlayerControllers(YAML::Node node, MigrationLog& log){
        int moved = 0;
        if (node.IsMap()){
            YAML::Node components = node["components"];
            if (components && components.IsMap()){
                if (node["registryEntity"] && node["properties"]) moved += moveOverrideController(node);
                else moved += moveEntityController(components, log);
            }
            for (auto it = node.begin(); it != node.end(); ++it){
                if (it->first.IsScalar() && it->first.Scalar() == "components") continue;
                moved += moveAnimationPlayerControllers(it->second, log);
            }
        }else if (node.IsSequence()){
            for (YAML::Node child : node){
                moved += moveAnimationPlayerControllers(child, log);
            }
        }
        return moved;
    }

    void logMovedControllers(int moved, MigrationLog& log){
        if (moved > 0){
            log.info("Animation Player controllers play on Animators now: " + std::to_string(moved) +
                     (moved == 1 ? " player" : " players") + " got an Animator with its controller and hand over to it (disabled; docs/animator.md)");
        }
    }

    // Scene 5 -> 6 (S21 absorbs upstream's anim-layers): an Animation Player's `controller` /
    // `controllerGuid` (upstream's controller on the player) move to an Animator on the same
    // GameObject; the player's layers and clip layers are read as they are (new optional keys)
    void sceneV5ToV6(YAML::Node& root, MigrationLog& log){
        logMovedControllers(moveAnimationPlayerControllers(root["entities"], log), log);
    }

    // Prefab 6 -> 7: the same for its members and the instances nested in them
    void prefabV6ToV7(YAML::Node& root, MigrationLog& log){
        logMovedControllers(moveAnimationPlayerControllers(root["members"], log), log);
    }

    // ---------------------------------------------------------------- upstream curves (S27 absorption)

    // Every Line and Trail Renderer map below node (GameObjects, prefab members, the instances
    // nested in them): a widthCurve or colorGradient in upstream's (origin/main lines and trails)
    // form ({keys, pre, post} curves, numbers, {mode, colors, alphas} gradients with linear
    // colours, single colours) rewritten in S27's one form (stream/CurveCodec, the same values).
    // The readers take either form, so this only makes the file say what the editor writes.
    int migrateRibbonCurves(YAML::Node node, MigrationLog& log){
        int converted = 0;
        if (node.IsMap()){
            for (auto it = node.begin(); it != node.end(); ++it){
                YAML::Node value = it->second;
                const bool ribbon = it->first.IsScalar() && (it->first.Scalar() == "linerenderer" || it->first.Scalar() == "trailrenderer");
                if (ribbon && value.IsMap()){
                    const std::string component = it->first.Scalar();
                    if (YAML::Node curveNode = value["widthCurve"]){
                        Curve curve;
                        std::string reason;
                        if (curvecodec::curveFromYaml(curveNode, curve, reason)){
                            const YAML::Node written = curvecodec::curveToYaml(curve);
                            if (YAML::Dump(written) != YAML::Dump(curveNode)){
                                value["widthCurve"] = written;
                                converted++;
                            }
                        }else{
                            log.info(component + ".widthCurve: " + reason + " (kept as it is: it reads as the default curve)");
                        }
                    }
                    if (YAML::Node gradientNode = value["colorGradient"]){
                        Gradient gradient;
                        std::string reason;
                        if (curvecodec::gradientFromYaml(gradientNode, gradient, reason)){
                            const YAML::Node written = curvecodec::gradientToYaml(gradient);
                            if (YAML::Dump(written) != YAML::Dump(gradientNode)){
                                value["colorGradient"] = written;
                                converted++;
                            }
                        }else{
                            log.info(component + ".colorGradient: " + reason + " (kept as it is: it reads as the default gradient)");
                        }
                    }
                }else{
                    converted += migrateRibbonCurves(value, log);
                }
            }
        }else if (node.IsSequence()){
            for (YAML::Node child : node){
                converted += migrateRibbonCurves(child, log);
            }
        }
        return converted;
    }

    void logRibbonCurves(int converted, MigrationLog& log){
        if (converted > 0){
            log.info("Line and Trail Renderers: " + std::to_string(converted) + (converted == 1 ? " curve or gradient" : " curves and gradients") +
                     " rewritten in the editor's curve form (the same values; docs/specs/S27-editor-tools.md, Upstream absorption)");
        }
    }

    // Scene 6 -> 7 (S27 upstream absorption): the Line and Trail Renderers' curves and gradients
    // in S27's form; the version tells an older editor (whose readers take upstream's form only)
    // that it cannot read them
    void sceneV6ToV7(YAML::Node& root, MigrationLog& log){
        logRibbonCurves(migrateRibbonCurves(root["entities"], log), log);
    }

    // Prefab 7 -> 8: the same for its members
    void prefabV7ToV8(YAML::Node& root, MigrationLog& log){
        logRibbonCurves(migrateRibbonCurves(root["members"], log), log);
    }

    // Scene 7 -> 8, prefab 8 -> 9 (S44): sprites may name a sprite sheet (sheet, sheetGuid,
    // frame, frameName); an older file has none, so nothing changes. The bump tells an older
    // editor it cannot draw sheet sprites.
    void sheetSpritesIdentity(YAML::Node&, MigrationLog&){
    }

}

const char* StreamMigrations::versionKey(DocumentKind kind){
    return kind == DocumentKind::Prefab ? "version" : "formatVersion";
}

int StreamMigrations::currentVersion(DocumentKind kind){
    switch (kind){
        case DocumentKind::Scene: return 8;
        case DocumentKind::Project: return 1;
        case DocumentKind::Material: return 1;
        // Prefab::FILE_VERSION
        case DocumentKind::Prefab: return 9;
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
        {4, 5, "splines (identity: an older scene has none)", sceneV4ToV5},
        {5, 6, "Animation Player controllers to Animators", sceneV5ToV6},
        {6, 7, "Line and Trail Renderer curves and gradients in the editor's curve form", sceneV6ToV7},
        {7, 8, "sprite sheets (identity: an older scene has none)", sheetSpritesIdentity},
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
        {6, 7, "Animation Player controllers to Animators", prefabV6ToV7},
        {7, 8, "Line and Trail Renderer curves and gradients in the editor's curve form", prefabV7ToV8},
        {8, 9, "sprite sheets (identity: an older prefab has none)", sheetSpritesIdentity},
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
