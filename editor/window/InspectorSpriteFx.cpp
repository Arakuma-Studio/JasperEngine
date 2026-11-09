// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sprite material Inspector's Pixel FX rows (docs/specs/S48-pixel-fx.md R37): a 40 x 40 live
// thumbnail beside each library:sprite feature of a Sprite's material (that feature alone at the
// material's values, rendered offscreen when a value changes), and a Preview row under the
// animated amounts (Flash Amount, Dither Fade, Dissolve Progress, Ramp Amount): a scrub slider and
// a play button that show the effect in the Scene and Game views without saving, like the Camera
// Shaker's Test strip. Stop, a save, Play or another selection puts the material's value back.

#include "InspectorWindow.h"

#include "App.h"
#include "Backend.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/SpriteFxPreview.h"
#include "util/UIProbe.h"
#include "window/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>

using namespace jasperengine;

namespace {

    struct SpriteFxInspectorState {
        // the thumbnails of the shown GameObject's material, by feature
        uint32_t thumbnailScene = 0;
        Entity thumbnailEntity = NULL_ENTITY;
        std::map<std::string, std::unique_ptr<editor::SpriteFxThumbnail>> thumbnails;
        editor::SpriteFxScrub scrub;
    };

    SpriteFxInspectorState& spriteFxState(){
        static SpriteFxInspectorState state;
        return state;
    }

    // "submeshes[N].material.shaderParams": N, else -1 (an extra pass or another component's values)
    int spriteFxSubmesh(const std::string& paramsProperty){
        static const std::string prefix = "submeshes[";
        static const std::string suffix = "].material.shaderParams";
        if (paramsProperty.compare(0, prefix.size(), prefix) != 0 || paramsProperty.size() <= prefix.size() + suffix.size() ||
            paramsProperty.compare(paramsProperty.size() - suffix.size(), suffix.size(), suffix) != 0)
            return -1;
        return std::atoi(paramsProperty.c_str() + prefix.size());
    }

    // the animated amounts a Preview row plays: from, to (the effect's direction)
    bool spriteFxAnimated(const std::string& member, float& from, float& to){
        if (member == "flashAmount"){ from = 1.0f; to = 0.0f; return true; }
        if (member == "ditherFadeAmount" || member == "dissolveProgress" || member == "rampAmount"){ from = 0.0f; to = 1.0f; return true; }
        return false;
    }

}

void editor::InspectorWindow::drawSpriteFxThumbnail(SceneProject* sceneProject, Entity entity, ComponentType cpType,
                                                   const std::string& paramsProperty, const std::string& define){
    const int submesh = spriteFxSubmesh(paramsProperty);
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (cpType != ComponentType::MeshComponent || submesh != 0 || !scene)
        return;
    const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!sprite || !mesh || mesh->numSubmeshes == 0)
        return;
    const Submesh& sub = mesh->submeshes[0];
    const Texture& texture = sub.material.baseColorTexture;
    if (texture.empty() || texture.getWidth() == 0 || texture.getHeight() == 0)
        return;

    SpriteFxInspectorState& state = spriteFxState();
    if (state.thumbnailScene != sceneProject->id || state.thumbnailEntity != entity){
        state.thumbnails.clear();
        state.thumbnailScene = sceneProject->id;
        state.thumbnailEntity = entity;
    }
    std::unique_ptr<SpriteFxThumbnail>& thumbnail = state.thumbnails[define];
    if (!thumbnail)
        thumbnail = std::make_unique<SpriteFxThumbnail>();
    const Rect rect = sub.hasTextureRect ? sub.textureRect : Rect(0.0f, 0.0f, 1.0f, 1.0f);
    const int texelsX = std::max(1, (int)std::lround(std::fabs(rect.getWidth()) * (float)texture.getWidth()));
    const int texelsY = std::max(1, (int)std::lround(std::fabs(rect.getHeight()) * (float)texture.getHeight()));
    thumbnail->update(sub.material, define, rect, texelsX, texelsY);
    if (thumbnail->isPending())
        Backend::getApp().requestFrame();

    ImGui::SameLine();
    Texture rendered = thumbnail->getTexture();
    const ImTextureID id = Backend::getImGuiTexture(rendered.getRender());
    const float size = (float)SpriteFxThumbnail::SIZE;
    ImGui::ImageButton(("##spritefx_thumb_" + define).c_str(), id, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                       ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    UIProbe::item("spritefx.thumb." + define);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s alone at the material's values (the frame at a whole zoom, Nearest)", define.c_str());
}

void editor::InspectorWindow::drawSpriteFxPreviewRow(SceneProject* sceneProject, Entity entity, ComponentType cpType,
                                                    const std::string& paramsProperty, const ShaderManifestProperty& property){
    float from = 0.0f, to = 1.0f;
    const int submesh = spriteFxSubmesh(paramsProperty);
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (cpType != ComponentType::MeshComponent || submesh < 0 || !scene || !spriteFxAnimated(property.member, from, to))
        return;
    // edit mode only: in Play the game drives its own values
    if (sceneProject->playState != ScenePlayState::STOPPED || !scene->findComponent<SpriteComponent>(entity))
        return;
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh || (unsigned int)submesh >= mesh->numSubmeshes)
        return;

    SpriteFxScrub& scrub = spriteFxState().scrub;
    const bool mine = scrub.isActive() && scrub.getSceneId() == sceneProject->id && scrub.getEntity() == entity &&
                      scrub.getMember() == property.member;
    if (mine && scrub.isPlaying()){
        scrub.update(ImGui::GetIO().DeltaTime);
        Backend::getApp().requestFrame();
    }

    ImGui::PushID(("spritefx_preview_" + property.member).c_str());
    propertyHeader("Preview");
    float shown = mine ? scrub.getValue() : from;
    const float button = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::SliderFloat(("##spritefx_scrub_" + property.member).c_str(), &shown, 0.0f, 1.0f))
        scrub.scrub(scene, sceneProject->id, entity, (unsigned int)submesh, property.member, shown);
    UIProbe::item("spritefx.scrub." + property.member);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shows the effect at this amount in the Scene and Game views; nothing is saved");
    ImGui::SameLine();
    const bool playing = mine && scrub.isPlaying();
    const std::string label = std::string(playing || mine ? ICON_FA_STOP : ICON_FA_PLAY) + "##spritefx_play_" + property.member;
    if (ImGui::Button(label.c_str(), ImVec2(button, 0))){
        if (mine)
            scrub.stop();
        else
            scrub.play(scene, sceneProject->id, entity, (unsigned int)submesh, property.member, from, to, 1.0f);
    }
    UIProbe::item("spritefx.play." + property.member);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(mine ? "Stop the preview (the material's value comes back)" : "Play the effect once over a second");
    ImGui::PopID();
}

void editor::InspectorWindow::syncSpriteFxPreview(SceneProject* shown, const std::vector<Entity>& selection){
    SpriteFxInspectorState& state = spriteFxState();
    // the thumbnails belong to the shown GameObject
    if (!shown || selection.size() != 1 || state.thumbnailScene != shown->id || state.thumbnailEntity != selection[0]){
        state.thumbnails.clear();
        state.thumbnailScene = 0;
        state.thumbnailEntity = NULL_ENTITY;
    }
    SpriteFxScrub& scrub = state.scrub;
    if (!scrub.isActive())
        return;
    const bool ownerShown = shown && shown->id == scrub.getSceneId() && shown->playState == ScenePlayState::STOPPED &&
                            selection.size() == 1 && selection[0] == scrub.getEntity();
    if (!ownerShown)
        scrub.stop();
}

void editor::InspectorWindow::stopSpriteFxPreview(){
    spriteFxState().scrub.stop();
}

void editor::InspectorWindow::forgetSpriteFxScene(uint32_t sceneId){
    SpriteFxInspectorState& state = spriteFxState();
    // the scene is gone: nothing to put back
    if (state.scrub.isActive() && state.scrub.getSceneId() == sceneId)
        state.scrub.forget();
    if (state.thumbnailScene == sceneId){
        state.thumbnails.clear();
        state.thumbnailScene = 0;
        state.thumbnailEntity = NULL_ENTITY;
    }
}
