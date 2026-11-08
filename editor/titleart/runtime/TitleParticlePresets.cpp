// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleParticlePresets.h"

#include "Project.h"
#include "command/Command.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "component/ParticleSystemComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "mcp/tools/ParticleShared.h"
#include "subsystem/TitleArtSystem.h"
#include "util/ParticleEffectEdit.h"
#include "util/ProjectUtils.h"
#include "util/TitleArtBakeInfo.h"

#include <algorithm>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using mcp::Json;

namespace {

    Json color(float r, float g, float b){
        return Json::array({r, g, b});
    }

    // the rectangle the particles start in: the title's ink, centred on the companion's pivot
    // (UI: y down, the element's own space; world: y up)
    Json rectangle(const TitleParticleFrame& f, bool bottomEdge){
        const float x0 = f.alphaX * f.width - f.width * 0.5f;
        const float x1 = (f.alphaX + f.alphaWidth) * f.width - f.width * 0.5f;
        float y0 = f.alphaY * f.height - f.height * 0.5f;
        float y1 = (f.alphaY + f.alphaHeight) * f.height - f.height * 0.5f;
        if (!f.ui){
            const float top = -y0, bottom = -y1;
            y0 = bottom;
            y1 = top;
        }
        if (bottomEdge){
            // embers rise from the bottom edge of the ink
            const float edge = f.ui ? y1 : y0;
            y0 = y1 = edge;
        }
        return Json{{"shape", "box"}, {"min", Json::array({x0, y0, 0.0f})}, {"max", Json::array({x1, y1, 0.0f})}};
    }

    // a unit of the title's size: UI and 2D world titles are in pixels-like units, a 3D one smaller
    float unit(const TitleParticleFrame& f){
        return std::max(1e-3f, std::min(f.width, f.height) / 10.0f);
    }

}

std::vector<TitleParticlePreset> editor::titleParticlePresets(const TitleParticleFrame& f){
    const float u = unit(f);
    const float up = f.ui ? -1.0f : 1.0f;   // the UI is y down
    std::vector<TitleParticlePreset> out;
    {
        // small additive stars, slow, the scheme's highlight colour
        TitleParticlePreset p{"Sparkle", "magic", Json::object()};
        p.args["preset"] = "magic";
        p.args["name"] = "Sparkle";
        p.args["emitter"] = rectangle(f, false);
        p.args["rate"] = 12;
        p.args["life"] = Json::array({0.6f, 1.2f});
        p.args["size"] = Json::array({u * 0.15f, u * 0.35f});
        p.args["velocity"] = Json{{"min", Json::array({-u * 0.1f, -u * 0.1f, 0.0f})}, {"max", Json::array({u * 0.1f, u * 0.1f, 0.0f})}};
        p.args["color"] = color(f.highlight[0], f.highlight[1], f.highlight[2]);
        p.args["alpha_over_life"] = Json::array({1.0f, 0.0f});
        out.push_back(std::move(p));
    }
    {
        // rise from the bottom edge, orange to red, drifting
        TitleParticlePreset p{"Embers", "sparks", Json::object()};
        p.args["preset"] = "sparks";
        p.args["name"] = "Embers";
        p.args["emitter"] = rectangle(f, true);
        p.args["rate"] = 24;
        p.args["life"] = Json::array({1.0f, 2.0f});
        p.args["size"] = Json::array({u * 0.08f, u * 0.2f});
        p.args["velocity"] = Json{{"min", Json::array({-u * 0.4f, up * u * 1.0f, 0.0f})}, {"max", Json::array({u * 0.4f, up * u * 2.5f, 0.0f})}};
        p.args["acceleration"] = Json{{"min", Json::array({-u * 0.3f, 0.0f, 0.0f})}, {"max", Json::array({u * 0.3f, 0.0f, 0.0f})}};
        p.args["gradient"] = Json::array({Json::array({0.0f, color(1.0f, 0.62f, 0.2f)}), Json::array({1.0f, color(0.8f, 0.1f, 0.05f)})});
        p.args["alpha_over_life"] = Json::array({1.0f, 0.0f});
        out.push_back(std::move(p));
    }
    {
        // one burst, gravity, five colours
        TitleParticlePreset p{"Confetti", "explosion", Json::object()};
        p.args["preset"] = "explosion";
        p.args["name"] = "Confetti";
        p.args["emitter"] = rectangle(f, false);
        p.args["loop"] = false;
        p.args["rate"] = 0;
        p.args["bursts"] = Json::array({Json{{"time", 0.0f}, {"count", 80}}});
        p.args["life"] = Json::array({1.5f, 2.5f});
        p.args["size"] = Json::array({u * 0.15f, u * 0.3f});
        p.args["velocity"] = Json{{"min", Json::array({-u * 4.0f, up * u * 2.0f, 0.0f})}, {"max", Json::array({u * 4.0f, up * u * 6.0f, 0.0f})}};
        p.args["acceleration"] = Json{{"min", Json::array({0.0f, -up * u * 6.0f, 0.0f})}, {"max", Json::array({0.0f, -up * u * 6.0f, 0.0f})}};
        p.args["gradient"] = Json::array({Json::array({0.0f, color(f.highlight[0], f.highlight[1], f.highlight[2])}),
                                          Json::array({0.25f, color(0.95f, 0.3f, 0.45f)}), Json::array({0.5f, color(0.3f, 0.7f, 1.0f)}),
                                          Json::array({0.75f, color(0.4f, 0.9f, 0.4f)}), Json::array({1.0f, color(1.0f, 1.0f, 1.0f)})});
        out.push_back(std::move(p));
    }
    {
        // falling gold discs: a burst, then 2 s at a steady rate
        TitleParticlePreset p{"Coin Shower", "sparks", Json::object()};
        p.args["preset"] = "sparks";
        p.args["name"] = "Coin Shower";
        p.args["emitter"] = rectangle(f, false);
        p.args["loop"] = false;
        p.args["duration"] = 2.0f;
        p.args["rate"] = 30;
        p.args["bursts"] = Json::array({Json{{"time", 0.0f}, {"count", 30}}});
        p.args["life"] = Json::array({1.2f, 1.8f});
        p.args["size"] = Json::array({u * 0.3f, u * 0.45f});
        p.args["velocity"] = Json{{"min", Json::array({-u * 1.0f, up * u * 1.0f, 0.0f})}, {"max", Json::array({u * 1.0f, up * u * 3.0f, 0.0f})}};
        p.args["acceleration"] = Json{{"min", Json::array({0.0f, -up * u * 9.0f, 0.0f})}, {"max", Json::array({0.0f, -up * u * 9.0f, 0.0f})}};
        p.args["color"] = color(1.0f, 0.8f, 0.25f);
        out.push_back(std::move(p));
    }
    return out;
}

TitleParticleFrame editor::titleParticleFrame(Project* project, uint32_t sceneId, Entity entity){
    TitleParticleFrame f;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene) return f;
    Scene* scene = sceneProject->scene;
    f.ui = TitleArtSystem::spaceOf(scene, entity) != titleart::TitleArtSpace::World;
    if (const RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity)){
        f.width = (float)std::max(1u, rect->width);
        f.height = (float)std::max(1u, rect->height);
    }else if (const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity)){
        f.width = (float)std::max(1u, sprite->width);
        f.height = (float)std::max(1u, sprite->height);
    }
    if (const TitleArtRendererComponent* title = scene->findComponent<TitleArtRendererComponent>(entity)){
        TitleArtBakeInfo info;
        std::string error;
        if (!title->title.path.empty() && TitleArtBakeInfo::load(titleArtSidecarPath(title->title.path), info, error) &&
            info.frame.width > 0 && info.frame.height > 0 && !info.alphaBounds.empty()){
            f.alphaX = std::clamp((float)(info.alphaBounds.x - info.frame.x) / info.frame.width, 0.0f, 1.0f);
            f.alphaY = std::clamp((float)(info.alphaBounds.y - info.frame.y) / info.frame.height, 0.0f, 1.0f);
            f.alphaWidth = std::clamp((float)info.alphaBounds.width / info.frame.width, 0.0f, 1.0f - f.alphaX);
            f.alphaHeight = std::clamp((float)info.alphaBounds.height / info.frame.height, 0.0f, 1.0f - f.alphaY);
        }
    }
    return f;
}

namespace {

    // the child Particle System and its effect, as one step
    class TitleParticlesCmd : public Command {
    public:
        TitleParticlesCmd(Project* project, uint32_t sceneId, Entity title, TitleParticlePreset preset, bool ui)
            : project(project), sceneId(sceneId), title(title), preset(std::move(preset)), ui(ui) {}

        bool execute() override {
            create = std::make_unique<CreateEntityCmd>(project, sceneId, preset.label, EntityCreationType::PARTICLE_SYSTEM, title);
            // the UI is y down: what rises in the world rises on screen (create_particles' rule)
            if (ui) create->addProperty<Quaternion>(ComponentType::Transform, "rotation", Quaternion(180.0f, Vector3(1.0f, 0.0f, 0.0f)));
            if (!create->execute()) return false;
            const Entity system = create->getEntity();
            particles::ParticleEffectDocument document;
            std::string error;
            if (!mcp::particlePresetEffect(preset.args, preset.base, true, document, error)) {
                create->undo();
                failure_ = error;
                return false;
            }
            if (ui && document.renderer.texture.empty()) {
                const std::string soft = ProjectUtils::ensureSoftParticleTexture(project);
                if (!soft.empty()) document.renderer.texture = project->normalizeToAssetsRelative(soft).generic_string();
            }
            edit.reset(ParticleEffectEdit::build(project, sceneId, {system}, [&document](particles::ParticleEffectDocument& doc) {
                doc = document;
                return true;
            }));
            if (edit && !edit->execute()) {
                create->undo();
                return false;
            }
            return true;
        }

        void undo() override {
            if (edit) edit->undo();
            if (create) create->undo();
        }

        std::string describe() const override { return "Add Title Particles (" + preset.label + ")"; }
        const char* typeName() const override { return "TitleParticlesCmd"; }
        std::string failure() const override { return failure_; }

    private:
        Project* project;
        uint32_t sceneId;
        Entity title;
        TitleParticlePreset preset;
        bool ui;
        std::unique_ptr<CreateEntityCmd> create;
        std::unique_ptr<Command> edit;
        std::string failure_;
    };

}

bool editor::addTitleParticles(Project* project, uint32_t sceneId, Entity title, const std::string& label, std::string* error){
    const TitleParticleFrame frame = titleParticleFrame(project, sceneId, title);
    for (const TitleParticlePreset& preset : titleParticlePresets(frame)){
        if (preset.label != label) continue;
        CommandHandle::get(sceneId)->addCommandNoMerge(new TitleParticlesCmd(project, sceneId, title, preset, frame.ui));
        return true;
    }
    if (error) *error = "no title particle preset '" + label + "' (Sparkle, Embers, Confetti, Coin Shower)";
    return false;
}
