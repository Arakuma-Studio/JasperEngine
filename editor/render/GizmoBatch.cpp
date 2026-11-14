// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GizmoBatch.h"

#include "object/LineRenderer.h"
#include "object/Sprite.h"
#include "texture/TextureData.h"

#include "resources/icons/sun-icon_png.h"
#include "resources/icons/bulb-icon_png.h"
#include "resources/icons/spot-icon_png.h"
#include "resources/icons/camera-icon_png.h"
#include "resources/icons/audio-source-icon_png.h"

#include <algorithm>
#include <array>
#include <cstdint>

using namespace jasperengine;

namespace {

    constexpr int GIZMO_ICON_SIZE = 128;
    constexpr int GIZMO_ICON_COUNT = (int)editor::GizmoIcon::Count;

    // room for a capacity of at least `count`, doubling (as LineRenderer and Mesh grow)
    unsigned int gizmoCapacity(size_t count, unsigned int current){
        unsigned int capacity = std::max(current, 16u);
        while (capacity < count) capacity *= 2;
        return capacity;
    }

    // the icons side by side in one texture, so one instanced sprite draws them all: the RGBA
    // texels live as long as the editor (the texture pool may read them at any load)
    const TextureData& gizmoIconAtlas(){
        static std::vector<unsigned char> texels;
        static TextureData atlas = [](){
            const std::array<std::pair<unsigned char*, unsigned int>, GIZMO_ICON_COUNT> pngs = {{
                {sun_icon_png, sun_icon_png_len}, {bulb_icon_png, bulb_icon_png_len}, {spot_icon_png, spot_icon_png_len},
                {camera_icon_png, camera_icon_png_len}, {audio_source_icon_png, audio_source_icon_png_len}}};
            const int width = GIZMO_ICON_SIZE * GIZMO_ICON_COUNT;
            texels.assign((size_t)width * GIZMO_ICON_SIZE * 4, 0);
            for (int i = 0; i < GIZMO_ICON_COUNT; i++){
                TextureData icon;
                if (!icon.loadTextureFromMemory(pngs[i].first, pngs[i].second)) continue;
                if (icon.getWidth() != GIZMO_ICON_SIZE || icon.getHeight() != GIZMO_ICON_SIZE){
                    icon.resize(GIZMO_ICON_SIZE, GIZMO_ICON_SIZE);
                }
                const bool rgba = icon.getColorFormat() == ColorFormat::RGBA;
                for (int y = 0; y < GIZMO_ICON_SIZE; y++){
                    for (int x = 0; x < GIZMO_ICON_SIZE; x++){
                        unsigned char* texel = &texels[((size_t)y * width + (size_t)(i * GIZMO_ICON_SIZE + x)) * 4];
                        for (int c = 0; c < 4; c++){
                            texel[c] = rgba ? icon.getColorComponent(x, y, c) : (c == 3 ? 255 : icon.getColorComponent(x, y, 0));
                        }
                    }
                }
            }
            return TextureData(width, GIZMO_ICON_SIZE, (unsigned int)texels.size(), ColorFormat::RGBA, 4, texels.data());
        }();
        return atlas;
    }

}

// ---------------------------------------------------------------- lines

void editor::GizmoLineBatch::addLine(const Vector3& pointA, const Vector3& pointB, const Vector4& color){
    addLine(pointA, pointB, color, color);
}

void editor::GizmoLineBatch::addLine(const Vector3& pointA, const Vector3& pointB, const Vector4& colorA, const Vector4& colorB){
    LineData line;
    line.pointA = pointA;
    line.pointB = pointB;
    line.colorA = colorA;
    line.colorB = colorB;
    pending.push_back(line);
}

void editor::GizmoLineBatch::addLines(const std::vector<LineData>& lines, const Matrix4& toWorld){
    for (LineData line : lines){
        line.pointA = toWorld * line.pointA;
        line.pointB = toWorld * line.pointB;
        pending.push_back(line);
    }
}

void editor::GizmoLineBatch::flush(){
    if (!renderer){
        return;
    }
    if (pending.empty()){
        hide();
        return;
    }
    renderer->setVisible(true);
    if (pending == shown){
        return;
    }
    // written in place: clearLines() would reload the renderer's buffers every time
    LineRendererComponent& lines = renderer->getComponent<LineRendererComponent>();
    if (lines.maxLines < pending.size()){
        renderer->setMaxLines(gizmoCapacity(pending.size(), lines.maxLines));
    }
    lines.mode = LineRendererMode::Segments;
    lines.lines = pending;
    renderer->updateLines();
    shown.swap(pending);
}

void editor::GizmoLineBatch::hide(){
    if (renderer){
        renderer->setVisible(false);
    }
    // a hidden renderer is written again when it shows (the render drops what a hidden one drew)
    shown.clear();
}

void editor::GizmoLineBatch::setRenderer(LineRenderer* lines){
    renderer = lines;
    shown.clear();
}

void editor::GizmoLineBatch::forgetRenderer(){
    renderer = nullptr;
    shown.clear();
}

// ---------------------------------------------------------------- icons

void editor::GizmoIconBatch::add(GizmoIcon icon, const Vector3& position, float scale){
    InstanceData instance;
    instance.position = position;
    instance.scale = Vector3(scale, scale, scale);
    const float width = 1.0f / (float)GIZMO_ICON_COUNT;
    instance.textureRect = Rect(width * (float)(int)icon, 0.0f, width, 1.0f);
    pending.push_back(instance);
}

void editor::GizmoIconBatch::flush(){
    if (!sprite){
        return;
    }
    if (pending.empty()){
        hide();
        return;
    }
    sprite->setVisible(true);
    const bool same = pending.size() == shown.size() && std::equal(pending.begin(), pending.end(), shown.begin(),
        [](const InstanceData& a, const InstanceData& b){
            Rect rectA = a.textureRect;   // (Rect compares non-const)
            return a.position == b.position && a.scale == b.scale && rectA == b.textureRect;
        });
    if (same){
        return;
    }
    InstancedMeshComponent& instmesh = sprite->getComponent<InstancedMeshComponent>();
    if (instmesh.maxInstances < pending.size()){
        sprite->setMaxInstances(gizmoCapacity(pending.size(), instmesh.maxInstances));
    }
    instmesh.instances = pending;
    sprite->updateInstances();
    shown.swap(pending);
}

void editor::GizmoIconBatch::hide(){
    if (sprite){
        sprite->setVisible(false);
    }
    // a hidden sprite's instances are written again when it shows (the render drops a hidden
    // instanced mesh's instances: the icon did not come back with the same ones)
    shown.clear();
}

void editor::GizmoIconBatch::setSprite(Sprite* icons){
    sprite = icons;
    shown.clear();
    if (!sprite){
        return;
    }
    sprite->setTexture("editor:resources:gizmo_icons", gizmoIconAtlas());
    sprite->setSize(GIZMO_ICON_SIZE, GIZMO_ICON_SIZE);
    sprite->setReceiveLights(false);
    sprite->setCastShadows(false);
    sprite->setReceiveShadows(false);
    sprite->setPivotPreset(PivotPreset::CENTER);
    // the icons' edges are soft: blended, whatever the texture reports
    sprite->setAutoTransparency(false);
    sprite->setTransparent(true);
    sprite->createInstancedMesh();
    sprite->setInstancedBillboard(billboard);
    sprite->setVisible(false);
}

void editor::GizmoIconBatch::forgetSprite(){
    sprite = nullptr;
    shown.clear();
}
