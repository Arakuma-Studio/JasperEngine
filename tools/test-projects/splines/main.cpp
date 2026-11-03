// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S18's test project (docs/specs/S18-splines.md R17, AC17): a 1.2 km Crown road, a follower on it
// and, 1.5 s in, a knot moved from Lua (lua/main.lua: Spline:setKnotPosition). In a build without
// pthreads (WEB_PROJECT=splines tools/build.sh web, GATE=splines tools/build.sh web-gate) the
// rebuild runs as Background jobs time-sliced by the engine's single-thread pump: every frame's
// spline work (SplineSystem::getWorkMs, the update and the chunk builds) stays within 4 ms and
// the rebuild completes within 10 frames, which the "[gate] spline rebuild" line reports (an
// error line otherwise). The canvas centre is the road's grey. On Android
// (ANDROID_PROJECT=splines tools/build.sh android) the emulator test reads the same lines.

#include "JasperEngine.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);
Light sun(&scene);
GameObject road(&scene);
GameObject cart(&scene);

namespace {

    uint64_t baseVersion = 0;
    uint64_t movedVersion = 0;
    int framesSinceMove = -1;
    double lastWork = 0.0;
    double worstFrameMs = 0.0;
    bool reported = false;

    void watchRebuild(){
        const double work = SplineSystem::getWorkMs();
        const double frame = work - lastWork;
        lastWork = work;
        Spline spline = road.getSpline();
        auto system = scene.getSystem<SplineSystem>();
        if (baseVersion == 0){
            // the version the scene started with (init's knots and radii)
            baseVersion = spline.getVersion();
        }
        if (movedVersion == 0 && spline.getVersion() != baseVersion){
            // the Lua edit landed this frame
            movedVersion = spline.getVersion();
            framesSinceMove = 0;
            worstFrameMs = 0.0;
        }
        if (framesSinceMove < 0 || reported){
            return;
        }
        worstFrameMs = std::max(worstFrameMs, frame);
        framesSinceMove++;
        if (!system->isRebuildPending(road.getEntity())){
            reported = true;
            if (framesSinceMove <= 10 && worstFrameMs <= 4.0){
                Log::print("[gate] spline rebuild within budget");
            }else{
                Log::error("[gate] spline rebuild over budget: %d frames, %.2f ms in its worst frame", framesSinceMove, worstFrameMs);
            }
            Log::print("[gate] spline rebuild took %d frames, at most %.2f ms of spline work a frame", framesSinceMove, worstFrameMs);
        }else if (framesSinceMove > 10){
            reported = true;
            Log::error("[gate] spline rebuild over budget: still pending after 10 frames");
        }
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(0.42f, 0.58f, 0.82f, 1.0f);

    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.2f, -1.0f, -0.3f);
    sun.setIntensity(2.0f);
    sun.setShadows(false);

    // a 1.2 km road along X (its rebuild is above the inline budget: Background jobs)
    road.setName("Road");
    Spline spline = road.getSpline();
    spline.setType(spline::SplineType::CatmullRom);
    for (int i = 0; i <= 12; i++){
        const float x = -600.0f + 100.0f * static_cast<float>(i);
        spline.addKnot(Vector3(x, 0.0f, 20.0f * std::sin(x * 0.004f)));
    }
    for (size_t i = 0; i < spline.getKnotCount(); i++){
        spline.setKnotRadius(i, 6.0f);
    }
    SplineExtrude extrude = road.getSplineExtrude();
    extrude.setShape(SplineExtrudeShape::Crown);
    extrude.setStationSpacing(0.5f);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(road.getEntity());
    mesh.submeshes[0].material.baseColorFactor = Vector4(0.45f, 0.45f, 0.45f, 1.0f);
    mesh.submeshes[0].material.metallicFactor = 0.0f;
    mesh.submeshes[0].material.roughnessFactor = 1.0f;

    cart.setName("Cart");
    SplineAnimate animate = cart.getSplineAnimate();
    animate.setSpline(road.getEntity());
    animate.setMethod(SplineAnimateMethod::Speed);
    animate.setSpeed(20.0f);

    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    // looking down at the knot Lua moves (to z = 25): the canvas centre is the road there
    sceneCamera.setPosition(0.0f, 60.0f, 85.0f);
    sceneCamera.setTarget(0.0f, 0.0f, 25.0f);
    sceneCamera.setFarClip(2000.0f);
    sceneCamera.activate();

    Engine::onPostUpdate = watchRebuild;
    Engine::setScene(&scene);
    Log::print("[gate] splines");
}
