// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteSkin.h"

#include "component/SpriteSkinComponent.h"
#include "sprite/SpriteSheet.h"

using namespace jasperengine;

SpriteSkin::SpriteSkin(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<SpriteSkinComponent>(entity)){
        scene->addComponent<SpriteSkinComponent>(entity, {});
    }
}

SpriteSkin::~SpriteSkin(){
}

SpriteSkin::SpriteSkin(const SpriteSkin& rhs) = default;
SpriteSkin& SpriteSkin::operator=(const SpriteSkin& rhs) = default;

void SpriteSkin::setEnabled(bool enabled){
    getComponent<SpriteSkinComponent>().enabled = enabled;
    markChanged<SpriteSkinComponent>();
}

bool SpriteSkin::isEnabled() const{
    return getComponent<SpriteSkinComponent>().enabled;
}

void SpriteSkin::setRootBone(Entity bone){
    getComponent<SpriteSkinComponent>().rootBone = bone;
    markChanged<SpriteSkinComponent>();
}

Entity SpriteSkin::getRootBone() const{
    return getComponent<SpriteSkinComponent>().rootBone;
}

void SpriteSkin::setBones(const std::vector<Entity>& bones){
    SpriteSkinComponent& skin = getComponent<SpriteSkinComponent>();
    skin.bones = bones;
    skin.paletteDirty = true;
    markChanged<SpriteSkinComponent>();
}

std::vector<Entity> SpriteSkin::getBones() const{
    return getComponent<SpriteSkinComponent>().bones;
}

int SpriteSkin::getSkinBoneCount() const{
    const SpriteSkinComponent& skin = getComponent<SpriteSkinComponent>();
    return skin.builtSkin ? static_cast<int>(skin.builtSkin->bones.size()) : 0;
}

bool SpriteSkin::isSkinned() const{
    return getComponent<SpriteSkinComponent>().builtSkinned;
}
