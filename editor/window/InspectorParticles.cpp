// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector > Particle System (particles spec 6.8). A version 2 system shows Unity's modules in
// Unity's order: Main always open, the others as foldouts with their enable checkbox in the
// header and a "+" menu for the ones that are off; MinMaxCurve / MinMaxGradient rows with the
// mode arrow (widget/MinMaxFields), the bursts table, the rows a module shows for its mode
// (the shape's type, separate axes, the texture sheet's time mode, the render mode). A version
// 1 system keeps its legacy rows under the "Upgrade to modules" banner, whose popup previews the
// report before one undoable upgrade. Every value is a Catalog property edited through
// PropertyCmd (undo, multi-selection, prefab override markers).

#include "InspectorWindow.h"

#include "Catalog.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ActionComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "action/ParticleSystem.h"
#include "external/IconsFontAwesome6.h"
#include "particles/ParticleFields.h"
#include "particles/ParticleUpgrade.h"
#include "util/ProjectUtils.h"
#include "window/widget/MinMaxFields.h"
#include "subsystem/ActionSystem.h"

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float RAD_TO_DEG = 57.2957795f;

    // a field's label: Unity's words ("startLifetime" -> "Start Lifetime"), with the few Unity spells differently
    std::string fieldLabel(const std::string& module, const std::string& name){
        static const std::map<std::string, std::string> labels = {
            {"main.startSize", "Start Size"}, {"main.startRotation", "Start Rotation"}, {"main.startSize3D", "3D Start Size"},
            {"main.startRotation3D", "3D Start Rotation"}, {"main.flipRotation", "Flip Rotation"}, {"main.maxParticles", "Max Particles"},
            {"main.autoRandomSeed", "Auto Random Seed"}, {"shape.shapeType", "Shape"}, {"shape.boxThickness", "Box Thickness"},
            {"shape.donutRadius", "Donut Radius"}, {"shape.alignToDirection", "Align To Direction"},
            {"rotationOverLifetime.z", "Angular Velocity"}, {"rotationBySpeed.z", "Angular Velocity"},
            {"textureSheetAnimation.numTilesX", "Tiles X"}, {"textureSheetAnimation.numTilesY", "Tiles Y"},
            {"textureSheetAnimation.fps", "FPS"}, {"renderer.renderMode", "Render Mode"}, {"renderer.alignment", "Render Alignment"},
            {"renderer.sortingFudge", "Sorting Fudge"}, {"renderer.sortPriority", "Order in Layer"},
            {"inheritVelocity.curve", "Multiplier"}, {"velocityOverLifetime.radial", "Radial"},
        };
        auto it = labels.find(module + "." + name);
        if (it != labels.end()) return it->second;
        std::string out;
        for (size_t i = 0; i < name.size(); i++){
            const char c = name[i];
            if (i == 0) out += (char)std::toupper((unsigned char)c);
            else if (std::isupper((unsigned char)c) && !std::isupper((unsigned char)name[i - 1])) { out += ' '; out += c; }
            else if (std::isdigit((unsigned char)c) && !std::isdigit((unsigned char)name[i - 1])) { out += ' '; out += c; }
            else out += c;
        }
        return out;
    }

    const char* moduleTitle(const std::string& module){
        static const std::map<std::string, const char*> titles = {
            {"main", "Main"}, {"emission", "Emission"}, {"shape", "Shape"}, {"velocityOverLifetime", "Velocity over Lifetime"},
            {"limitVelocityOverLifetime", "Limit Velocity over Lifetime"}, {"inheritVelocity", "Inherit Velocity"},
            {"forceOverLifetime", "Force over Lifetime"}, {"colorOverLifetime", "Color over Lifetime"}, {"colorBySpeed", "Color by Speed"},
            {"sizeOverLifetime", "Size over Lifetime"}, {"sizeBySpeed", "Size by Speed"}, {"rotationOverLifetime", "Rotation over Lifetime"},
            {"rotationBySpeed", "Rotation by Speed"}, {"noise", "Noise"}, {"textureSheetAnimation", "Texture Sheet Animation"},
            {"renderer", "Renderer"}};
        auto it = titles.find(module);
        return it == titles.end() ? "Module" : it->second;
    }

    const char* fieldHelp(const std::string& path){
        static const std::map<std::string, const char*> help = {
            {"main.duration", "The length of a cycle in seconds (the emission's curves and the bursts run over it)."},
            {"main.prewarm", "Starts as if a whole cycle had already played (looping systems only)."},
            {"main.startDelay", "Seconds before the first emission."},
            {"main.flipRotation", "The share (0..1) of particles that rotate the other way."},
            {"main.gravityModifier", "Times the scene's gravity (Physics 3D, or the 2D world's in the scene's units)."},
            {"main.simulationSpace", "Local: the particles move with the GameObject. World: they stay where they were emitted."},
            {"main.scalingMode", "Hierarchy: the whole world scale; Local: the GameObject's own scale; Shape: only the shape's positions."},
            {"main.playOnAwake", "Plays when the scene starts or the GameObject becomes active."},
            {"main.stopAction", "When the system stops: disable or destroy the GameObject, or call onParticleSystemStopped() on its scripts."},
            {"main.autoRandomSeed", "A new random seed at every play; off: Random Seed, so it replays the same."},
            {"emission.rateOverDistance", "Particles per unit the GameObject moves (in the world)."},
            {"shape.radiusThickness", "0: on the surface, 1: the whole volume."},
            {"shape.arcSpread", "The arc in steps of this share (0: anywhere)."},
            {"shape.randomDirectionAmount", "Blends the direction towards a random one (1: fully random)."},
            {"shape.sphericalDirectionAmount", "Blends the direction towards the one from the shape's centre."},
            {"limitVelocityOverLifetime.dampen", "The share of the excess speed removed (frame-rate independent: per 1/30 s)."},
            {"noise.damping", "The strength divided by the frequency (Unity's)."},
            {"noise.frequency", "Lower: smoother, larger turbulence."},
            {"textureSheetAnimation.frameOverTime", "The frame over the particle's life, 0..1 of the frames."},
            {"textureSheetAnimation.startFrame", "Added to the frame (in frames)."},
            {"renderer.sortingFudge", "Added to the camera distance when sorting with other transparent objects: lower draws on top."},
            {"renderer.minParticleSize", "The smallest share of the viewport height a particle may cover."},
            {"renderer.maxParticleSize", "The largest share of the viewport height a particle may cover (0: no limit)."},
            {"renderer.pivot", "Moves each particle by this many particle sizes."},
            {"renderer.flip", "The share (0..1) of particles mirrored on each axis."},
        };
        auto it = help.find(path);
        return it == help.end() ? "" : it->second;
    }

    // Unity's enum labels ("ConeVolume" -> "Cone Volume"), one table per enum
    std::vector<editor::EnumEntry>* enumEntries(const particles::FieldInfo& field){
        static std::map<const std::vector<std::string>*, std::pair<std::vector<std::string>, std::vector<editor::EnumEntry>>> tables;
        auto it = tables.find(field.enumNames);
        if (it != tables.end()) return &it->second.second;
        auto& entry = tables[field.enumNames];
        for (const std::string& name : *field.enumNames){
            std::string label;
            for (size_t i = 0; i < name.size(); i++){
                if (i > 0 && std::isupper((unsigned char)name[i]) && std::islower((unsigned char)name[i - 1])) label += ' ';
                label += name[i];
            }
            entry.first.push_back(label);
        }
        for (size_t i = 0; i < entry.first.size(); i++) entry.second.push_back({(int)i, entry.first[i].c_str()});
        return &entry.second;
    }

    // whether a field shows for the module's current mode (the first selected system's)
    bool visible(const particles::FieldInfo& f, const ParticleSystemComponent& c){
        const std::string m = f.module, n = f.name;
        if (n == "enabled") return false;
        if (m == "main"){
            if (n == "startSizeY" || n == "startSizeZ") return c.main.startSize3D;
            if (n == "startRotationX" || n == "startRotationY") return c.main.startRotation3D;
            if (n == "randomSeed") return !c.main.autoRandomSeed;
            if (n == "prewarm") return c.main.looping;
            if (n == "bursts") return false;
        }
        if (m == "emission" && n == "bursts") return false;
        if (m == "shape"){
            using T = ParticleSystemShapeType;
            const T t = c.shape.shapeType;
            const bool round = t == T::Sphere || t == T::Hemisphere || t == T::Cone || t == T::ConeVolume || t == T::Circle || t == T::Donut;
            if (n == "radius") return round || t == T::Edge;
            if (n == "radiusThickness") return round;
            if (n == "radiusMode" || n == "radiusSpread") return t == T::Edge || t == T::Circle || t == T::Cone || t == T::Donut;
            if (n == "radiusSpeed") return (t == T::Edge || t == T::Circle || t == T::Cone || t == T::Donut) &&
                                          (c.shape.radiusMode == ParticleSystemShapeMultiModeValue::Loop || c.shape.radiusMode == ParticleSystemShapeMultiModeValue::PingPong);
            if (n == "angle") return t == T::Cone || t == T::ConeVolume;
            if (n == "length") return t == T::ConeVolume;
            if (n == "arc" || n == "arcMode" || n == "arcSpread") return round;
            if (n == "arcSpeed") return round && (c.shape.arcMode == ParticleSystemShapeMultiModeValue::Loop || c.shape.arcMode == ParticleSystemShapeMultiModeValue::PingPong);
            if (n == "boxThickness") return t == T::BoxShell || t == T::BoxEdge;
            if (n == "donutRadius") return t == T::Donut;
        }
        if (m == "limitVelocityOverLifetime"){
            if (n == "limit") return !c.limitVelocityOverLifetime.separateAxes;
            if (n == "limitX" || n == "limitY" || n == "limitZ") return c.limitVelocityOverLifetime.separateAxes;
        }
        auto axes = [&](bool separate){
            if (n == "y" || n == "z") return separate;
            return true;
        };
        if (m == "sizeOverLifetime") return axes(c.sizeOverLifetime.separateAxes);
        if (m == "sizeBySpeed") return axes(c.sizeBySpeed.separateAxes);
        if (m == "rotationOverLifetime" && (n == "x" || n == "y")) return c.rotationOverLifetime.separateAxes;
        if (m == "rotationBySpeed" && (n == "x" || n == "y")) return c.rotationBySpeed.separateAxes;
        if (m == "noise" && (n == "strengthY" || n == "strengthZ")) return c.noise.separateAxes;
        if (m == "textureSheetAnimation"){
            const auto& s = c.textureSheetAnimation;
            const bool grid = s.mode == ParticleSystemAnimationMode::Grid;
            if (n == "numTilesX" || n == "numTilesY" || n == "animation") return grid;
            if (n == "rowMode") return grid && s.animation == ParticleSystemAnimationType::SingleRow;
            if (n == "rowIndex") return grid && s.animation == ParticleSystemAnimationType::SingleRow && s.rowMode == ParticleSystemAnimationRowMode::Custom;
            if (n == "sprites") return false;
            if (n == "frameOverTime" || n == "cycles") return s.timeMode == ParticleSystemAnimationTimeMode::Lifetime;
            if (n == "fps") return s.timeMode == ParticleSystemAnimationTimeMode::FPS;
            if (n == "speedRange") return s.timeMode == ParticleSystemAnimationTimeMode::Speed;
        }
        if (m == "renderer"){
            const auto mode = c.renderer.renderMode;
            if (n == "cameraVelocityScale" || n == "velocityScale" || n == "lengthScale") return mode == ParticleSystemRenderMode::Stretch;
            if (n == "alignment") return mode == ParticleSystemRenderMode::Billboard || mode == ParticleSystemRenderMode::Mesh;
            if (n == "mesh") return false;
        }
        return true;
    }

    std::set<std::string> openModules = {"main", "emission", "shape", "renderer"};

    const std::vector<std::string> RENDERER_MESHES = {"quad", "cube", "sphere", "cylinder", "capsule", "plane"};

}

void editor::InspectorWindow::drawParticleSystemComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    int v1 = 0, v2 = 0;
    for (Entity entity : entities){
        if (const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity)){
            (ps->version >= 2 ? v2 : v1)++;
        }
    }
    if (v1 > 0 && v2 > 0){
        ImGui::TextWrapped("The selection mixes legacy (version 1) and module (version 2) Particle Systems: select one kind to edit them.");
        return;
    }
    if (v1 > 0){
        drawParticleUpgradeBanner(cpType, sceneProject, entities);
        drawParticleSystemLegacy(cpType, sceneProject, entities);
        return;
    }
    drawParticleModules(cpType, sceneProject, entities);
}

void editor::InspectorWindow::drawParticleUpgradeBanner(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    Scene* scene = sceneProject->scene;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " A legacy Particle System (version 1: initializers and modifiers). Upgrade it to Unity's modules "
                       "to use start speed along the shape, curves, bursts with cycles, noise, render modes and the Lua modules.");
    ImGui::PopStyleColor();
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    ImGui::BeginDisabled(!stopped);
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Upgrade to modules...")){
        particleUpgradeForce = false;
        ImGui::OpenPopup("particle_upgrade_popup");
    }
    ImGui::EndDisabled();
    if (!stopped && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("Stop the scene to upgrade");
    }

    if (ImGui::BeginPopup("particle_upgrade_popup")){
        bool any = false, refused = false;
        for (Entity entity : entities){
            const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
            if (!ps || ps->version >= 2) continue;
            const Entity target = scene->getSystem<ActionSystem>()->particleTarget(entity);
            const particles::UpgradeResult result = particles::upgradeLegacy(*ps, ParticleSystem::legacyTargetOf(scene, target), particleUpgradeForce);
            any = true;
            refused = refused || !result.report.upgraded;
            ImGui::SeparatorText(scene->getEntityName(entity).c_str());
            if (ImGui::BeginTable("##upgrade_report", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)){
                for (const particles::UpgradeItem& item : result.report.items){
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(item.field.c_str());
                    ImGui::TableSetColumnIndex(1);
                    const ImVec4 color = item.status == particles::UpgradeStatus::Exact ? ImVec4(0.4f, 0.8f, 0.4f, 1.0f)
                                       : item.status == particles::UpgradeStatus::Approximated ? ImVec4(0.9f, 0.75f, 0.3f, 1.0f)
                                       : ImVec4(0.9f, 0.35f, 0.3f, 1.0f);
                    ImGui::TextColored(color, "%s", particles::upgradeStatusName(item.status));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(item.message.c_str());
                }
                ImGui::EndTable();
            }
        }
        if (refused){
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.0f), "Something has no module equivalent (a position modifier).");
            ImGui::Checkbox("Drop it and upgrade anyway", &particleUpgradeForce);
        }
        ImGui::BeginDisabled(!any || (refused && !particleUpgradeForce));
        if (ImGui::Button("Upgrade")){
            MultiPropertyCmd* multi = new MultiPropertyCmd();
            for (Entity entity : entities){
                const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
                if (!ps || ps->version >= 2) continue;
                const Entity target = scene->getSystem<ActionSystem>()->particleTarget(entity);
                const particles::UpgradeResult result = particles::upgradeLegacy(*ps, ParticleSystem::legacyTargetOf(scene, target), particleUpgradeForce);
                if (result.report.upgraded){
                    ProjectUtils::addParticleSystemValues(*multi, project, sceneProject->id, scene, entity, result.component);
                }
            }
            multi->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();
    (void)cpType;
}

void editor::InspectorWindow::drawParticleModules(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    Scene* scene = sceneProject->scene;
    const ParticleSystemComponent* firstPtr = scene->findComponent<ParticleSystemComponent>(entities[0]);
    if (!firstPtr) return;
    const ParticleSystemComponent first = *firstPtr;

    // the runtime readout (Play and the preview)
    if (first.state.playing || first.state.count() > 0){
        ImGui::TextDisabled("%s  %zu particles  time %.2f s", first.state.paused ? "Paused" : (first.state.playing ? "Playing" : "Stopped"),
                            first.state.count(), first.state.time);
    }
    // no renderer to draw on: tell how to get one
    const Entity target = scene->getSystem<ActionSystem>()->particleTarget(entities[0]);
    if (!scene->findComponent<MeshComponent>(target) && !scene->findComponent<PointsComponent>(target)){
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.3f, 1.0f));
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Nothing draws these particles: add a Mesh (its instances draw them) or Points to this "
                           "GameObject, or create one with GameObject > Effects > Particle System.");
        ImGui::PopStyleColor();
    }

    for (const std::string& module : particles::moduleNames()){
        const bool hasEnabled = particles::moduleHasEnabled(module);
        bool enabled = true;
        if (hasEnabled){
            const particles::FieldInfo* field = particles::findField(module + ".enabled");
            enabled = *static_cast<const bool*>(field->get(first));
            // only the enabled ones and the main ones; the others are in the "+" menu
            if (!enabled && module != "emission" && module != "shape") continue;
        }
        ImGui::PushID(module.c_str());
        if (hasEnabled){
            bool check = enabled;
            if (ImGui::Checkbox("##enabled", &check)){
                MultiPropertyCmd* multi = new MultiPropertyCmd();
                for (Entity entity : entities){
                    multi->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, module + ".enabled", check);
                }
                multi->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
            }
            ImGui::SameLine();
        }
        const bool wasOpen = openModules.count(module) > 0;
        ImGui::SetNextItemOpen(wasOpen);
        const bool open = ImGui::TreeNodeEx(moduleTitle(module), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                            (module == "main" ? ImGuiTreeNodeFlags_Framed : 0));
        if (open) openModules.insert(module); else openModules.erase(module);
        if (open){
            ImGui::BeginDisabled(hasEnabled && !enabled);
            drawParticleModuleRows(module, cpType, sceneProject, entities, first);
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }

    // the modules that are off
    if (ImGui::Button(ICON_FA_PLUS " Module")){
        ImGui::OpenPopup("particle_add_module");
    }
    if (ImGui::BeginPopup("particle_add_module")){
        bool anyOff = false;
        for (const std::string& module : particles::moduleNames()){
            if (!particles::moduleHasEnabled(module) || module == "emission" || module == "shape") continue;
            const particles::FieldInfo* field = particles::findField(module + ".enabled");
            if (*static_cast<const bool*>(field->get(first))) continue;
            anyOff = true;
            if (ImGui::MenuItem(moduleTitle(module))){
                MultiPropertyCmd* multi = new MultiPropertyCmd();
                for (Entity entity : entities){
                    multi->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, module + ".enabled", true);
                }
                multi->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
                openModules.insert(module);
            }
        }
        if (!anyOff) ImGui::TextDisabled("Every module is on");
        ImGui::EndPopup();
    }
}

void editor::InspectorWindow::drawParticleModuleRows(const std::string& module, ComponentType cpType, SceneProject* sceneProject,
                                                     const std::vector<Entity>& entities, const ParticleSystemComponent& first){
    beginTable(cpType, getLabelSize("Limit Velocity Space X"), "particles_" + module);
    if (module == "main"){
        RowSettings unscaled;
        unscaled.help = "Advances with the unscaled time: it keeps playing at Engine.timeScale 0 (UI particles).";
        propertyRow(RowPropertyType::Bool, cpType, "useUnscaledTime", "Unscaled Time", sceneProject, entities, unscaled);
    }
    for (const particles::FieldInfo* field : particles::moduleFields(module)){
        if (!visible(*field, first)) continue;
        const std::string path = field->path();
        std::string label = fieldLabel(module, field->name);
        // Unity's X / Y / Z labels where the axes are separate
        if ((module == "main" && path == "main.startSize" && first.main.startSize3D) ||
            (module == "main" && path == "main.startRotation" && first.main.startRotation3D)){
            label += path == "main.startSize" ? " X" : " Z";
        }
        if ((module == "sizeOverLifetime" && first.sizeOverLifetime.separateAxes && field->name == std::string("size")) ||
            (module == "sizeBySpeed" && first.sizeBySpeed.separateAxes && field->name == std::string("size"))){
            label = "Size X";
        }
        if (module == "noise" && first.noise.separateAxes && field->name == std::string("strength")) label = "Strength X";
        RowSettings settings;
        settings.help = fieldHelp(path);
        if (field->radians){
            settings.displayScale = RAD_TO_DEG;
            settings.format = "%.1f deg";
            settings.stepSize = 1.0f;
        }
        RowPropertyType type = RowPropertyType::Float;
        switch (field->type){
            case particles::FieldType::Bool: type = RowPropertyType::Bool; break;
            case particles::FieldType::Int: type = RowPropertyType::Int; settings.secondColSize = 6 * ImGui::GetFontSize(); break;
            case particles::FieldType::UInt: type = RowPropertyType::UInt; settings.secondColSize = 6 * ImGui::GetFontSize(); break;
            case particles::FieldType::Float: type = RowPropertyType::Float; settings.secondColSize = 6 * ImGui::GetFontSize(); break;
            case particles::FieldType::Vector2: type = RowPropertyType::Vector2; break;
            case particles::FieldType::Vector3: type = RowPropertyType::Vector3; break;
            case particles::FieldType::Curve: type = RowPropertyType::MinMaxCurve; break;
            case particles::FieldType::Gradient: type = RowPropertyType::MinMaxGradient; break;
            case particles::FieldType::Enum: type = RowPropertyType::Enum; settings.enumEntries = enumEntries(*field); break;
            case particles::FieldType::String: type = RowPropertyType::String; break;
            case particles::FieldType::Bursts:
            case particles::FieldType::IntList: continue;
        }
        if (path == "main.flipRotation" || path == "textureSheetAnimation.flipU" || path == "textureSheetAnimation.flipV" ||
            path == "shape.radiusThickness" || path == "shape.randomDirectionAmount" || path == "shape.sphericalDirectionAmount" ||
            path == "limitVelocityOverLifetime.dampen" || path == "shape.arcSpread" || path == "shape.radiusSpread"){
            type = RowPropertyType::Float_0_1;
        }
        propertyRow(type, cpType, path, label, sceneProject, entities, settings);
    }

    if (module == "renderer"){
        // the Mesh render mode's mesh: a primitive the target draws (models: the Unity importer)
        if (first.renderer.renderMode == ParticleSystemRenderMode::Mesh && entities.size() == 1){
            const Entity entity = entities[0];
            const Entity target = sceneProject->scene->getSystem<ActionSystem>()->particleTarget(entity);
            propertyHeader("Mesh");
            const std::string current = first.renderer.mesh;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##particle_mesh", current.c_str())){
                for (const std::string& option : RENDERER_MESHES){
                    if (ImGui::Selectable(option.c_str(), option == current) && option != current){
                        Scene* scene = sceneProject->scene;
                        auto* cmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "renderer.mesh", option, [scene, target, entity](){
                            if (const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity)){
                                ParticleSystem::applyRendererMesh(scene, target, ps->renderer.mesh);
                            }
                        });
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                        finishProperty = true;
                    }
                }
                ImGui::EndCombo();
            }
        }
    }
    endTable();

    if (module == "emission"){
        if (entities.size() == 1){
            drawParticleBursts(cpType, sceneProject, entities[0]);
        }else{
            ImGui::TextDisabled("Bursts: select one Particle System to edit them");
        }
    }
    if (module == "textureSheetAnimation" && first.textureSheetAnimation.mode == ParticleSystemAnimationMode::Sprites){
        drawParticleFrameList(cpType, "textureSheetAnimation.sprites", "particle_sheet_sprites", sceneProject, entities);
    }
}

void editor::InspectorWindow::drawParticleBursts(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    Scene* scene = sceneProject->scene;
    const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    if (!ps) return;
    const std::vector<ParticleEmissionBurst> bursts = ps->emission.bursts;
    auto commit = [&](const std::vector<ParticleEmissionBurst>& value, bool end){
        auto* cmd = new PropertyCmd<std::vector<ParticleEmissionBurst>>(project, sceneProject->id, entity, cpType, "emission.bursts", value);
        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        if (end) finishProperty = true;
    };

    ImGui::TextUnformatted("Bursts");
    if (ImGui::BeginTable("##particle_bursts", 6, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)){
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Count");
        ImGui::TableSetupColumn("Cycles");
        ImGui::TableSetupColumn("Interval");
        ImGui::TableSetupColumn("Probability");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < bursts.size(); i++){
            ImGui::PushID((int)i);
            ParticleEmissionBurst burst = bursts[i];
            bool changed = false, finished = false;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##time", &burst.time, 0.01f, 0.0f, 1000.0f, "%.2f")) changed = true;
            finished = finished || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TableSetColumnIndex(1);
            const MinMaxFieldResult count = minMaxCurveField("##count", burst.count, false, -1.0f, 1.0f, "%.0f", 1.0f);
            changed = changed || count.changed;
            finished = finished || count.finished;
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragInt("##cycles", &burst.cycles, 0.1f, 0, 100000, burst.cycles == 0 ? "Infinite" : "%d")) changed = true;
            finished = finished || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##interval", &burst.interval, 0.005f, 0.0001f, 100.0f, "%.3f")) changed = true;
            finished = finished || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##probability", &burst.probability, 0.0f, 1.0f, "%.2f")) changed = true;
            finished = finished || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TableSetColumnIndex(5);
            const bool remove = ImGui::Button(ICON_FA_TRASH_CAN);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the burst");
            ImGui::PopID();
            if (remove){
                std::vector<ParticleEmissionBurst> value = bursts;
                value.erase(value.begin() + (std::ptrdiff_t)i);
                commit(value, true);
                break;
            }
            if (changed || finished){
                std::vector<ParticleEmissionBurst> value = bursts;
                burst.cycles = std::max(0, burst.cycles);
                burst.interval = std::max(0.0001f, burst.interval);
                value[i] = burst;
                commit(value, finished);
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(ICON_FA_PLUS " Burst")){
        std::vector<ParticleEmissionBurst> value = bursts;
        ParticleEmissionBurst burst;
        if (!value.empty()) burst.time = value.back().time + 0.5f;
        value.push_back(burst);
        commit(value, true);
    }
}
