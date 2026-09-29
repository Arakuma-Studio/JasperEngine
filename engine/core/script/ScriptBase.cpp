// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptBase.h"

using namespace jasperengine;

ScriptBase::ScriptBase(Scene* scene, Entity entity)
    : scene(scene), entity(entity) {
}

Scene* ScriptBase::getScene() const {
    return scene;
}

Entity ScriptBase::getEntity() const {
    return entity;
}