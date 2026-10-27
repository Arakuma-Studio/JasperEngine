// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A terrain displaced by a 16-bit heightmap (assets/heightmap.png: a Gaussian dome, 40 m at the
// centre), the format the editor's terrain tools write. GLES3 and WebGL2 have no 16-bit unorm
// textures, so the heightmap must reach the vertex shader as a format they sample
// (WEB_PROJECT=terrain tools/build.sh web, GATE=terrain tools/build.sh web-gate). The camera
// looks level at 5 m: the canvas centre is the dome's flank only when its heights are read; a
// heightmap that is not sampled leaves the terrain flat (or undrawn) and the centre is sky.

#include "JasperEngine.h"

using namespace jasperengine;

Scene scene;
Camera camera(&scene);
Light sun(&scene);
Terrain terrain(&scene);

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(0.42f, 0.58f, 0.82f, 1.0f);

    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.3f, -0.6f, -1.0f);
    sun.setIntensity(2.0f);
    sun.setShadows(false);

    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 5.0f, 95.0f);
    camera.setTarget(0.0f, 5.0f, 0.0f);
    camera.setFarClip(1000.0f);
    camera.activate();

    // a terrain is built for the scene's camera: after it
    terrain.setHeightMap("heightmap.png");
    terrain.setSize(200.0f);
    terrain.setMaxHeight(40.0f);
    terrain.createTerrain();

    Engine::setScene(&scene);
    Log::print("[gate] terrain");
}
