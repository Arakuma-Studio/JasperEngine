#include "JasperEngine.h"
using namespace jasperengine;

#include "Image.h"

Scene scene;
Image image(&scene);

JASPERENGINE_INIT void init(){
    image.setAnchorPreset(AnchorPreset::CENTER);
    image.setTexture("jasperengine.png");

    Engine::setScalingMode(Scaling::FITWIDTH);
    Engine::setCanvasSize(1000,480);
    Engine::setScene(&scene);
}
