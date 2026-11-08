// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelPerfectEditor.h"

#include "Project.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/UIComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;

namespace jasperengine::editor::pixelart {

    void gameViewSize(const Project& project, int& width, int& height){
        float viewWidth = 0.0f, viewHeight = 0.0f, dpi = 0.0f;
        project.getEditViewSize(viewWidth, viewHeight, dpi);
        width = (int)std::lround(viewWidth);
        height = (int)std::lround(viewHeight);
        if (width <= 0 || height <= 0){
            width = (int)project.getCanvasWidth();
            height = (int)project.getCanvasHeight();
        }
    }

    bool computeEditLayout(const Project& project, Scene* scene, Entity camera, jasperengine::pixelart::PixelPerfectLayout& layout){
        PixelPerfectViewRequest request;
        if (!Engine::getPixelPerfectRequest(scene, camera, request))
            return false;
        // Use Project Canvas: the project's canvas (outside Play the Engine's is the editor view's)
        if (request.referenceWidth <= 0 || request.referenceHeight <= 0){
            request.referenceWidth = (int)project.getCanvasWidth();
            request.referenceHeight = (int)project.getCanvasHeight();
        }
        int width = 0, height = 0;
        gameViewSize(project, width, height);
        layout = Engine::computePixelPerfectView(request, width, height, Engine::getPixelPerfectForcedScale());
        const PixelPerfectCameraComponent& component = scene->getComponent<PixelPerfectCameraComponent>(camera);
        if (!component.upscaleRenderTexture){
            layout.targetWidth = layout.baseWidth;
            layout.targetHeight = layout.baseHeight;
        }
        return layout.baseWidth > 0;
    }

    void refreshEditLayout(const Project& project, Scene* scene, Entity camera, bool playing){
        if (playing || !scene || !scene->isComponentRegistered<PixelPerfectCameraComponent>())
            return;
        PixelPerfectCameraComponent* component = scene->findComponent<PixelPerfectCameraComponent>(camera);
        if (!component)
            return;
        jasperengine::pixelart::PixelPerfectLayout layout;
        if (!computeEditLayout(project, scene, camera, layout))
            layout = jasperengine::pixelart::PixelPerfectLayout();
        component->layout = layout;
    }

    std::string describeLayout(const jasperengine::pixelart::PixelPerfectLayout& layout, int screenWidth, int screenHeight){
        const int barsX = std::max(0, (int)std::lround(layout.viewport.getX()));
        const int barsY = std::max(0, (int)std::lround(layout.viewport.getY()));
        char text[160];
        std::snprintf(text, sizeof(text), "%dx - %dx%d in %dx%d, bars %dx%d%s", layout.scale, layout.baseWidth, layout.baseHeight,
                      screenWidth, screenHeight, barsX, barsY, layout.cropped ? " (cropped)" : "");
        return text;
    }

    bool isLayerScene(const Project& project, uint32_t sceneId){
        for (const SceneProject& other : project.getScenes()){
            for (const ChildSceneRef& child : other.childScenes){
                if (child.id == sceneId && other.id != sceneId)
                    return true;
            }
        }
        return false;
    }

    std::string fixedResolutionNote(const SceneProject* sceneProject){
        if (!scenePixelPerfect(sceneProject))
            return std::string();
        return "Driven by Pixel Perfect Camera on " + sceneProject->scene->getEntityName(sceneProject->mainCamera) +
               ": these settings are kept and unused.";
    }

    bool scenePixelPerfect(const SceneProject* sceneProject){
        PixelPerfectViewRequest request;
        return sceneProject && sceneProject->scene &&
               Engine::getPixelPerfectRequest(sceneProject->scene, sceneProject->mainCamera, request);
    }

    float pixelZoomScale(float zoom, int pixelsPerUnit){
        return zoom > 0.0f ? 1.0f / (zoom * (float)std::max(1, pixelsPerUnit)) : 1.0f;
    }

    float zoomForPixelScale(float scale, int pixelsPerUnit){
        return 1.0f / (std::max(scale, 1.0f / 64.0f) * (float)std::max(1, pixelsPerUnit));
    }

    const std::vector<float>& pixelZoomSteps(){
        static const std::vector<float> steps = [](){
            std::vector<float> values = {0.125f, 0.25f, 0.5f};
            for (int k = 1; k <= 32; k++)
                values.push_back((float)k);
            return values;
        }();
        return steps;
    }

    float nearestPixelZoom(float scale){
        const std::vector<float>& steps = pixelZoomSteps();
        float best = steps.front();
        for (float step : steps){
            // compared as ratios: 1/2 and 1 are as far apart as 1 and 2
            if (std::fabs(std::log(step / scale)) < std::fabs(std::log(best / scale)))
                best = step;
        }
        return best;
    }

    float stepPixelZoom(float scale, int direction){
        const std::vector<float>& steps = pixelZoomSteps();
        constexpr float epsilon = 1e-3f;
        if (direction > 0){
            for (float step : steps){
                if (step > scale * (1.0f + epsilon))
                    return step;
            }
            return steps.back();
        }
        for (auto it = steps.rbegin(); it != steps.rend(); ++it){
            if (*it < scale * (1.0f - epsilon))
                return *it;
        }
        return steps.front();
    }

    std::string pixelZoomLabel(float scale){
        char text[16];
        if (scale >= 1.0f)
            std::snprintf(text, sizeof(text), "%dx", (int)std::lround(scale));
        else
            std::snprintf(text, sizeof(text), "1/%dx", (int)std::lround(1.0f / scale));
        return text;
    }

    float snapToViewPixels(float position, float zoom){
        return zoom > 0.0f ? std::round(position / zoom) * zoom : position;
    }

    std::vector<std::string> pixelPerfectWarnings(Scene* scene, Entity camera, Entity mainCamera){
        std::vector<std::string> warnings;
        const PixelPerfectCameraComponent* component = scene->findComponent<PixelPerfectCameraComponent>(camera);
        const CameraComponent* cameraComponent = scene->findComponent<CameraComponent>(camera);
        if (!component)
            return warnings;
        if (!cameraComponent){
            warnings.push_back("Pixel Perfect Camera needs a Camera on the same GameObject.");
            return warnings;
        }
        if (cameraComponent->type == CameraType::CAMERA_PERSPECTIVE)
            warnings.push_back("Pixel Perfect Camera needs an orthographic or UI camera: it does nothing on a perspective one.");
        if (mainCamera != camera && !cameraComponent->renderToTexture)
            warnings.push_back("Not the main camera of its scene: it lays out nothing until it is.");
        if (component->rotationMode == PixelRotation::ON_GRID && !component->upscaleRenderTexture)
            warnings.push_back("Rotate On Grid without Upscale Render Texture rotates at the screen's resolution (mixels).");
        if (component->enabled && scene->isFixedResolutionEnabled())
            warnings.push_back("Overrides the scene's Fixed Resolution (kept, not used while this drives the camera).");
        if (Engine::getPixelsPerUnit() > 1 && scene->getComponentArray<UIComponent>()->size() > 0)
            warnings.push_back("Pixels Per Unit above 1 with UI in this scene: the UI stays in camera units (its texels are not "
                               "base pixels). Draw the HUD in a layer scene.");
        return warnings;
    }

}
