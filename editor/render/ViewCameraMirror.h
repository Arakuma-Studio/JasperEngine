// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A camera in one scene that sees what a camera of another scene sees: an Engine::ViewLayer
// draws a scene through a camera of that scene, so a view of a scene through the editor's
// camera (which lives in the edited scene) needs a copy of it there. Prefab Mode In Context
// mirrors the stage's camera into the context scene; the Scene view during Play mirrors the
// editor camera into the scene a game load replaced the played one with.

#pragma once

#include "Scene.h"
#include "math/Matrix4.h"

namespace jasperengine::editor::ViewCameraMirror {

    // The mirror camera in `target` (a system entity with Transform and Camera), made when `slot`
    // names none there
    Entity ensure(Scene* target, Entity& slot, const char* name);

    // `toCamera` in `to` takes the projection of `fromCamera` in `from` and its world placement,
    // moved by `toTarget` (identity: the same world). False when `fromCamera` is no camera.
    bool copy(Scene* from, Entity fromCamera, Scene* to, Entity toCamera, const Matrix4& toTarget = Matrix4());

}
