// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Builds are byte-identical for an unchanged project (S98 REQ-5, E27): every build writes the
// components into cooked scenes and prefabs, and the editor decodes a component into a local
// (`RigidbodyComponent body;`, default-initialised). A field without an initialiser keeps what the
// memory held, and a build would write it. So every component, default-initialised over two
// different fillings, writes the same cooked bytes; and what the editor's drawing made of a
// component (a camera sized to the view, a Text's atlas) is no part of the build.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/TextComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "cook/SceneCooker.h"
#include "serialization/CookedScene.h"
#include "reflect/Reflect.h"
#include "serialization/CookStream.h"
#include "serialization/RuntimeFields.h"
#include "util/Fnv.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct AlignedDelete {
        void operator()(unsigned char* bytes) const { ::operator delete(bytes, std::align_val_t(64)); }
    };

    // the cooked record of the component default-initialised over `filling`, and its strings
    std::vector<uint8_t> cookedDefault(const reflect::ComponentDesc& desc, const jasperengine::cook::ComponentTable& table, unsigned char filling) {
        const size_t size = desc.body.ops->size;
        std::unique_ptr<unsigned char, AlignedDelete> storage(static_cast<unsigned char*>(::operator new(size, std::align_val_t(64))));
        // volatile: the compiler may not drop stores to memory an object is then made in
        volatile unsigned char* bytes = storage.get();
        for (size_t i = 0; i < size; i++) bytes[i] = filling;
        desc.defaultInitialise(storage.get());

        Scene scene;
        const Entity entity = scene.createEntity();
        jasperengine::cook::StringTable strings;
        jasperengine::cook::Writer w(strings, [](Entity e) { return static_cast<uint32_t>(e); });
        jasperengine::cook::WriteContext ctx;
        ctx.registry = &scene;
        ctx.entity = entity;
        jasperengine::cook::writeComponent(w, table, storage.get(), ctx);
        std::vector<uint8_t> out = w.data();
        for (const std::string& text : strings.strings()) out.insert(out.end(), text.begin(), text.end());
        // a Camera's framebuffer is the RenderSystem's to free when the component goes from a
        // scene, not its destructor's: this one was never in a scene
        if (desc.type == ComponentType::CameraComponent) {
            CameraComponent* camera = static_cast<CameraComponent*>(static_cast<void*>(storage.get()));
            delete camera->framebuffer;
            camera->framebuffer = nullptr;
        }
        desc.body.ops->destroy(storage.get());
        return out;
    }

}

TEST(CookDeterminism, EveryComponentDefaultInitialisedCooksTheSameBytes) {
    size_t checked = 0;
    for (const reflect::ComponentDesc* desc : reflect::all()) {
        const jasperengine::cook::ComponentTable* table = jasperengine::cook::findComponentTable(fnv1a32(desc->key));
        if (!table) continue;   // a component builds do not carry has no runtime table (RuntimeFields.Generated lists them)
        ASSERT_NE(desc->defaultInitialise, nullptr) << desc->cppName;
        EXPECT_EQ(cookedDefault(*desc, *table, 0xA5), cookedDefault(*desc, *table, 0x5A))
            << desc->cppName << ": a field without an initialiser is written as whatever the memory held";
        checked++;
    }
    EXPECT_GT(checked, 50u);
}

namespace {

    std::vector<uint8_t> cookOne(Scene& scene, Entity entity) {
        return editor::cook::SceneCooker::cookScene(&scene, 7, {entity}, NULL_ENTITY, SceneResources{});
    }

}

// A camera sized by the canvas has the size of the view that drew last in the editor: a game sizes
// it to its own canvas, so a build writes none of it (the field's derivedIf), whatever view drew
TEST(CookDeterminism, ACameraSizedByTheCanvasCooksNoSize) {
    std::vector<std::vector<uint8_t>> cooked;
    for (float aspect : {1.0f, 1.7777f}) {
        Scene scene;
        const Entity camera = scene.createUserEntity();
        scene.addComponent<Transform>(camera, {});
        CameraComponent component;
        component.autoResize = true;
        component.aspect = aspect;
        component.leftClip = -aspect;
        component.rightClip = aspect;
        scene.addComponent<CameraComponent>(camera, component);
        cooked.push_back(cookOne(scene, camera));
    }
    EXPECT_EQ(cooked[0], cooked[1]) << "the editor's view size reached the build";

    // one sized by hand keeps its size
    Scene scene;
    const Entity camera = scene.createUserEntity();
    scene.addComponent<Transform>(camera, {});
    CameraComponent fixed;
    fixed.autoResize = false;
    fixed.aspect = 1.7777f;
    scene.addComponent<CameraComponent>(camera, fixed);
    const std::vector<uint8_t> bytes = cookOne(scene, camera);
    Scene loaded;
    std::string error;
    ASSERT_TRUE(CookedSceneLoader::loadScene(&loaded, std::span<const uint8_t>(bytes.data(), bytes.size()), &error)) << error;
    EXPECT_FLOAT_EQ(loaded.getComponent<CameraComponent>(camera).aspect, 1.7777f);
}

// A Text's texture is the font atlas the drawing made ("font|20"): a build ships its sampler only
TEST(CookDeterminism, ATextShipsNoFontAtlas) {
    Scene scene;
    const Entity text = scene.createUserEntity();
    scene.addComponent<Transform>(text, {});
    UIComponent ui;
    ui.texture.setId("font|20#7");
    ui.texture.setMinFilter(TextureFilter::NEAREST);
    scene.addComponent<UIComponent>(text, ui);
    scene.addComponent<TextComponent>(text, {});
    const std::vector<uint8_t> bytes = cookOne(scene, text);
    const std::string blob(bytes.begin(), bytes.end());
    EXPECT_EQ(blob.find("font|"), std::string::npos);
    Scene loaded;
    std::string error;
    ASSERT_TRUE(CookedSceneLoader::loadScene(&loaded, std::span<const uint8_t>(bytes.data(), bytes.size()), &error)) << error;
    EXPECT_EQ(loaded.getComponent<UIComponent>(text).texture.getMinFilter(), TextureFilter::NEAREST);
}
