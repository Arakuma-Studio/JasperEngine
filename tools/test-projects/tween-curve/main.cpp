// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S27's test project (docs/specs/S27-editor-tools.md AC7): a Position Tween eased along a curve
// (EaseType::CURVE, the bump 0 -> 1 -> 0 with Clamped Auto keys, the curve the editor's MCP test
// and the tween-curve smoke fixture use) moves a box from x = 0 to 10 over 1 s. Every frame the
// tween's value must be the curve at its normalized time and the box at 10 x that value; the
// first frames past t = 0.1, 0.3, 0.5, 0.7 and 0.9 log their samples. A mismatch is an error
// line. The web build (WEB_PROJECT=tween-curve tools/build.sh web, GATE=tween-curve
// tools/build.sh web-gate) and Android (ANDROID_PROJECT=tween-curve tools/build.sh android, the
// emulator test with android-expect.txt) check the same lines, so the curve evaluates alike on
// every target.

#include "JasperEngine.h"
#include "component/TweenComponent.h"

#include <cmath>
#include <cstdio>

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);
Primitive box(&scene);
PositionTween tween(&scene);

namespace {

    Curve bump() {
        Curve curve({CurveKey{0.0f, 0.0f, 0.0f, 0.0f}, CurveKey{0.5f, 1.0f, 0.0f, 0.0f}, CurveKey{1.0f, 0.0f, 0.0f, 0.0f}});
        for (CurveKey& key : curve.keys) key.setBothModes(CurveTangentMode::ClampedAuto);
        curve.updateAllTangents();
        return curve;
    }

    const float kSamples[] = {0.1f, 0.3f, 0.5f, 0.7f, 0.9f};
    int nextSample = 0;
    bool failed = false;
    Curve expected;

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);

    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    sceneCamera.setPosition(5, 2, 14);
    sceneCamera.setTarget(5, 0, 0);
    sceneCamera.activate();
    box.createBox(1, 1, 1);
    box.setColor(0.9f, 0.3f, 0.2f, 1.0f);

    expected = bump();
    tween.setTarget(&box);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f, false);
    tween.setFunctionType(EaseType::CURVE);
    tween.setCurve(expected);
    tween.start();

    Engine::onUpdate.add("tweenCurveTest", [](){
        if (failed || nextSample >= 5) return;
        const TweenComponent& t = tween.getComponent<TweenComponent>();
        const float curveValue = expected.evaluate(t.time);
        const float x = box.getPosition().x;
        if (std::fabs(t.value - curveValue) > 1e-5f || std::fabs(x - 10.0f * t.value) > 1e-3f) {
            failed = true;
            Log::error("[gate] tween_curve mismatch at time %f: value %f, curve %f, x %f", (double)t.time, (double)t.value, (double)curveValue, (double)x);
            return;
        }
        while (nextSample < 5 && t.time >= kSamples[nextSample]) {
            Log::print("[gate] tween_curve sample %d ok", nextSample + 1);
            nextSample++;
        }
        if (nextSample == 5) Log::print("[gate] tween_curve 5 samples: value = curve(time), x = 10 value");
    });
}
