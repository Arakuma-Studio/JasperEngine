// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S21's test project: the Animator on the web (WebGL2, simd128 and scalar pose kernels) and on
// Android (GLES3, SSE2 / NEON). The animated hero plays an Animator Controller
// (assets/anim/hero.controller: Idle, and a Speed blend tree of Idle and Walk reached by the Go
// trigger) with the Animator writing the skin; the A-pose robot plays the same controller
// retargeted. Both hold their states at a fixed normalized time (speed 0), so the pose the gate
// reads is the same every frame and on every backend: WEB_PROJECT=animator tools/build.sh web,
// GATE=animator tools/build.sh web-gate; ANDROID_PROJECT=animator tools/build.sh android and
// tools/android-emulator-test.sh with android-expect.txt.

#include "JasperEngine.h"
#include "animation/AnimatorPose.h"
#include "subsystem/MeshSystem.h"

#include <cmath>

using namespace jasperengine;

Scene scene;
Camera camera(&scene);
Light sun(&scene);
Model hero(&scene);
Model robot(&scene);
int frames = 0;
bool reported = false;

static float round2(float value){
    return std::round(value * 100.0f) / 100.0f;
}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(0.10f, 0.11f, 0.14f, 1.0f);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -1.0f, -0.6f);
    sun.setIntensity(4.0f);
    sun.setShadows(false);

    // as a scene's ModelComponent loads: the file's root node under the model's GameObject
    scene.getSystem<MeshSystem>()->loadGLTF(hero.getEntity(), "models/hero_animated.gltf", false, false, false);
    scene.getSystem<MeshSystem>()->loadGLTF(robot.getEntity(), "models/robot_apose.gltf", false, false, false);
    hero.setPosition(-0.8f, 0.0f, 0.0f);
    robot.setPosition(0.8f, 0.0f, 0.0f);
    for (Model* model : {&hero, &robot}){
        Animator animator(&scene, model->getEntity());
        animator.setController("anim/hero.controller");
        animator.setSpeed(0.0f);
    }

    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 1.0f, 4.0f);
    camera.setTarget(0.0f, 0.9f, 0.0f);
    camera.activate();
    Engine::setScene(&scene);
    Log::print("[gate] animator pose kernels: %s", anim::poseNativeBackendName());

    Engine::onUpdate.add("animatorTest", [](){
        frames++;
        Animator heroAnimator(&scene, hero.getEntity());
        Animator robotAnimator(&scene, robot.getEntity());
        if (frames == 5){
            // the trigger takes Idle to Move at once; the tree half way, held at 25 % of the walk
            for (Animator* animator : {&heroAnimator, &robotAnimator}){
                animator->setTrigger("Go");
            }
        }
        if (frames == 8){
            for (Animator* animator : {&heroAnimator, &robotAnimator}){
                animator->play("Move", 0, 0.25f);
            }
        }
        if (frames == 12 && !reported){
            reported = true;
            const Vector3 h = heroAnimator.getBonePosition("left_foot");
            const Vector3 r = robotAnimator.getBonePosition("left_foot");
            Log::print("[gate] animator %s %s humanoid %d %d", heroAnimator.getCurrentState(0).name.c_str(),
                       robotAnimator.getCurrentState(0).name.c_str(), heroAnimator.isHumanoid() ? 1 : 0, robotAnimator.isHumanoid() ? 1 : 0);
            Log::print("[gate] animator left_foot %.2f %.2f %.2f / %.2f %.2f %.2f", round2(h.x), round2(h.y), round2(h.z),
                       round2(r.x), round2(r.y), round2(r.z));
        }
    });
}
