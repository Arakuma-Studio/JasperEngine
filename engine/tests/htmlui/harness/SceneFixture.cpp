// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneFixture.h"

#include "GpuEnvironment.h"

#include "Engine.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UIDocumentSystem.h"

using namespace jasperengine;
using namespace jasperengine::test;

SceneFixture::SceneFixture(){
    GpuEnvironment::ensure();

    scene.reset(new Scene());
    scene->setBackgroundColor(backgroundRed, backgroundGreen, backgroundBlue, 1.0f);

    camera.reset(new Camera(scene.get()));
    camera->setType(CameraType::CAMERA_UI);
    camera->activate();

    Engine::setScene(scene.get());
}

SceneFixture::~SceneFixture(){
    Engine::removeScene(scene.get());
    objects.clear();
    camera.reset();
    scene.reset();
}

Scene& SceneFixture::getScene(){
    return *scene;
}

Camera& SceneFixture::getCamera(){
    return *camera;
}

UIDocument SceneFixture::addDocument(const std::string& file, int sortOrder){
    UIDocument* document = new UIDocument(scene.get());
    objects.emplace_back(document);
    document->setSortOrder(sortOrder);
    document->setFilename(file);
    hasDocuments = true;
    return UIDocument(scene.get(), document->getEntity());
}

bool SceneFixture::waitUntilLoaded(int maxFrames){
    Scene* current = scene.get();
    const bool documents = hasDocuments;
    return GpuEnvironment::runFrames([current, documents](){
        if (!current->getSystem<RenderSystem>()->isAllLoaded())
            return false;
        // documents need their UIDocumentSystem (registered by Scene::init) loaded too
        return !documents || current->getSystem<UIDocumentSystem>()->isAllLoaded();
    }, maxFrames);
}
