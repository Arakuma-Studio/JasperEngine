// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_SCENE_FIXTURE_H
#define HTMLUI_TEST_SCENE_FIXTURE_H

#include "Scene.h"
#include "object/Camera.h"
#include "object/ui/UIDocument.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // A scene with a CAMERA_UI camera and an opaque #1E1E1E background, set as the engine's
    // main scene for the lifetime of the fixture (needs GpuEnvironment::ensure()). Objects
    // created through it belong to it; the handles it returns do not own their entity.
    class SceneFixture {
    public:
        static constexpr float backgroundRed = 0x1E / 255.0f;
        static constexpr float backgroundGreen = 0x1E / 255.0f;
        static constexpr float backgroundBlue = 0x1E / 255.0f;

        SceneFixture();
        ~SceneFixture();

        SceneFixture(const SceneFixture&) = delete;
        SceneFixture& operator=(const SceneFixture&) = delete;

        Scene& getScene();
        Camera& getCamera();

        UIDocument addDocument(const std::string& file, int sortOrder = 0);

        // any GameObject type constructible from a Scene*, kept alive by the fixture
        template<typename T>
        T& add(){
            T* object = new T(scene.get());
            objects.emplace_back(object);
            return *object;
        }

        // frames until the scene's render resources (and documents, when there are any)
        // are loaded, then 2 more; false if that never happened
        bool waitUntilLoaded(int maxFrames = 300);

    private:
        std::unique_ptr<Scene> scene;
        std::unique_ptr<Camera> camera;
        std::vector<std::unique_ptr<EntityHandle>> objects;
        bool hasDocuments = false;
    };

} }

#endif //HTMLUI_TEST_SCENE_FIXTURE_H
