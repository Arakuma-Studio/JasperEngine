// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S13's test project: the post FX of a game on a phone's GPU path (GLES3 / WebGL2). A lit box on
// a floor drawn through the uber pass with the VHS tape, the Lens film grain, film damage and
// halation (its own two passes, bloom being off), with dynamic resolution on. Once the uber pass
// runs it logs what the renderer applies; tools/android-emulator-test.sh checks the lines
// (ANDROID_PROJECT=post-fx tools/build.sh android, then android-expect.txt).

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;
Camera camera(&scene);
Primitive floorBox(&scene);
Primitive box(&scene);
Light sun(&scene);
int frames = 0;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(960, 540);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.2f, 0.25f, 0.35f);

    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 2.0f, 6.0f);
    camera.setTarget(Vector3(0.0f, 0.5f, 0.0f));
    scene.setCamera(&camera);

    floorBox.createBox(8.0f, 0.1f, 8.0f);
    floorBox.setColor(0.6f, 0.6f, 0.6f);
    box.createBox(1.0f, 1.0f, 1.0f);
    box.setPosition(0.0f, 0.55f, 0.0f);
    box.setColor(1.0f, 0.9f, 0.8f);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -1.0f, -0.6f);
    sun.setIntensity(4.0f);

    PostProcessingSettings post;
    post.vhsEnabled = true;
    post.filmGrainEnabled = true;
    post.filmGrainType = FilmGrainType::LENS;
    post.filmDamageEnabled = true;
    post.halationEnabled = true;
    post.halationThreshold = 0.6f;
    post.dynamicResolutionEnabled = true;
    scene.setPostProcessing(post);

    // logged once the uber pass runs and dynamic resolution has measured a frame (the variant may
    // still be building at first: runtime shaders on a phone), or after 600 frames as it is then
    Engine::onUpdate.add("postFxTest", [](){
        if (frames < 0)
            return;
        frames++;
        RenderSystem* render = scene.getSystem<RenderSystem>().get();
        const bool settled = render->getActivePostProcessingTiers().uber && render->getDynamicResolutionStatus().running &&
                             std::string(render->getDynamicResolutionStatus().source) != "none";
        if (!settled && frames < 600)
            return;
        frames = -1;
        const PostProcessingTiers& tiers = render->getActivePostProcessingTiers();
        Log::print("[postfx] uber %d features 0x%x chain %d", tiers.uber ? 1 : 0, tiers.uberFeatures, tiers.halationChain ? 1 : 0);
        const DynamicResolutionStatus status = render->getDynamicResolutionStatus();
        Log::print("[postfx] dynamic resolution running %d source %s", status.running ? 1 : 0, status.source);
        Log::print("[postfx] scale %.2f target %.1f ms", status.scale, status.targetMs);
    });
}
